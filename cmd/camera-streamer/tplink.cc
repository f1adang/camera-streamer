// A TP-Link LTE router's mobile connection - such as an Archer MR402-Outdoor
// (AC1200 Wireless Dual Band 4G LTE Outdoor Router) - for the panel under
// the picture on /telemetry: the operator, the network, the signal, the
// band and the data.
//
// Authenticates with the router's API using its ECC (secp224r1) key exchange
// and AES-128-CBC encryption. Reads connection status and figures every five
// seconds while somebody watches. This host asks, not the browser; the
// password stays here.
//
// Plain HTTP, with HTTP/1.0 requests.

extern "C" {

#include "cmd/camera-streamer/tplink.h"
#include "util/opts/log.h"

extern tplink_options_t tplink_options;

};

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>

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

#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/bio.h>

namespace {

#define TPLINK_POLL_MS     5000
#define TPLINK_RETRY_MS    30000  // after a failure, not to lock the user out
#define TPLINK_IDLE_MS     10000  // nobody watching this long, and it stops asking
#define TPLINK_TIMEOUT_S   5
#define TPLINK_MAX_ANSWER  (1 << 20)

static pthread_mutex_t tplink_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tplink_asked = PTHREAD_COND_INITIALIZER;
static bool tplink_watched;
static uint64_t tplink_asked_at;
static uint64_t tplink_heard_at;    // 0 until the first answer
static nlohmann::json tplink_modem;
static std::string tplink_error;

static std::string router_host, router_port;
static std::string router_model_name = "TP-Link AC1200 4G LTE Router";

static uint64_t now_ms()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void tplink_set_error(const std::string &error)
{
  pthread_mutex_lock(&tplink_lock);
  if (!error.empty() && error != tplink_error)
    LOG_INFO(NULL, "TP-Link: %s.", error.c_str());
  tplink_error = error;
  pthread_mutex_unlock(&tplink_lock);
}

// "192.168.1.1", "http://192.168.1.1/" or "router.lan:8080".
static bool tplink_parse_url(const char *url)
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

struct tplink_http_answer {
  int status = 0;
  std::string location;
  std::string set_cookie;
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

static bool tplink_http(const std::string &request, tplink_http_answer &answer, std::string &error)
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
    struct timeval tv = { TPLINK_TIMEOUT_S, 0 };
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
  size_t header_end = std::string::npos;
  size_t content_length = 0;
  bool has_content_length = false;
  bool is_chunked = false;

  while (ok) {
    char buf[4096];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n == 0)
      break;
    if (n < 0) {
      if (raw.empty() || header_end == std::string::npos)
        ok = false;
      break;
    }
    if (raw.size() + n > TPLINK_MAX_ANSWER) {
      ok = false;
      break;
    }
    raw.append(buf, n);

    if (header_end == std::string::npos) {
      header_end = raw.find("\r\n\r\n");
      if (header_end != std::string::npos) {
        for (size_t at = raw.find("\r\n") + 2; at < header_end; at = raw.find("\r\n", at) + 2) {
          std::string h = raw.substr(at, raw.find("\r\n", at) - at);
          if (!strncasecmp(h.c_str(), "content-length:", 15)) {
            content_length = strtoul(h.substr(15).c_str(), NULL, 10);
            has_content_length = true;
          } else if (!strncasecmp(h.c_str(), "transfer-encoding:", 18) && strcasestr(h.c_str(), "chunked")) {
            is_chunked = true;
          }
        }
      }
    }

    if (header_end != std::string::npos) {
      size_t body_len = raw.size() - (header_end + 4);
      if (has_content_length && body_len >= content_length)
        break;
      if (is_chunked && raw.size() >= 5 && raw.rfind("0\r\n\r\n") == raw.size() - 5)
        break;
    }
  }
  int saved = errno;
  close(fd);

  size_t end = raw.find("\r\n\r\n");
  if (!ok && end == std::string::npos) {
    error = "Lost the router at " + router_host + ":" + router_port + ": " + strerror(saved);
    return false;
  }
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
    else if (!strncasecmp(header.c_str(), "set-cookie:", 11))
      answer.set_cookie = header.substr(header.find_first_not_of(' ', 11));
    else if (!strncasecmp(header.c_str(), "transfer-encoding:", 18) && strcasestr(header.c_str(), "chunked"))
      chunked = true;
  }
  if (chunked)
    answer.body = unchunk(answer.body);
  return true;
}

// Hex and Base64 crypto helpers
static std::string hex_str(const unsigned char *data, size_t len)
{
  std::string s;
  s.resize(len * 2);
  const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < len; ++i) {
    s[i * 2] = hex[(data[i] >> 4) & 0xf];
    s[i * 2 + 1] = hex[data[i] & 0xf];
  }
  return s;
}

static std::string b64_encode(const unsigned char *data, size_t len)
{
  int b64len = 4 * ((len + 2) / 3);
  std::string out;
  out.resize(b64len + 1);
  int ret = EVP_EncodeBlock((unsigned char*)&out[0], data, len);
  if (ret > 0) out.resize(ret);
  return out;
}

static std::vector<unsigned char> b64_decode(const std::string &in)
{
  BIO *bio = BIO_new_mem_buf(in.data(), in.size());
  BIO *b64 = BIO_new(BIO_f_base64());
  BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
  bio = BIO_push(b64, bio);
  std::vector<unsigned char> out(in.size());
  int len = BIO_read(bio, out.data(), out.size());
  BIO_free_all(bio);
  if (len <= 0) return {};
  out.resize(len);
  return out;
}

static std::string url_encode(const std::string &in)
{
  std::string out;
  const char hex[] = "0123456789ABCDEF";
  for (unsigned char c : in) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(c);
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0xf]);
    }
  }
  return out;
}

static std::string sha256_hex(const std::string &in)
{
  unsigned char md[SHA256_DIGEST_LENGTH];
  SHA256((const unsigned char*)in.data(), in.size(), md);
  return hex_str(md, SHA256_DIGEST_LENGTH);
}

static std::string hmac_sha256_hex(const std::string &key, const std::string &data)
{
  unsigned char md[SHA256_DIGEST_LENGTH];
  unsigned int md_len = 0;
  HMAC(EVP_sha256(), key.data(), key.size(), (const unsigned char*)data.data(), data.size(), md, &md_len);
  return hex_str(md, md_len);
}

static std::string aes_128_cbc_encrypt(const std::string &key, const std::string &iv, const std::string &plain)
{
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, (const unsigned char*)key.data(), (const unsigned char*)iv.data());
  std::vector<unsigned char> cipher(plain.size() + 16);
  int outl1 = 0, outl2 = 0;
  EVP_EncryptUpdate(ctx, cipher.data(), &outl1, (const unsigned char*)plain.data(), plain.size());
  EVP_EncryptFinal_ex(ctx, cipher.data() + outl1, &outl2);
  EVP_CIPHER_CTX_free(ctx);
  return b64_encode(cipher.data(), outl1 + outl2);
}

static std::string aes_128_cbc_decrypt(const std::string &key, const std::string &iv, const std::string &b64_cipher)
{
  std::vector<unsigned char> raw = b64_decode(b64_cipher);
  if (raw.empty()) return "";
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, (const unsigned char*)key.data(), (const unsigned char*)iv.data());
  std::vector<unsigned char> plain(raw.size() + 16);
  int outl1 = 0, outl2 = 0;
  if (EVP_DecryptUpdate(ctx, plain.data(), &outl1, raw.data(), raw.size()) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    return "";
  }
  if (EVP_DecryptFinal_ex(ctx, plain.data() + outl1, &outl2) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    return "";
  }
  EVP_CIPHER_CTX_free(ctx);
  plain.resize(outl1 + outl2);
  return std::string((char*)plain.data(), plain.size());
}

// Router crypto session
struct tplink_session {
  std::string aes_key;
  std::string aes_iv;
  std::string pubkey_x;
  std::string pubkey_y;
  std::string password_hash;
  uint64_t seq = 0;
  std::string cookie;
  std::string token;

  EC_GROUP *group = NULL;
  BN_CTX *ctx = NULL;
  EC_POINT *router_point = NULL;

  tplink_session() {
    group = EC_GROUP_new_by_curve_name(NID_secp224r1);
    ctx = BN_CTX_new();
  }

  ~tplink_session() {
    if (router_point) EC_POINT_free(router_point);
    if (ctx) BN_CTX_free(ctx);
    if (group) EC_GROUP_free(group);
  }

  void set_ecc_key(const std::string &x_hex, const std::string &y_hex) {
    pubkey_x = x_hex;
    pubkey_y = y_hex;
    BIGNUM *x = NULL, *y = NULL;
    BN_hex2bn(&x, x_hex.c_str());
    BN_hex2bn(&y, y_hex.c_str());
    if (router_point) EC_POINT_free(router_point);
    router_point = EC_POINT_new(group);
    EC_POINT_set_affine_coordinates(group, router_point, x, y, ctx);
    BN_free(x);
    BN_free(y);
  }

  void gen_aes_key() {
    unsigned char buf[16];
    RAND_bytes(buf, sizeof(buf));
    aes_key.resize(16);
    aes_iv.resize(16);
    for (int i = 0; i < 8; ++i) {
      int n = buf[i] % 100;
      aes_key[i * 2] = '0' + (n / 10);
      aes_key[i * 2 + 1] = '0' + (n % 10);
      int m = buf[i + 8] % 100;
      aes_iv[i * 2] = '0' + (m / 10);
      aes_iv[i * 2 + 1] = '0' + (m % 10);
    }
  }

  std::string ecc_encrypt(const std::string &plain) {
    BIGNUM *order = BN_new();
    EC_GROUP_get_order(group, order, ctx);
    BIGNUM *k = BN_new();
    BN_priv_rand_range(k, order);

    EC_POINT *R = EC_POINT_new(group);
    EC_POINT_mul(group, R, k, NULL, NULL, ctx);
    BIGNUM *rx = BN_new(), *ry = BN_new();
    EC_POINT_get_affine_coordinates(group, R, rx, ry, ctx);
    unsigned char rx_buf[28] = {0}, ry_buf[28] = {0};
    BN_bn2binpad(rx, rx_buf, 28);
    BN_bn2binpad(ry, ry_buf, 28);
    std::string tag = hex_str(rx_buf, 28) + hex_str(ry_buf, 28);

    EC_POINT *S = EC_POINT_new(group);
    EC_POINT_mul(group, S, NULL, router_point, k, ctx);
    BIGNUM *sx = BN_new(), *sy = BN_new();
    EC_POINT_get_affine_coordinates(group, S, sx, sy, ctx);
    unsigned char sx_buf[28] = {0};
    BN_bn2binpad(sx, sx_buf, 28);

    unsigned char k_hash[SHA256_DIGEST_LENGTH];
    SHA256(sx_buf, 28, k_hash);
    std::vector<unsigned char> aes_k(k_hash, k_hash + 16);

    unsigned char iv_bytes[16];
    RAND_bytes(iv_bytes, 16);
    std::string iv_hex = hex_str(iv_bytes, 16);

    EVP_CIPHER_CTX *c_ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(c_ctx, EVP_aes_128_cbc(), NULL, aes_k.data(), iv_bytes);
    std::vector<unsigned char> ct(plain.size() + 16);
    int l1 = 0, l2 = 0;
    EVP_EncryptUpdate(c_ctx, ct.data(), &l1, (const unsigned char*)plain.data(), plain.size());
    EVP_EncryptFinal_ex(c_ctx, ct.data() + l1, &l2);
    EVP_CIPHER_CTX_free(c_ctx);
    ct.resize(l1 + l2);
    std::string ct_hex = hex_str(ct.data(), ct.size());

    BN_free(order);
    BN_free(k);
    EC_POINT_free(R);
    BN_free(rx);
    BN_free(ry);
    EC_POINT_free(S);
    BN_free(sx);
    BN_free(sy);

    nlohmann::json j = {
      {"iv", iv_hex},
      {"ct", ct_hex},
      {"tag", tag}
    };
    return j.dump();
  }

  std::pair<std::string, std::string> aes_encrypt_with_sign(const std::string &data, bool is_login) {
    std::string hmac_hex = hmac_sha256_hex(aes_key, data);
    std::string enc_data = aes_128_cbc_encrypt(aes_key, aes_iv, data);
    uint64_t s = seq + enc_data.size();

    std::string sign_plain;
    if (is_login) {
      sign_plain = "key=" + aes_key + "&iv=" + aes_iv + "&h=" + password_hash + "&s=" + std::to_string(s);
    } else {
      sign_plain = "h=" + password_hash + "&s=" + std::to_string(s);
    }

    std::string sign_json_str = ecc_encrypt(sign_plain);
    nlohmann::json sign_j = nlohmann::json::parse(sign_json_str);
    sign_j["hmac"] = hmac_hex;
    return {enc_data, sign_j.dump()};
  }
};

static std::string tplink_band_name(int code)
{
  switch (code) {
  case 40: return "450 MHz";
  case 41: return "480 MHz";
  case 42: return "750 MHz";
  case 43: return "850 MHz";
  case 44: case 45: case 46: return "900 MHz";
  case 47: return "1800 MHz";
  case 48: return "1900 MHz";
  case 80: case 120: return "B1";
  case 81: case 121: return "B2";
  case 82: case 122: return "B3";
  case 83: case 123: return "B4";
  case 84: case 124: return "B5";
  case 85: case 125: return "B6";
  case 86: case 126: return "B7";
  case 87: case 127: return "B8";
  case 88: case 128: return "B9";
  case 129: return "B10";
  case 90: case 130: return "B11";
  case 91: case 131: return "B12";
  case 132: return "B13";
  case 133: return "B14";
  case 134: return "B17";
  case 143: return "B18";
  case 144: return "B19";
  case 145: return "B20";
  case 146: return "B21";
  case 152: return "B23";
  case 147: return "B24";
  case 148: return "B25";
  case 153: return "B26";
  case 158: return "B28";
  case 159: return "B29";
  case 160: return "B30";
  case 154: return "B32";
  case 135: return "B33";
  case 136: return "B34";
  case 137: return "B35";
  case 138: return "B36";
  case 139: return "B37";
  case 140: return "B38";
  case 141: return "B39";
  case 142: return "B40";
  case 149: return "B41";
  case 150: return "B42";
  case 151: return "B43";
  default:
    if (code > 0) return "B" + std::to_string(code);
    return "";
  }
}

static const char *network_type_name(int type)
{
  switch (type) {
  case 1: return "2G (GSM)";
  case 2: return "3G (WCDMA)";
  case 3: return "4G LTE";
  case 4: return "TD-SCDMA";
  case 5: return "CDMA 1x";
  case 6: return "CDMA 1x Ev-Do";
  case 7: return "4G+ LTE";
  default: return "Mobile";
  }
}

static bool tplink_login(tplink_session &session, std::string &error)
{
  std::string host_hdr = (router_host.find(':') != std::string::npos ? "[" + router_host + "]" : router_host)
    + (router_port != "80" ? ":" + router_port : "");

  // 1. POST /cgi/getParm
  std::string req1 = "POST /cgi/getParm HTTP/1.0\r\nHost: " + host_hdr + "\r\nReferer: http://" + router_host + "/\r\nConnection: close\r\n\r\n";
  tplink_http_answer ans1;
  if (!tplink_http(req1, ans1, error))
    return false;

  auto parm = nlohmann::json::parse(ans1.body, nullptr, false);
  if (parm.is_discarded() || !parm.contains("eccPubKey_X_HEX") || !parm.contains("eccPubKey_Y_HEX") || !parm.contains("seq")) {
    error = "The router did not answer with its public key; is it a TP-Link LTE router?";
    return false;
  }

  session.set_ecc_key(parm["eccPubKey_X_HEX"].get<std::string>(), parm["eccPubKey_Y_HEX"].get<std::string>());
  session.seq = strtoull(parm["seq"].get<std::string>().c_str(), NULL, 10);
  std::string user = tplink_options.user[0] ? tplink_options.user : "admin";
  session.password_hash = sha256_hex(user + tplink_options.password);
  session.gen_aes_key();

  // 2. POST /cgi/login
  auto login_enc = session.aes_encrypt_with_sign(user + "\n" + tplink_options.password, true);
  std::string login_url = "/cgi/login?data=" + url_encode(login_enc.first) + "&sign=" + url_encode(login_enc.second) + "&Action=1&LoginStatus=0";
  std::string req2 = "POST " + login_url + " HTTP/1.0\r\nHost: " + host_hdr + "\r\nReferer: http://" + router_host + "/\r\nConnection: close\r\n\r\n";
  tplink_http_answer ans2;
  if (!tplink_http(req2, ans2, error))
    return false;

  auto login_ret = nlohmann::json::parse(ans2.body, nullptr, false);
  if (login_ret.is_discarded() || login_ret.value("ret", -1) != 0) {
    error = "The router refused the login";
    return false;
  }

  if (ans2.set_cookie.empty()) {
    error = "The router gave no session cookie";
    return false;
  }
  session.cookie = ans2.set_cookie.substr(0, ans2.set_cookie.find(';'));

  // 3. GET / to read token and model name
  std::string req3 = "GET / HTTP/1.0\r\nHost: " + host_hdr + "\r\nReferer: http://" + router_host + "/\r\nCookie: " + session.cookie + "\r\nConnection: close\r\n\r\n";
  tplink_http_answer ans3;
  if (!tplink_http(req3, ans3, error))
    return false;

  size_t tok_pos = ans3.body.find("var token=\"");
  if (tok_pos == std::string::npos) {
    error = "The router gave no session token";
    return false;
  }
  session.token = ans3.body.substr(tok_pos + 11, ans3.body.find('"', tok_pos + 11) - (tok_pos + 11));

  size_t model_pos = ans3.body.find("var modelName=\"");
  if (model_pos != std::string::npos) {
    size_t end_pos = ans3.body.find('"', model_pos + 15);
    if (end_pos != std::string::npos) {
      std::string m = ans3.body.substr(model_pos + 15, end_pos - (model_pos + 15));
      if (!m.empty())
        router_model_name = "TP-Link " + m;
    }
  }

  return true;
}

// Parses key-value blocks like "[stack]index\nkey=val\n..."
static bool tplink_read_modem(tplink_session &session, nlohmann::json &out, std::string &error)
{
  std::string host_hdr = (router_host.find(':') != std::string::npos ? "[" + router_host + "]" : router_host)
    + (router_port != "80" ? ":" + router_port : "");

  std::vector<std::string> oids = {
    "WAN_LTE_LINK_CFG",
    "LTE_NET_STATUS",
    "LTE_PROF_STAT",
    "WAN_LTE_INTF_CFG"
  };
  std::string q_header = "5&5&5&5";
  std::string q_data = q_header + "\r\n";
  for (size_t i = 0; i < oids.size(); ++i) {
    q_data += "[" + oids[i] + "#0,0,0,0,0,0#0,0,0,0,0,0]" + std::to_string(i) + ",0\r\n";
  }

  auto q_enc = session.aes_encrypt_with_sign(q_data, false);
  std::string q_body = "sign=" + q_enc.second + "\r\ndata=" + q_enc.first + "\r\n";
  std::string req = "POST /cgi_gdpr?" + q_header + " HTTP/1.0\r\nHost: " + host_hdr +
    "\r\nReferer: http://" + router_host + "/\r\nCookie: " + session.cookie +
    "\r\nTokenID: " + session.token + "\r\nConnection: close\r\nContent-Type: text/plain\r\nContent-Length: " +
    std::to_string(q_body.size()) + "\r\n\r\n" + q_body;

  tplink_http_answer ans;
  if (!tplink_http(req, ans, error))
    return false;

  if (ans.status != 200) {
    error = "The router answered /cgi_gdpr with HTTP " + std::to_string(ans.status);
    return false;
  }

  std::string decrypted = aes_128_cbc_decrypt(session.aes_key, session.aes_iv, ans.body);
  if (decrypted.empty()) {
    error = "Failed to decrypt router response";
    return false;
  }

  // Parse lines into maps by index and stack
  // Map: index -> (stack -> map<key, val>)
  std::map<int, std::map<std::string, std::map<std::string, std::string>>> blocks;
  std::string current_stack;
  int current_index = -1;

  size_t at = 0;
  while (at < decrypted.size()) {
    size_t eol = decrypted.find('\n', at);
    if (eol == std::string::npos) eol = decrypted.size();
    std::string line = decrypted.substr(at, eol - at);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.pop_back();

    if (!line.empty() && line.front() == '[') {
      size_t rb = line.find(']');
      if (rb != std::string::npos) {
        current_stack = line.substr(1, rb - 1);
        current_index = atoi(line.c_str() + rb + 1);
      }
    } else if (!line.empty() && current_index >= 0) {
      size_t eq = line.find('=');
      if (eq != std::string::npos) {
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        blocks[current_index][current_stack][k] = v;
      }
    }
    at = eol + 1;
  }

  // Find the active stack (the one with connectStatus=1 or connStat=1)
  std::string active_stack = "2,1,0,0,0,0"; // default LTE stack on Archer
  if (blocks.count(0)) {
    for (const auto &it : blocks[0]) {
      const auto &m = it.second;
      auto cs = m.find("connectStatus");
      if (cs != m.end() && cs->second == "1") {
        active_stack = it.first;
        break;
      }
    }
  }

  // Extract fields
  const auto &link_cfg = blocks[0][active_stack];
  const auto &net_stat = blocks[1][active_stack];
  const auto &prof_stat = blocks[2][active_stack];
  // INTF_CFG may have a 0 in 2nd position, e.g. "2,0,0,0,0,0"
  std::string intf_stack = active_stack;
  if (!blocks[3].count(intf_stack) && active_stack.size() >= 3) {
    intf_stack[2] = '0';
  }
  const auto &intf_cfg = blocks[3][intf_stack];

  int conn_status = link_cfg.count("connectStatus") ? atoi(link_cfg.at("connectStatus").c_str()) : 0;
  int net_type = link_cfg.count("networkType") ? atoi(link_cfg.at("networkType").c_str()) : 3;
  int roaming = link_cfg.count("roamingStatus") ? atoi(link_cfg.at("roamingStatus").c_str()) : 0;

  std::string op;
  if (prof_stat.count("ispName") && !prof_stat.at("ispName").empty())
    op = prof_stat.at("ispName");
  else if (prof_stat.count("spn") && !prof_stat.at("spn").empty())
    op = prof_stat.at("spn");

  std::string band_str;
  if (net_stat.count("rfInfoBand")) {
    int raw_band = atoi(net_stat.at("rfInfoBand").c_str());
    int b1 = raw_band & 0xff;
    int b2 = (raw_band >> 8) & 0xff;
    band_str = tplink_band_name(b1);
    if (b2 != 0) {
      std::string s2 = tplink_band_name(b2);
      if (!s2.empty()) {
        if (!band_str.empty()) band_str += ", ";
        band_str += s2;
      }
    }
  }

  nlohmann::json rsrp_val = nullptr;
  if (net_stat.count("rfInfoRsrp")) {
    int v = atoi(net_stat.at("rfInfoRsrp").c_str());
    if (v != 0) rsrp_val = v;
  }

  nlohmann::json rsrq_val = nullptr;
  if (net_stat.count("rfInfoRsrq")) {
    int v = atoi(net_stat.at("rfInfoRsrq").c_str());
    if (v != 0) rsrq_val = v;
  }

  nlohmann::json sinr_val = nullptr;
  if (net_stat.count("rfInfoSnr")) {
    int v = atoi(net_stat.at("rfInfoSnr").c_str());
    sinr_val = v / 10.0;
  }

  nlohmann::json rssi_val = nullptr;
  if (rsrp_val.is_null() && net_stat.count("rfInfoRssi")) {
    int v = atoi(net_stat.at("rfInfoRssi").c_str());
    if (v != 0) rssi_val = v;
  }

  nlohmann::json total_bytes = nullptr;
  if (intf_cfg.count("totalStatistics")) {
    double bytes = strtod(intf_cfg.at("totalStatistics").c_str(), NULL);
    if (bytes > 0) total_bytes = bytes;
  }

  std::string net_name = network_type_name(net_type);

  out = {
    { "router_model", router_model_name },
    { "modem", net_name },
    { "modem_manufacturer", "TP-Link" },
    { "modem_model", router_model_name.find("Archer") != std::string::npos ?
        router_model_name.substr(router_model_name.find("Archer")) : "Archer MR402-Outdoor" },
    { "operator", op.empty() ? nlohmann::json(nullptr) : nlohmann::json(op) },
    { "conntype", net_name },
    { "ntype", net_name },
    { "state", conn_status == 1 ? "Connected" : "Not connected" },
    { "netstate", roaming ? "Roaming" : "Connected" },
    { "simstate", "Ready" },
    { "band", band_str.empty() ? nlohmann::json(nullptr) : nlohmann::json(band_str) },
    { "rssi", rssi_val },
    { "rsrp", rsrp_val },
    { "rsrq", rsrq_val },
    { "sinr", sinr_val },
    { "signal_quality", nullptr },
    { "temperature", nullptr },
    { "rxbytes", total_bytes },
    { "txbytes", nullptr },
    { "nr_rsrp", nullptr },
    { "nr_sinr", nullptr },
  };

  return true;
}

static bool tplink_is_watched(uint64_t now)
{
  return tplink_watched && now - tplink_asked_at < TPLINK_IDLE_MS;
}

// Waits out the delay, and then until somebody watches.
static void tplink_wait_for_viewers(unsigned delay_ms)
{
  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += delay_ms / 1000;
  until.tv_nsec += (delay_ms % 1000) * 1000000;
  if (until.tv_nsec >= 1000000000) {
    until.tv_sec++;
    until.tv_nsec -= 1000000000;
  }

  pthread_mutex_lock(&tplink_lock);
  while (delay_ms > 0 && pthread_cond_timedwait(&tplink_asked, &tplink_lock, &until) != ETIMEDOUT)
    ;
  while (!tplink_is_watched(now_ms()))
    pthread_cond_wait(&tplink_asked, &tplink_lock);
  pthread_mutex_unlock(&tplink_lock);
}

static void *tplink_thread(void *arg)
{
  tplink_session session;
  unsigned delay_ms = 0;

  if (!tplink_parse_url(tplink_options.url)) {
    tplink_set_error("-tplink-url is not a plain http:// address: " + std::string(tplink_options.url));
    return NULL;
  }

  while (true) {
    tplink_wait_for_viewers(delay_ms);
    delay_ms = TPLINK_RETRY_MS;

    std::string error;
    nlohmann::json modem;

    if (session.token.empty()) {
      if (!tplink_login(session, error)) {
        tplink_set_error(error);
        continue;
      }
    }

    if (!tplink_read_modem(session, modem, error)) {
      // If query failed with unauthorized/expired token, retry login immediately
      session.token.clear();
      session.cookie.clear();
      tplink_set_error(error);
      continue;
    }

    pthread_mutex_lock(&tplink_lock);
    tplink_modem = modem;
    tplink_heard_at = now_ms();
    tplink_error.clear();
    pthread_mutex_unlock(&tplink_lock);
    delay_ms = TPLINK_POLL_MS;
  }
  return NULL;
}

static pthread_once_t tplink_once = PTHREAD_ONCE_INIT;

static void tplink_start()
{
  pthread_t thread;
  pthread_create(&thread, NULL, tplink_thread, NULL);
  pthread_detach(thread);
}

} // namespace

extern "C" void tplink_json(FILE *stream)
{
  if (!tplink_options.url[0]) {
    fputs("null", stream);
    return;
  }

  pthread_once(&tplink_once, tplink_start);

  pthread_mutex_lock(&tplink_lock);
  uint64_t now = now_ms();
  tplink_watched = true;
  tplink_asked_at = now;
  pthread_cond_broadcast(&tplink_asked);
  nlohmann::json out = tplink_heard_at ? tplink_modem : nlohmann::json::object();
  out["age"] = tplink_heard_at ? nlohmann::json(now - tplink_heard_at) : nlohmann::json(nullptr);
  out["error"] = tplink_error;
  pthread_mutex_unlock(&tplink_lock);

  fputs(out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).c_str(), stream);
}
