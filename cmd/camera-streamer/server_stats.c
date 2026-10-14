// The health of the host camera-streamer runs on, for the server panel on
// /telemetry: what the board is, whether its supply has sagged or it has
// throttled, how hot it is, how loaded, its Wi-Fi signal, what it sends, and
// the stream's frame rate.
//
// Read from /proc and /sys rather than from vcgencmd, which would be a
// process a second. Sampled at most once a second however many are
// watching, which also gives the rates an interval to be rates over. A value
// the host does not have - no Wi-Fi, no Raspberry Pi firmware - is null.

#include "cmd/camera-streamer/server_stats.h"
#include "device/buffer_lock.h"
#include "output/output.h"

#include <dirent.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SERVER_STATS_MS 1000

static pthread_mutex_t server_stats_lock = PTHREAD_MUTEX_INITIALIZER;
static char server_stats[1024] = "{}";
static uint64_t sampled_at;
static unsigned long long last_tx;
static int last_frames;

static uint64_t now_ms()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// The file's first line, without the newline.
static bool read_line(const char *path, char *buf, size_t len)
{
  FILE *f = fopen(path, "r");
  if (!f)
    return false;
  bool ok = fgets(buf, len, f) != NULL;
  fclose(f);
  if (ok)
    buf[strcspn(buf, "\n")] = 0;
  return ok;
}

// "Raspberry Pi Zero 2 W Rev 1.0", as the firmware names the board. Kept to
// what can go in a JSON string unescaped.
static void read_model(char *model, size_t len)
{
  FILE *f = fopen("/proc/device-tree/model", "r");
  size_t n = 0;

  if (f) {
    n = fread(model, 1, len - 1, f);
    fclose(f);
  }
  model[n] = 0;
  for (char *p = model; *p; p++) {
    if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20)
      *p = ' ';
  }
}

// The firmware's throttled flags: bit 0 under-voltage, 1 clock capped,
// 2 throttled, 3 soft temperature limit, each now; bits 16-19 the same since
// boot.
static long read_throttled()
{
  char line[64];

  if (read_line("/sys/devices/platform/soc/soc:firmware/get_throttled", line, sizeof(line)))
    return strtol(line, NULL, 16);
  return -1;
}

// Kernels without those flags have the hwmon alarm, which is only
// under-voltage, and only now.
static int read_undervoltage()
{
  DIR *dir = opendir("/sys/class/hwmon");
  if (!dir)
    return -1;

  int undervoltage = -1;
  struct dirent *entry;
  while ((entry = readdir(dir)) && undervoltage < 0) {
    char path[300], name[64];
    snprintf(path, sizeof(path), "/sys/class/hwmon/%s/name", entry->d_name);
    if (!read_line(path, name, sizeof(name)) || strcmp(name, "rpi_volt"))
      continue;
    snprintf(path, sizeof(path), "/sys/class/hwmon/%s/in0_lcrit_alarm", entry->d_name);
    if (read_line(path, name, sizeof(name)))
      undervoltage = atoi(name) ? 1 : 0;
  }
  closedir(dir);
  return undervoltage;
}

// The signal of the first wireless interface, in dBm.
static bool read_wifi(int *dbm)
{
  FILE *f = fopen("/proc/net/wireless", "r");
  char line[256];
  bool found = false;

  if (!f)
    return false;
  for (int i = 0; fgets(line, sizeof(line), f) && !found; i++) {
    float level;
    if (i >= 2 && sscanf(line, " %*[^:]: %*x %*f %f", &level) == 1) {
      *dbm = (int)level;
      found = true;
    }
  }
  fclose(f);
  return found;
}

// Bytes sent by every interface but the loopback, which is only the way to
// a proxy on this host, such as frpc.
static bool read_tx(unsigned long long *tx)
{
  FILE *f = fopen("/proc/net/dev", "r");
  char line[512];

  if (!f)
    return false;
  *tx = 0;
  for (int i = 0; fgets(line, sizeof(line), f); i++) {
    char name[64];
    unsigned long long bytes;
    if (i >= 2 && sscanf(line, " %63[^:]: %*u %*u %*u %*u %*u %*u %*u %*u %llu", name, &bytes) == 2
        && strcmp(name, "lo")) {
      *tx += bytes;
    }
  }
  fclose(f);
  return true;
}

static void read_memory(unsigned long *total_kb, unsigned long *available_kb)
{
  FILE *f = fopen("/proc/meminfo", "r");
  char line[128];

  *total_kb = *available_kb = 0;
  if (!f)
    return;
  while (fgets(line, sizeof(line), f)) {
    sscanf(line, "MemTotal: %lu kB", total_kb);
    sscanf(line, "MemAvailable: %lu kB", available_kb);
  }
  fclose(f);
}

static void server_stats_sample(uint64_t now)
{
  static char model[128];
  static bool model_read;
  char line[128], *p = server_stats, *end = server_stats + sizeof(server_stats);

  if (!model_read) {
    read_model(model, sizeof(model));
    model_read = true;
  }

  p += snprintf(p, end - p, "{\"model\":\"%s\"", model);

  long throttled = read_throttled();
  int undervoltage = throttled < 0 ? read_undervoltage() : -1;
  if (throttled >= 0)
    p += snprintf(p, end - p, ",\"throttled\":%ld,\"undervoltage\":null", throttled);
  else if (undervoltage >= 0)
    p += snprintf(p, end - p, ",\"throttled\":null,\"undervoltage\":%d", undervoltage);
  else
    p += snprintf(p, end - p, ",\"throttled\":null,\"undervoltage\":null");

  if (read_line("/sys/class/thermal/thermal_zone0/temp", line, sizeof(line)))
    p += snprintf(p, end - p, ",\"temp\":%.1f", atol(line) / 1000.0);
  else
    p += snprintf(p, end - p, ",\"temp\":null");

  if (read_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", line, sizeof(line)))
    p += snprintf(p, end - p, ",\"mhz\":%ld", atol(line) / 1000);
  else
    p += snprintf(p, end - p, ",\"mhz\":null");

  double load = 0;
  if (read_line("/proc/loadavg", line, sizeof(line)) && sscanf(line, "%lf", &load) == 1)
    p += snprintf(p, end - p, ",\"load\":%.2f", load);
  else
    p += snprintf(p, end - p, ",\"load\":null");

  unsigned long total_kb, available_kb;
  read_memory(&total_kb, &available_kb);
  p += snprintf(p, end - p, ",\"mem_total_kb\":%lu,\"mem_available_kb\":%lu", total_kb, available_kb);

  int dbm;
  if (read_wifi(&dbm))
    p += snprintf(p, end - p, ",\"wifi_dbm\":%d", dbm);
  else
    p += snprintf(p, end - p, ",\"wifi_dbm\":null");

  double uptime = 0;
  if (read_line("/proc/uptime", line, sizeof(line)) && sscanf(line, "%lf", &uptime) == 1)
    p += snprintf(p, end - p, ",\"uptime_s\":%.0f", uptime);
  else
    p += snprintf(p, end - p, ",\"uptime_s\":null");

  // Rates over the time since the last sample; none on the first, or after
  // a long while nobody watched.
  uint64_t interval = now - sampled_at;
  bool rated = sampled_at > 0 && interval < 10 * SERVER_STATS_MS;

  unsigned long long tx = 0;
  bool have_tx = read_tx(&tx);
  if (have_tx && rated && tx >= last_tx)
    p += snprintf(p, end - p, ",\"tx_bps\":%.0f", (tx - last_tx) * 1000.0 / interval);
  else
    p += snprintf(p, end - p, ",\"tx_bps\":null");
  if (have_tx)
    last_tx = tx;

  pthread_mutex_lock(&stream_lock.lock);
  bool streaming = stream_lock.buf_list != NULL;
  int frames = stream_lock.counter;
  pthread_mutex_unlock(&stream_lock.lock);
  if (streaming && rated && frames >= last_frames)
    p += snprintf(p, end - p, ",\"fps\":%.1f", (frames - last_frames) * 1000.0 / interval);
  else
    p += snprintf(p, end - p, ",\"fps\":null");
  last_frames = frames;

  snprintf(p, end - p, "}");
  sampled_at = now;
}

void server_stats_json(FILE *stream)
{
  char json[sizeof(server_stats)];

  pthread_mutex_lock(&server_stats_lock);
  uint64_t now = now_ms();
  if (now - sampled_at >= SERVER_STATS_MS)
    server_stats_sample(now);
  strcpy(json, server_stats);
  pthread_mutex_unlock(&server_stats_lock);

  fputs(json, stream);
}
