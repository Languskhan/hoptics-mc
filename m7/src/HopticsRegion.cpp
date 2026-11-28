// Hoptics M7: the region's repeaters and rooms from a CoreScope analyzer the owner picks (off until set), so the map and the packet paths can name
// nodes this radio hasn't heard an advert from yet.
//
// Everything that comes back is treated as hostile: TLS is verified against the built-in CA bundle, redirects are
// refused (no downgrade), each page is parsed through an ArduinoJson filter with a nesting limit, keys must be
// 64 hex characters, coordinates must be in range, names are cut to fit, and the number of pages is capped.
#include "HopticsData.h"
#include <helpers/NetworkLink.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>
#include <Preferences.h>

extern const uint8_t rootca_crt_bundle_start[] asm("_binary_src_certs_x509_crt_bundle_bin_start") __attribute__((weak));
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_src_certs_x509_crt_bundle_bin_end") __attribute__((weak));

namespace hx {
namespace {

// Reads at most `left` bytes from the HTTP body, so a broken or hostile server can't feed the parser forever.
class CappedStream : public Stream {
 public:
  CappedStream(Stream& s, size_t cap) : s_(s), left_(cap) {}
  int available() override { return left_ ? s_.available() : 0; }
  int read() override {
    if (!left_) return -1;
    int c = s_.read();
    if (c >= 0) left_--;
    return c;
  }
  int peek() override { return left_ ? s_.peek() : -1; }
  size_t readBytes(char* buf, size_t n) override {
    if (n > left_) n = left_;
    size_t got = s_.readBytes(buf, n);
    left_ -= got;
    return got;
  }
  size_t write(uint8_t) override { return 0; }
  bool exhausted() const { return left_ == 0; }

 private:
  Stream& s_;
  size_t left_;
};

// The analyzer's address, e.g. https://scope.example.org. Empty = off. Set from the System tab.
portMUX_TYPE src_mux = portMUX_INITIALIZER_UNLOCKED;
char src[96];
volatile bool src_changed;

void getSrc(char* out, size_t n) {
  portENTER_CRITICAL(&src_mux);
  strlcpy(out, src, n);
  portEXIT_CRITICAL(&src_mux);
}
constexpr int kPage = 100, kMaxPages = 25;           // at most 2500 nodes per refresh
constexpr uint32_t kEveryMs = 6UL * 3600UL * 1000UL;  // refresh every 6 hours
constexpr uint32_t kRetryMs = 10UL * 60UL * 1000UL;   // or 10 minutes after a failure

int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parseKey(const char* s, uint8_t out[4]) {
  if (!s || strlen(s) != 64) return false;
  for (int i = 0; i < 64; i++) if (hexv(s[i]) < 0) return false;
  for (int i = 0; i < 4; i++) out[i] = (uint8_t)(hexv(s[i * 2]) << 4 | hexv(s[i * 2 + 1]));
  return true;
}

// "2026-11-28T07:53:36Z" -> seconds since 1970 (0 if it doesn't look right)
uint32_t parseTime(const char* s) {
  int y, mo, d, h, mi, se;
  if (!s || sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
  if (y < 2020 || y > 2100 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return 0;
  y -= mo <= 2;  // days from civil (Howard Hinnant)
  const int era = y / 400, yoe = y - era * 400;
  const int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = (long)era * 146097 + doe - 719468;
  return (uint32_t)(days * 86400L + h * 3600L + mi * 60L + se);
}

uint8_t roleType(const char* r) {
  if (!r) return 0;
  if (!strcmp(r, "repeater")) return 2;
  if (!strcmp(r, "room")) return 3;
  if (!strcmp(r, "companion")) return 1;
  if (!strcmp(r, "sensor")) return 4;
  return 0;
}

// one page; returns nodes added, or -1 on error. *more says whether the server has another page.
int fetchPage(const char* role, int offset, bool* more, char* err, size_t err_len) {
  *more = false;
  const size_t bundle_len = (rootca_crt_bundle_start && rootca_crt_bundle_end > rootca_crt_bundle_start)
                                ? (size_t)(rootca_crt_bundle_end - rootca_crt_bundle_start) : 0;
  if (!bundle_len) { snprintf(err, err_len, "no CA bundle"); return -1; }
  WiFiClientSecure tls;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  tls.setCACertBundle(rootca_crt_bundle_start, bundle_len);
#else
  tls.setCACertBundle(rootca_crt_bundle_start);
#endif
  tls.setTimeout(15);
  HTTPClient http;
  char base[96], url[192];
  getSrc(base, sizeof(base));
  if (!base[0]) { snprintf(err, err_len, "no regional analyzer set"); return -1; }
  snprintf(url, sizeof(url), "%s/api/nodes?role=%s&limit=%d&offset=%d", base, role, kPage, offset);
  if (!http.begin(tls, url)) { snprintf(err, err_len, "connect"); return -1; }
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  http.useHTTP10(true);  // plain, unchunked body for the stream parser
  http.setTimeout(15000);
  http.setUserAgent("Hoptics-M7");
  const int code = http.GET();
  if (code != 200) { snprintf(err, err_len, "HTTP %d", code); http.end(); return -1; }

  JsonDocument filter;
  JsonObject f = filter["nodes"][0].to<JsonObject>();
  for (const char* k : {"name", "public_key", "role", "lat", "lon", "last_seen", "relay_count_24h"}) f[k] = true;
  filter["has_more"] = true;
  JsonDocument doc;
  CappedStream body(http.getStream(), 512 * 1024);  // a page of 100 nodes is far smaller
  DeserializationError e = deserializeJson(doc, body, DeserializationOption::Filter(filter),
                                           DeserializationOption::NestingLimit(4));
  http.end();
  if (e || body.exhausted()) { snprintf(err, err_len, "bad reply: %s", e ? e.c_str() : "too large"); return -1; }

  int added = 0;
  for (JsonObject n : doc["nodes"].as<JsonArray>()) {
    uint8_t pub[4];
    if (!parseKey(n["public_key"] | (const char*)nullptr, pub)) continue;
    const float lat = n["lat"] | 0.0f, lon = n["lon"] | 0.0f;
    const bool loc = (lat != 0 || lon != 0) && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180;
    const int32_t relays = n["relay_count_24h"] | 0;
    addRegionNode(pub, roleType(n["role"] | (const char*)nullptr), n["name"] | "", loc, (int32_t)(lat * 1e6f),
                  (int32_t)(lon * 1e6f), parseTime(n["last_seen"] | (const char*)nullptr),
                  relays < 0 ? 0 : (uint32_t)relays);
    added++;
  }
  *more = doc["has_more"] | false;
  return added;
}

void task(void*) {
  uint32_t next = 30000;  // first try 30 s after start
  const uint32_t t0 = millis();
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    if (src_changed) { src_changed = false; regionClear(); next = millis() - t0; }
    if ((int32_t)(millis() - t0 - next) < 0) continue;
    { char b[4]; getSrc(b, sizeof(b)); if (!b[0]) { next = millis() - t0 + 60000; continue; } }   // off
    // needs a network and real time (certificates are checked against the clock)
    if (!activeNetworkLink().isConnected() || time(nullptr) < 1700000000) { next = millis() - t0 + 30000; continue; }
    char err[48] = "";
    int total = 0;
    bool ok = true;
    regionBegin();
    for (const char* role : {"repeater", "room"}) {
      bool more = true;
      for (int page = 0; more && page < kMaxPages; page++) {
        const int n = fetchPage(role, page * kPage, &more, err, sizeof(err));
        if (n < 0) { ok = false; break; }
        total += n;
        vTaskDelay(pdMS_TO_TICKS(300));  // be gentle with the server
      }
      if (!ok) break;
    }
    regionDone(ok, total, err);
    Serial.printf("Hoptics: region nodes %s (%d)%s%s\n", ok ? "updated" : "failed", total, ok ? "" : ": ", err);
    next = millis() - t0 + (ok ? kEveryMs : kRetryMs);
  }
}

}  // namespace

void startRegion() {
  static bool started = false;
  if (started) return;
  started = true;
  Preferences p;
  if (p.begin("hxregion", true)) { char b[96] = ""; p.getString("url", b, sizeof(b)); p.end(); portENTER_CRITICAL(&src_mux); strlcpy(src, b, sizeof(src)); portEXIT_CRITICAL(&src_mux); }
  xTaskCreatePinnedToCore(task, "hx_region", 16384, nullptr, 1, nullptr, 1);
}

// https://host[/path], a public name (not an address), no port, user or query; "" turns it off
bool regionConfigure(const char* url, char* err, size_t err_len) {
  char b[96];
  const size_t len = strlen(url);
  if (len == 0) b[0] = 0;
  else {
    if (len >= sizeof(b) || strncmp(url, "https://", 8) != 0) { snprintf(err, err_len, "use https://your.analyzer.site"); return false; }
    strlcpy(b, url, sizeof(b));
    size_t n = strlen(b);
    while (n > 8 && b[n - 1] == '/') b[--n] = 0;
    const char* host = b + 8;
    const char* slash = strchr(host, '/');
    const size_t hl = slash ? (size_t)(slash - host) : strlen(host);
    bool dot = false, letter = false;
    for (size_t i = 0; i < hl; i++) {
      const char c = host[i];
      if (c == '.') dot = true;
      else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) letter = true;
      else if (!(c >= '0' && c <= '9') && c != '-') { snprintf(err, err_len, "bad host name"); return false; }
    }
    if (!hl || !dot || !letter || host[0] == '.' || host[0] == '-') { snprintf(err, err_len, "use a public host name"); return false; }
    for (const char* c = slash; c && *c; c++)
      if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || strchr("/-_.", *c))) {
        snprintf(err, err_len, "bad path"); return false;
      }
  }
  Preferences p;
  if (!p.begin("hxregion", false)) { snprintf(err, err_len, "storage"); return false; }
  p.putString("url", b);
  p.end();
  portENTER_CRITICAL(&src_mux);
  strlcpy(src, b, sizeof(src));
  portEXIT_CRITICAL(&src_mux);
  src_changed = true;
  return true;
}

void regionSource(char* out, size_t n) { getSrc(out, n); }

}  // namespace hx
