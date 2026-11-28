// Hoptics M7: Discord alerts through a webhook (off until the owner adds one).
//
// The webhook URL is a secret: it is stored on the node, never sent back to the page in full, and only
// https://discord.com/api/webhooks/... (or discordapp.com) is accepted, so the node can't be pointed at any
// other server. Sending is TLS-verified with redirects refused, rate-limited, and text from the air is
// stripped of Discord markup and mentions are disabled, so a node name can't ping @everyone or fake a message.
#include "HopticsData.h"
#include <helpers/NetworkLink.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <time.h>

extern const uint8_t rootca_crt_bundle_start[] asm("_binary_src_certs_x509_crt_bundle_bin_start") __attribute__((weak));
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_src_certs_x509_crt_bundle_bin_end") __attribute__((weak));

namespace hx {
namespace {

constexpr int kQueue = 8, kMsg = 360;
constexpr uint32_t kMinGapMs = 60000;   // at most one message a minute
constexpr int kPerDay = 40;             // and 40 a day

SemaphoreHandle_t amu;
char url[200];                          // empty = off
uint8_t flags;                          // ALERT_* below
char queue[kQueue][kMsg];
int q_head, q_count;
int sent_today;
uint32_t day_start_ms, last_send_ms;
char last_err[48];

struct ALock {
  ALock() { xSemaphoreTake(amu, portMAX_DELAY); }
  ~ALock() { xSemaphoreGive(amu); }
};

bool urlOk(const char* u) {
  const char* p = nullptr;
  if (!strncmp(u, "https://discord.com/api/webhooks/", 33)) p = u + 33;
  else if (!strncmp(u, "https://discordapp.com/api/webhooks/", 36)) p = u + 36;
  if (!p) return false;
  // <numeric id>/<token>: digits, then letters, digits, '-' and '_' only; nothing else (no query, no ..)
  int digits = 0;
  while (*p >= '0' && *p <= '9') { p++; digits++; }
  if (digits < 5 || *p++ != '/') return false;
  int tok = 0;
  for (; *p; p++, tok++) {
    const char c = *p;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  }
  return tok >= 20 && strlen(u) < sizeof(url);
}

// plain text for Discord: drop markup and mention characters, cut to fit
void clean(char* dst, size_t n, const char* src) {
  size_t o = 0;
  for (; *src && o + 1 < n; src++) {
    const unsigned char c = (unsigned char)*src;
    if (c < 0x20 || strchr("@`*_~|<>[]()\\#", c)) continue;
    dst[o++] = (char)c;
  }
  dst[o] = 0;
}

void jsonEsc(String& out, const char* s) {
  for (; *s; s++) {
    const unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
    else if (c == '\n') out += "\\n";
    else if (c >= 0x20) out += (char)c;
  }
}

bool post(const char* text, char* err, size_t err_len) {
  const size_t bundle_len = (rootca_crt_bundle_start && rootca_crt_bundle_end > rootca_crt_bundle_start)
                                ? (size_t)(rootca_crt_bundle_end - rootca_crt_bundle_start) : 0;
  if (!bundle_len) { snprintf(err, err_len, "no CA bundle"); return false; }
  char u[sizeof(url)];
  { ALock l; strlcpy(u, url, sizeof(u)); }
  if (!u[0]) return true;
  WiFiClientSecure tls;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  tls.setCACertBundle(rootca_crt_bundle_start, bundle_len);
#else
  tls.setCACertBundle(rootca_crt_bundle_start);
#endif
  tls.setTimeout(15);
  HTTPClient http;
  if (!http.begin(tls, u)) { snprintf(err, err_len, "connect"); return false; }
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  http.addHeader("Content-Type", "application/json");
  String body = "{\"username\":\"Hoptics M7\",\"allowed_mentions\":{\"parse\":[]},\"content\":\"";
  jsonEsc(body, text);
  body += "\"}";
  const int code = http.POST(body);
  http.end();
  if (code < 200 || code > 299) { snprintf(err, err_len, "Discord said %d", code); return false; }
  return true;
}

void task(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    char msg[kMsg];
    {
      ALock l;
      if (millis() - day_start_ms > 86400000UL) { day_start_ms = millis(); sent_today = 0; }
      if (!q_count || !url[0] || sent_today >= kPerDay) continue;
      if (last_send_ms && millis() - last_send_ms < kMinGapMs) continue;
      strlcpy(msg, queue[q_head], sizeof(msg));
    }
    if (!activeNetworkLink().isConnected() || time(nullptr) < 1700000000) continue;
    char err[48] = "";
    const bool ok = post(msg, err, sizeof(err));
    ALock l;
    last_send_ms = millis();
    if (ok) {
      q_head = (q_head + 1) % kQueue;
      q_count--;
      sent_today++;
      last_err[0] = 0;
    } else {
      strlcpy(last_err, err, sizeof(last_err));
      if (strstr(err, "said 4")) { q_head = (q_head + 1) % kQueue; q_count--; }  // rejected: don't retry it
    }
  }
}

}  // namespace

void alertName(char* dst, size_t n, const char* name) {
  clean(dst, n, name && name[0] ? name : "unnamed");
  if (!dst[0]) strlcpy(dst, "unnamed", n);
}

void alertsBegin() {
  if (amu) return;
  amu = xSemaphoreCreateMutex();
  Preferences p;
  if (p.begin("hxalert", true)) {
    String u = p.getString("url", "");
    if (urlOk(u.c_str())) strlcpy(url, u.c_str(), sizeof(url));
    flags = p.getUChar("flags", ALERT_SILENT | ALERT_DAILY | ALERT_NEW);
    p.end();
  } else {
    flags = ALERT_SILENT | ALERT_DAILY | ALERT_NEW;
  }
  day_start_ms = millis();
  xTaskCreatePinnedToCore(task, "hx_alerts", 12288, nullptr, 1, nullptr, 1);
}

bool alertsWanted(uint8_t kind) {
  if (!amu) return false;
  ALock l;
  return url[0] && (flags & kind);
}

void alert(uint8_t kind, const char* text) {  // text is ours; names in it went through alertName()
  if (!alertsWanted(kind)) return;
  char m[kMsg];
  strlcpy(m, text, sizeof(m));
  ALock l;
  if (q_count >= kQueue) return;  // full: drop the newest rather than flood later
  strlcpy(queue[(q_head + q_count) % kQueue], m, kMsg);
  q_count++;
}

// set from the page: url "" keeps the saved one, "-" removes it
bool alertsConfigure(const char* new_url, uint8_t new_flags, char* err, size_t err_len) {
  if (!amu) { snprintf(err, err_len, "not ready"); return false; }
  const bool clear = !strcmp(new_url, "-");
  if (!clear && new_url[0] && !urlOk(new_url)) {
    snprintf(err, err_len, "that isn't a Discord webhook link");
    return false;
  }
  Preferences p;
  if (!p.begin("hxalert", false)) { snprintf(err, err_len, "can't save"); return false; }
  ALock l;
  if (clear) { url[0] = 0; p.remove("url"); q_count = 0; }
  else if (new_url[0]) { strlcpy(url, new_url, sizeof(url)); p.putString("url", url); }
  flags = new_flags & (ALERT_SILENT | ALERT_DAILY | ALERT_NEW);
  p.putUChar("flags", flags);
  p.end();
  return true;
}

void alertsTest() {
  if (!amu) return;
  ALock l;
  if (!url[0] || q_count >= kQueue) return;
  strlcpy(queue[(q_head + q_count) % kQueue], "Test from Hoptics M7: alerts are set up.", kMsg);
  q_count++;
}

void writeAlerts(Print& out) {
  if (!amu) { out.print("{\"set\":false}"); return; }
  ALock l;
  const size_t n = strlen(url);
  // only the last 4 characters of the link ever leave the node
  out.printf("{\"set\":%s,\"hint\":\"%s\",\"flags\":%u,\"queued\":%d,\"today\":%d,\"err\":\"", n ? "true" : "false",
             n > 4 ? url + n - 4 : "", flags, q_count, sent_today);
  for (const char* e = last_err; *e; e++) if (*e != '"' && *e != '\\' && (unsigned char)*e >= 0x20) out.print(*e);
  out.print("\"}");
}

}  // namespace hx
