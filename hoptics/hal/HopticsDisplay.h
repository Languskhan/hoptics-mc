// Hoptics-MC: MeshCore's examples expect a DisplayDriver (DISPLAY_CLASS) for their simple text screens. Hoptics draws
// everything itself through LVGL, so this one only says "a display is here" and ignores the drawing calls. Force-
// included by the build (-include) so the variants' target.cpp can declare `DISPLAY_CLASS display;`. MIT licence.
#pragma once
// only in MeshCore's own sources (C files and libraries see nothing)
#if defined(__cplusplus) && __has_include(<helpers/ui/DisplayDriver.h>)
#include <helpers/ui/DisplayDriver.h>

// The display class is set here, not by build flags: PlatformIO hands -U and -D to the compiler in its own order, and
// "-U DISPLAY_CLASS -D DISPLAY_CLASS=HopticsDisplay" ended with no DISPLAY_CLASS at all, so MeshCore built without a
// display and the Hoptics UI never started. A force-included header runs after every command-line macro.
#undef DISPLAY_CLASS
#define DISPLAY_CLASS HopticsDisplay

namespace hxhal
{
void splash(void); // the "starting up" screen, shown before storage and radio start (hx_hal_<board>.cpp)
void splashError(const char *what, const char *todo); // MeshCore stopped (halt): say why instead of freezing
void bootReport(void);   // first thing in setup(): why the last start ended, if it stopped part-way (hx_boot.cpp)
void bootStage(int step); // each start-up step as it finishes, in the serial log and kept across a reset
}

class HopticsDisplay : public DisplayDriver
{
    bool on = true;

  public:
    HopticsDisplay() : DisplayDriver(320, 240) {}
    explicit HopticsDisplay(void *peripheralPower) : HopticsDisplay() { (void)peripheralPower; } // Heltec target.cpp
    bool begin()
    {
        hxhal::splash(); // the first start after installing formats storage: say so instead of a dark screen
        return true;
    }
    bool isOn() override { return on; }
    void turnOn() override { on = true; }
    void turnOff() override { on = false; }
    void clear() override {}
    void startFrame(ColorVal) override {}
    void setTextSize(int) override {}
    void setColor(ColorVal) override {}
    void setCursor(int, int) override {}
    void print(const char *) override {}
    void fillRect(int, int, int, int) override {}
    void drawRect(int, int, int, int) override {}
    void drawXbm(int, int, const uint8_t *, int, int) override {}
    uint16_t getTextWidth(const char *) override { return 0; }
    void endFrame() override {}
};
#endif
