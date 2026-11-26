// Live feed from a CoreScope server (see hx_feed.h). The JSON is not parsed in full: only the few fields used are
// picked out, and anything that doesn't look right is dropped. The WebSocket client is the minimum the feed needs:
// text messages in, pings answered, nothing else sent.
#include "hx_log.h"
#include "hx_feed.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

namespace hxfeed
{

namespace
{
int hexVal(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// the text after "key": (skipping spaces), or nullptr
const char *field(const char *s, const char *end, const char *key)
{
    size_t kn = strlen(key);
    for (const char *p = s; p + kn + 3 <= end; p++) {
        if (*p != '"' || memcmp(p + 1, key, kn) != 0 || p[kn + 1] != '"')
            continue;
        const char *q = p + kn + 2;
        while (q < end && (*q == ' ' || *q == ':'))
            q++;
        return q < end ? q : nullptr;
    }
    return nullptr;
}

// a JSON string value into out (escapes kept simple: \x -> x, \uXXXX -> '?'); false when it is not a string
bool str(const char *v, const char *end, char *out, size_t n)
{
    if (!v || *v != '"')
        return false;
    size_t o = 0;
    for (const char *p = v + 1; p < end; p++) {
        char c = *p;
        if (c == '"') {
            out[o] = 0;
            return true;
        }
        if (c == '\\' && p + 1 < end) {
            c = *++p;
            if (c == 'u') {
                p += 4;
                c = '?';
            }
        }
        if (o + 1 < n)
            out[o++] = c;
    }
    return false;
}
} // namespace

bool parse(const char *json, size_t n, Item &out)
{
    const char *end = json + n;
    const char *v = field(json, end, "raw_hex");
    if (!v || *v != '"')
        return false;
    out.len = 0;
    for (const char *p = v + 1; p + 1 < end && *p != '"'; p += 2) {
        int a = hexVal(p[0]), b = hexVal(p[1]);
        if (a < 0 || b < 0 || out.len == sizeof(out.raw))
            return false;
        out.raw[out.len++] = (uint8_t)(a << 4 | b);
    }
    if (out.len < 2)
        return false;
    memset(out.obsKey, 0, sizeof(out.obsKey));
    char id[16];
    if (str(field(json, end, "observer_id"), end, id, sizeof(id)) && strlen(id) >= 12)
        for (int i = 0; i < 6; i++)
            out.obsKey[i] = (uint8_t)(hexVal(id[2 * i]) << 4 | hexVal(id[2 * i + 1]));
    out.obsName[0] = 0;
    str(field(json, end, "observer_name"), end, out.obsName, sizeof(out.obsName));
    v = field(json, end, "snr");
    out.snr4 = (v && *v != 'n') ? (int8_t)(atof(v) * 4) : 0;
    return true;
}

bool splitUrl(const char *url, bool &tls, char *host, size_t hostN, uint16_t &port, char *path, size_t pathN)
{
    while (*url == ' ')
        url++;
    tls = true;
    if (!strncmp(url, "wss://", 6) || !strncmp(url, "https://", 8))
        url = strchr(url, '/') + 2;
    else if (!strncmp(url, "ws://", 5) || !strncmp(url, "http://", 7)) {
        url = strchr(url, '/') + 2;
        tls = false;
    }
    size_t h = strcspn(url, ":/ ");
    if (!h || h >= hostN)
        return false;
    memcpy(host, url, h);
    host[h] = 0;
    url += h;
    port = tls ? 443 : 80;
    if (*url == ':') {
        long p = strtol(url + 1, (char **)&url, 10);
        if (p <= 0 || p > 65535)
            return false;
        port = (uint16_t)p;
    }
    if (*url != '/')
        url = "/ws"; // CoreScope's live feed
    size_t pn = strcspn(url, " ");
    if (pn >= pathN)
        return false;
    memcpy(path, url, pn);
    path[pn] = 0;
    return true;
}

} // namespace hxfeed

#if defined(ARDUINO_ARCH_ESP32)
#include "hx_feed_roots.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace hxfeed
{
namespace
{
constexpr int RING = 16;           // packets waiting for the main loop
constexpr size_t MSG_MAX = 12288;  // a feed message is a few KB; bigger ones are skipped
constexpr uint32_t SILENT_MS = 75000; // the server pings every 30 s: this long without a byte means the link is gone

SemaphoreHandle_t lock = nullptr;
Item *ring = nullptr;
int head = 0, count = 0;
char url[96] = "", ua[48] = "HopticsMC";
volatile bool want = false;
volatile uint32_t gen = 0; // bumps on every start()/stop(): the task drops its connection and starts over
volatile State st = State::OFF;
volatile uint32_t nPackets = 0;
char err[64] = "";
char moved[96] = ""; // where the server said it moved to (followed for this session)
const char *dropWhy = ""; // why the last connection ended (for the USB log)
int dropCode = 0;          // and the error code under it, when there is one
volatile uint32_t busy = 0; // microseconds spent reading, decrypting and picking packets out (not waiting)

// adds the time from here to the end of the scope to busy
struct Busy {
    uint32_t t = micros();
    ~Busy() { busy += micros() - t; }
};
bool memWait = false;      // the last try waited for memory: try again soon, without backing off

void setErr(const char *e)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    strncpy(err, e, sizeof(err) - 1);
    err[sizeof(err) - 1] = 0;
    xSemaphoreGive(lock);
}

void push(const Item &it)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (count == RING) { // the main loop is behind: the oldest goes
        head = (head + 1) % RING;
        count--;
    }
    ring[(head + count) % RING] = it;
    count++;
    xSemaphoreGive(lock);
    nPackets++;
}

// read exactly n bytes, or give up (timeout, closed, or asked to stop)
bool readN(Client &c, uint8_t *p, size_t n, uint32_t gen0)
{
    Busy b;
    uint32_t t0 = millis();
    while (n) {
        if (gen != gen0)
            return false;
        if (!c.connected()) {
            dropWhy = "the connection closed";
            return false;
        }
        int a = c.available();
        if (a < 0) { // the secure client's read failed: a is mbedTLS's error code
            dropCode = a;
            dropWhy = "a read failed";
            return false;
        }
        if (a == 0) {
            if (millis() - t0 > SILENT_MS) {
                dropWhy = "nothing heard for 75 s";
                return false;
            }
            busy += micros() - b.t; // the wait isn't work
            vTaskDelay(pdMS_TO_TICKS(20));
            b.t = micros();
            continue;
        }
        int got = c.read(p, n < (size_t)a ? n : (size_t)a);
        if (got <= 0) {
            dropWhy = "a read failed";
            return false;
        }
        p += got, n -= got;
        t0 = millis();
    }
    return true;
}

// a client frame (clients must mask what they send)
void sendFrame(Client &c, uint8_t op, const uint8_t *data, size_t n)
{
    uint8_t f[6 + 125];
    if (n > 125)
        n = 125;
    uint32_t mask = esp_random();
    f[0] = 0x80 | op;
    f[1] = 0x80 | (uint8_t)n;
    memcpy(f + 2, &mask, 4);
    for (size_t i = 0; i < n; i++)
        f[6 + i] = data[i] ^ f[2 + (i & 3)];
    c.write(f, 6 + n);
}

void base64(const uint8_t *in, int n, char *out)
{
    static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        uint32_t v = in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = t[v >> 18 & 63];
        out[o++] = t[v >> 12 & 63];
        out[o++] = i + 1 < n ? t[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[o] = 0;
}

// one connection, from the handshake until it drops or the settings change
void session(uint32_t gen0, uint8_t *msg)
{
    char u[96], host[64], path[64];
    xSemaphoreTake(lock, portMAX_DELAY);
    strcpy(u, moved[0] ? moved : url);
    xSemaphoreGive(lock);
    bool tls;
    uint16_t port;
    if (!splitUrl(u, tls, host, sizeof(host), port, path, sizeof(path))) {
        setErr("Server address not understood");
        st = State::FAILED;
        return;
    }
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 10 * 1024) { // Bluetooth and Wi-Fi come first
        setErr("Waiting for free memory");
        st = State::FAILED;
        memWait = true;
        return;
    }
    // the clients live on the heap: a TLS client is too big for this task's stack
    Client *c = tls ? (Client *)new WiFiClientSecure() : (Client *)new WiFiClient();
    if (tls) {
        static_cast<WiFiClientSecure *>(c)->setCACert(HOPTICS_FEED_ROOTS);
        static_cast<WiFiClientSecure *>(c)->setHandshakeTimeout(15);
    }
    st = State::CONNECTING;
    dropCode = 0;
    if (!c->connect(host, port)) {
        char e[64] = "Couldn't reach the server", m[48];
        if (tls && static_cast<WiFiClientSecure *>(c)->lastError(m, sizeof(m)) != 0) // mbedTLS says why (a certificate it doesn't trust...)
            snprintf(e, sizeof(e), "Secure link failed: %.40s", m);
        setErr(e);
        st = State::FAILED;
        delete c;
        return;
    }
    uint8_t key[16];
    char key64[28], req[320];
    esp_fill_random(key, sizeof(key));
    base64(key, sizeof(key), key64);
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\nUser-Agent: %s\r\n\r\n",
                     path, host, key64, ua);
    c->write((const uint8_t *)req, n);
    // the answer's headers, up to the blank line; the first must be "HTTP/1.1 101" (switching to WebSocket)
    char line[128];
    int ln = 0, lines = 0, code = 0;
    for (;;) {
        uint8_t b;
        if (!readN(*c, &b, 1, gen0)) {
            setErr("No answer from the server");
            st = State::FAILED;
            c->stop();
            delete c;
            return;
        }
        if (b == '\r')
            continue;
        if (b != '\n') {
            if (ln < (int)sizeof(line) - 1) // long header lines are cut short; only the first one is read
                line[ln++] = (char)b;
            continue;
        }
        line[ln] = 0;
        if (lines++ == 0)
            code = atoi(line + (ln > 9 ? 9 : ln));
        else if (code >= 301 && code <= 308 && !strncasecmp(line, "Location:", 9)) {
            const char *v = line + 9;
            while (*v == ' ')
                v++;
            snprintf(moved, sizeof(moved), "%s", v); // the task tries there next
        }
        if (!ln)
            break; // end of the headers
        ln = 0;
    }
    if (code != 101) {
        char e[64];
        snprintf(e, sizeof(e), "Server said: %d", code);
        setErr(e);
        st = State::FAILED;
        c->stop();
        delete c;
        return;
    }
    setErr("");
    st = State::LIVE;
    hxlog::line("feed live from %s, stack left %u, internal heap %u", host, (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    size_t have = 0;
    bool skip = false; // the message being read is too big: drop it
    while (gen == gen0 && want) {
        uint8_t h[2];
        if (!readN(*c, h, 2, gen0))
            break;
        uint8_t op = h[0] & 0x0F;
        bool fin = h[0] & 0x80;
        uint64_t len = h[1] & 0x7F;
        if (len == 126 || len == 127) {
            uint8_t e[8];
            int k = len == 126 ? 2 : 8;
            if (!readN(*c, e, k, gen0))
                break;
            len = 0;
            for (int i = 0; i < k; i++)
                len = len << 8 | e[i];
        }
        uint8_t mk[4] = {0, 0, 0, 0};
        if ((h[1] & 0x80) && !readN(*c, mk, 4, gen0))
            break;
        if (op >= 8) { // control frame: at most 125 bytes
            uint8_t pl[125];
            if (len > 125 || !readN(*c, pl, (size_t)len, gen0))
                break;
            if (op == 8) { // the server is closing
                dropWhy = "the server closed it";
                break;
            }
            if (op == 9)
                sendFrame(*c, 0x0A, pl, (size_t)len); // ping -> pong
            continue;
        }
        if (op == 1 || op == 2) // a new message starts (0 continues the last one)
            have = 0, skip = false;
        if (skip || have + len > MSG_MAX) {
            skip = true;
            uint8_t sink[256];
            while (len) { // read it to keep in step, without keeping it
                size_t k = len < sizeof(sink) ? (size_t)len : sizeof(sink);
                if (!readN(*c, sink, k, gen0))
                    break;
                len -= k;
            }
            if (len)
                break;
            continue;
        }
        if (!readN(*c, msg + have, (size_t)len, gen0))
            break;
        have += (size_t)len;
        if (fin && !skip) {
            static Item it; // the task's own scratch (one task)
            Busy b;
            if (parse((const char *)msg, have, it))
                push(it);
            have = 0;
        }
    }
    c->stop();
    delete c;
    if (gen == gen0 && want) {
        char code[24] = "";
        if (dropCode)
            snprintf(code, sizeof(code), ", error -0x%04x", (unsigned)-dropCode);
        hxlog::line("feed dropped (%s%s), stack left %u, internal heap %u", dropWhy, code, (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        setErr("Connection dropped");
        st = State::FAILED;
    }
}

void task(void *)
{
    uint8_t *msg = (uint8_t *)heap_caps_malloc(MSG_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!msg)
        msg = (uint8_t *)malloc(MSG_MAX);
    uint32_t backoff = 5000;
    for (;;) {
        if (!want) {
            st = State::OFF;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (WiFi.status() != WL_CONNECTED) {
            st = State::NO_WIFI;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        uint32_t gen0 = gen;
        uint32_t t0 = millis();
        const bool wasMoved = moved[0] != 0;
        memWait = false;
        session(gen0, msg);
        if (gen != gen0 || !want)
            continue; // settings changed: go again right away
        if (moved[0] && !wasMoved)
            continue; // the server moved: follow it once, right away
        if (memWait) {
            for (int i = 0; i < 120 && gen == gen0 && want; i++)
                vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (millis() - t0 > 60000)
            backoff = 5000; // it was up for a while: start the wait over
        uint32_t until = millis() + backoff;
        while ((int32_t)(until - millis()) > 0 && gen == gen0 && want)
            vTaskDelay(pdMS_TO_TICKS(250));
        backoff = backoff < 300000 ? backoff * 2 : 300000; // 5 s, 10 s ... 5 min
    }
}
} // namespace

void start(const char *u, const char *userAgent)
{
    if (!lock) {
        lock = xSemaphoreCreateMutex();
        ring = (Item *)heap_caps_malloc(sizeof(Item) * RING, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!ring)
            ring = (Item *)malloc(sizeof(Item) * RING);
        // TLS needs a deep stack; it stays in internal RAM
        xTaskCreatePinnedToCore(task, "hoptics-feed", 8 * 1024, nullptr, 1, nullptr, tskNO_AFFINITY);
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    strncpy(url, u, sizeof(url) - 1);
    strncpy(ua, userAgent, sizeof(ua) - 1);
    moved[0] = 0;
    count = 0;
    xSemaphoreGive(lock);
    nPackets = 0;
    want = true;
    gen++;
}

void stop()
{
    want = false;
    gen++;
}

bool pop(Item &out)
{
    if (!lock || !ring)
        return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool got = count > 0;
    if (got) {
        out = ring[head];
        head = (head + 1) % RING;
        count--;
    }
    xSemaphoreGive(lock);
    return got;
}

State state() { return want ? st : State::OFF; }
uint32_t packets() { return nPackets; }
const char *lastError() { return err; }
uint32_t busyUs() { return busy; }

} // namespace hxfeed

#else // a PC (the screen simulator): no network

namespace hxfeed
{
static bool on = false;
void start(const char *, const char *) { on = true; }
void stop() { on = false; }
bool pop(Item &) { return false; }
State state() { return on ? State::NO_WIFI : State::OFF; }
uint32_t packets() { return 0; }
const char *lastError() { return ""; }
uint32_t busyUs() { return 0; }
} // namespace hxfeed

#endif
