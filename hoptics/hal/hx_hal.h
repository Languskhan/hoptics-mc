// Hoptics-MC hardware layer: one per board (hx_hal_tdeck.cpp, hx_hal_m9.cpp, ...). Starts the display under LVGL,
// reads touch / keyboard / trackball, drives the backlight. Runs on MeshCore's main loop only: the display shares
// its SPI bus with the LoRa radio, and one thread means the two never talk over each other. MIT licence.
#pragma once
#include <cstdint>

namespace hxhal
{
bool begin(void);              // display + LVGL display + inputs; false if the panel did not start
void splash(void);             // panel + backlight + a "starting up" screen, before LVGL (safe to call first)
void splashError(const char *what, const char *todo); // same screen with an error, when MeshCore stops
void setBacklight(uint8_t v);  // 0 = off, 255 = full
uint32_t readKey(void);        // 0 = nothing; a character, or an hx::Key
bool takeTouched(void);        // true once after any touch (for "wake / hide the key ring")
const char *boardName(void);
bool hasTouch(void);
bool hasKeyboard(void);        // false: the screens bring up an on-screen keyboard for typing
bool hasKeyLight(void);        // a keyboard backlight the firmware can set (T-Deck)
void setKeyLight(uint8_t v);   // 0 = off, 255 = full
uint32_t framesDrawn(void);    // screen redraws finished since start, for the status line (hx_lvgl.cpp)
void refreshTimes(uint32_t &refrUs, uint32_t &drawUs); // microseconds in LVGL's refreshes (layout + drawing), and drawing
} // namespace hxhal
