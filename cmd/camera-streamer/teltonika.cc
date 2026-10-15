// A Teltonika router's mobile connection - a TRB500 on RutOS 7 - for the
// panel under the picture on /telemetry: the operator, the network, the
// signal, the band, the data and the modem's temperature.
//
// Read from the router's API, as its WebUI reads it: POST /api/login for a
// token, GET /api/system/device/status with it once for the router's model,
// then GET /api/modems/status every five seconds while somebody watches. The token lasts five minutes and is used until then, or
// until the router says it has run out. This host asks, not the browser,
// which may be anywhere; the password stays here.
//
// Plain HTTP, with HTTP/1.0 requests, so that the answer is the body and
// nothing else, ended by the router closing the connection.

extern "C" {

#include "cmd/camera-streamer/teltonika.h"
#include "util/opts/log.h"

extern teltonika_options_t teltonika_options;

};

#include <nlohmann/json.hpp>
#include <string>

#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define TELTONIKA_POLL_MS     5000
#define TELTONIKA_RETRY_MS    30000  // after a failure, not to lock the user out
#define TELTONIKA_IDLE_MS     10000  // nobody watching this long, and it stops asking
#define TELTONIKA_TIMEOUT_S   5
#define TELTONIKA_MAX_ANSWER  (1 << 20)

static pthread_mutex_t teltonika_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t teltonika_asked = PTHREAD_COND_INITIALIZER;
static bool teltonika_watched;
static uint64_t teltonika_asked_at;
static uint64_t teltonika_heard_at;    // 0 until the first answer
static nlohmann::json teltonika_modem;
static std::string teltonika_error;

static std::string router_host, router_port;
static nlohmann::json router_model;    // "TRB500", once the router has said

static uint64_t now_ms()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void teltonika_set_error(const std::string &error)
{
  pthread_mutex_lock(&teltonika_lock);
  if (!error.empty() && error != teltonika_error)
    LOG_INFO(NULL, "Teltonika: %s.", error.c_str());
  teltonika_error = error;
  pthread_mutex_unlock(&teltonika_lock);
}

// "192.168.2.1", "http://192.168.2.1/" or "router.lan:8080".
static bool teltonika_parse_url(const char *url)
{
  std::string s = url;

  if (!s.compare(0, 7, "http://"))
    s.erase(0, 7);
  else if (s.find("://") != std::string::npos)
    return false; // no TLS here

  s = s.substr(0, s.find('/'));
  size_t colon = s.rfind(':');
  if (colon != std::string::npos && s.find(']', colon) == std::string::npos) {
    router_port = s.substr(colon + 1);
    s.erase(colon);
  } else {
    router_port = "80";
  }
  if (s.size() > 2 && s.front() == '[' && s.back() == ']')
    s = s.substr(1, s.size() - 2);
  router_host = s;
  return !router_host.empty() && !router_port.empty()
    && router_port.find_first_not_of("0123456789") == std::string::npos;
}

struct http_answer {
  int status = 0;
  std::string location;
  std::string body;
};

// A chunked body, in case the router sends one all the same.
static std::string unchunk(const std::string &in)
{
  std::string out;
  size_t at = 0;

  while (at < in.size()) {
    size_t eol = in.find("\r\n", at);
    if (eol == std::string::npos)
      break;
    size_t len = strtoul(in.c_str() + at, NULL, 16);
    if (len == 0)
      break;
    out.append(in, eol + 2, len);
    at = eol + 2 + len + 2;
  }
  return out;
}

static bool teltonika_http(const std::string &request, http_answer &answer, std::string &error)
{
  struct addrinfo hints = {}, *addrs = NULL;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  int fd = -1;

  int ret = getaddrinfo(router_host.c_str(), router_port.c_str(), &hints, &addrs);
  if (ret) {
    error = "Cannot resolve " + router_host + ": " + gai_strerror(ret);
    return false;
  }
  for (struct addrinfo *addr = addrs; addr; addr = addr->ai_next) {
    fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
    if (fd < 0)
      continue;
    // On Linux the send timeout is the connect timeout too.
    struct timeval tv = { TELTONIKA_TIMEOUT_S, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(fd, addr->ai_addr, addr->ai_addrlen) == 0)
      break;
    error = "Cannot connect to " + router_host + ":" + router_port + ": " + strerror(errno);
    close(fd);
    fd = -1;
  }
  freeaddrinfo(addrs);
  if (fd < 0)
    return false;

  std::string raw;
  bool ok = send(fd, request.data(), request.size(), MSG_NOSIGNAL) == (ssize_t)request.size();
  while (ok) {
    char buf[4096];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n == 0)
      break;
    if (n < 0 || raw.size() + n > TELTONIKA_MAX_ANSWER)
      ok = false;
    else
      raw.append(buf, n);
  }
  int saved = errno;
  close(fd);
  if (!ok) {
    error = "Lost the router at " + router_host + ":" + router_port + ": " + strerror(saved);
    return false;
  }

  size_t end = raw.find("\r\n\r\n");
  if (raw.compare(0, 5, "HTTP/") || end == std::string::npos) {
    error = router_host + " does not answer as a web server";
    return false;
  }
  answer.status = atoi(raw.c_str() + raw.find(' ') + 1);
  answer.body = raw.substr(end + 4);

  bool chunked = false;
  for (size_t at = raw.find("\r\n") + 2; at < end; at = raw.find("\r\n", at) + 2) {
    std::string header = raw.substr(at, raw.find("\r\n", at) - at);
    if (!strncasecmp(header.c_str(), "location:", 9))
      answer.location = header.substr(header.find_first_not_of(' ', 9));
    else if (!strncasecmp(header.c_str(), "transfer-encoding:", 18) && strcasestr(header.c_str(), "chunked"))
      chunked = true;
  }
  if (chunked)
    answer.body = unchunk(answer.body);
  return true;
}

// The router's JSON answer to a request of the API. An answer that is not
// JSON, or is a redirect, is an error.
static bool teltonika_api(const char *method, const char *path, const std::string &token,
  const std::string &body, int &status, nlohmann::json &json, std::string &error)
{
  std::string request = std::string(method) + " /api" + path + " HTTP/1.0\r\n"
    "Host: " + (router_host.find(':') != std::string::npos ? "[" + router_host + "]" : router_host)
    + (router_port != "80" ? ":" + router_port : "") + "\r\n"
    "Accept: application/json\r\n";
  if (!token.empty())
    request += "Authorization: Bearer " + token + "\r\n";
  if (!body.empty())
    request += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
  request += "\r\n" + body;

  http_answer answer;
  if (!teltonika_http(request, answer, error))
    return false;

  status = answer.status;
  if (status >= 300 && status < 400) {
    error = "The router sends /api" + std::string(path) + " to " + answer.location
      + "; only plain http:// is supported";
    return false;
  }
  json = nlohmann::json::parse(answer.body, nullptr, false);
  if (json.is_discarded() || !json.is_object()) {
    error = "The router answered /api" + std::string(path) + " with HTTP " + std::to_string(status)
      + ", not with its API; is it a Teltonika on RutOS 7?";
    return false;
  }
  return true;
}

// What the router says went wrong: {"success":false,"errors":[{"error":…}]}.
static std::string teltonika_said(const nlohmann::json &json, int status)
{
  auto errors = json.find("errors");
  if (errors != json.end() && errors->is_array() && !errors->empty()
      && (*errors)[0].contains("error") && (*errors)[0]["error"].is_string()) {
    // Without its full stop, which the page and the log give it.
    std::string said = (*errors)[0]["error"].get<std::string>();
    while (!said.empty() && said.back() == '.')
      said.pop_back();
    return said;
  }
  return "HTTP " + std::to_string(status);
}

static bool teltonika_login(std::string &token, uint64_t &until, std::string &error)
{
  nlohmann::json body = {
    { "username", teltonika_options.user },
    { "password", teltonika_options.password },
  };
  nlohmann::json json;
  int status;

  if (!teltonika_api("POST", "/login", "", body.dump(), status, json, error))
    return false;

  auto data = json.find("data");
  if (!json.value("success", false) || data == json.end() || !data->is_object()) {
    error = "The router refused the login: " + teltonika_said(json, status);
    return false;
  }
  if (data->value("2fa_login", false)) {
    error = "The router asks " + std::string(teltonika_options.user)
      + " for a second factor; give a user without two-factor authentication";
    return false;
  }
  token = data->value("token", "");
  if (token.empty()) {
    error = "The router gave no token for the login";
    return false;
  }
  // A little before it runs out, so that no request is made with a token
  // on its last second.
  int expires = data->value("expires", 300);
  until = now_ms() + (expires > 60 ? expires - 30 : expires) * 1000ULL;
  return true;
}

// The router's figures are numbers, or numbers in strings, or empty.
static nlohmann::json number(const nlohmann::json &m, const char *key)
{
  auto it = m.find(key);
  if (it == m.end())
    return nullptr;
  if (it->is_number())
    return *it;
  if (it->is_string()) {
    const std::string &s = it->get_ref<const std::string &>();
    char *end;
    double v = strtod(s.c_str(), &end);
    if (end != s.c_str() && *end == 0)
      return v;
  }
  return nullptr;
}

static nlohmann::json text(const nlohmann::json &m, const char *key)
{
  auto it = m.find(key);
  if (it == m.end() || !it->is_string() || it->get_ref<const std::string &>().empty())
    return nullptr;
  return *it;
}

// The modem that is up - the first with a connection state; an offline one
// has none - and of it what the page shows. In 5G NSA, the 5G cell is the
// one in cell_info with an NR-ARFCN, and its RSRP and SINR are the 5G ones.
static bool teltonika_read_modem(const nlohmann::json &json, nlohmann::json &out, std::string &error)
{
  auto data = json.find("data");
  if (data == json.end() || !data->is_array()) {
    error = "The router's modem status has no modems";
    return false;
  }

  for (const auto &m : *data) {
    if (!m.is_object() || !m.contains("state"))
      continue;

    out = {
      { "modem", text(m, "name") },
      { "modem_manufacturer", text(m, "manufacturer") },
      { "modem_model", text(m, "model") },
      { "operator", m.contains("operator") ? text(m, "operator") : text(m, "provider") },
      { "conntype", text(m, "conntype") },
      { "ntype", text(m, "ntype") },
      { "state", text(m, "state") },
      { "netstate", text(m, "netstate") },
      { "simstate", text(m, "simstate") },
      { "band", text(m, "band") },
      { "rssi", number(m, "rssi") },
      { "rsrp", number(m, "rsrp") },
      { "rsrq", number(m, "rsrq") },
      { "sinr", number(m, "sinr") },
      { "signal_quality", number(m, "signal_quality") },
      { "temperature", number(m, "temperature") },
      { "rxbytes", number(m, "rxbytes") },
      { "txbytes", number(m, "txbytes") },
      { "nr_rsrp", nullptr },
      { "nr_sinr", nullptr },
    };
    auto cells = m.find("cell_info");
    if (cells != m.end() && cells->is_array()) {
      for (const auto &cell : *cells) {
        if (cell.is_object() && cell.contains("nr-arfcn")) {
          out["nr_rsrp"] = number(cell, "rsrp");
          out["nr_sinr"] = number(cell, "sinr");
          break;
        }
      }
    }
    return true;
  }

  error = "The router's modem is offline";
  return false;
}

// The router's own model, from its static description, or its board's. A
// user that may not read it does without.
static void teltonika_read_model(const std::string &token)
{
  nlohmann::json json;
  std::string error;
  int status;

  if (!teltonika_api("GET", "/system/device/status", token, "", status, json, error) || status != 200)
    return;
  const nlohmann::json &data = json.value("data", nlohmann::json::object());
  if (data.contains("static") && data["static"].is_object())
    router_model = text(data["static"], "model");
  if (router_model.is_null() && data.contains("board") && data["board"].is_object()
      && data["board"].contains("model") && data["board"]["model"].is_object())
    router_model = text(data["board"]["model"], "name");
}

static bool teltonika_is_watched(uint64_t now)
{
  return teltonika_watched && now - teltonika_asked_at < TELTONIKA_IDLE_MS;
}

// Waits out the delay, and then until somebody watches.
static void teltonika_wait_for_viewers(unsigned delay_ms)
{
  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += delay_ms / 1000;
  until.tv_nsec += (delay_ms % 1000) * 1000000;
  if (until.tv_nsec >= 1000000000) {
    until.tv_sec++;
    until.tv_nsec -= 1000000000;
  }

  pthread_mutex_lock(&teltonika_lock);
  while (delay_ms > 0 && pthread_cond_timedwait(&teltonika_asked, &teltonika_lock, &until) != ETIMEDOUT)
    ;
  while (!teltonika_is_watched(now_ms()))
    pthread_cond_wait(&teltonika_asked, &teltonika_lock);
  pthread_mutex_unlock(&teltonika_lock);
}

static void *teltonika_thread(void *arg)
{
  std::string token;
  uint64_t token_until = 0;
  unsigned delay_ms = 0;

  if (!teltonika_parse_url(teltonika_options.url)) {
    teltonika_set_error("-teltonika-url is not a plain http:// address: " + std::string(teltonika_options.url));
    return NULL;
  }

  while (true) {
    teltonika_wait_for_viewers(delay_ms);
    delay_ms = TELTONIKA_RETRY_MS;

    std::string error;
    nlohmann::json json, modem;
    int status = 0;

    bool fresh = token.empty() || now_ms() > token_until;
    if (fresh) {
      token.clear();
      if (!teltonika_login(token, token_until, error)) {
        teltonika_set_error(error);
        continue;
      }
      if (router_model.is_null())
        teltonika_read_model(token);
    }

    if (!teltonika_api("GET", "/modems/status", token, "", status, json, error)) {
      teltonika_set_error(error);
      continue;
    }
    if ((status == 401 || status == 403) && fresh) {
      teltonika_set_error("The router does not let " + std::string(teltonika_options.user)
        + " read the modem status: " + teltonika_said(json, status));
      token.clear();
      continue;
    }
    if (status == 401 || status == 403) {
      // The router has let the token go, restarted perhaps: once more with
      // a new one, at once.
      token.clear();
      delay_ms = 0;
      continue;
    }
    if (status != 200 || !json.value("success", false)) {
      teltonika_set_error("The router did not give its modem status: " + teltonika_said(json, status));
      continue;
    }
    if (!teltonika_read_modem(json, modem, error)) {
      teltonika_set_error(error);
      continue;
    }

    modem["router_model"] = router_model;
    pthread_mutex_lock(&teltonika_lock);
    teltonika_modem = modem;
    teltonika_heard_at = now_ms();
    teltonika_error.clear();
    pthread_mutex_unlock(&teltonika_lock);
    delay_ms = TELTONIKA_POLL_MS;
  }
  return NULL;
}

static pthread_once_t teltonika_once = PTHREAD_ONCE_INIT;

static void teltonika_start()
{
  pthread_t thread;
  pthread_create(&thread, NULL, teltonika_thread, NULL);
  pthread_detach(thread);
}

extern "C" void teltonika_json(FILE *stream)
{
  if (!teltonika_options.url[0]) {
    fputs("null", stream);
    return;
  }

  pthread_once(&teltonika_once, teltonika_start);

  pthread_mutex_lock(&teltonika_lock);
  uint64_t now = now_ms();
  teltonika_watched = true;
  teltonika_asked_at = now;
  pthread_cond_broadcast(&teltonika_asked);
  nlohmann::json out = teltonika_heard_at ? teltonika_modem : nlohmann::json::object();
  out["age"] = teltonika_heard_at ? nlohmann::json(now - teltonika_heard_at) : nlohmann::json(nullptr);
  out["error"] = teltonika_error;
  pthread_mutex_unlock(&teltonika_lock);

  // What the router said may not be UTF-8.
  fputs(out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).c_str(), stream);
}
