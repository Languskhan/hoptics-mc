// Hoptics-MC hardware layer: LilyGO T-Deck / T-Deck Plus. MIT licence.
// 2.8" ST7789 320x240 on the radio's SPI bus (HSPI, shared), GT911 touch and the keyboard's ESP32-C3 on I2C
// (SDA 18, SCL 8, already started by radio_init()), trackball on GPIO 1/2/3/15 with its click on GPIO 0.
#if defined(LILYGO_TDECK) && defined(HOPTICS_UI)
#include "hx_log.h"
#include "hx_hal.h"
#include "hx_lvgl.h"
#include "hx_ui.h"
#include <Arduino.h>
#include <Wire.h>
#include <algorithm>

namespace hxhal
{
namespace
{
constexpr int PIN_BL = 42, PIN_TOUCH_INT = 16;
constexpr int TB_UP = 3, TB_DOWN = 15, TB_LEFT = 1, TB_RIGHT = 2, TB_CLICK = 0;
constexpr uint8_t KB_ADDR = 0x55, GT911_ADDR = 0x5D;

class Panel : public lgfx::LGFX_Device
{
    lgfx::Panel_ST7789 panel;
    lgfx::Bus_SPI bus;

  public:
    Panel()
    {
        auto b = bus.config();
        b.spi_host = SPI2_HOST; // the host MeshCore's radio uses (its SPIClass is FSPI = SPI2): one owner of pins 38/40/41
        b.spi_mode = 0;
        b.freq_write = 40000000;
        b.freq_read = 16000000;
        b.spi_3wire = false;
        b.use_lock = false; // one thread: the main loop
        b.dma_channel = 0;  // no DMA: the radio driver touches the same host between frames
        b.pin_sclk = 40;
        b.pin_mosi = 41;
        b.pin_miso = 38;
        b.pin_dc = 11;
        bus.config(b);
        panel.setBus(&bus);
        auto p = panel.config();
        p.pin_cs = 12;
        p.pin_rst = -1;
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
lv_indev_t *touchDev = nullptr;
volatile int tbCount[4] = {0, 0, 0, 0};
bool touchedFlag = false, haveTouch = false;
int clickState = 1;
uint32_t clickAt = 0;
uint8_t blLevel = 0; // 0..16 steps of the backlight driver
bool panelUp = false;

bool panelInit(void)
{
    if (panelUp)
        return true;
    // everything on the shared SPI bus deselected first (LoRa 9, microSD 39, screen 12), then peripheral power on and
    // a moment for the screen and the keyboard's controller to wake: MeshCore powers them up just before this
    for (int cs : {9, 39, 12}) {
        pinMode(cs, OUTPUT);
        digitalWrite(cs, HIGH);
    }
    pinMode(10, OUTPUT);
    digitalWrite(10, HIGH);
    delay(300);
    hxlog::line("display init (T-Deck)");
    if (!tft.init()) {
        hxlog::line("display did not start");
        return false;
    }
    tft.setRotation(1); // landscape, keyboard below
    panelUp = true;
    hxlog::line("display up");
    return true;
}

void IRAM_ATTR tbUp() { tbCount[0]++; }
void IRAM_ATTR tbDown() { tbCount[1]++; }
void IRAM_ATTR tbLeft() { tbCount[2]++; }
void IRAM_ATTR tbRight() { tbCount[3]++; }

bool gtRead(uint16_t reg, uint8_t *buf, uint8_t n)
{
    Wire.beginTransmission(GT911_ADDR);
    Wire.write(reg >> 8);
    Wire.write(reg & 0xFF);
    if (Wire.endTransmission() != 0)
        return false;
    return Wire.requestFrom(GT911_ADDR, n) == n && Wire.readBytes(buf, n) == n;
}
void gtWrite(uint16_t reg, uint8_t v)
{
    Wire.beginTransmission(GT911_ADDR);
    Wire.write(reg >> 8);
    Wire.write(reg & 0xFF);
    Wire.write(v);
    Wire.endTransmission();
}

void touchRead(lv_indev_t *, lv_indev_data_t *data)
{
    static int16_t lx = 0, ly = 0;
    static bool down = false;
    static uint32_t last = 0;
    // the GT911 updates ~100 times a second. Between its updates, and when it has nothing new, the last answer stands:
    // saying "released" then would break a finger's drag into separate taps
    if (millis() - last >= 10) {
        last = millis();
        uint8_t st, p[4];
        if (gtRead(0x814E, &st, 1) && (st & 0x80)) { // a new report
            if (!(st & 0x0F))
                down = false; // no finger
            else if (gtRead(0x8150, p, 4)) {
                int16_t rx = p[0] | (p[1] << 8), ry = p[2] | (p[3] << 8);
                // the touch panel reports in the portrait frame; the screen is landscape (rotation 1)
                lx = std::min<int16_t>(std::max<int16_t>(ry, 0), 319);
                ly = std::min<int16_t>(std::max<int16_t>(239 - rx, 0), 239);
                down = true;
                touchedFlag = true;
            }
            gtWrite(0x814E, 0);
        }
    }
    data->point.x = lx;
    data->point.y = ly;
    data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
} // namespace

const char *boardName(void) { return "LilyGO T-Deck Plus"; }
bool hasTouch(void) { return haveTouch; }
bool hasKeyboard(void) { return true; }

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

    // touch (GT911): present on every T-Deck; probe so a broken one doesn't stall the bus
    pinMode(PIN_TOUCH_INT, INPUT);
    uint8_t id[4];
    haveTouch = gtRead(0x8140, id, 4);
    if (haveTouch) {
        touchDev = lv_indev_create();
        lv_indev_set_type(touchDev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(touchDev, touchRead);
    }
    // trackball: every edge on a direction pin is one step
    for (int p : {TB_UP, TB_DOWN, TB_LEFT, TB_RIGHT})
        pinMode(p, INPUT_PULLUP);
    pinMode(TB_CLICK, INPUT_PULLUP);
    attachInterrupt(TB_UP, tbUp, CHANGE);
    attachInterrupt(TB_DOWN, tbDown, CHANGE);
    attachInterrupt(TB_LEFT, tbLeft, CHANGE);
    attachInterrupt(TB_RIGHT, tbRight, CHANGE);
    setBacklight(200);
    return true;
}

// Backlight: PWM on its enable pin, as Meshtastic's T-Deck driver does (44.1 kHz, 8 bits).
void setBacklight(uint8_t v)
{
    static bool attached = false;
    if (!attached) {
        ledcSetup(2, 44100, 8);
        ledcAttachPin(PIN_BL, 2);
        attached = true;
    }
    ledcWrite(2, v);
    blLevel = v;
}

// Keyboard backlight: the keyboard's own controller (an ESP32-C3, LilyGO firmware from December 2024 on) takes
// command 0x01 and a level. Older keyboard firmware ignores it; Alt+B on the keyboard toggles the light either way.
bool hasKeyLight(void) { return true; }
void setKeyLight(uint8_t v)
{
    Wire.beginTransmission(KB_ADDR);
    Wire.write(0x01);
    Wire.write(v);
    Wire.endTransmission();
}

bool takeTouched(void)
{
    bool t = touchedFlag;
    touchedFlag = false;
    return t;
}

uint32_t readKey(void)
{
    // trackball: two edges make one step (each detent toggles the line)
    static const uint32_t dirKey[4] = {hx::K_UP, hx::K_DOWN, hx::K_LEFT, hx::K_RIGHT};
    for (int i = 0; i < 4; i++) {
        if (tbCount[i] >= 2) {
            noInterrupts();
            tbCount[i] -= 2;
            interrupts();
            for (int j = 0; j < 4; j++)
                if (j != i)
                    tbCount[j] = 0; // a diagonal roll counts once, in its main direction
            return dirKey[i];
        }
    }
    // trackball click (active low, debounced)
    int c = digitalRead(TB_CLICK);
    if (c != clickState && millis() - clickAt > 40) {
        clickState = c;
        clickAt = millis();
        if (c == LOW)
            return hx::K_ENTER;
    }
    // keyboard: its controller answers one byte per request, 0 when nothing is waiting
    static uint32_t lastKb = 0;
    if (millis() - lastKb >= 10) {
        lastKb = millis();
        if (Wire.requestFrom(KB_ADDR, (uint8_t)1) == 1) {
            uint8_t k = Wire.read();
            if (k == 0x08)
                return hx::K_BKSP;
            if (k == 0x0D)
                return hx::K_ENTER;
            if (k >= 0x20 && k < 0x7F)
                return k;
        }
    }
    return 0;
}
} // namespace hxhal
#endif
