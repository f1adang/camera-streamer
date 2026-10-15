// The health of the host camera-streamer runs on, for the server box on
// /telemetry: what the board is - its model, SoC, CPU, clock and memory -
// and the camera's sensor, whether its supply has sagged or it has
// throttled, how hot it is, how loaded, its Wi-Fi signal, what it sends, and
// the stream's frame rate.
//
// Read from /proc and /sys rather than from vcgencmd, which would be a
// process a second. Sampled at most once a second however many are
// watching, which also gives the rates an interval to be rates over. A value
// the host does not have - no Wi-Fi, no Raspberry Pi firmware - is null.

#include "cmd/camera-streamer/server_stats.h"
#include "device/buffer_lock.h"
#include "device/camera/camera.h"
#include "output/output.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SERVER_STATS_MS 1000

extern camera_options_t camera_options;

static pthread_mutex_t server_stats_lock = PTHREAD_MUTEX_INITIALIZER;
static char server_stats[1536] = "{}";
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

// Kept to what can go in a JSON string unescaped.
static void json_safe(char *s)
{
  for (; *s; s++) {
    if (*s == '"' || *s == '\\' || (unsigned char)*s < 0x20)
      *s = ' ';
  }
}

// "Raspberry Pi Zero 2 W Rev 1.0", as the firmware names the board.
static void read_model(char *model, size_t len)
{
  FILE *f = fopen("/proc/device-tree/model", "r");
  size_t n = 0;

  if (f) {
    n = fread(model, 1, len - 1, f);
    fclose(f);
  }
  model[n] = 0;
  json_safe(model);
}

// What the board is, beyond its name: "BCM2837" from the device tree's
// compatible list, the CPU's cores and design, its top clock, and the
// memory the board was made with, from the revision code a Raspberry Pi's
// firmware leaves in the device tree (MemTotal is that less what the GPU
// and the kernel keep).
typedef struct {
  char soc[32];
  int cores;
  char cpu[32];
  long max_mhz;
  long ram_mb;
} board_specs_t;

static const char *cpu_part(unsigned part)
{
  switch (part) {
  case 0xb76: return "ARM1176";
  case 0xc07: return "Cortex-A7";
  case 0xd03: return "Cortex-A53";
  case 0xd04: return "Cortex-A35";
  case 0xd05: return "Cortex-A55";
  case 0xd07: return "Cortex-A57";
  case 0xd08: return "Cortex-A72";
  case 0xd0b: return "Cortex-A76";
  default: return NULL;
  }
}

static void read_specs(board_specs_t *specs)
{
  char buf[512], line[128];
  FILE *f;

  memset(specs, 0, sizeof(*specs));

  // "raspberrypi,model-zero-2-w\0brcm,bcm2837\0": the SoC is the brcm one.
  if ((f = fopen("/proc/device-tree/compatible", "r"))) {
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    for (size_t at = 0; at < n; at += strlen(buf + at) + 1) {
      if (!strncmp(buf + at, "brcm,", 5)) {
        snprintf(specs->soc, sizeof(specs->soc), "%s", buf + at + 5);
        for (char *c = specs->soc; *c; c++)
          *c = toupper((unsigned char)*c);
      }
    }
  }

  specs->cores = sysconf(_SC_NPROCESSORS_CONF);

  if ((f = fopen("/proc/cpuinfo", "r"))) {
    unsigned part;
    while (fgets(line, sizeof(line), f) && !specs->cpu[0]) {
      if (sscanf(line, "CPU part : %x", &part) == 1 && cpu_part(part))
        snprintf(specs->cpu, sizeof(specs->cpu), "%s", cpu_part(part));
    }
    fclose(f);
  }

  if (read_line("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", line, sizeof(line)))
    specs->max_mhz = atol(line) / 1000;

  // A new-style revision code, bit 23 set, has the memory in bits 20-22.
  unsigned char rev[4];
  if ((f = fopen("/proc/device-tree/system/linux,revision", "r"))) {
    if (fread(rev, 1, 4, f) == 4) {
      unsigned long code = (unsigned long)rev[0] << 24 | rev[1] << 16 | rev[2] << 8 | rev[3];
      if (code & 0x800000)
        specs->ram_mb = 256L << (code >> 20 & 7);
    }
    fclose(f);
  }
}

// The sensor's maker, from how its driver is named.
static const char *sensor_maker(const char *driver)
{
  static const struct { const char *prefix, *maker; } makers[] = {
    { "imx", "Sony" }, { "ov", "OmniVision" }, { "ar0", "onsemi" }, { "mt9", "onsemi" },
    { "gc", "GalaxyCore" }, { "s5k", "Samsung" }, { "hm", "Himax" }, { "arducam", "Arducam" },
  };
  for (size_t i = 0; i < sizeof(makers) / sizeof(makers[0]); i++) {
    if (!strncmp(driver, makers[i].prefix, strlen(makers[i].prefix)))
      return makers[i].maker;
  }
  return NULL;
}

// "Sony IMX219": the sensor a CSI camera's driver names in its subdevice,
// "imx219 10-0010", or, for a USB camera, the name it gives itself.
static void read_camera(char *camera, size_t len)
{
  DIR *dir = opendir("/sys/class/video4linux");
  struct dirent *entry;
  char path[300], name[128];

  camera[0] = 0;
  while (dir && (entry = readdir(dir)) && !camera[0]) {
    if (strncmp(entry->d_name, "v4l-subdev", 10))
      continue;
    snprintf(path, sizeof(path), "/sys/class/video4linux/%s/name", entry->d_name);
    if (!read_line(path, name, sizeof(name)))
      continue;
    name[strcspn(name, " ")] = 0;
    const char *maker = sensor_maker(name);
    if (!maker)
      continue;
    if (!strcmp(maker, "Arducam")) {
      snprintf(camera, len, "%s", name);
    } else {
      for (char *c = name; *c; c++)
        *c = toupper((unsigned char)*c);
      snprintf(camera, len, "%s %s", maker, name);
    }
  }
  if (dir)
    closedir(dir);

  // A USB camera: the device given, or the first.
  const char *video = !strncmp(camera_options.path, "/dev/video", 10) ? camera_options.path + 5 : "video0";
  snprintf(path, sizeof(path), "/sys/class/video4linux/%s/name", video);
  if (!camera[0] && read_line(path, name, sizeof(name)) && strncmp(name, "unicam", 6)
      && strncmp(name, "rp1-cfe", 7) && strncmp(name, "bcm2835", 7) && strncmp(name, "pispbe", 6))
    snprintf(camera, len, "%s", name);
  json_safe(camera);
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
  static char model[128], camera[128];
  static board_specs_t specs;
  static bool model_read;
  char line[128], *p = server_stats, *end = server_stats + sizeof(server_stats);

  if (!model_read) {
    read_model(model, sizeof(model));
    read_specs(&specs);
    read_camera(camera, sizeof(camera));
    model_read = true;
  }

  p += snprintf(p, end - p, "{\"model\":\"%s\"", model);
  p += snprintf(p, end - p, specs.soc[0] ? ",\"soc\":\"%s\"" : ",\"soc\":null", specs.soc);
  p += snprintf(p, end - p, ",\"cpu_cores\":%d", specs.cores);
  p += snprintf(p, end - p, specs.cpu[0] ? ",\"cpu\":\"%s\"" : ",\"cpu\":null", specs.cpu);
  if (specs.max_mhz)
    p += snprintf(p, end - p, ",\"cpu_max_mhz\":%ld", specs.max_mhz);
  else
    p += snprintf(p, end - p, ",\"cpu_max_mhz\":null");
  if (specs.ram_mb)
    p += snprintf(p, end - p, ",\"ram_mb\":%ld", specs.ram_mb);
  else
    p += snprintf(p, end - p, ",\"ram_mb\":null");
  p += snprintf(p, end - p, camera[0] ? ",\"camera\":\"%s\"" : ",\"camera\":null", camera);

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
