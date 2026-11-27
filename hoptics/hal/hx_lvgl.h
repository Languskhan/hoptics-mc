// LVGL glue shared by every LovyanGFX board: draw buffer, flush, tick. MIT licence.
#pragma once
#include "lvgl.h"
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

namespace hxhal
{
// creates the LVGL display on top of an initialised LovyanGFX device (landscape or portrait as rotated); once
lv_display_t *lvglAttach(lgfx::LGFX_Device &tft);
void lvglUseDma(void);       // before lvglAttach: the panel's SPI host takes DMA (no radio on it; a card waits for the frame's end)
// the start-up screen on it (hx_bootscreen.cpp), or the same screen saying why start-up stopped
void showBoot(lgfx::LGFX_Device &tft, const char *board);
void showBootError(lgfx::LGFX_Device &tft, const char *board, const char *what, const char *todo);
int bootArt(void); // which boot picture this start shows: the next one each time (hx_boot.cpp)
} // namespace hxhal
