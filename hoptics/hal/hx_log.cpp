// Hoptics-MC log keeper (see hx_log.h). Lines collect in a ring in PSRAM; once a minute what is new is appended to
// /hx_log on SPIFFS (a file past 64 KB is rewritten with the newest 24 KB). At the first loop after a start, /hx_log
// becomes /hx_log.1 and /hx_log.1 becomes /hx_log.2. MIT licence.
#if defined(HOPTICS_UI) && defined(ESP32)
#include "hx_log.h"
#include <Arduino.h>
#include <SPIFFS.h>
#include <esp_core_dump.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <algorithm>
#include <stdarg.h>

namespace hxlog
{
namespace
{
constexpr size_t RING = 24 * 1024, FILE_MAX = 64 * 1024;
const char *const CUR = "/hx_log", *const LAST = "/hx_log.1", *const BEFORE = "/hx_log.2";
char *ring = nullptr;
size_t wr = 0, savedWr = 0; // bytes ever written to the ring, and how many of them are on flash
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
bool started = false;
uint32_t lastSave = 0;

void keep(const char *s, size_t n)
{
    if (!ring) // first used at the very start (one task), from PSRAM
        ring = (char *)heap_caps_malloc(RING, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ring)
        return;
    portENTER_CRITICAL(&mux);
    for (size_t i = 0; i < n; i++)
        ring[(wr + i) % RING] = s[i];
    wr += n;
    portEXIT_CRITICAL(&mux);
}

void vline(bool kept, const char *fmt, va_list ap)
{
    char b[240];
    const int p = snprintf(b, sizeof(b), "[%lu] ", (unsigned long)(millis() / 1000)); // uptime, in the kept copy only
    int n = p + snprintf(b + p, sizeof(b) - p, "hoptics: ");
    n += vsnprintf(b + n, sizeof(b) - n, fmt, ap);
    n = std::min<int>(n, sizeof(b) - 2);
    if (b[n - 1] != '\n')
        b[n++] = '\n';
    b[n] = 0;
    Serial.print(b + p);
    if (kept)
        keep(b, n);
}

// the newest bytes of the ring, oldest first (at most 'max')
size_t snapshot(char *out, size_t max, size_t &upTo)
{
    portENTER_CRITICAL(&mux);
    upTo = wr;
    const size_t n = std::min(std::min(wr, RING), max);
    for (size_t i = 0; i < n; i++)
        out[i] = ring[(wr - n + i) % RING];
    portEXIT_CRITICAL(&mux);
    return n;
}

void printFile(const char *path, const char *title)
{
    File f = SPIFFS.open(path, FILE_READ);
    if (!f)
        return;
    const size_t size = f.size();
    Serial.printf("===== Hoptics-MC log: %s =====\n", title);
    if (size > RING) // the newest part is the useful one
        f.seek(size - RING);
    char b[512];
    for (int n; (n = f.read((uint8_t *)b, sizeof(b))) > 0;)
        Serial.write((const uint8_t *)b, n);
    f.close();
    Serial.printf("===== end of %s =====\n", title);
}

// a crash the last session left in the core dump partition: where it happened, once
void crashReport(void)
{
    if (esp_core_dump_image_check() != ESP_OK)
        return;
    static esp_core_dump_summary_t s; // ~200 bytes: off the stack
    if (esp_core_dump_get_summary(&s) == ESP_OK) {
        line("the last session crashed in task '%.15s' at 0x%08lx (cause %lu, address 0x%08lx), firmware %s", s.exc_task,
             (unsigned long)s.exc_pc, (unsigned long)s.ex_info.exc_cause, (unsigned long)s.ex_info.exc_vaddr,
             (const char *)s.app_elf_sha256);
        char bt[16 * 11 + 1] = "";
        for (uint32_t i = 0, o = 0; i < s.exc_bt_info.depth && i < 16; i++)
            o += snprintf(bt + o, sizeof(bt) - o, " 0x%08lx", (unsigned long)s.exc_bt_info.bt[i]);
        line("backtrace:%s%s", bt, s.exc_bt_info.corrupted ? " (cut short)" : "");
    }
    esp_core_dump_image_erase();
}
} // namespace

void line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vline(true, fmt, ap);
    va_end(ap);
}

void usb(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vline(false, fmt, ap);
    va_end(ap);
}

static SemaphoreHandle_t saveLock = nullptr; // the log task's minute and the main loop's save() (before a restart)

void save(void)
{
    if (!started || !ring || !saveLock)
        return;
    xSemaphoreTake(saveLock, portMAX_DELAY);
    lastSave = millis();
    char *buf = (char *)heap_caps_malloc(RING, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf)
        return;
    size_t upTo;
    const size_t have = snapshot(buf, RING, upTo);
    const size_t fresh = std::min(upTo - savedWr, have);
    if (fresh) {
        File f = SPIFFS.open(CUR, FILE_APPEND);
        if (f && f.size() + fresh <= FILE_MAX)
            f.write((const uint8_t *)buf + have - fresh, fresh);
        else { // too long: start the file again with the newest part
            if (f)
                f.close();
            f = SPIFFS.open(CUR, FILE_WRITE);
            if (f)
                f.write((const uint8_t *)buf, have);
        }
        if (f)
            f.close();
        savedWr = upTo;
    }
    free(buf);
    xSemaphoreGive(saveLock);
}

void tick(void)
{
    if (!started) {
        started = true;
        // this start's log is new; keep the last two sessions
        if (SPIFFS.exists(LAST)) {
            if (SPIFFS.exists(BEFORE))
                SPIFFS.remove(BEFORE);
            SPIFFS.rename(LAST, BEFORE);
        }
        if (SPIFFS.exists(CUR))
            SPIFFS.rename(CUR, LAST);
        printFile(LAST, "the last session");
        crashReport();
        saveLock = xSemaphoreCreateMutex();
        save();
        // from now on the log goes to flash once a minute from its own task: a flash write can take a second or two,
        // and the main loop shouldn't stall for it
        xTaskCreatePinnedToCore(
            [](void *) {
                for (;;) {
                    vTaskDelay(pdMS_TO_TICKS(60000));
                    save();
                }
            },
            "hoptics-log", 4096, nullptr, 1, nullptr, tskNO_AFFINITY);
        return;
    }
}

void dump(void)
{
    save();
    printFile(BEFORE, "two sessions ago");
    printFile(LAST, "the last session");
    printFile(CUR, "this session");
}

void text(std::string &out)
{
    save();
    static const char *const files[][2] = {{BEFORE, "two sessions ago"}, {LAST, "the last session"}, {CUR, "this session"}};
    for (const auto &f : files) {
        File in = SPIFFS.open(f[0], FILE_READ);
        if (!in)
            continue;
        const size_t size = in.size();
        if (size > RING) // the newest part is the useful one
            in.seek(size - RING);
        out += "===== Hoptics-MC log: ";
        out += f[1];
        out += " =====\n";
        char b[512];
        for (int n; (n = in.read((uint8_t *)b, sizeof(b))) > 0;)
            out.append(b, n);
        in.close();
        out += "===== end of ";
        out += f[1];
        out += " =====\n";
    }
}
} // namespace hxlog
#endif
