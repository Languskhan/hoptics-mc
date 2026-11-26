// Hoptics-MC hardware layer: Elecrow ThinkNode M9. MIT licence.
// 2.4" ST7789 240x320 shown in landscape, on the one SPI bus it shares with the LR1110 and the microSD (FSPI).
// No touch: a keyboard + d-pad on a companion MCU (STC8H) on its own I2C bus (Wire1, SDA 20 / SCL 21, 0x6C or 0x6D),
// with a key-ready interrupt on GPIO 12. Backlight GPIO 17 and the peripheral rail GPIO 18 are active-low.
#if defined(THINKNODE_M9) && defined(HOPTICS_UI)
#include "hx_log.h"
#include "hx_hal.h"
#include "hx_lvgl.h"
#include "hx_ui.h"
#include <Arduino.h>
#include <Wire.h>

namespace hxhal
{
namespace
{
constexpr int PIN_BL = 17, PIN_KB_INT = 12, PIN_KB_SDA = 20, PIN_KB_SCL = 21, PIN_KB_LED = 46;

class Panel : public lgfx::LGFX_Device
{
    lgfx::Panel_ST7789 panel;
    lgfx::Bus_SPI bus;

  public:
    Panel()
    {
        auto b = bus.config();
        b.spi_host = SPI2_HOST; // the global SPI (FSPI) the LR1110 uses: one owner of pins 38/40/47
        b.spi_mode = 0;
        b.freq_write = 40000000;
        b.freq_read = 16000000;
        b.spi_3wire = false;
        b.use_lock = false;
        b.dma_channel = 0;
        b.pin_sclk = 40;
        b.pin_mosi = 47;
        b.pin_miso = 38;
        b.pin_dc = 15;
        bus.config(b);
        panel.setBus(&bus);
        auto p = panel.config();
        p.pin_cs = 16;
        p.pin_rst = 14;
        p.pin_busy = -1;
        p.panel_width = 240;
        p.panel_height = 320;
        p.offset_rotation = 0;
        p.readable = false;
        p.invert = true;
        p.rgb_order = false;
        p.dlen_16bit = false;
        p.bus_shared = true;
        panel.config(p);
        setPanel(&panel);
    }
};
Panel tft;
uint8_t kbAddr = 0;
bool panelUp = false;

bool panelInit(void)
{
    if (panelUp)
        return true;
    pinMode(PIN_BL, OUTPUT);
    digitalWrite(PIN_BL, HIGH); // off until something is drawn
    if (!tft.init()) {
        hxlog::line("display did not start");
        return false;
    }
    tft.setRotation(3); // landscape, keyboard below (1 is upside down on the M9)
    panelUp = true;
    hxlog::line("display up");
    return true;
}
volatile bool kbEvent = false;
void IRAM_ATTR kbIsr() { kbEvent = true; }

// the keypad has its own controller (an STC8H) that may still be starting: look again until it answers
void kbProbe(void)
{
    static uint32_t last = 0;
    static int tries = 0;
    if (kbAddr || (last && millis() - last < 2000))
        return;
    last = millis();
    uint8_t res[2];
    int i = 0;
    for (uint8_t a : {(uint8_t)0x6C, (uint8_t)0x6D}) { // board revisions 1.0 and 1.1
        Wire1.beginTransmission(a);
        res[i] = Wire1.endTransmission();
        if (res[i] == 0) {
            kbAddr = a;
            hxlog::line("keyboard at 0x%02X", a);
            return;
        }
        i++;
    }
    // bring-up: say how it failed (2 = no answer, 5 = timeout/bus held) for the first few tries
    if (tries++ < 3)
        hxlog::line("keyboard not answering on I2C 20/21 (0x6C: %u, 0x6D: %u), will keep trying", res[0],
                      res[1]);
}

uint8_t kbReadReg(uint8_t reg)
{
    Wire1.beginTransmission(kbAddr);
    Wire1.write(reg);
    if (Wire1.endTransmission(false) != 0)
        return 0xFF;
    if (Wire1.requestFrom(kbAddr, (uint8_t)1) != 1)
        return 0xFF;
    return Wire1.read();
}
} // namespace

const char *boardName(void) { return "Elecrow ThinkNode M9"; }
bool hasTouch(void) { return false; }
bool hasKeyboard(void) { return true; }
bool hasKeyLight(void) { return false; }
void setKeyLight(uint8_t) {}

void splash(void)
{
    if (!panelInit())
        return;
    showBoot(tft, boardName());
    setBacklight(200);
}

void splashError(const char *what, const char *todo)
{
    if (!panelInit())
        return;
    showBootError(tft, boardName(), what, todo);
    setBacklight(200);
}

bool begin(void)
{
    if (!panelInit())
        return false;
    lvglAttach(tft);

    // the board's begin() already freed GPIO 20 from the USB pad and started this bus to read the board revision
    // (ThinkNodeM9Board.cpp); this is a no-op then, and a fallback otherwise. 100 kHz, as Meshtastic runs the keypad.
    Wire1.begin(PIN_KB_SDA, PIN_KB_SCL, 100000);
    kbProbe();
    pinMode(PIN_KB_INT, INPUT);
    attachInterrupt(PIN_KB_INT, kbIsr, RISING);
    pinMode(PIN_KB_LED, OUTPUT);
    digitalWrite(PIN_KB_LED, LOW);
    setBacklight(200);
    return true;
}

// The backlight is switched by a PNP transistor (on = low); PWM still dims it, inverted.
void setBacklight(uint8_t v)
{
    static bool attached = false;
    if (!attached) {
        ledcSetup(1, 5000, 8);
        ledcAttachPin(PIN_BL, 1);
        attached = true;
    }
    ledcWrite(1, 255 - v);
}

bool takeTouched(void) { return false; }

uint32_t readKey(void)
{
    if (!kbAddr) {
        kbProbe();
        return 0;
    }
    // the controller raises its interrupt line for each press; also poll now and then in case an edge was missed
    static uint32_t lastPoll = 0;
    if (!kbEvent && millis() - lastPoll < 150)
        return 0;
    lastPoll = millis();
    kbEvent = false;
    uint8_t c = kbReadReg(0x01);
    switch (c) {
    case 0x00:
    case 0xFF:
    case 0x88: // "no key"
        return 0;
    case 0xB5:
        return hx::K_UP;
    case 0xB6:
        return hx::K_DOWN;
    case 0xB4:
        return hx::K_LEFT;
    case 0xB7:
        return hx::K_RIGHT;
    case 0x0D:
        return hx::K_ENTER;
    case 0x08:
        return hx::K_BKSP;
    case 0xA3: // Enter, held
        return hx::K_ENTER;
    // the keys above the keyboard (the TM9 controller's codes, as in Elecrow's own firmware)
    case 0x81: // message bubbles
        return hx::K_GOTO + hx::App::T_CHATS;
    case 0x82: // house
        return hx::K_HOME;
    case 0x83: // second bubble (preset message)
        return hx::K_QUICK;
    case 0x84: // location
        return hx::K_ADVERT;
    case 0x87: // location, held
        return hx::K_ADVERT_FLOOD;
    case 0x85: // map
        return hx::K_GOTO + hx::App::T_MAP;
    case 0x86: // back
        return hx::K_BACK;
    default:
        return (c >= 0x20 && c < 0x7F) ? c : 0;
    }
}
} // namespace hxhal
#endif
