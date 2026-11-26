// Hoptics-MC message sounds (see hx_sound.h). MIT licence.
#include "hx_sound.h"
#include "hx_log.h"

#if defined(ARDUINO_ARCH_ESP32) && defined(HOPTICS_UI)
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <math.h>

// the sound hardware of each board (the same pins Meshtastic's board files use)
#if defined(LILYGO_TDECK)
#define HX_SPEAKER 1 // MAX98357A on I2S
#include <driver/i2s.h>
constexpr int I2S_BCK = 7, I2S_WS = 5, I2S_DOUT = 6;
#elif defined(THINKNODE_M9) || defined(HELTEC_V4_R8_TFT)
constexpr int BUZZER = PIN_BUZZER; // the board file's: 9 on the M9, 4 on the V4-R8 kit
#elif defined(HELTEC_LORA_V4_TFT)
constexpr int BUZZER = 6;
#else
constexpr int BUZZER = -1; // no sound on this board
#endif

namespace hxsound
{
namespace
{
struct Note {
    uint16_t hz, ms; // hz 0: a pause
};
// short and not shrill; each ends with silence so two in a row stay apart
const Note CHIRP_N[] = {{1800, 40}, {2600, 70}, {0, 0}};
const Note CHIME_N[] = {{1319, 110}, {1568, 110}, {2093, 220}, {0, 0}};
const Note PING_N[] = {{2400, 90}, {0, 0}};
const Note *const SONGS[TONE_COUNT] = {nullptr, CHIRP_N, CHIME_N, PING_N};

struct Req {
    uint8_t tone, vol;
};
QueueHandle_t q = nullptr;

#if defined(HX_SPEAKER)
constexpr int RATE = 16000;
bool i2sUp = false;

void startI2s()
{
    if (i2sUp)
        return;
    i2s_config_t c = {};
    c.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    c.sample_rate = RATE;
    c.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    c.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    c.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    c.dma_buf_count = 4;
    c.dma_buf_len = 256;
    c.tx_desc_auto_clear = true; // silence, not the last samples again, when nothing is written
    i2s_pin_config_t p = {};
    p.bck_io_num = I2S_BCK;
    p.ws_io_num = I2S_WS;
    p.data_out_num = I2S_DOUT;
    p.data_in_num = I2S_PIN_NO_CHANGE;
    p.mck_io_num = I2S_PIN_NO_CHANGE;
    const esp_err_t a = i2s_driver_install(I2S_NUM_0, &c, 0, nullptr), b = a == ESP_OK ? i2s_set_pin(I2S_NUM_0, &p) : a;
    i2sUp = b == ESP_OK;
    hxlog::line(i2sUp ? "sound: speaker ready" : "sound: the speaker didn't start (error %d)", (int)b);
}

// one note as a sine, faded in and out over 3 ms so it doesn't click
void sound(const Note &n, uint8_t vol)
{
    const float amp = 30000.0f * (vol / 100.0f) * (vol / 100.0f); // loudness feels square-law
    const int total = RATE * n.ms / 1000, fade = RATE * 3 / 1000;
    int16_t buf[2 * 128];
    float ph = 0;
    const float step = 2.0f * (float)M_PI * n.hz / RATE;
    for (int i = 0; i < total;) {
        int k = 0;
        for (; k < 128 && i < total; k++, i++) {
            float g = 1;
            if (i < fade)
                g = (float)i / fade;
            else if (total - i < fade)
                g = (float)(total - i) / fade;
            const int16_t s = n.hz ? (int16_t)(amp * g * sinf(ph)) : 0;
            ph += step;
            if (ph > 2.0f * (float)M_PI)
                ph -= 2.0f * (float)M_PI;
            buf[2 * k] = buf[2 * k + 1] = s; // both channels: the amplifier mixes them
        }
        size_t w;
        i2s_write(I2S_NUM_0, buf, k * 4, &w, portMAX_DELAY);
    }
}
void quiet() { i2s_zero_dma_buffer(I2S_NUM_0); }
#else
constexpr int CH = 4; // LEDC channels 1 and 2 drive the backlights; 4 has a timer of its own
bool pwmUp = false;

// a piezo is loudest at 50 % duty and quieter as the pulses get narrower
void sound(const Note &n, uint8_t vol)
{
    if (!pwmUp) {
        ledcSetup(CH, 2000, 10);
        ledcAttachPin(BUZZER, CH);
        pwmUp = true;
    }
    if (n.hz) {
        ledcChangeFrequency(CH, n.hz, 10);
        ledcWrite(CH, (uint32_t)(512.0f * (vol / 100.0f) * (vol / 100.0f)) + (vol ? 1 : 0));
    } else
        ledcWrite(CH, 0);
    vTaskDelay(pdMS_TO_TICKS(n.ms));
}
void quiet() { ledcWrite(CH, 0); } // low: a buzzer left on draws current
#endif

void task(void *)
{
    Req r;
    for (;;) {
        if (xQueueReceive(q, &r, portMAX_DELAY) != pdTRUE)
            continue;
        const Note *s = r.tone < TONE_COUNT ? SONGS[r.tone] : nullptr;
        for (; s && s->ms; s++) {
            if (uxQueueMessagesWaiting(q)) // a newer sound replaces this one
                break;
            sound(*s, r.vol);
        }
        sound({0, 30}, 0);
        quiet();
    }
}
} // namespace

void begin()
{
#if !defined(HX_SPEAKER)
    if (BUZZER >= 0) { // as MeshCore's own buzzer code does: low until a sound plays
        pinMode(BUZZER, OUTPUT);
        digitalWrite(BUZZER, LOW);
    }
#endif
}

bool present()
{
#if defined(HX_SPEAKER)
    return true;
#else
    return BUZZER >= 0;
#endif
}

void play(uint8_t tone, uint8_t vol)
{
    if (tone == OFF || tone >= TONE_COUNT || vol == 0 || !present())
        return;
    if (!q) {
        q = xQueueCreate(2, sizeof(Req));
#if defined(HX_SPEAKER)
        startI2s();
        if (!i2sUp)
            return;
#endif
        xTaskCreatePinnedToCore(task, "hoptics-sound", 3072, nullptr, 1, nullptr, tskNO_AFFINITY);
    }
    const Req r{tone, vol > 100 ? (uint8_t)100 : vol};
    xQueueSend(q, &r, 0); // full: this one is dropped (two are already waiting)
}
} // namespace hxsound

#else // a PC, or a board without sound

namespace hxsound
{
void begin() {}
bool present() { return false; }
void play(uint8_t, uint8_t) {}
} // namespace hxsound

#endif
