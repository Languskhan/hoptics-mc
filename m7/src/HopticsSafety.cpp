// Hoptics M7: rollback after a bad update, and the read-only stats feed for Home Assistant / Grafana.
#include "HopticsData.h"
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_random.h>

namespace hx {

// ---- rollback. Before restarting into a new image the node notes which slot it came from. The new image
// counts its starts; if it can't stay up for 3 minutes three times in a row, the node switches back to the
// previous slot. A good run clears the note.
void markUpdateInstalled() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running) return;
  Preferences p;
  if (!p.begin("hxboot", false)) return;
  p.putString("prev", running->label);
  p.putUChar("tries", 0);
  p.putBool("pending", true);
  p.end();
}

void bootCheck() {
  Preferences p;
  if (!p.begin("hxboot", false)) return;
  if (!p.getBool("pending", false)) { p.end(); return; }
  const uint8_t tries = p.getUChar("tries", 0) + 1;
  p.putUChar("tries", tries);
  const String prev = p.getString("prev", "");
  if (tries <= 3) { p.end(); return; }
  p.putBool("pending", false);  // whatever happens next, don't loop
  p.end();
  const esp_partition_t* back = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, prev.c_str());
  if (back && back != esp_ota_get_running_partition() && esp_ota_set_boot_partition(back) == ESP_OK) {
    Serial.println("Hoptics: new firmware kept failing, going back to the previous one");
    delay(200);
    ESP.restart();
  }
}

void bootGood() {
  Preferences p;
  if (!p.begin("hxboot", false)) return;
  if (p.getBool("pending", false)) {
    p.putBool("pending", false);
    Serial.println("Hoptics: new firmware confirmed");
  }
  p.end();
}

// ---- stats token: lets Home Assistant or Prometheus read numbers (nothing else) without the admin password
namespace {
char token[33];
}

void statsBegin() {
  Preferences p;
  if (!p.begin("hxstats", true)) return;
  String t = p.getString("token", "");
  p.end();
  if (t.length() == 32) strlcpy(token, t.c_str(), sizeof(token));
}

bool statsTokenOk(const char* t) {
  if (!token[0] || !t || strlen(t) != 32) return false;
  uint8_t diff = 0;  // constant time
  for (int i = 0; i < 32; i++) diff |= (uint8_t)(t[i] ^ token[i]);
  return diff == 0;
}

// new random token ("" turns the feed off); returns it so the page can show it once
const char* statsNewToken(bool off) {
  Preferences p;
  if (!p.begin("hxstats", false)) return "";
  if (off) {
    token[0] = 0;
    p.remove("token");
  } else {
    uint8_t r[16];
    esp_fill_random(r, sizeof(r));
    for (int i = 0; i < 16; i++) sprintf(token + i * 2, "%02x", r[i]);
    p.putString("token", token);
  }
  p.end();
  return token;
}

bool statsOn() { return token[0] != 0; }

}  // namespace hx

namespace hx {
// Green status LED: 0 no network (slow blink), 1 online (short blip every 3 s), 2 setup hotspot (fast blink),
// 3 button held (solid).
void ledTick(uint8_t state) {
#ifdef PIN_STATUS_LED
  static bool init = false;
  if (!init) { pinMode(PIN_STATUS_LED, OUTPUT); init = true; }
  const uint32_t t = millis();
  bool lit;
  switch (state) {
    case 0: lit = (t % 2000) < 1000; break;
    case 2: lit = (t % 250) < 125; break;
    case 3: lit = true; break;
    default: lit = (t % 3000) < 60; break;
  }
  digitalWrite(PIN_STATUS_LED, lit ? LED_STATE_ON : !LED_STATE_ON);
#else
  (void)state;
#endif
}
}  // namespace hx
