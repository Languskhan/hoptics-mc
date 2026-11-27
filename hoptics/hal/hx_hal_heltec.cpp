// Hoptics-MC hardware layer: Heltec V4 Expansion Kit (V4 + 2.8" touch screen) and V4-R8 Expansion Kit V2. MIT licence.
// ST7789 240x320 in portrait on its own SPI host (the radio has SPI3: the variant's target.cpp makes its SPIClass on
// HSPI), CHSC6x touch controller at 0x2E (V4: its own I2C on 47/48, reset on 44; R8: the board's I2C on 17/18, which
// MeshCore has started), the user button on GPIO 0. Pins as in Meshtastic's variants for these kits, where Hoptics MT
// runs on the same hardware. No keyboard: the screens bring up an on-screen one.
#if (defined(HELTEC_LORA_V4_TFT) || defined(HELTEC_V4_R8_TFT)) && defined(HOPTICS_UI)
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
#ifdef HELTEC_V4_R8_TFT
constexpr int SCK = 16, MOSI = 15, MISO = 45, DC = 48, CS = 47, RST = 21, BL = 44, SD_CS = 3;
constexpr int T_SDA = 17, T_SCL = 18, T_RST = -1;
constexpr bool THREE_WIRE = false;
const char *const NAME = "Heltec V4-R8 Expansion Kit V2";
TwoWire &touchBus = Wire; // the board's I2C, started by MeshCore
constexpr bool OWN_TOUCH_BUS = false;
#else
constexpr int SCK = 17, MOSI = 33, MISO = -1, DC = 16, CS = 15, RST = 18, BL = 21, SD_CS = -1;
constexpr int T_SDA = 47, T_SCL = 48, T_RST = 44;
constexpr bool THREE_WIRE = true; // one data line, as on the kit
const char *const NAME = "Heltec V4 Expansion Kit";
TwoWire &touchBus = Wire1; // the touch controller's own pins
constexpr bool OWN_TOUCH_BUS = true;
#endif
constexpr int PIN_BTN = 0;
constexpr uint8_t CHSC6X = 0x2E;

// the kits' ST7789 looks right with gamma curve 1 (as Meshtastic sets it)
class PanelST7789 : public lgfx::Panel_ST7789
{
  protected:
    const uint8_t *getInitCommands(uint8_t listno) const override
    {
        static const uint8_t list[] = {0x26 /* GAMMASET */, 1, 0x01, 0xFF, 0xFF};
        return listno == 1 ? list : lgfx::Panel_ST7789::getInitCommands(listno);
    }
};

class Panel : public lgfx::LGFX_Device
{
    PanelST7789 panel;
    lgfx::Bus_SPI bus;

  public:
    Panel()
    {
        auto b = bus.config();
        b.spi_host = SPI2_HOST; // its own host: the radio is on SPI3
        b.spi_mode = 0;
        b.freq_write = 40000000;
        b.freq_read = 16000000;
        b.spi_3wire = THREE_WIRE;
        b.use_lock = false; // one thread: the main loop
        b.dma_channel = SPI_DMA_CH_AUTO;
        b.pin_sclk = SCK;
        b.pin_mosi = MOSI;
        b.pin_miso = MISO;
        b.pin_dc = DC;
        bus.config(b);
        panel.setBus(&bus);
        auto p = panel.config();
        p.pin_cs = CS;
        p.pin_rst = RST;
        p.pin_busy = -1;
        p.panel_width = 240;
        p.panel_height = 320;
        p.offset_rotation = 0;
        p.readable = false;
        p.invert = true;
        p.rgb_order = false;
        p.dlen_16bit = false;
        p.bus_shared = SD_CS >= 0; // R8: the microSD card sits on the same lines
        panel.config(p);
        setPanel(&panel);
    }
};
Panel tft;
bool panelUp = false, touchedFlag = false;
int btnState = HIGH;
uint32_t btnAt = 0;

bool panelInit(void)
{
    if (panelUp)
        return true;
    // MeshCore runs these boards at 80 MHz to save power; the screens need the full 240
    setCpuFrequencyMhz(240);
    if (SD_CS >= 0) { // the card stays deselected while the screen talks
        pinMode(SD_CS, OUTPUT);
        digitalWrite(SD_CS, HIGH);
    }
    hxlog::line("display init (%s)", NAME);
    if (!tft.init()) {
        hxlog::line("display did not start");
        return false;
    }
    tft.setRotation(0); // portrait
    lvglUseDma();       // the screen has this SPI host to itself: strips go out by DMA while the next is drawn
    panelUp = true;
    hxlog::line("display up");
    return true;
}

// CHSC6x: register 0 holds the touch report; byte 2 = points, bytes 3-6 = the first point (4-bit high, 8-bit low).
// 1: a finger (x, y set), 0: no finger, -1: the read failed (says nothing either way)
int touchPoint(int16_t &x, int16_t &y)
{
    touchBus.beginTransmission(CHSC6X);
    touchBus.write(0x00);
    if (touchBus.endTransmission(false) != 0)
        return -1;
    uint8_t b[16];
    if (touchBus.requestFrom(CHSC6X, (uint8_t)sizeof(b)) != sizeof(b) || touchBus.readBytes(b, sizeof(b)) != sizeof(b))
        return -1;
    if ((b[2] & 0x07) == 0)
        return 0;
    if ((b[3] & 0xC0) == 0xC0)
        return -1; // not a valid point this time
    x = (int16_t)(((b[3] & 0x0F) << 8) | b[4]);
    y = (int16_t)(((b[5] & 0x0F) << 8) | b[6]);
    return x < 240 && y < 320 ? 1 : -1;
}

// A press ends only after two empty reports in a row (~30 ms): a single missed sample in the middle of a press
// (seen on the V4 kit, whose touch chip has its own I2C pins) made LVGL see a release and a new press, so one tap
// could click a button several times. A failed read keeps the state as it was. Small wobbles of the finger
// (2 px) are held still, so a tap isn't taken for the start of a scroll.
void touchRead(lv_indev_t *, lv_indev_data_t *data)
{
    static int16_t lx = 0, ly = 0;
    static uint32_t last = 0;
    static bool down = false;
    static uint8_t misses = 0;
    if (millis() - last >= 15) { // don't hog the I2C bus between its updates
        last = millis();
        int16_t x, y;
        const int r = touchPoint(x, y);
        if (r == 1) {
            misses = 0;
            if (!down || abs(x - lx) > 2 || abs(y - ly) > 2)
                lx = x, ly = y;
            down = true;
            touchedFlag = true;
        } else if (r == 0 && down && ++misses >= 2)
            down = false;
    }
    data->point.x = lx;
    data->point.y = ly;
    data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
} // namespace

const char *boardName(void) { return NAME; }
bool hasTouch(void) { return true; }
bool hasKeyboard(void) { return false; }
bool hasKeyLight(void) { return false; }
void setKeyLight(uint8_t) {}

void splash(void)
{
    if (!panelInit())
        return;
    showBoot(tft, NAME);
    setBacklight(200);
}

void splashError(const char *what, const char *todo)
{
    if (!panelInit())
        return;
    showBootError(tft, NAME, what, todo);
    setBacklight(200);
}

bool begin(void)
{
    if (!panelInit())
        return false;
    lvglAttach(tft);
    // touch: its reset, then its bus
    if (T_RST >= 0) {
        pinMode(T_RST, OUTPUT);
        digitalWrite(T_RST, LOW);
        delay(20);
        digitalWrite(T_RST, HIGH);
        delay(30);
    }
    if (OWN_TOUCH_BUS)
        touchBus.begin(T_SDA, T_SCL, 400000);
    touchBus.beginTransmission(CHSC6X); // for reports: is the touch controller there?
    hxlog::line("touch %s", touchBus.endTransmission() == 0 ? "answers" : "did not answer");
    lv_indev_t *t = lv_indev_create();
    lv_indev_set_type(t, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(t, touchRead);
    pinMode(PIN_BTN, INPUT_PULLUP);
    setBacklight(200);
    return true;
}

void setBacklight(uint8_t v)
{
    static bool attached = false;
    if (!attached) {
        ledcSetup(2, 44100, 8);
        ledcAttachPin(BL, 2);
        attached = true;
    }
    ledcWrite(2, v);
}

bool takeTouched(void)
{
    bool t = touchedFlag;
    touchedFlag = false;
    return t;
}

// the user button (the only key): Back
uint32_t readKey(void)
{
    int b = digitalRead(PIN_BTN);
    if (b != btnState && millis() - btnAt > 40) {
        btnState = b;
        btnAt = millis();
        if (b == LOW)
            return hx::K_BACK;
    }
    return 0;
}
} // namespace hxhal
#endif
