// Hoptics-MC: start-up trace. Each step of MeshCore's setup() is logged as it finishes and remembered in RTC memory,
// so after a crash or a watchdog reset the next start says which step it stopped at. MIT licence.
#if defined(HOPTICS_UI) && defined(ESP32)
#include "hx_log.h"
#include "hx_lvgl.h"
#include "hx_ui.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_attr.h>
#include <esp_system.h>

namespace hxhal
{
namespace
{
constexpr uint32_t MAGIC = 0x48584254; // "HXBT"
RTC_NOINIT_ATTR uint32_t rtcMagic;
RTC_NOINIT_ATTR uint32_t rtcStep;
const char *const stepName[] = {"power on", "screen", "radio", "storage", "mesh", "connections", "sensors", "screens", "running"};
constexpr int LAST = 8;

const char *resetName(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "restart";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "wake from sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "other";
    }
}
} // namespace

void bootReport(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    if (rtcMagic == MAGIC && rtcStep < LAST)
        hxlog::line("the last start stopped after '%s' (this reset: %s)", stepName[rtcStep], resetName(r));
    else
        hxlog::line("starting (reset: %s)", resetName(r));
    rtcMagic = MAGIC;
    rtcStep = 0;
}

void bootStage(int step)
{
    if (step < 0 || step > LAST)
        return;
    rtcMagic = MAGIC;
    rtcStep = step;
    hxlog::line("start %d/%d %s (%lu ms)", step, LAST, stepName[step], (unsigned long)millis());
    delay(2); // let the USB serial send it before the next step, in case that one never returns
    hx::bootStep(step);
}

int bootArt(void)
{
    // a counter in NVS, one small write per start (NVS spreads its writes); the screen takes it modulo its pictures
    Preferences p;
    if (!p.begin("hoptics", false))
        return 0;
    uint8_t n = p.getUChar("bootbg", 0);
    p.putUChar("bootbg", (uint8_t)((n + 1) % 60));
    p.end();
    return n;
}
} // namespace hxhal
#endif
