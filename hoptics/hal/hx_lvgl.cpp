// LVGL glue shared by every LovyanGFX board. MIT licence.
#include "hx_ota.h"
#include "hx_lvgl.h"
#include "hx_ui.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

namespace hxhal
{
namespace
{
lgfx::LGFX_Device *gTft = nullptr;
lv_display_t *gDisp = nullptr;
bool gDma = false;      // the panel's SPI host takes DMA: send one strip while the next is drawn
bool gWriting = false;  // a frame's SPI transaction is open (DMA boards keep it open from the first strip to the last)
uint32_t gFrames = 0;   // finished redraws, for the status line
uint32_t gRefrUs = 0, gRenderUs = 0, gRefrAt = 0, gRenderAt = 0; // time in LVGL's refresh (layout + drawing) and drawing

void refrStart(lv_event_t *) { gRefrAt = micros(); }
void refrReady(lv_event_t *) { gRefrUs += micros() - gRefrAt; }
void renderStart(lv_event_t *) { gRenderAt = micros(); }
void renderReady(lv_event_t *) { gRenderUs += micros() - gRenderAt; }

// LVGL draws in the panel's own byte order (RGB565 swapped), so the bytes go out as they are: no conversion pass
void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    const int32_t w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
    const bool last = lv_display_flush_is_last(d);
    if (gDma) {
        // two buffers: LVGL draws the next strip into the other one while this one goes out. Wait for the strip
        // before this one (its buffer is the one LVGL will draw into next), then start this one and hand back at once
        if (!gWriting) {
            gTft->startWrite();
            gWriting = true;
        }
        gTft->waitDMA();
        gTft->setAddrWindow(a->x1, a->y1, w, h);
        gTft->writePixelsDMA(reinterpret_cast<const uint16_t *>(px), w * h, false);
        if (last) { // the frame is done: let the strip finish and close the transaction
            gTft->waitDMA();
            gTft->endWrite();
            gWriting = false;
        }
    } else {
        gTft->startWrite();
        gTft->setAddrWindow(a->x1, a->y1, w, h);
        gTft->writePixels(reinterpret_cast<const uint16_t *>(px), w * h, false);
        gTft->endWrite(); // releases the shared SPI bus for the radio after every strip
    }
    if (last)
        gFrames++;
    lv_display_flush_ready(d);
}
uint32_t tick(void) { return millis(); }

// a strip of rows LVGL draws into: in internal RAM (fast to draw into, and DMA-capable)
void *strip(size_t bytes)
{
    return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
}
} // namespace

void lvglUseDma(void) { gDma = true; }
uint32_t framesDrawn(void) { return gFrames; }
bool frameOpen(void) { return gWriting; }
void refreshTimes(uint32_t &refrUs, uint32_t &drawUs)
{
    refrUs = gRefrUs;
    drawUs = gRenderUs;
}

lv_display_t *lvglAttach(lgfx::LGFX_Device &tft)
{
    if (gDisp)
        return gDisp; // already up: the start-up screen started it
    gTft = &tft;
    lv_init();
    lv_tick_set_cb(tick);
    const int32_t w = tft.width(), h = tft.height();
    lv_display_t *d = lv_display_create(w, h);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565_SWAPPED);
    // strips of 40 rows (6 per 320x240 screen): LVGL walks the screen's objects once per strip, so taller strips mean
    // less work per frame. 25 KB each; Wi-Fi's secure connections keep their buffers in PSRAM, so this fits
    size_t bytes = (size_t)w * 40 * 2;
    void *a = strip(bytes), *b = gDma ? strip(bytes) : nullptr;
    if (!a || (gDma && !b)) { // short of internal RAM: one strip of 16 rows, as before
        heap_caps_free(a);
        heap_caps_free(b);
        b = nullptr;
        gDma = false;
        bytes = (size_t)w * 16 * 2;
        a = strip(bytes);
        if (!a)
            a = malloc(bytes);
    }
    lv_display_set_buffers(d, a, b, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush);
    lv_display_add_event_cb(d, refrStart, LV_EVENT_REFR_START, nullptr);
    lv_display_add_event_cb(d, refrReady, LV_EVENT_REFR_READY, nullptr);
    lv_display_add_event_cb(d, renderStart, LV_EVENT_RENDER_START, nullptr);
    lv_display_add_event_cb(d, renderReady, LV_EVENT_RENDER_READY, nullptr);
    gDisp = d;
    return d;
}

void showBoot(lgfx::LGFX_Device &tft, const char *board)
{
    if (lvglAttach(tft))
        hx::bootShow(board, "Hoptics-MC v" HOPTICS_VERSION, bootArt());
}

void showBootError(lgfx::LGFX_Device &tft, const char *board, const char *what, const char *todo)
{
    showBoot(tft, board); // does nothing if it is already showing
    hx::bootError(what, todo);
    hxota::bootFailed(); // a build on trial goes back to the last one
}
} // namespace hxhal
