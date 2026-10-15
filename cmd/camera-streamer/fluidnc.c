// A FluidNC controller's status reports, read on this host and relayed to
// /telemetry, so that a browser that cannot reach the controller (behind a
// proxy, on another network) still sees it.
//
// One websocket to the controller is shared by every viewer, and held only
// while somebody watches: FluidNC keeps four websockets and drops the oldest
// for a fifth, so this is a guest on it. While anything moves, FluidNC sends
// every websocket a status report every 200 ms on its own -
// `<Run|MPos:89.032,122.430,44.991|FS:494,0|SD:65.13,/sd/a.gcode>` - and
// /command answers 503 for as long, so the reports are read, not asked for.
// Standing still they stop, so `?`, the realtime status request, is sent
// after a quiet while; no answer at all, and the socket is taken as dead.
//
// Viewers get the latest report as server-sent events on /fluidnc: one
// long-lived response, as /stream is, which passes a proxy that passes the
// stream, rather than a request per report. Each event also counts the
// viewers, the /fluidnc responses open, with or without a controller.

#include "cmd/camera-streamer/fluidnc.h"
#include "cmd/camera-streamer/server_stats.h"
#include "cmd/camera-streamer/teltonika.h"
#include "cmd/camera-streamer/victron.h"
#include "util/http/http.h"
#include "util/opts/log.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define FLUIDNC_ASK_MS      4000   // quiet this long, and it asks
#define FLUIDNC_DEAD_MS     12000  // quiet this long, and it reconnects
#define FLUIDNC_IDLE_MS     10000  // nobody watching this long, and it lets go
#define FLUIDNC_CONNECT_S   5
#define FLUIDNC_EVENT_MS    1000   // an event at least this often, changed or not
#define FLUIDNC_LINE        512
#define FLUIDNC_ERROR       1024

extern fluidnc_options_t fluidnc_options;

static const unsigned fluidnc_retry_ms[] = { 2000, 5000, 10000, 30000 };

typedef struct {
  char host[256];       // as written, with any [] of an IPv6 address
  char port[8];
  char path[256];
  const char *protocol;
} fluidnc_url_t;

static struct {
  pthread_mutex_t lock;
  pthread_cond_t changed;
  int viewers;
  uint64_t idle_since;
  bool connected;
  unsigned seq;
  uint64_t heard_at;
  char status[FLUIDNC_LINE];
  char wco[128];
  char ov[64];
  char error[FLUIDNC_ERROR];
} fluidnc = {
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .changed = PTHREAD_COND_INITIALIZER,
};

static pthread_once_t fluidnc_once = PTHREAD_ONCE_INIT;

static uint64_t now_ms()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void fluidnc_set_error(const char *error)
{
  pthread_mutex_lock(&fluidnc.lock);
  snprintf(fluidnc.error, sizeof(fluidnc.error), "%s", error);
  fluidnc.connected = false;
  fluidnc.seq++;
  pthread_cond_broadcast(&fluidnc.changed);
  pthread_mutex_unlock(&fluidnc.lock);
}

// "fluidnc.local", "http://10.0.0.5/", "10.0.0.5:81" or "ws://[fe80::1]:81".
// FluidNC's WebUI 3 socket is on the web port at /, as "webui-v3"; older
// firmware and WebUI 2 have it on port 81, as "arduino". Asking for the
// wrong one fails the handshake.
static int fluidnc_parse_url(const char *url, fluidnc_url_t *out)
{
  const char *p = url;

  if (!strncmp(p, "http://", 7)) {
    p += 7;
  } else if (!strncmp(p, "ws://", 5)) {
    p += 5;
  } else if (strstr(p, "://")) {
    return -1; // FluidNC has no TLS
  }

  size_t host_len = p[0] == '[' ? strcspn(p, "]") + 1 : strcspn(p, ":/");
  if (host_len == 0 || host_len >= sizeof(out->host))
    return -1;
  memcpy(out->host, p, host_len);
  out->host[host_len] = 0;
  p += host_len;

  strcpy(out->port, "80");
  if (*p == ':') {
    size_t port_len = strspn(++p, "0123456789");
    if (port_len == 0 || port_len >= sizeof(out->port))
      return -1;
    memcpy(out->port, p, port_len);
    out->port[port_len] = 0;
    p += port_len;
  }

  snprintf(out->path, sizeof(out->path), "%s", *p == '/' ? p : "/");
  out->protocol = strcmp(out->port, "81") ? "webui-v3" : "arduino";
  return 0;
}

static void fluidnc_random(unsigned char *buf, size_t len)
{
  for (size_t i = 0; i < len; i++)
    buf[i] = rand() & 0xff;
}

static void base64(const unsigned char *in, size_t len, char *out)
{
  static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  for (size_t i = 0; i < len; i += 3) {
    unsigned v = in[i] << 16 | (i + 1 < len ? in[i + 1] << 8 : 0) | (i + 2 < len ? in[i + 2] : 0);
    *out++ = chars[v >> 18 & 63];
    *out++ = chars[v >> 12 & 63];
    *out++ = i + 1 < len ? chars[v >> 6 & 63] : '=';
    *out++ = i + 2 < len ? chars[v & 63] : '=';
  }
  *out = 0;
}

// A client's frames are masked; a short one is all this ever sends.
static int fluidnc_send(int fd, int opcode, const char *data, size_t len)
{
  unsigned char frame[6 + 125];

  if (len > 125)
    return -1;

  frame[0] = 0x80 | opcode;
  frame[1] = 0x80 | len;
  fluidnc_random(frame + 2, 4);
  for (size_t i = 0; i < len; i++)
    frame[6 + i] = data[i] ^ frame[2 + i % 4];

  return send(fd, frame, 6 + len, MSG_NOSIGNAL) == (ssize_t)(6 + len) ? 0 : -1;
}

static int fluidnc_connect(fluidnc_url_t *url, char *leftover, size_t *leftover_len)
{
  char host[256], error[FLUIDNC_ERROR];
  struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *addrs = NULL;
  int fd = -1;

  // The address without the brackets an IPv6 one is written with.
  snprintf(host, sizeof(host), "%s", url->host[0] == '[' ? url->host + 1 : url->host);
  host[strcspn(host, "]")] = 0;

  int ret = getaddrinfo(host, url->port, &hints, &addrs);
  if (ret) {
    snprintf(error, sizeof(error), "Cannot resolve %s: %s", host, gai_strerror(ret));
    fluidnc_set_error(error);
    return -1;
  }

  for (struct addrinfo *addr = addrs; addr; addr = addr->ai_next) {
    fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
    if (fd < 0)
      continue;

    // On Linux the send timeout is the connect timeout too.
    struct timeval tv = { .tv_sec = FLUIDNC_CONNECT_S };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(fd, addr->ai_addr, addr->ai_addrlen) == 0)
      break;

    snprintf(error, sizeof(error), "Cannot connect to %s:%s: %s", url->host, url->port, strerror(errno));
    close(fd);
    fd = -1;
  }
  freeaddrinfo(addrs);

  if (fd < 0) {
    fluidnc_set_error(error);
    return -1;
  }

  unsigned char key[16];
  char key64[25], request[1024], response[2048];
  size_t used = 0;

  fluidnc_random(key, sizeof(key));
  base64(key, sizeof(key), key64);
  int len = snprintf(request, sizeof(request),
    "GET %s HTTP/1.1\r\n"
    "Host: %s%s%s\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: %s\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "Sec-WebSocket-Protocol: %s\r\n"
    "\r\n",
    url->path, url->host, strcmp(url->port, "80") ? ":" : "", strcmp(url->port, "80") ? url->port : "",
    key64, url->protocol);

  if (send(fd, request, len, MSG_NOSIGNAL) != len) {
    snprintf(error, sizeof(error), "Cannot talk to %s:%s: %s", url->host, url->port, strerror(errno));
    goto error;
  }

  // The answer's headers, and whatever came in behind them, which is the
  // first of the socket's data.
  char *end = NULL;
  while (!end) {
    ssize_t n = recv(fd, response + used, sizeof(response) - 1 - used, 0);
    if (n <= 0) {
      snprintf(error, sizeof(error), "%s:%s did not answer the websocket handshake", url->host, url->port);
      goto error;
    }
    used += n;
    response[used] = 0;
    end = strstr(response, "\r\n\r\n");
    if (!end && used >= sizeof(response) - 1) {
      snprintf(error, sizeof(error), "%s:%s gave an overlong answer to the websocket handshake", url->host, url->port);
      goto error;
    }
  }

  if (strncmp(response, "HTTP/1.1 101", 12)) {
    response[strcspn(response, "\r\n")] = 0;
    snprintf(error, sizeof(error), "%s%s is not a FluidNC websocket: %.200s", url->host, url->path, response);
    goto error;
  }

  end += 4;
  *leftover_len = response + used - end;
  memcpy(leftover, end, *leftover_len);

  // From here the timeout is a tick of the loop that reads it.
  struct timeval tick = { .tv_sec = 1 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tick, sizeof(tick));
  int on = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
  return fd;

error:
  close(fd);
  fluidnc_set_error(error);
  return -1;
}

// Copies the value of a report's field, "|WCO:" or "|Ov:", if it has it.
static void fluidnc_keep(const char *line, const char *field, char *value, size_t len)
{
  const char *at = strstr(line, field);
  if (!at)
    return;
  at += strlen(field);
  size_t value_len = strcspn(at, "|>");
  if (value_len < len) {
    memcpy(value, at, value_len);
    value[value_len] = 0;
  }
}

// A line of the console. A status report is kept; anything else only says
// the socket is alive.
static void fluidnc_line(const char *line)
{
  size_t len = strlen(line);

  pthread_mutex_lock(&fluidnc.lock);
  fluidnc.heard_at = now_ms();

  if (len >= 2 && line[0] == '<' && line[len - 1] == '>') {
    snprintf(fluidnc.status, sizeof(fluidnc.status), "%s", line);

    // The work offset and the overrides come in every few reports, not in
    // each; they are kept, or a viewer that missed that one report could
    // give neither work positions nor the feed override.
    fluidnc_keep(line, "|WCO:", fluidnc.wco, sizeof(fluidnc.wco));
    fluidnc_keep(line, "|Ov:", fluidnc.ov, sizeof(fluidnc.ov));

    fluidnc.seq++;
    pthread_cond_broadcast(&fluidnc.changed);
  }

  pthread_mutex_unlock(&fluidnc.lock);
}

typedef struct {
  char text[FLUIDNC_LINE];
  size_t len;
} fluidnc_line_t;

static void fluidnc_line_add(fluidnc_line_t *line, char c)
{
  if (c == '\n') {
    line->text[line->len] = 0;
    // Trim the \r a console line ends with.
    while (line->len > 0 && line->text[line->len - 1] == '\r')
      line->text[--line->len] = 0;
    fluidnc_line(line->text);
    line->len = 0;
  } else if (line->len < sizeof(line->text) - 1) {
    line->text[line->len++] = c;
  }
}

// The frames as they come, a byte at a time, since a frame may be cut
// between reads and may be larger than any buffer here. The console comes
// as binary messages, coalesced: one can hold several lines and a line can
// be cut between two. FluidNC's own bookkeeping (currentID:, PING:) comes as
// text messages, each one whole and without a newline, so text and binary
// are put together into lines apart.
typedef struct {
  unsigned char header[14];
  size_t header_len;
  uint64_t remaining;
  int opcode;
  bool fin;
  bool masked;
  unsigned char mask[4];
  uint64_t offset;
  bool text_message;
  fluidnc_line_t text, binary;
  char control[126];
  size_t control_len;
} fluidnc_reader_t;

// Returns -1 when the controller closes the socket.
static int fluidnc_frame_end(int fd, fluidnc_reader_t *r)
{
  switch (r->opcode) {
  case 0x8: // close
    return -1;

  case 0x9: // ping
    fluidnc_send(fd, 0xA, r->control, r->control_len);
    break;

  case 0x1:
  case 0x2:
  case 0x0:
    if (r->fin && r->text_message)
      fluidnc_line_add(&r->text, '\n');
    break;
  }
  return 0;
}

static int fluidnc_read(int fd, fluidnc_reader_t *r, const unsigned char *data, size_t len)
{
  for (size_t i = 0; i < len; i++) {
    if (r->remaining == 0) {
      r->header[r->header_len++] = data[i];
      if (r->header_len < 2)
        continue;

      size_t need = 2;
      unsigned short_len = r->header[1] & 0x7f;
      if (short_len == 126)
        need += 2;
      else if (short_len == 127)
        need += 8;
      if (r->header[1] & 0x80)
        need += 4;
      if (r->header_len < need)
        continue;

      r->fin = r->header[0] & 0x80;
      r->opcode = r->header[0] & 0x0f;
      r->masked = r->header[1] & 0x80;
      r->remaining = short_len;
      if (short_len == 126) {
        r->remaining = r->header[2] << 8 | r->header[3];
      } else if (short_len == 127) {
        r->remaining = 0;
        for (int b = 2; b < 10; b++)
          r->remaining = r->remaining << 8 | r->header[b];
      }
      if (r->masked)
        memcpy(r->mask, r->header + need - 4, 4);
      if (r->opcode == 0x1)
        r->text_message = true;
      else if (r->opcode == 0x2)
        r->text_message = false;
      r->header_len = 0;
      r->offset = 0;
      r->control_len = 0;

      if (r->remaining == 0 && fluidnc_frame_end(fd, r) < 0)
        return -1;
      continue;
    }

    char c = data[i] ^ (r->masked ? r->mask[r->offset % 4] : 0);
    r->offset++;

    if (r->opcode >= 0x8) {
      if (r->control_len < sizeof(r->control))
        r->control[r->control_len++] = c;
    } else {
      fluidnc_line_add(r->text_message ? &r->text : &r->binary, c);
    }

    if (--r->remaining == 0 && fluidnc_frame_end(fd, r) < 0)
      return -1;
  }
  return 0;
}

// Waits out a retry's delay, and then until somebody watches.
static void fluidnc_wait_for_viewers(unsigned delay_ms)
{
  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += delay_ms / 1000;
  until.tv_nsec += (delay_ms % 1000) * 1000000;
  if (until.tv_nsec >= 1000000000) {
    until.tv_sec++;
    until.tv_nsec -= 1000000000;
  }

  pthread_mutex_lock(&fluidnc.lock);
  while (delay_ms > 0 && pthread_cond_timedwait(&fluidnc.changed, &fluidnc.lock, &until) != ETIMEDOUT)
    ;
  while (fluidnc.viewers == 0)
    pthread_cond_wait(&fluidnc.changed, &fluidnc.lock);
  pthread_mutex_unlock(&fluidnc.lock);
}

static void *fluidnc_thread(void *arg)
{
  fluidnc_url_t url;
  unsigned retries = 0;
  unsigned delay_ms = 0;

  if (fluidnc_parse_url(fluidnc_options.url, &url) < 0) {
    char error[FLUIDNC_ERROR];
    snprintf(error, sizeof(error), "Not a FluidNC address: %s", fluidnc_options.url);
    LOG_INFO(NULL, "%s", error);
    fluidnc_set_error(error);
    return NULL;
  }

  srand(time(NULL) ^ getpid());

  while (true) {
    fluidnc_wait_for_viewers(delay_ms);

    char leftover[2048];
    size_t leftover_len = 0;

    int fd = fluidnc_connect(&url, leftover, &leftover_len);
    if (fd < 0) {
      delay_ms = fluidnc_retry_ms[retries < 3 ? retries : 3];
      retries++;
      LOG_INFO(NULL, "FluidNC: %s. Trying again in %u s.", fluidnc.error, delay_ms / 1000);
      continue;
    }

    LOG_INFO(NULL, "FluidNC: Connected to %s:%s%s (%s).", url.host, url.port, url.path, url.protocol);

    pthread_mutex_lock(&fluidnc.lock);
    fluidnc.connected = true;
    fluidnc.error[0] = 0;
    fluidnc.status[0] = 0;  // what it said before the drop is not what it says now
    fluidnc.heard_at = now_ms();
    fluidnc.seq++;
    pthread_cond_broadcast(&fluidnc.changed);
    pthread_mutex_unlock(&fluidnc.lock);

    fluidnc_reader_t reader = { 0 };
    uint64_t asked_at = now_ms();
    const char *error = NULL;
    char why[FLUIDNC_ERROR];

    fluidnc_send(fd, 0x1, "?", 1);
    if (fluidnc_read(fd, &reader, (unsigned char *)leftover, leftover_len) < 0)
      error = "The controller closed the connection";

    while (!error) {
      unsigned char buf[4096];
      ssize_t n = recv(fd, buf, sizeof(buf), 0);

      if (n > 0) {
        if (fluidnc_read(fd, &reader, buf, n) < 0)
          error = "The controller closed the connection";
      } else if (n == 0) {
        error = "The controller closed the connection";
      } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        snprintf(why, sizeof(why), "Lost the connection: %s", strerror(errno));
        error = why;
      }
      if (error)
        break;

      uint64_t now = now_ms();
      pthread_mutex_lock(&fluidnc.lock);
      uint64_t quiet = now - fluidnc.heard_at;
      bool idle = fluidnc.viewers == 0 && now - fluidnc.idle_since > FLUIDNC_IDLE_MS;
      pthread_mutex_unlock(&fluidnc.lock);

      if (idle) {
        // Nobody is watching: give the socket back to the controller.
        fluidnc_send(fd, 0x8, NULL, 0);
        break;
      }
      if (quiet > FLUIDNC_DEAD_MS) {
        error = "The controller stopped answering";
      } else if (quiet > FLUIDNC_ASK_MS && now - asked_at > FLUIDNC_ASK_MS) {
        asked_at = now;
        if (fluidnc_send(fd, 0x1, "?", 1) < 0) {
          snprintf(why, sizeof(why), "Lost the connection: %s", strerror(errno));
          error = why;
        }
      }
    }

    close(fd);

    if (error) {
      // A socket that was up a while was not refused: start the back-off
      // over, as a browser that heard something would.
      delay_ms = fluidnc_retry_ms[0];
      retries = 1;
      LOG_INFO(NULL, "FluidNC: %s. Trying again in %u s.", error, delay_ms / 1000);
      fluidnc_set_error(error);
    } else {
      LOG_INFO(NULL, "FluidNC: Nobody watching; disconnected.");
      delay_ms = 0;
      retries = 0;
      fluidnc_set_error("");
    }
  }
  return NULL;
}

static void fluidnc_start()
{
  pthread_t thread;
  pthread_create(&thread, NULL, fluidnc_thread, NULL);
  pthread_detach(thread);
}

void json_string(FILE *stream, const char *s)
{
  fputc('"', stream);
  for (; *s; s++) {
    unsigned char c = *s;
    if (c == '"' || c == '\\')
      fprintf(stream, "\\%c", c);
    else if (c < 0x20)
      fprintf(stream, "\\u%04x", c);
    else
      fputc(c, stream);
  }
  fputc('"', stream);
}

void http_fluidnc_events(http_worker_t *worker, FILE *stream)
{
  // X-Accel-Buffering keeps nginx from holding the events back until a
  // buffer fills, which at a report of 80 bytes is a minute.
  fprintf(stream,
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/event-stream\r\n"
    "Cache-Control: no-cache\r\n"
    "X-Accel-Buffering: no\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Connection: close\r\n"
    "\r\n"
    "retry: 2000\n\n");
  if (fflush(stream))
    return;

  // Without a controller there is still the count of who is watching.
  if (fluidnc_options.url[0])
    pthread_once(&fluidnc_once, fluidnc_start);

  // Everybody watching hears at once that somebody came, or went.
  pthread_mutex_lock(&fluidnc.lock);
  fluidnc.viewers++;
  fluidnc.seq++;
  pthread_cond_broadcast(&fluidnc.changed);

  unsigned seen = fluidnc.seq - 1;

  while (true) {
    if (fluidnc.seq == seen) {
      struct timespec until;
      clock_gettime(CLOCK_REALTIME, &until);
      until.tv_sec += FLUIDNC_EVENT_MS / 1000;
      pthread_cond_timedwait(&fluidnc.changed, &fluidnc.lock, &until);
    }

    seen = fluidnc.seq;
    int viewers = fluidnc.viewers;
    bool connected = fluidnc.connected;
    uint64_t age = now_ms() - fluidnc.heard_at;
    char status[FLUIDNC_LINE], wco[128], ov[64], error[FLUIDNC_ERROR];
    strcpy(status, connected ? fluidnc.status : "");
    strcpy(wco, fluidnc.wco);
    strcpy(ov, fluidnc.ov);
    strcpy(error, fluidnc.error);
    pthread_mutex_unlock(&fluidnc.lock);

    // Written without the lock: a viewer whose network is slow holds up
    // nobody but itself.
    fprintf(stream, "data: {\"viewers\":%d,\"connected\":%s,\"age\":%llu,\"status\":",
      viewers, connected ? "true" : "false", connected ? (unsigned long long)age : 0ULL);
    json_string(stream, status);
    fputs(",\"wco\":", stream);
    json_string(stream, wco);
    fputs(",\"ov\":", stream);
    json_string(stream, ov);
    fputs(",\"error\":", stream);
    json_string(stream, error);
    fputs(",\"server\":", stream);
    server_stats_json(stream);
    fputs(",\"battery\":", stream);
    victron_json(stream);
    fputs(",\"mobile\":", stream);
    teltonika_json(stream);
    fputs("}\n\n", stream);
    bool gone = fflush(stream) || ferror(stream);

    pthread_mutex_lock(&fluidnc.lock);
    if (gone)
      break;
  }

  if (--fluidnc.viewers == 0)
    fluidnc.idle_since = now_ms();
  fluidnc.seq++;
  pthread_cond_broadcast(&fluidnc.changed);
  pthread_mutex_unlock(&fluidnc.lock);
}
