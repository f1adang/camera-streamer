// A Victron battery monitor - a SmartShunt, a BMV, or one on a VE.Direct
// Bluetooth Smart dongle - heard over Bluetooth, for the battery panel on
// /telemetry.
//
// With Instant Readout on, the monitor puts its figures into every
// advertisement it sends, encrypted, about once a second, to anybody near
// enough to hear: no connection, no pairing, and nothing that would keep the
// VictronConnect app off it. The key is the one VictronConnect shows under
// Product info, Instant readout details; it changes with the Bluetooth PIN.
//
// The advertisements are heard through BlueZ, over D-Bus, rather than on a
// raw HCI socket, so that bluetoothd keeps the adapter and nothing here
// needs more than to be allowed to talk to org.bluez. They are listened for
// only while somebody watches: a Raspberry Pi's Wi-Fi and Bluetooth share a
// radio, and a scan takes time from the stream.

#include "cmd/camera-streamer/victron.h"
#include "cmd/camera-streamer/fluidnc.h"
#include "util/opts/log.h"
#include "version.h"

#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#ifdef USE_VICTRON
#include <errno.h>
#include <openssl/evp.h>
#include <systemd/sd-bus.h>
#endif

#define VICTRON_IDLE_MS     10000  // nobody watching this long, and it stops listening
#define VICTRON_DEAD_MS     60000  // nothing heard this long, and it starts the scan over
#define VICTRON_RETRY_MS    10000
#define VICTRON_COMPANY     0x02e1 // Victron's Bluetooth company identifier
#define VICTRON_ERROR       256

extern victron_options_t victron_options;

// What the monitor reports. NAN, or -1, is what it says it does not know.
typedef struct {
  uint16_t model;
  int ttg_min;               // time to go, in minutes
  double voltage;
  double current;            // A, positive while charging
  double consumed_ah;
  double soc;                // %
  unsigned alarm;            // the alarm reasons, a bit each
  double temperature;        // °C, the aux input as a temperature sensor
  double starter_voltage;    // V, the aux input on a second battery
  double midpoint_voltage;   // V, the aux input on the middle of the bank
} victron_reading_t;

static struct {
  pthread_mutex_t lock;
  pthread_cond_t asked;
  bool watched;
  uint64_t asked_at;
  uint64_t heard_at;         // 0 until the first reading
  victron_reading_t reading;
  bool have_rssi;
  int rssi;
  char error[VICTRON_ERROR];
} victron = {
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .asked = PTHREAD_COND_INITIALIZER,
};

static uint64_t now_ms()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char *victron_model(uint16_t model)
{
  switch (model) {
  case 0x0203: return "BMV-700";
  case 0x0204: return "BMV-702";
  case 0x0205: return "BMV-700H";
  case 0xA380: return "BMV-710 Smart";
  case 0xA381: return "BMV-712 Smart";
  case 0xA382: return "BMV-710H Smart";
  case 0xA383: return "BMV-712 Smart";
  case 0xA389: return "SmartShunt 500A/50mV";
  case 0xA38A: return "SmartShunt 1000A/50mV";
  case 0xA38B: return "SmartShunt 2000A/50mV";
  case 0xA38C: return "SmartShunt IP67 500A/50mV";
  case 0xA38D: return "SmartShunt IP67 1000A/50mV";
  case 0xA38E: return "SmartShunt IP67 2000A/50mV";
  case 0xC030: case 0xC035: return "SmartShunt IP65 500A/50mV";
  case 0xC031: case 0xC036: return "SmartShunt IP65 1000A/50mV";
  case 0xC032: case 0xC037: return "SmartShunt IP65 2000A/50mV";
  case 0xC034: return "BMV-800 Smart";
  case 0xC038: return "SmartShunt 300A/50mV";
  default: return NULL;
  }
}

static void victron_set_error(const char *error)
{
  pthread_mutex_lock(&victron.lock);
  snprintf(victron.error, sizeof(victron.error), "%s", error);
  pthread_mutex_unlock(&victron.lock);
}

#ifdef USE_VICTRON

#define VICTRON_PREFIX           0x10
#define VICTRON_BATTERY_MONITOR  0x02  // the record type

#define TRY(x) do { int r_ = (x); if (r_ < 0) return r_; } while (0)

static uint8_t victron_key[16];
static char adapter_path[] = "/org/bluez/hci0";
static char device_path[64];

static int hex_digit(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// The hex digits of `in`, skipping the separators in `skip`, into `len`
// bytes; there must be exactly as many.
static int parse_hex(const char *in, const char *skip, uint8_t *out, size_t len)
{
  size_t digits = 0;

  for (; *in; in++) {
    if (strchr(skip, *in))
      continue;
    int d = hex_digit(*in);
    if (d < 0 || digits >= len * 2)
      return -1;
    out[digits / 2] = digits % 2 ? out[digits / 2] << 4 | d : d;
    digits++;
  }
  return digits == len * 2 ? 0 : -1;
}

// AES-128 in CTR mode, the counter starting at the advertisement's nonce
// and counting as a little-endian 128-bit number. OpenSSL's CTR counts
// big-endian, so the counter blocks are made here and encrypted one by one.
static int victron_decrypt(uint16_t nonce, const uint8_t *in, size_t len, uint8_t *out)
{
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  uint8_t counter[16] = { nonce & 0xff, nonce >> 8 }, block[16];
  int n;
  bool ok = ctx
    && EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, victron_key, NULL)
    && EVP_CIPHER_CTX_set_padding(ctx, 0);

  for (size_t i = 0; ok && i < len; i += 16) {
    ok = EVP_EncryptUpdate(ctx, block, &n, counter, sizeof(counter)) && n == sizeof(block);
    for (size_t j = 0; j < 16 && i + j < len; j++)
      out[i + j] = in[i + j] ^ block[j];
    for (int b = 0; b < 16 && ++counter[b] == 0; b++)
      ;
  }
  EVP_CIPHER_CTX_free(ctx);
  return ok ? 0 : -1;
}

// The record is packed a bit at a time, least significant first.
static uint32_t read_bits(const uint8_t *data, unsigned *at, unsigned n)
{
  uint32_t value = 0;
  for (unsigned i = 0; i < n; i++, (*at)++)
    value |= (uint32_t)(data[*at / 8] >> (*at % 8) & 1) << i;
  return value;
}

static int32_t to_signed(uint32_t value, unsigned n)
{
  return value & 1u << (n - 1) ? (int32_t)value - (int32_t)(1u << n) : (int32_t)value;
}

// What Victron puts after its company identifier: 0x10, a byte, the model,
// the record type, the nonce, the key's first byte - which says whether the
// key is the right one before anything is decrypted with it - and the
// record, encrypted. A battery monitor's record is, in bits: time to go 16,
// voltage 16 signed, alarm 16, aux 16, aux input 2, current 22 signed,
// consumed Ah 20, state of charge 10; each all ones (the signed ones,
// their greatest value) where the monitor does not know it.
static int victron_decode(const uint8_t *data, size_t len, victron_reading_t *out, char *error, size_t error_len)
{
  uint8_t record[32] = { 0 };

  if (len < 8 || data[0] != VICTRON_PREFIX)
    return 1; // not an Instant Readout: Victron advertises other things too

  if (data[4] != VICTRON_BATTERY_MONITOR) {
    snprintf(error, error_len, "This Victron device is not a battery monitor (record type 0x%02x)", data[4]);
    return -1;
  }
  if (data[7] != victron_key[0]) {
    snprintf(error, error_len, "The encryption key does not fit; it changes with the Bluetooth PIN");
    return -1;
  }
  if (len - 8 < 15 || len - 8 > sizeof(record)) {
    snprintf(error, error_len, "A battery monitor's record of %zu bytes is not one", len - 8);
    return -1;
  }
  if (victron_decrypt(data[5] | data[6] << 8, data + 8, len - 8, record) < 0) {
    snprintf(error, error_len, "Cannot decrypt with OpenSSL");
    return -1;
  }

  unsigned at = 0;
  uint32_t ttg = read_bits(record, &at, 16);
  uint32_t voltage = read_bits(record, &at, 16);
  uint32_t alarm = read_bits(record, &at, 16);
  uint32_t aux = read_bits(record, &at, 16);
  uint32_t aux_input = read_bits(record, &at, 2);
  uint32_t current = read_bits(record, &at, 22);
  uint32_t consumed = read_bits(record, &at, 20);
  uint32_t soc = read_bits(record, &at, 10);

  out->model = data[2] | data[3] << 8;
  out->ttg_min = ttg == 0xffff ? -1 : (int)ttg;
  out->voltage = voltage == 0x7fff ? NAN : to_signed(voltage, 16) / 100.0;
  out->alarm = alarm;
  out->current = current == 0x1fffff ? NAN : to_signed(current, 22) / 1000.0;
  out->consumed_ah = consumed == 0xfffff ? NAN : consumed / 10.0;
  out->soc = soc == 0x3ff ? NAN : soc / 10.0;
  out->temperature = out->starter_voltage = out->midpoint_voltage = NAN;
  if (aux_input == 0 && aux != 0x7fff)
    out->starter_voltage = to_signed(aux, 16) / 100.0;
  else if (aux_input == 1 && aux != 0xffff)
    out->midpoint_voltage = aux / 100.0;
  else if (aux_input == 2 && aux != 0xffff)
    out->temperature = aux / 100.0 - 273.15;
  return 0;
}

static void victron_heard(const uint8_t *data, size_t len)
{
  victron_reading_t reading;
  char error[VICTRON_ERROR] = "";

  int ret = victron_decode(data, len, &reading, error, sizeof(error));
  if (ret > 0)
    return;

  pthread_mutex_lock(&victron.lock);
  if (ret == 0) {
    victron.reading = reading;
    victron.heard_at = now_ms();
  } else if (strcmp(victron.error, error)) {
    LOG_INFO(NULL, "Victron: %s.", error);
  }
  snprintf(victron.error, sizeof(victron.error), "%s", error);
  pthread_mutex_unlock(&victron.lock);
}

// org.bluez.Device1's properties, a{sv}, as they change: the advertisement's
// ManufacturerData, a{qv} of company and bytes, and its RSSI.
static int victron_read_device(sd_bus_message *m)
{
  int r;

  TRY(sd_bus_message_enter_container(m, 'a', "{sv}"));
  while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
    const char *name;
    TRY(sd_bus_message_read(m, "s", &name));

    if (!strcmp(name, "ManufacturerData")) {
      TRY(sd_bus_message_enter_container(m, 'v', "a{qv}"));
      TRY(sd_bus_message_enter_container(m, 'a', "{qv}"));
      while ((r = sd_bus_message_enter_container(m, 'e', "qv")) > 0) {
        uint16_t company;
        TRY(sd_bus_message_read(m, "q", &company));
        if (company == VICTRON_COMPANY) {
          const void *data;
          size_t len;
          TRY(sd_bus_message_enter_container(m, 'v', "ay"));
          TRY(sd_bus_message_read_array(m, 'y', &data, &len));
          TRY(sd_bus_message_exit_container(m));
          victron_heard(data, len);
        } else {
          TRY(sd_bus_message_skip(m, "v"));
        }
        TRY(sd_bus_message_exit_container(m));
      }
      TRY(r);
      TRY(sd_bus_message_exit_container(m));
      TRY(sd_bus_message_exit_container(m));
    } else if (!strcmp(name, "RSSI")) {
      int16_t rssi;
      TRY(sd_bus_message_enter_container(m, 'v', "n"));
      TRY(sd_bus_message_read(m, "n", &rssi));
      TRY(sd_bus_message_exit_container(m));
      pthread_mutex_lock(&victron.lock);
      victron.have_rssi = true;
      victron.rssi = rssi;
      pthread_mutex_unlock(&victron.lock);
    } else {
      TRY(sd_bus_message_skip(m, "v"));
    }
    TRY(sd_bus_message_exit_container(m));
  }
  TRY(r);
  return sd_bus_message_exit_container(m);
}

// Every advertisement after the first: PropertiesChanged (s a{sv} as).
static int victron_on_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
  const char *interface;

  if (sd_bus_message_read(m, "s", &interface) < 0 || strcmp(interface, "org.bluez.Device1"))
    return 0;
  victron_read_device(m);
  return 0;
}

// The first, when BlueZ makes the device up: InterfacesAdded (o a{sa{sv}}).
// BlueZ forgets a device it has not heard for a while, so the first can come
// again.
static int victron_on_added(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
  const char *path, *interface;

  if (sd_bus_message_read(m, "o", &path) < 0 || strcmp(path, device_path))
    return 0;
  if (sd_bus_message_enter_container(m, 'a', "{sa{sv}}") < 0)
    return 0;
  while (sd_bus_message_enter_container(m, 'e', "sa{sv}") > 0) {
    if (sd_bus_message_read(m, "s", &interface) < 0)
      return 0;
    if (!strcmp(interface, "org.bluez.Device1")) {
      if (victron_read_device(m) < 0)
        return 0;
    } else if (sd_bus_message_skip(m, "a{sv}") < 0) {
      return 0;
    }
    if (sd_bus_message_exit_container(m) < 0)
      return 0;
  }
  return 0;
}

static void victron_bus_error(const char *what, int r, sd_bus_error *error)
{
  char text[VICTRON_ERROR];

  if (error && sd_bus_error_is_set(error))
    snprintf(text, sizeof(text), "%s: %s", what, error->message);
  else
    snprintf(text, sizeof(text), "%s: %s", what, strerror(-r));
  LOG_INFO(NULL, "Victron: %s.", text);
  victron_set_error(text);
}

static int victron_connect(sd_bus **bus)
{
  int r = sd_bus_open_system(bus);
  if (r < 0) {
    victron_bus_error("Cannot reach the system bus", r, NULL);
    return r;
  }

  r = sd_bus_match_signal(*bus, NULL, "org.bluez", device_path,
    "org.freedesktop.DBus.Properties", "PropertiesChanged", victron_on_changed, NULL);
  if (r >= 0)
    r = sd_bus_match_signal(*bus, NULL, "org.bluez", "/",
      "org.freedesktop.DBus.ObjectManager", "InterfacesAdded", victron_on_added, NULL);
  if (r < 0) {
    victron_bus_error("Cannot listen to BlueZ", r, NULL);
    *bus = sd_bus_flush_close_unref(*bus);
  }
  return r;
}

// Every advertisement, not only those that differ from the last, though
// the monitor's all differ in the nonce, and only Bluetooth Low Energy.
static int victron_start_scan(sd_bus *bus)
{
  sd_bus_error error = SD_BUS_ERROR_NULL;

  int r = sd_bus_call_method(bus, "org.bluez", adapter_path, "org.bluez.Adapter1",
    "SetDiscoveryFilter", &error, NULL, "a{sv}", 2,
    "Transport", "s", "le",
    "DuplicateData", "b", 1);
  if (r >= 0)
    r = sd_bus_call_method(bus, "org.bluez", adapter_path, "org.bluez.Adapter1",
      "StartDiscovery", &error, NULL, "");
  if (r < 0)
    victron_bus_error("Cannot scan for Bluetooth devices", r, &error);
  sd_bus_error_free(&error);
  return r;
}

static void victron_stop_scan(sd_bus *bus)
{
  sd_bus_call_method(bus, "org.bluez", adapter_path, "org.bluez.Adapter1",
    "StopDiscovery", NULL, NULL, "");
}

static bool victron_watched(uint64_t now)
{
  return victron.watched && now - victron.asked_at < VICTRON_IDLE_MS;
}

// Waits out a retry's delay, and then until somebody watches.
static void victron_wait_for_viewers(unsigned delay_ms)
{
  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += delay_ms / 1000;
  until.tv_nsec += (delay_ms % 1000) * 1000000;
  if (until.tv_nsec >= 1000000000) {
    until.tv_sec++;
    until.tv_nsec -= 1000000000;
  }

  pthread_mutex_lock(&victron.lock);
  while (delay_ms > 0 && pthread_cond_timedwait(&victron.asked, &victron.lock, &until) != ETIMEDOUT)
    ;
  while (!victron_watched(now_ms()))
    pthread_cond_wait(&victron.asked, &victron.lock);
  pthread_mutex_unlock(&victron.lock);
}

static void *victron_thread(void *arg)
{
  uint8_t mac[6];
  sd_bus *bus = NULL;
  unsigned delay_ms = 0;

  if (parse_hex(victron_options.mac, ":-", mac, sizeof(mac)) < 0) {
    victron_set_error("-victron-mac is not a Bluetooth address");
    return NULL;
  }
  if (parse_hex(victron_options.key, "", victron_key, sizeof(victron_key)) < 0) {
    victron_set_error("-victron-key is not the 32 hex digits of an encryption key");
    return NULL;
  }
  snprintf(device_path, sizeof(device_path), "%s/dev_%02X_%02X_%02X_%02X_%02X_%02X",
    adapter_path, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  while (true) {
    victron_wait_for_viewers(delay_ms);
    delay_ms = VICTRON_RETRY_MS;

    if (!bus && victron_connect(&bus) < 0)
      continue;
    if (victron_start_scan(bus) < 0)
      continue;

    LOG_INFO(NULL, "Victron: Listening for %s.", device_path + strlen(adapter_path) + 1);
    victron_set_error("");
    uint64_t scanned_at = now_ms();

    while (true) {
      int r;
      while ((r = sd_bus_process(bus, NULL)) > 0)
        ;
      if (r >= 0)
        r = sd_bus_wait(bus, 1000000);
      if (r < 0 && r != -EINTR) {
        victron_bus_error("Lost the system bus", r, NULL);
        bus = sd_bus_flush_close_unref(bus);
        break;
      }

      uint64_t now = now_ms();
      pthread_mutex_lock(&victron.lock);
      bool watched = victron_watched(now);
      uint64_t heard_at = victron.heard_at;
      pthread_mutex_unlock(&victron.lock);

      if (!watched) {
        victron_stop_scan(bus);
        LOG_INFO(NULL, "Victron: Nobody watching; stopped listening.");
        delay_ms = 0;
        break;
      }

      // Nothing for a minute: the monitor may be out of range or off, but
      // a scan also ends without a word when bluetoothd restarts.
      if (now - scanned_at > VICTRON_DEAD_MS && now - heard_at > VICTRON_DEAD_MS) {
        LOG_INFO(NULL, "Victron: Nothing heard for %d s; scanning again.", VICTRON_DEAD_MS / 1000);
        victron_stop_scan(bus);
        if (victron_start_scan(bus) < 0)
          break;
        scanned_at = now;
      }
    }
  }
  return NULL;
}

static pthread_once_t victron_once = PTHREAD_ONCE_INIT;

static void victron_start()
{
  pthread_t thread;
  pthread_create(&thread, NULL, victron_thread, NULL);
  pthread_detach(thread);
}

#endif // USE_VICTRON

static void json_number(FILE *stream, const char *name, const char *format, double value)
{
  fprintf(stream, ",\"%s\":", name);
  if (isnan(value))
    fputs("null", stream);
  else
    fprintf(stream, format, value);
}

void victron_json(FILE *stream)
{
  if (!victron_options.mac[0]) {
    fputs("null", stream);
    return;
  }

#ifdef USE_VICTRON
  pthread_once(&victron_once, victron_start);
#else
  victron_set_error("This Brušocam was built without Bluetooth: install pkg-config, libsystemd-dev "
    "and libssl-dev, and build it again");
#endif

  pthread_mutex_lock(&victron.lock);
  uint64_t now = now_ms();
  victron.watched = true;
  victron.asked_at = now;
  pthread_cond_broadcast(&victron.asked);
  uint64_t heard_at = victron.heard_at;
  victron_reading_t r = victron.reading;
  bool have_rssi = victron.have_rssi;
  int rssi = victron.rssi;
  char error[VICTRON_ERROR];
  strcpy(error, victron.error);
  pthread_mutex_unlock(&victron.lock);

  const char *model = heard_at ? victron_model(r.model) : NULL;
  char unknown[32];
  if (heard_at && !model) {
    snprintf(unknown, sizeof(unknown), "product 0x%04X", r.model);
    model = unknown;
  }

  fputs("{\"name\":", stream);
  json_string(stream, victron_options.name);
  fputs(",\"model\":", stream);
  if (model)
    json_string(stream, model);
  else
    fputs("null", stream);
  fputs(",\"error\":", stream);
  json_string(stream, error);

  if (!heard_at) {
    fputs(",\"age\":null}", stream);
    return;
  }

  fprintf(stream, ",\"age\":%llu", (unsigned long long)(now - heard_at));
  if (have_rssi)
    fprintf(stream, ",\"rssi\":%d", rssi);
  else
    fputs(",\"rssi\":null", stream);
  json_number(stream, "soc", "%.1f", r.soc);
  json_number(stream, "voltage", "%.2f", r.voltage);
  json_number(stream, "current", "%.3f", r.current);
  json_number(stream, "consumed_ah", "%.1f", r.consumed_ah);
  if (r.ttg_min >= 0)
    fprintf(stream, ",\"ttg_min\":%d", r.ttg_min);
  else
    fputs(",\"ttg_min\":null", stream);
  fprintf(stream, ",\"alarm\":%u", r.alarm);
  json_number(stream, "temperature", "%.1f", r.temperature);
  json_number(stream, "starter_voltage", "%.2f", r.starter_voltage);
  json_number(stream, "midpoint_voltage", "%.2f", r.midpoint_voltage);
  fputs("}", stream);
}
