// Hoptics-MC microSD card (see hx_sd.h). The card's pins, from the boards' variants (MeshCore's and Meshtastic's):
//   T-Deck Plus        CS 39 on SCK 40 / MISO 38 / MOSI 41, with the screen and the radio
//   ThinkNode M9       CS 48 on SCK 40 / MISO 38 / MOSI 47, with the screen and the radio (SDCARD_CS in its variant.h)
//   V4-R8 Kit V2       CS 3 on the screen's SCK 16 / MISO 45 / MOSI 15
//   V4 Expansion Kit   no slot
// On every board it is the global SPI object, on the host the screen's driver starts (LovyanGFX does SPI.begin() on the
// default host): a second SPIClass on that host would reset the peripheral under the screen. 4 MHz: on the V4-R8 kit
// the card answered at the 400 kHz start-up clock but its data reads failed at 20 MHz and up (found with Hoptics MT on
// the same hardware); 4 MHz is also Meshtastic's own default for cards. Raise after a hardware test. MIT licence.
#if defined(HOPTICS_UI) && defined(ESP32)
#include "hx_sd.h"
#include "hx_hal.h"
#include "hx_log.h"
#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <SPI.h>
#include <ff.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <soc/spi_reg.h>
#include <string.h>

namespace hxsd
{
namespace
{
#if defined(LILYGO_TDECK)
constexpr int CS = 39;
#elif defined(THINKNODE_M9)
constexpr int CS = SDCARD_CS;
#elif defined(HELTEC_V4_R8_TFT)
constexpr int CS = 3;
#else
constexpr int CS = -1;
#endif
constexpr uint32_t FREQ = 4000000;

bool up = false;
char drv[4] = "0:";
uint32_t cluster = 0, clusters = 0, freeClusters = 0;
TaskHandle_t mainTask = nullptr;
SemaphoreHandle_t gate = nullptr; // one waiting task at a time
SemaphoreHandle_t done = nullptr; // the main loop finished the job
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
struct Job {
    void (*fn)(void *);
    void *ctx;
    bool ran;
};
Job *pending = nullptr; // guarded by mux
bool changed = false;   // a job ran since the free space was last read
uint32_t spaceAt = 0;

void readSpace(void)
{
    FATFS *fs;
    DWORD fre;
    if (f_getfree(drv, &fre, &fs) == FR_OK)
        freeClusters = fre;
}

bool runHere(void (*fn)(void *), void *ctx)
{
    if (hxhal::frameOpen())
        return false; // mid-frame (a DMA board's transaction is open): never between two strips
    // the screen's driver leaves DMA switched on for the host after a DMA frame (V4-R8: it clears it on its next
    // write); the card's driver sends from the processor, so it goes off first. All the card buses are on SPI2
    REG_CLR_BIT(SPI_DMA_CONF_REG(2), SPI_DMA_TX_ENA | SPI_DMA_RX_ENA);
    fn(ctx);
    changed = true;
    return true;
}
} // namespace

bool begin(void)
{
    if (CS < 0 || up)
        return up;
    mainTask = xTaskGetCurrentTaskHandle();
    if (!gate) {
        gate = xSemaphoreCreateMutex();
        done = xSemaphoreCreateBinary();
    }
    REG_CLR_BIT(SPI_DMA_CONF_REG(2), SPI_DMA_TX_ENA | SPI_DMA_RX_ENA);
    const uint32_t t0 = millis();
    // A card can be slow to wake (the ThinkNode M9's built-in one does not answer its first tries at boot): a few
    // tries with a pause, then slower clocks, as WADAMESH found on the same boards. No card costs about 1.2 s.
    static const struct {
        uint16_t settleMs;
        uint32_t hz;
    } kTries[] = {{0, FREQ}, {220, FREQ}, {300, 1000000}, {650, 400000}};
    bool ok = false;
    uint32_t hz = 0;
    for (const auto &t : kTries) {
        if (t.settleMs) {
            SD.end();
            pinMode(CS, OUTPUT);
            digitalWrite(CS, HIGH); // deselected while it waits: the screen and the radio share the bus
            delay(t.settleMs);
        }
        if (SD.begin(CS, SPI, t.hz, "/sd", 4, false) && SD.cardType() != CARD_NONE) {
            ok = true;
            hz = t.hz;
            break;
        }
    }
    if (!ok) {
        SD.end();
        pinMode(CS, OUTPUT);
        digitalWrite(CS, HIGH);
        hxlog::line("sd: no card, or not FAT32 (%lu ms)", (unsigned long)(millis() - t0));
        return false;
    }
    if (hz != FREQ)
        hxlog::line("sd: answered at %lu kHz", (unsigned long)(hz / 1000));
    // FatFS's drive number for the card (the only FAT volume on these boards; checked against the card's size)
    const uint64_t total = SD.totalBytes();
    for (int i = 0; i < FF_VOLUMES; i++) {
        char d[4] = {(char)('0' + i), ':', 0, 0};
        FATFS *fs;
        DWORD fre;
        if (f_getfree(d, &fre, &fs) == FR_OK && (uint64_t)fs->csize * fs->ssize * (fs->n_fatent - 2) == total) {
            memcpy(drv, d, sizeof(drv));
            cluster = (uint32_t)fs->csize * fs->ssize;
            clusters = fs->n_fatent - 2;
            freeClusters = fre;
            up = true;
            break;
        }
    }
    if (!up) {
        hxlog::line("sd: mounted but its FAT volume wasn't found");
        SD.end();
        return false;
    }
    hxlog::line("sd: card mounted, %lu MB, %lu MB free, %lu KB clusters (%lu ms)", (unsigned long)(totalBytes() >> 20),
                (unsigned long)(freeBytes() >> 20), (unsigned long)(cluster / 1024), (unsigned long)(millis() - t0));
    return true;
}

bool mounted(void) { return up; }
uint64_t totalBytes(void) { return (uint64_t)clusters * cluster; }
uint64_t freeBytes(void) { return (uint64_t)freeClusters * cluster; }
uint32_t clusterBytes(void) { return cluster; }

void tick(void)
{
    if (!up)
        return;
    Job *j;
    portENTER_CRITICAL(&mux);
    j = pending;
    pending = nullptr;
    portEXIT_CRITICAL(&mux);
    if (j) {
        j->ran = runHere(j->fn, j->ctx);
        xSemaphoreGive(done);
    } else if (changed && millis() - spaceAt > 2000) {
        changed = false;
        spaceAt = millis();
        readSpace(); // FAT's own count, kept up to date as files come and go: no card access after the first time
    }
}

bool run(void (*fn)(void *), void *ctx, uint32_t waitMs)
{
    if (!up)
        return false;
    if (xTaskGetCurrentTaskHandle() == mainTask)
        return runHere(fn, ctx);
    if (xSemaphoreTake(gate, pdMS_TO_TICKS(waitMs)) != pdTRUE)
        return false;
    Job j{fn, ctx, false};
    xSemaphoreTake(done, 0); // nothing stale
    portENTER_CRITICAL(&mux);
    pending = &j;
    portEXIT_CRITICAL(&mux);
    if (xSemaphoreTake(done, pdMS_TO_TICKS(waitMs)) != pdTRUE) {
        portENTER_CRITICAL(&mux);
        const bool notStarted = pending == &j;
        if (notStarted)
            pending = nullptr; // withdrawn
        portEXIT_CRITICAL(&mux);
        if (!notStarted)
            xSemaphoreTake(done, portMAX_DELAY); // the main loop is doing it now: it is ours to wait for (j is here)
    }
    xSemaphoreGive(gate);
    return j.ran;
}

void fatPath(char *out, size_t n, const char *path) { snprintf(out, n, "%s%s", drv, path); }

namespace
{
// the folders above path, made where missing
void makeParents(const char *path)
{
    char p[200];
    fatPath(p, sizeof(p), path);
    for (char *s = strchr(p + 3, '/'); s; s = strchr(s + 1, '/')) { // past "0:/"
        *s = 0;
        f_mkdir(p); // FR_EXIST when it is there
        *s = '/';
    }
}
} // namespace

bool writeFile(const char *path, const void *data, size_t len, bool append)
{
    bool ok = false;
    run([&] {
        char p[200];
        fatPath(p, sizeof(p), path);
        static FIL *f = (FIL *)heap_caps_malloc(sizeof(FIL), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); // ~4 KB, main loop only
        if (!f)
            return;
        const BYTE mode = FA_WRITE | (append ? FA_OPEN_APPEND : FA_CREATE_ALWAYS);
        FRESULT r = f_open(f, p, mode);
        if (r == FR_NO_PATH) {
            makeParents(path);
            r = f_open(f, p, mode);
        }
        if (r != FR_OK)
            return;
        UINT bw = 0;
        ok = f_write(f, data, (UINT)len, &bw) == FR_OK && bw == len;
        ok = f_close(f) == FR_OK && ok;
    });
    return ok;
}

int64_t fileSize(const char *path)
{
    int64_t size = -1;
    run([&] {
        char p[200];
        fatPath(p, sizeof(p), path);
        FILINFO fi;
        if (f_stat(p, &fi) == FR_OK)
            size = (int64_t)fi.fsize;
    });
    return size;
}

bool removeFile(const char *path)
{
    bool ok = false;
    run([&] {
        char p[200];
        fatPath(p, sizeof(p), path);
        ok = f_unlink(p) == FR_OK;
    });
    return ok;
}

bool renameFile(const char *from, const char *to)
{
    bool ok = false;
    run([&] {
        char a[200], b[200];
        fatPath(a, sizeof(a), from);
        fatPath(b, sizeof(b), to);
        f_unlink(b);
        ok = f_rename(a, b) == FR_OK;
    });
    return ok;
}
} // namespace hxsd
#endif
