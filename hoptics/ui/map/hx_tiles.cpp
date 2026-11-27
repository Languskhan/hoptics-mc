// Hoptics street map engine (see hx_tiles.h). MIT licence. Written for Hoptics MT by the same author and
// relicensed here under MIT for Hoptics-MC.
#include "hx_tiles.h"
#include <PNGdec.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>
#include <string>

#if defined(ARDUINO_ARCH_ESP32)
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hx_map_roots.h"
#include "hx_sd.h"
#include <HTTPClient.h>
#include <SD.h>
#include <SPIFFS.h>
#include <ff.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#else
#include <chrono>
#include <dirent.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#endif

#if defined(ARDUINO_ARCH_ESP32)
#include "hx_log.h"
#define ILOG_INFO(fmt, ...) hxlog::line(fmt, ##__VA_ARGS__)
#else
#define ILOG_INFO(fmt, ...) printf("hoptics: " fmt "\n", ##__VA_ARGS__)
#endif
#define ILOG_WARN ILOG_INFO
#define ILOG_ERROR ILOG_INFO
#ifndef HOPTICS_VERSION
#define HOPTICS_VERSION "0"
#endif

namespace hoptics
{
namespace
{
// A distinct, stable User-Agent naming the app, as the OSM tile policy asks
constexpr const char *kUserAgent = "HopticsMC/" HOPTICS_VERSION " (MeshCore touch-screen firmware; ESP32)";
constexpr size_t kMaxTileBytes = 192 * 1024; // OSM tiles are 5-60 KB; anything bigger is refused
constexpr uint32_t kMinKeepSecs = 7 * 24 * 3600; // policy minimum before an expired tile is re-checked
constexpr uint32_t kBlock = 4096;                // flash file system block
constexpr char kMagic[4] = {'H', 'M', 'T', '1'};

// header in front of every saved tile
struct SavedHeader {
    char magic[4];
    uint32_t fetched; // unix time it was downloaded or re-checked (0 = clock was not set)
    uint32_t maxAge;  // seconds, from the server's Cache-Control
    uint32_t pngLen;
    char etag[48];
};
static_assert(sizeof(SavedHeader) == 64, "saved tile header must stay 64 bytes");

#if defined(ARDUINO_ARCH_ESP32)
const char *kRoot = "/hmap";
uint32_t nowMs(void)
{
    return millis();
}
void sleepMs(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}
void *bigAlloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}
void *bigRealloc(void *p, size_t n)
{
    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q ? q : realloc(p, n);
}
#else
std::string hostRoot = "/tmp/hoptics-map/hmap";
std::string hostSource;
std::atomic<bool> hostOnline{true};
uint32_t nowMs(void)
{
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
void sleepMs(uint32_t ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
void *bigAlloc(size_t n)
{
    return malloc(n);
}
void *bigRealloc(void *p, size_t n)
{
    return realloc(p, n);
}
#endif

// unix time, or 0 while the clock is not set
uint32_t wallClock(void)
{
    time_t t = time(nullptr);
    return t > 1704067200 ? (uint32_t)t : 0;
}

int32_t floorDiv(int32_t a, int32_t b)
{
    int32_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// where saved tiles are kept, and the card's map packs (defined with the storage code below, per platform)
bool onCard(void);
uint64_t capBytes(void);
void findPacks(void);
bool packTile(uint8_t z, uint32_t x, uint32_t y, Blob &out);

uint64_t tileKey(uint8_t z, uint32_t x, uint32_t y)
{
    return ((uint64_t)z << 56) | ((uint64_t)x << 28) | y;
}

void tilePath(char *out, size_t n, uint8_t z, uint32_t x, uint32_t y)
{
#if defined(ARDUINO_ARCH_ESP32)
    if (hxsd::mounted()) // the card has folders: one per column, so no folder gets long (FAT looks names up in order)
        snprintf(out, n, "/hoptics/tiles/%u/%u/%u.hmt", (unsigned)z, (unsigned)x, (unsigned)y);
    else // SPIFFS has no folders: every file is listed from the root by its whole path
        snprintf(out, n, "%s/%u/%u_%u.hmt", kRoot, (unsigned)z, (unsigned)x, (unsigned)y);
#else
    snprintf(out, n, "%s/%u/%u_%u.hmt", hostRoot.c_str(), (unsigned)z, (unsigned)x, (unsigned)y);
#endif
}

// ------------------------------------------------------------------ colours
// The tiles are made for a white page. Dark themes flip lightness but keep hue, so land goes dark,
// roads and labels go light, water stays blue and parks green; the night themes go monochrome.
inline int clamp255(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

// map a tile luminance to the dark-map one: OSM's land (~239) becomes the darkest tone, white roads
// lift a little above it, everything darker than land (labels, outlines) comes up towards white
inline int darkLum(int y)
{
    return y >= 239 ? 22 + (y - 239) * 52 / 16 : 22 + (239 - y) * 190 / 239;
}

void recolor(uint16_t *px, int n, MapStyle st)
{
    if (st == MapStyle::Light)
        return;
    for (int i = 0; i < n; i++) {
        uint16_t c = px[i];
        int r = (c >> 8) & 0xF8, g = (c >> 3) & 0xFC, b = (c << 3) & 0xF8;
        r |= r >> 5;
        g |= g >> 6;
        b |= b >> 5;
        int y = (77 * r + 150 * g + 29 * b) >> 8;
        int l = darkLum(y);
        int R, G, B;
        switch (st) {
        case MapStyle::Dark: // keep 70% of the colour around the new lightness, slight blue cast like the theme
            R = clamp255(l + (r - y) * 7 / 10 - 2);
            G = clamp255(l + (g - y) * 7 / 10 + 1);
            B = clamp255(l + (b - y) * 7 / 10 + 7);
            break;
        case MapStyle::Red: // red on black only
            R = clamp255(l * 21 / 20);
            G = l / 14;
            B = l / 16;
            break;
        default: // Amber phosphor
            R = clamp255(l * 21 / 20);
            G = clamp255(l * 3 / 4);
            B = l / 12;
            break;
        }
        px[i] = (uint16_t)(((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3));
    }
}

#if defined(ARDUINO_ARCH_ESP32)
// HTTPClient::writeToStream() target that grows a Blob
class BlobStream : public Stream
{
  public:
    explicit BlobStream(Blob &b) : blob(b) {}
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t *p, size_t n) override { return blob.append(p, n, kMaxTileBytes) ? n : 0; }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }

  private:
    Blob &blob;
};
#endif
} // namespace

// ------------------------------------------------------------------ Blob
Blob::~Blob()
{
    free(data);
}

bool Blob::reserve(size_t n)
{
    if (n <= cap)
        return true;
    uint8_t *p = (uint8_t *)(data ? bigRealloc(data, n) : bigAlloc(n));
    if (!p)
        return false;
    data = p;
    cap = n;
    return true;
}

bool Blob::append(const uint8_t *p, size_t n, size_t max)
{
    if (len + n > max) {
        overflow = true;
        return false;
    }
    if (len + n > cap && !reserve(std::min(max, std::max(len + n, cap * 2 + 4096))))
        return false;
    memcpy(data + len, p, n);
    len += n;
    return true;
}

void Blob::reset(void)
{
    len = 0;
    overflow = false;
}

// ------------------------------------------------------------------ UI-task API
TileMap &TileMap::get(void)
{
    static TileMap *inst = new TileMap();
    return *inst;
}

void TileMap::setAllocator(void *(*alloc)(size_t), void (*release)(void *))
{
    longAlloc = alloc;
    longFree = release;
}

uint16_t *TileMap::buffer(int16_t w, int16_t h)
{
    std::lock_guard<std::mutex> lk(bufLock);
    if (buf && bufW == w && bufH == h)
        return buf;
    if (buf) {
        if (longFree)
            longFree(buf);
        else
            free(buf);
        buf = nullptr;
        bufW = bufH = 0;
        gen++; // anything the worker was drawing belongs to the old buffer
    }
    if (w <= 0 || h <= 0)
        return nullptr;
    size_t n = (size_t)w * h * 2;
    buf = (uint16_t *)(longAlloc ? longAlloc(n) : malloc(n));
    if (!buf) {
        ILOG_ERROR("Hoptics map: no memory for a %dx%d view", w, h);
        return nullptr;
    }
    bufW = w;
    bufH = h;
    return buf;
}

void TileMap::request(const MapView &v)
{
    {
        std::lock_guard<std::mutex> lk(reqLock);
        if (v.same(req) && (started || (idleGen.load() == gen.load() && !(incomplete && online()))))
            return; // nothing new to draw (a worker that ended is only woken for a new view, or once Wi-Fi is back)
        req = v;
    }
    gen++;
    startWorker();
}

void TileMap::shiftAndRequest(int32_t dx, int32_t dy, const MapView &v)
{
    {
        // the buffer moves and the new view takes over in one step, so the worker can never
        // paint a row of the old view onto the moved picture
        std::lock_guard<std::mutex> lk(bufLock);
        if (buf && bufW == v.w && bufH == v.h && (dx || dy)) {
            const int32_t w = bufW, h = bufH;
            if (std::abs(dx) >= w || std::abs(dy) >= h) {
                std::fill_n(buf, (size_t)w * h, v.bg);
            } else {
                const int32_t cw = w - std::abs(dx); // columns that stay visible
                const int32_t sx = dx < 0 ? -dx : 0, tx = dx > 0 ? dx : 0;
                if (dy > 0) {
                    for (int32_t y = h - 1; y >= dy; y--)
                        memmove(buf + y * w + tx, buf + (y - dy) * w + sx, cw * 2);
                } else {
                    for (int32_t y = 0; y < h + dy; y++)
                        memmove(buf + y * w + tx, buf + (y - dy) * w + sx, cw * 2);
                }
                // uncovered rows and columns
                for (int32_t y = 0; y < h; y++) {
                    uint16_t *r = buf + y * w;
                    if ((dy > 0 && y < dy) || (dy < 0 && y >= h + dy)) {
                        std::fill_n(r, w, v.bg);
                        continue;
                    }
                    if (dx > 0)
                        std::fill_n(r, dx, v.bg);
                    else if (dx < 0)
                        std::fill_n(r + w + dx, -dx, v.bg);
                }
            }
            dirty = true;
        }
        std::lock_guard<std::mutex> rk(reqLock);
        req = v;
        gen++;
    }
    startWorker();
}

void TileMap::setEnabled(bool on)
{
    if (enabled.exchange(on) != on)
        gen++;
}

MapStatus TileMap::status(void)
{
    MapStatus s;
    {
        std::lock_guard<std::mutex> lk(statLock);
        s = stat;
    }
    s.enabled = enabled.load();
    s.online = online();
    s.onCard = onCard();
    s.capKB = (uint32_t)(capBytes() / 1024);
    if (s.online && s.problem == MapStatus::OFFLINE)
        s.problem = MapStatus::NONE; // Wi-Fi came up; the worker retries shortly
    return s;
}

void TileMap::clearSaved(void)
{
    wipeRequested = true;
    startWorker();
}

#if defined(ARDUINO_ARCH_ESP32)
namespace
{
WiFiClientSecure *tls = nullptr; // the worker's connection to the tile server
HTTPClient *http = nullptr;
} // namespace
#endif

// ------------------------------------------------------------------ worker
void TileMap::startWorker(void)
{
    bool was = false;
    if (!started.compare_exchange_strong(was, true))
        return; // one is running
#if defined(ARDUINO_ARCH_ESP32)
    // lowest priority: it only uses time the UI and the mesh loop leave over. The stack stays in
    // internal RAM because the worker writes flash.
    xTaskCreatePinnedToCore(
        [](void *p) {
            ((TileMap *)p)->workerLoop();
            vTaskDelete(nullptr);
        },
        "hoptics-map", 12 * 1024, this, 0, nullptr, tskNO_AFFINITY);
#else
    std::thread([this] { workerLoop(); }).detach();
#endif
}

void TileMap::workerLoop(void)
{
    findPacks();
    loadIndex();
    publishStatus();
    uint32_t done = gen.load() - 1;
    uint32_t lastTry = 0, idleSince = nowMs();
    for (;;) {
        if (wipeRequested.exchange(false)) {
            wipeSaved();
            publishStatus();
        }
        takeImport();
        flushIndex(); // the saved-tile list, after any change
        const uint32_t g = gen.load();
        if (!enabled.load()) {
            idleGen = g;
            sleepMs(100);
            if (nowMs() - idleSince > 15000 && retire(g))
                return;
            continue;
        }
        if (g != done) {
            working = true;
            renderView(g);
            if (gen.load() == g) {
                lastTry = nowMs();
                downloadMissing(g);
            }
            if (gen.load() == g)
                revalidate(g);
            publishStatus();
            if (gen.load() == g) {
                done = g;
                idleGen = g;
            }
            working = false;
            idleSince = nowMs();
            continue;
        }
        // same view: fetch what is still missing once Wi-Fi is back or a back-off has expired
        bool missing = std::any_of(tiles.begin(), tiles.end(), [](const Tile &t) { return t.inWorld && !t.shown && !t.gone; });
        bool stale = std::any_of(tiles.begin(), tiles.end(), [](const Tile &t) { return t.stale; });
        if ((missing || stale) && online() && (int32_t)(nowMs() - backoffUntil) >= 0 && nowMs() - lastTry >= 2000) {
            working = true;
            lastTry = nowMs();
            downloadMissing(g);
            if (gen.load() == g)
                revalidate(g);
            publishStatus();
            working = false;
            idleSince = nowMs();
            continue;
        }
        incomplete = missing;
        if (missing && online())
            idleSince = nowMs(); // still has tiles to fetch: stays
        else if (nowMs() - idleSince > 15000 && retire(g))
            return; // the next view starts a new one
        sleepMs(50);
    }
}

// a tile from the web page (see importTile): written here, the one task that touches the saved tiles
void TileMap::takeImport(void)
{
    if (!impWaiting.load())
        return;
    std::lock_guard<std::mutex> lk(impLock);
    if (!imp)
        return;
    imp->result = save(imp->z, imp->x, imp->y, imp->png, imp->len, 365u * 24 * 3600, "") ? IMPORTED : FULL;
    imp = nullptr;
    impWaiting = false;
    publishStatus();
}

TileMap::ImportResult TileMap::importTile(uint8_t z, uint32_t x, uint32_t y, const uint8_t *png, size_t len, uint32_t waitMs)
{
    static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (z > 19 || x >= (1u << z) || y >= (1u << z) || len < 64 || len > kMaxTileBytes || memcmp(png, SIG, 8))
        return BAD;
    Import job{z, x, y, png, len, BUSY};
    {
        std::lock_guard<std::mutex> lk(impLock);
        if (imp)
            return BUSY;
        imp = &job;
        impWaiting = true;
    }
    startWorker();
    for (uint32_t waited = 0;; waited += 5) {
        {
            std::lock_guard<std::mutex> lk(impLock);
            if (imp != &job)
                return job.result; // written (the worker holds the lock while it writes)
            if (waited >= waitMs) { // not started: withdrawn
                imp = nullptr;
                impWaiting = false;
                return BUSY;
            }
        }
        sleepMs(5);
    }
}

// End the worker after a quiet spell, giving back its stack and the tile server connection (internal RAM is
// scarce once Wi-Fi is on). False when a new view came in meanwhile: it carries on.
bool TileMap::retire(uint32_t g)
{
    flushIndex(true);
#if defined(ARDUINO_ARCH_ESP32)
    if (tls)
        tls->stop();
    ILOG_INFO("Hoptics map: worker resting, stack left %u", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
#else
    ILOG_INFO("Hoptics map: worker resting");
#endif
    started = false;
    if (gen.load() == g && !wipeRequested.load() && !impWaiting.load())
        return true;
    bool was = false; // something arrived: carry on, unless a new worker has already started for it
    return !started.compare_exchange_strong(was, true);
}

void TileMap::renderView(uint32_t g)
{
    {
        std::lock_guard<std::mutex> lk(reqLock);
        cur = req;
    }
    tiles.clear();
    {
        std::lock_guard<std::mutex> lk(bufLock);
        if (!buf || bufW != cur.w || bufH != cur.h)
            return;
    }
    const int32_t n = 1 << cur.z;
    const int32_t tx0 = floorDiv(cur.ox, 256), tx1 = floorDiv(cur.ox + cur.w - 1, 256);
    const int32_t ty0 = floorDiv(cur.oy, 256), ty1 = floorDiv(cur.oy + cur.h - 1, 256);
    for (int32_t ty = ty0; ty <= ty1; ty++)
        for (int32_t tx = tx0; tx <= tx1; tx++) {
            Tile t{};
            t.tx = tx;
            t.ty = ty;
            t.x = (uint32_t)(((tx % n) + n) % n);
            t.y = (uint32_t)ty;
            t.inWorld = ty >= 0 && ty < n;
            tiles.push_back(t);
        }
    // middle of the screen first
    const int32_t cx = cur.ox + cur.w / 2, cy = cur.oy + cur.h / 2;
    std::sort(tiles.begin(), tiles.end(), [cx, cy](const Tile &a, const Tile &b) {
        int64_t ax = a.tx * 256 + 128 - cx, ay = a.ty * 256 + 128 - cy, bx = b.tx * 256 + 128 - cx, by = b.ty * 256 + 128 - cy;
        return ax * ax + ay * ay < bx * bx + by * by;
    });

    Blob png;
    for (Tile &t : tiles) {
        if (gen.load() != g)
            return;
        if (t.inWorld) {
            png.reset();
            if (packTile(cur.z, t.x, t.y, png)) {
                if (drawPng(png.data, png.len, t, cur, g)) {
                    t.shown = true;
                    dirty = true;
                    continue;
                }
                if (gen.load() != g)
                    return;
                png.reset(); // not a usable PNG: a saved or downloaded tile instead
            }
            uint32_t fetched = 0, maxAge = 0;
            if (readSaved(cur.z, t.x, t.y, png, fetched, maxAge, t.etag)) {
                if (drawPng(png.data, png.len, t, cur, g)) {
                    t.shown = true;
                    dirty = true;
                    uint32_t now = wallClock();
                    t.stale = now && (fetched == 0 || now > fetched + std::max(maxAge, kMinKeepSecs));
                    continue;
                }
                if (gen.load() != g)
                    return;
                ILOG_WARN("Hoptics map: a saved zoom %u tile was unreadable, dropped", cur.z); // no x/y: they say where you are
                forget(cur.z, t.x, t.y);
            }
            t.gone = std::find(notFound.begin(), notFound.end(), tileKey(cur.z, t.x, t.y)) != notFound.end();
        }
        fillTile(t, cur, g);
        dirty = true;
    }
    publishStatus();
}

void TileMap::downloadMissing(uint32_t g)
{
    Blob png;
    for (Tile &t : tiles) {
        if (!t.inWorld || t.shown || t.gone)
            continue;
        if (gen.load() != g)
            return;
        if (!online()) {
            setProblem(MapStatus::OFFLINE);
            return;
        }
        if ((int32_t)(nowMs() - backoffUntil) < 0)
            return;
        char etag[48];
        uint32_t maxAge = 0;
        png.reset();
        uint32_t t0 = nowMs();
        int code = fetch(cur.z, t.x, t.y, nullptr, png, etag, maxAge);
        if (code == 200) {
            ILOG_INFO("Hoptics map: zoom %u tile downloaded (%u B, %u ms)", cur.z, (unsigned)png.len, (unsigned)(nowMs() - t0));
            backoffMs = 0;
            setProblem(MapStatus::NONE);
            bool ok = drawPng(png.data, png.len, t, cur, g);
            if (ok || gen.load() != g)
                save(cur.z, t.x, t.y, png.data, png.len, maxAge, etag); // a newer view interrupted the drawing, keep it anyway
            if (ok) {
                t.shown = true;
                dirty = true;
            } else if (gen.load() == g) {
                ILOG_WARN("Hoptics map: a zoom %u tile is not a 256 px PNG", cur.z);
                t.gone = true;
            }
            publishStatus();
        } else if (code == 404 || (code >= 400 && code < 500 && code != 403 && code != 429)) {
            ILOG_WARN("Hoptics map: a zoom %u tile answered %d", cur.z, code);
            if (notFound.size() >= 64)
                notFound.erase(notFound.begin());
            notFound.push_back(tileKey(cur.z, t.x, t.y));
            t.gone = true;
        } else {
            // 403/429/5xx: the server wants us to slow down; < 0: no connection or TLS failure
            backoffMs = backoffMs ? std::min<uint32_t>(backoffMs * 2, 30 * 60 * 1000) : (code > 0 ? 60000 : 20000);
            backoffUntil = nowMs() + backoffMs;
            setProblem(code > 0 ? MapStatus::BUSY : MapStatus::NO_LINK);
            ILOG_WARN("Hoptics map: tile server %d, waiting %u s", code, (unsigned)(backoffMs / 1000));
            return;
        }
    }
}

void TileMap::revalidate(uint32_t g)
{
    Blob png;
    for (Tile &t : tiles) {
        if (!t.stale)
            continue;
        if (gen.load() != g || !online() || (int32_t)(nowMs() - backoffUntil) < 0)
            return;
        char etag[48];
        uint32_t maxAge = 0;
        png.reset();
        int code = fetch(cur.z, t.x, t.y, t.etag[0] ? t.etag : nullptr, png, etag, maxAge);
        if (code == 304) {
            touchSaved(cur.z, t.x, t.y, maxAge, etag[0] ? etag : t.etag);
            t.stale = false;
        } else if (code == 200) {
            ILOG_INFO("Hoptics map: a zoom %u tile changed, updated", cur.z);
            if (drawPng(png.data, png.len, t, cur, g)) {
                save(cur.z, t.x, t.y, png.data, png.len, maxAge, etag);
                dirty = true;
            }
            t.stale = false;
        } else if (code >= 400 && code < 500 && code != 403 && code != 429) {
            t.stale = false; // keep showing the saved one
        } else {
            backoffMs = backoffMs ? std::min<uint32_t>(backoffMs * 2, 30 * 60 * 1000) : (code > 0 ? 60000 : 20000);
            backoffUntil = nowMs() + backoffMs;
            return;
        }
    }
}

// ------------------------------------------------------------------ drawing
bool TileMap::drawPng(const uint8_t *data, size_t len, const Tile &t, const MapView &v, uint32_t g)
{
    if (!data || len < 64)
        return false;
    if (!png) {
        void *m = longAlloc ? longAlloc(sizeof(PNG)) : malloc(sizeof(PNG));
        if (!m) {
            ILOG_ERROR("Hoptics map: no memory for the PNG decoder");
            return false;
        }
        png = new (m) PNG();
    }
    if (png->openRAM((uint8_t *)data, (int)len, (PNG_DRAW_CALLBACK *)drawRow) != PNG_SUCCESS)
        return false;
    if (png->getWidth() != 256 || png->getHeight() != 256) {
        png->close();
        return false;
    }
    DrawCtx ctx{this, &v, t.tx * 256 - v.ox, t.ty * 256 - v.oy, g, false, false};
    int rc = png->decode(&ctx, 0);
    png->close();
    if (ctx.aborted)
        return false;
    return rc == PNG_SUCCESS || (rc == PNG_QUIT_EARLY && ctx.complete);
}

int TileMap::drawRow(png_draw_tag *d)
{
    DrawCtx *c = (DrawCtx *)d->pUser;
    TileMap *self = c->self;
    const MapView &v = *c->v;
    const int32_t vy = c->dy + d->y;
    if (d->y == 255)
        c->complete = true;
    if (vy < 0)
        return 1; // still above the view
    if (vy >= v.h) {
        c->complete = true; // rest of the tile is below the view
        return 0;
    }
    const int32_t x0 = std::max<int32_t>(0, -c->dx), x1 = std::min<int32_t>(d->iWidth, v.w - c->dx);
    if (x0 >= x1)
        return 1;
    self->png->getLineAsRGB565(d, self->row, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
    recolor(self->row + x0, x1 - x0, v.style);
    std::lock_guard<std::mutex> lk(self->bufLock);
    if (self->gen.load() != c->gen || !self->buf || self->bufW != v.w || self->bufH != v.h) {
        c->aborted = true;
        return 0;
    }
    memcpy(self->buf + vy * v.w + c->dx + x0, self->row + x0, (x1 - x0) * 2);
    return 1;
}

void TileMap::fillTile(const Tile &t, const MapView &v, uint32_t g)
{
    const int32_t x0 = std::max<int32_t>(0, t.tx * 256 - v.ox), x1 = std::min<int32_t>(v.w, t.tx * 256 + 256 - v.ox);
    const int32_t y0 = std::max<int32_t>(0, t.ty * 256 - v.oy), y1 = std::min<int32_t>(v.h, t.ty * 256 + 256 - v.oy);
    if (x0 >= x1 || y0 >= y1)
        return;
    std::lock_guard<std::mutex> lk(bufLock);
    if (gen.load() != g || !buf || bufW != v.w || bufH != v.h)
        return;
    for (int32_t y = y0; y < y1; y++)
        std::fill_n(buf + y * v.w + x0, x1 - x0, v.bg);
}

void TileMap::setProblem(MapStatus::Problem p)
{
    std::lock_guard<std::mutex> lk(statLock);
    stat.problem = p;
}

void TileMap::publishStatus(void)
{
    uint8_t need = 0, shown = 0, pending = 0;
    for (const Tile &t : tiles) {
        if (!t.inWorld)
            continue;
        need++;
        if (t.shown)
            shown++;
        else if (!t.gone)
            pending++;
    }
    std::lock_guard<std::mutex> lk(statLock);
    stat.need = need;
    stat.shown = shown;
    stat.pending = pending;
    if (!pending && stat.problem == MapStatus::OFFLINE)
        stat.problem = MapStatus::NONE;
    stat.savedTiles = (uint32_t)saved.size();
    stat.savedKB = savedBytes / 1024;
}

// ------------------------------------------------------------------ saved tiles: flash, or the card
#if defined(ARDUINO_ARCH_ESP32)
// With a card in, saved tiles live on the card (/hoptics/tiles/<z>/<x>/<y>.hmt) and the ones on flash wait there,
// unused, until the card is out again. The card is on the main loop's SPI bus (hx_sd.h): each piece of card work below
// is handed to the main loop with io(), one file operation at a time, and the worker waits for it. Flash work is done
// here, as before.
namespace
{
constexpr uint64_t kCardCap = 1024ull << 20, kCardReserve = 64ull << 20;
constexpr size_t kPiece = 256; // index records per card job (3 KB)
std::vector<std::string> packRoots; // the map packs found on the card, in the order they are tried

bool onCard(void) { return hxsd::mounted(); }
fs::FS &store(void) { return onCard() ? (fs::FS &)SD : (fs::FS &)SPIFFS; }
template <class F> bool io(F &&f)
{
    if (!onCard()) {
        f();
        return true;
    }
    return hxsd::run(f);
}
uint32_t block(void) { return onCard() ? std::max<uint32_t>(hxsd::clusterBytes(), 512) : kBlock; }
uint64_t capBytes(void) { return onCard() ? kCardCap : HOPTICS_MAP_CACHE_KB * 1024ull; }
uint64_t reserveBytes(void) { return onCard() ? kCardReserve : HOPTICS_MAP_FS_RESERVE_KB * 1024ull; }
// free space, given `freed` bytes deleted during this call (the card's count is refreshed by the main loop later)
uint64_t storeFree(uint64_t freed)
{
    if (onCard())
        return hxsd::freeBytes() + freed;
    size_t total = SPIFFS.totalBytes(), used = SPIFFS.usedBytes();
    return total > used ? total - used : 0;
}
const char *indexPath(void) { return onCard() ? "/hoptics/tiles/index.bin" : "/hmap.idx"; }

// one folder on the card through FatFS: each entry's name, whether it is a folder, and its size, in one job
template <class F> bool listCard(const char *path, F &&each)
{
    return hxsd::run([&] {
        char p[64];
        hxsd::fatPath(p, sizeof(p), path);
        FF_DIR d;
        FILINFO fi;
        if (f_opendir(&d, p) != FR_OK)
            return;
        while (f_readdir(&d, &fi) == FR_OK && fi.fname[0])
            each((const char *)fi.fname, (fi.fattrib & AM_DIR) != 0, (uint32_t)fi.fsize);
        f_closedir(&d);
    });
}

// the map packs on the card: looked for each time the worker starts, so a pack copied in from the web page shows
void findPacks(void)
{
    packRoots.clear();
    if (!onCard())
        return;
    std::string style;
    listCard("/maps", [&](const char *name, bool dir, uint32_t) {
        if (dir && style.empty() && name[0] != '.')
            style = std::string("/maps/") + name;
    });
    if (!style.empty())
        packRoots.push_back(style);
    hxsd::run([&] {
        FILINFO fi;
        for (const char *r : {"/map", "/tiles"}) {
            char p[16];
            hxsd::fatPath(p, sizeof(p), r);
            if (f_stat(p, &fi) == FR_OK && (fi.fattrib & AM_DIR))
                packRoots.push_back(r);
        }
    });
}

// a tile from a map pack, as it is on the card (any size the engine takes)
bool packTile(uint8_t z, uint32_t x, uint32_t y, Blob &out)
{
    if (packRoots.empty())
        return false;
    bool found = false;
    hxsd::run([&] {
        static FIL *f = (FIL *)bigAlloc(sizeof(FIL)); // ~4 KB (PSRAM); the main loop's, one job at a time
        if (!f)
            return;
        for (const std::string &r : packRoots) {
            char path[96];
            snprintf(path, sizeof(path), "%s/%u/%u/%u.png", r.c_str(), (unsigned)z, (unsigned)x, (unsigned)y);
            char p[100];
            hxsd::fatPath(p, sizeof(p), path);
            if (f_open(f, p, FA_READ) != FR_OK)
                continue;
            const FSIZE_t size = f_size(f);
            UINT got = 0;
            found = size >= 64 && size <= kMaxTileBytes && out.reserve(size) && f_read(f, out.data, (UINT)size, &got) == FR_OK &&
                    got == size;
            f_close(f);
            if (found) {
                out.len = size;
                return;
            }
        }
    });
    return found;
}

// the folders above a tile's file on the card
void makeTileDirs(const char *path)
{
    char p[48];
    snprintf(p, sizeof(p), "%s", path);
    for (char *s = strchr(p + 1, '/'); s; s = strchr(s + 1, '/')) {
        *s = 0;
        SD.mkdir(p); // false when it is there already
        *s = '/';
    }
}
} // namespace

// The list of saved tiles is kept in one small file: listing SPIFFS's root walks the whole 3.4 MB partition, and while
// flash is read nothing else runs from it, so the screen froze for 6 s the first time the map opened. It is read and
// written in pieces, so the card's main loop is never held for long
struct IndexRec {
    uint8_t z;
    uint32_t x, y, bytes;
} __attribute__((packed));

void TileMap::loadIndex(void)
{
    if (indexLoaded)
        return;
    indexLoaded = true;
    std::vector<IndexRec> recs(kPiece);
    const char *ix = indexPath();
    bool have = false, bad = false;
    for (size_t at = 0;; at += kPiece) {
        size_t got = 0;
        if (!io([&] {
                File f = store().open(ix, "r");
                if (!f)
                    return;
                have = true;
                bad = f.size() % sizeof(IndexRec) != 0;
                if (!bad && f.seek(at * sizeof(IndexRec)))
                    got = f.read(reinterpret_cast<uint8_t *>(recs.data()), kPiece * sizeof(IndexRec)) / sizeof(IndexRec);
                f.close();
            }))
            bad = true;
        for (size_t i = 0; i < got; i++)
            if (recs[i].z <= 31) {
                saved.push_back({recs[i].z, recs[i].x, recs[i].y, recs[i].bytes});
                savedBytes += recs[i].bytes;
            }
        if (bad || got < kPiece)
            break;
    }
    if (have && !bad) {
        ILOG_INFO("Hoptics map: %u saved tiles, %u KB%s", (unsigned)saved.size(), (unsigned)(savedBytes / 1024), onCard() ? " on the card" : "");
        return;
    }
    saved.clear();
    savedBytes = 0;
    // no list yet (the first start of this version, or a new card): the slow walk, once, then the list is written
    const uint32_t blk = block();
    if (onCard()) { // /hoptics/tiles/<z>/<x>/<y>.hmt: one job per folder
        std::vector<unsigned> zs, xs;
        listCard("/hoptics/tiles", [&](const char *n, bool dir, uint32_t) {
            if (dir && atoi(n) <= 31)
                zs.push_back((unsigned)atoi(n));
        });
        for (unsigned z : zs) {
            char zp[32];
            snprintf(zp, sizeof(zp), "/hoptics/tiles/%u", z);
            xs.clear();
            listCard(zp, [&](const char *n, bool dir, uint32_t) {
                if (dir)
                    xs.push_back((unsigned)strtoul(n, nullptr, 10));
            });
            for (unsigned x : xs) {
                char xp[48];
                snprintf(xp, sizeof(xp), "%s/%u", zp, x);
                listCard(xp, [&](const char *n, bool dir, uint32_t size) {
                    unsigned y;
                    char ext[8];
                    if (!dir && sscanf(n, "%u.%7s", &y, ext) == 2 && !strcmp(ext, "hmt")) {
                        const uint32_t bytes = (size + blk - 1) / blk * blk;
                        saved.push_back({(uint8_t)z, x, y, bytes});
                        savedBytes += bytes;
                    }
                });
            }
        }
    } else { // SPIFFS has no folders: every file is listed from the root by its whole path, "/hmap/<z>/<x>_<y>.hmt"
        File root = SPIFFS.open("/");
        if (!root)
            return;
        for (File f = root.openNextFile(); f; f = root.openNextFile()) {
            const char *p = strstr(f.path(), "hmap/");
            unsigned z, x, y;
            if (!p || sscanf(p, "hmap/%u/%u_%u.hmt", &z, &x, &y) != 3 || z > 31)
                continue;
            uint32_t bytes = ((uint32_t)f.size() + blk - 1) / blk * blk;
            saved.push_back({(uint8_t)z, x, y, bytes});
            savedBytes += bytes;
        }
    }
    indexDirty = true;
    flushIndex(true);
    ILOG_INFO("Hoptics map: %u saved tiles, %u KB%s (listed once)", (unsigned)saved.size(), (unsigned)(savedBytes / 1024),
              onCard() ? " on the card" : "");
}

void TileMap::flushIndex(bool now)
{
    if (!indexDirty || (onCard() && !now && nowMs() - indexAt < 60000)) // the card's: at most once a minute, and at rest
        return;
    indexDirty = false;
    indexAt = nowMs();
    const char *ix = indexPath();
    char tmp[40];
    snprintf(tmp, sizeof(tmp), "%s.new", ix);
    std::vector<IndexRec> recs(kPiece);
    bool ok = true;
    for (size_t at = 0; ok && (at == 0 || at < saved.size()); at += kPiece) { // a new file, swapped in at the end
        const size_t n = std::min(kPiece, saved.size() - at);
        for (size_t i = 0; i < n; i++)
            recs[i] = {saved[at + i].z, saved[at + i].x, saved[at + i].y, saved[at + i].bytes};
        ok = io([&] {
                 if (at == 0 && onCard())
                     makeTileDirs("/hoptics/tiles/x");
                 File f = store().open(tmp, at ? FILE_APPEND : FILE_WRITE);
                 ok = f && f.write(reinterpret_cast<const uint8_t *>(recs.data()), n * sizeof(IndexRec)) == n * sizeof(IndexRec);
                 if (f)
                     f.close();
             }) && ok;
    }
    io([&] {
        if (ok) {
            store().remove(ix);
            ok = store().rename(tmp, ix);
        }
        if (!ok)
            store().remove(tmp);
    });
    if (!ok)
        indexDirty = true; // again next time
}

bool TileMap::readSaved(uint8_t z, uint32_t x, uint32_t y, Blob &out, uint32_t &fetched, uint32_t &maxAge, char *etag)
{
    auto it = std::find_if(saved.begin(), saved.end(), [&](const Saved &s) { return s.z == z && s.x == x && s.y == y; });
    if (it == saved.end())
        return false;
    char path[48];
    tilePath(path, sizeof(path), z, x, y);
    bool found = false, ok = false;
    SavedHeader h;
    if (!io([&] {
            File f = store().open(path, "r");
            if (!f)
                return;
            found = true;
            ok = f.read(reinterpret_cast<uint8_t *>(&h), sizeof(h)) == sizeof(h) && memcmp(h.magic, kMagic, 4) == 0 &&
                 h.pngLen + sizeof(h) == f.size() && h.pngLen <= kMaxTileBytes && out.reserve(h.pngLen) &&
                 f.read(out.data, h.pngLen) == h.pngLen;
            f.close();
        }))
        return false; // the card was busy: still listed
    if (!found) {
        savedBytes -= it->bytes; // listed, but its file is gone
        saved.erase(it);
        indexDirty = true;
        return false;
    }
    if (!ok)
        return false;
    out.len = h.pngLen;
    fetched = h.fetched;
    maxAge = h.maxAge;
    h.etag[sizeof(h.etag) - 1] = 0;
    strcpy(etag, h.etag);
    return true;
}

bool TileMap::save(uint8_t z, uint32_t x, uint32_t y, const uint8_t *pngData, size_t pngLen, uint32_t maxAge, const char *etag)
{
    const uint32_t blk = block(), bytes = ((uint32_t)(pngLen + sizeof(SavedHeader)) + blk - 1) / blk * blk;
    forget(z, x, y); // replacing an old copy
    if (!makeRoom(bytes)) {
        std::lock_guard<std::mutex> lk(statLock);
        stat.storageFull = true;
        return false;
    }
    SavedHeader h{};
    memcpy(h.magic, kMagic, 4);
    h.fetched = wallClock();
    h.maxAge = maxAge;
    h.pngLen = (uint32_t)pngLen;
    if (etag && strlen(etag) < sizeof(h.etag))
        strcpy(h.etag, etag);
    char path[48];
    tilePath(path, sizeof(path), z, x, y);
    bool ok = false;
    io([&] {
        File f = store().open(path, "w");
        if (!f && onCard()) { // its folders, the first time
            makeTileDirs(path);
            f = store().open(path, "w");
        }
        ok = f && f.write(reinterpret_cast<const uint8_t *>(&h), sizeof(h)) == sizeof(h) && f.write(pngData, pngLen) == pngLen;
        if (f)
            f.close();
        if (!ok)
            store().remove(path);
    });
    if (!ok) {
        std::lock_guard<std::mutex> lk(statLock);
        stat.storageFull = true;
        return false;
    }
    saved.push_back({z, x, y, bytes});
    savedBytes += bytes;
    indexDirty = true;
    std::lock_guard<std::mutex> lk(statLock);
    stat.storageFull = false;
    return true;
}

void TileMap::touchSaved(uint8_t z, uint32_t x, uint32_t y, uint32_t maxAge, const char *etag)
{
    char path[48];
    tilePath(path, sizeof(path), z, x, y);
    io([&] {
        File f = store().open(path, "r+");
        if (!f)
            return;
        SavedHeader h;
        if (f.read(reinterpret_cast<uint8_t *>(&h), sizeof(h)) == sizeof(h) && memcmp(h.magic, kMagic, 4) == 0) {
            h.fetched = wallClock();
            if (maxAge)
                h.maxAge = maxAge;
            if (etag && strlen(etag) < sizeof(h.etag))
                strcpy(h.etag, etag);
            f.seek(0);
            f.write(reinterpret_cast<const uint8_t *>(&h), sizeof(h));
        }
        f.close();
    });
}

void TileMap::forget(uint8_t z, uint32_t x, uint32_t y)
{
    auto it = std::find_if(saved.begin(), saved.end(), [&](const Saved &s) { return s.z == z && s.x == x && s.y == y; });
    if (it == saved.end())
        return;
    char path[48];
    tilePath(path, sizeof(path), z, x, y);
    io([&] { store().remove(path); });
    savedBytes -= it->bytes;
    saved.erase(it);
    indexDirty = true;
}

void TileMap::wipeSaved(void)
{
    // sixteen files per card job; then the card's emptied folders, and the list
    std::vector<uint64_t> cols; // z/x of each folder on the card
    for (size_t at = 0; at < saved.size(); at += 16)
        io([&] {
            for (size_t i = at; i < std::min(at + 16, saved.size()); i++) {
                char path[48];
                tilePath(path, sizeof(path), saved[i].z, saved[i].x, saved[i].y);
                store().remove(path);
            }
        });
    if (onCard()) {
        for (const Saved &s : saved)
            cols.push_back(((uint64_t)s.z << 32) | s.x);
        std::sort(cols.begin(), cols.end());
        cols.erase(std::unique(cols.begin(), cols.end()), cols.end());
        for (size_t at = 0; at < cols.size(); at += 16)
            io([&] {
                for (size_t i = at; i < std::min(at + 16, cols.size()); i++) {
                    char p[40];
                    snprintf(p, sizeof(p), "/hoptics/tiles/%u/%u", (unsigned)(cols[i] >> 32), (unsigned)cols[i]);
                    SD.rmdir(p); // only when empty
                    if (i + 1 == cols.size() || (cols[i + 1] >> 32) != (cols[i] >> 32)) {
                        snprintf(p, sizeof(p), "/hoptics/tiles/%u", (unsigned)(cols[i] >> 32));
                        SD.rmdir(p);
                    }
                }
            });
    }
    ILOG_INFO("Hoptics map: cleared %u saved tiles", (unsigned)saved.size());
    saved.clear();
    savedBytes = 0;
    dirMade = 0;
    indexDirty = true;
    flushIndex(true);
}
#else
// ---- PC harness: plain files under hostRoot, a pretend 3.4 MB flash with 600 KB of node files on it
namespace
{
size_t fsFreeHost(uint32_t savedBytes)
{
    const size_t total = 3456 * 1024, other = 600 * 1024;
    return total > other + savedBytes ? total - other - savedBytes : 0;
}
bool onCard(void) { return false; }
uint64_t capBytes(void) { return HOPTICS_MAP_CACHE_KB * 1024ull; }
uint64_t reserveBytes(void) { return HOPTICS_MAP_FS_RESERVE_KB * 1024ull; }
void findPacks(void) {}
bool packTile(uint8_t, uint32_t, uint32_t, Blob &) { return false; }
} // namespace

void TileMap::flushIndex(bool) { indexDirty = false; }

void TileMap::loadIndex(void)
{
    if (indexLoaded)
        return;
    indexLoaded = true;
    DIR *root = opendir(hostRoot.c_str());
    if (!root)
        return;
    while (dirent *d = readdir(root)) {
        if (d->d_name[0] == '.')
            continue;
        int z = atoi(d->d_name);
        std::string dir = hostRoot + "/" + d->d_name;
        DIR *zd = opendir(dir.c_str());
        if (!zd)
            continue;
        dirMade |= 1u << z;
        while (dirent *f = readdir(zd)) {
            unsigned x, y;
            if (sscanf(f->d_name, "%u_%u.hmt", &x, &y) != 2)
                continue;
            struct stat st;
            if (::stat((dir + "/" + f->d_name).c_str(), &st) != 0)
                continue;
            uint32_t bytes = ((uint32_t)st.st_size + kBlock - 1) / kBlock * kBlock;
            saved.push_back({(uint8_t)z, x, y, bytes});
            savedBytes += bytes;
        }
        closedir(zd);
    }
    closedir(root);
    ILOG_INFO("Hoptics map: %u saved tiles, %u KB", (unsigned)saved.size(), (unsigned)(savedBytes / 1024));
}

bool TileMap::readSaved(uint8_t z, uint32_t x, uint32_t y, Blob &out, uint32_t &fetched, uint32_t &maxAge, char *etag)
{
    auto it = std::find_if(saved.begin(), saved.end(), [&](const Saved &s) { return s.z == z && s.x == x && s.y == y; });
    if (it == saved.end())
        return false;
    char path[256];
    tilePath(path, sizeof(path), z, x, y);
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    SavedHeader h;
    bool ok = fread(&h, 1, sizeof(h), f) == sizeof(h) && memcmp(h.magic, kMagic, 4) == 0 &&
              (long)(h.pngLen + sizeof(h)) == size && h.pngLen <= kMaxTileBytes && out.reserve(h.pngLen) &&
              fread(out.data, 1, h.pngLen, f) == h.pngLen;
    fclose(f);
    if (!ok)
        return false;
    out.len = h.pngLen;
    fetched = h.fetched;
    maxAge = h.maxAge;
    h.etag[sizeof(h.etag) - 1] = 0;
    strcpy(etag, h.etag);
    return true;
}

bool TileMap::save(uint8_t z, uint32_t x, uint32_t y, const uint8_t *pngData, size_t pngLen, uint32_t maxAge, const char *etag)
{
    const uint32_t bytes = ((uint32_t)(pngLen + sizeof(SavedHeader)) + kBlock - 1) / kBlock * kBlock;
    forget(z, x, y);
    if (!makeRoom(bytes)) {
        std::lock_guard<std::mutex> lk(statLock);
        stat.storageFull = true;
        return false;
    }
    std::string dir = hostRoot + "/" + std::to_string(z);
    mkdir(hostRoot.c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    dirMade |= 1u << z;
    SavedHeader h{};
    memcpy(h.magic, kMagic, 4);
    h.fetched = wallClock();
    h.maxAge = maxAge;
    h.pngLen = (uint32_t)pngLen;
    if (etag && strlen(etag) < sizeof(h.etag))
        strcpy(h.etag, etag);
    char path[256];
    tilePath(path, sizeof(path), z, x, y);
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(&h, 1, sizeof(h), f) == sizeof(h) && fwrite(pngData, 1, pngLen, f) == pngLen;
    if (f)
        fclose(f);
    if (!ok) {
        remove(path);
        std::lock_guard<std::mutex> lk(statLock);
        stat.storageFull = true;
        return false;
    }
    saved.push_back({z, x, y, bytes});
    savedBytes += bytes;
    std::lock_guard<std::mutex> lk(statLock);
    stat.storageFull = false;
    return true;
}

void TileMap::touchSaved(uint8_t z, uint32_t x, uint32_t y, uint32_t maxAge, const char *etag)
{
    char path[256];
    tilePath(path, sizeof(path), z, x, y);
    FILE *f = fopen(path, "r+b");
    if (!f)
        return;
    SavedHeader h;
    if (fread(&h, 1, sizeof(h), f) == sizeof(h) && memcmp(h.magic, kMagic, 4) == 0) {
        h.fetched = wallClock();
        if (maxAge)
            h.maxAge = maxAge;
        if (etag && strlen(etag) < sizeof(h.etag))
            strcpy(h.etag, etag);
        fseek(f, 0, SEEK_SET);
        fwrite(&h, 1, sizeof(h), f);
    }
    fclose(f);
}

void TileMap::forget(uint8_t z, uint32_t x, uint32_t y)
{
    auto it = std::find_if(saved.begin(), saved.end(), [&](const Saved &s) { return s.z == z && s.x == x && s.y == y; });
    if (it == saved.end())
        return;
    char path[256];
    tilePath(path, sizeof(path), z, x, y);
    remove(path);
    savedBytes -= it->bytes;
    saved.erase(it);
}

void TileMap::wipeSaved(void)
{
    for (const Saved &s : saved) {
        char path[256];
        tilePath(path, sizeof(path), s.z, s.x, s.y);
        remove(path);
    }
    for (int z = 0; z < 32; z++)
        if (dirMade & (1u << z))
            rmdir((hostRoot + "/" + std::to_string(z)).c_str());
    rmdir(hostRoot.c_str());
    ILOG_INFO("Hoptics map: cleared %u saved tiles", (unsigned)saved.size());
    saved.clear();
    savedBytes = 0;
    dirMade = 0;
}
#endif

// keep the budget (flash or card): drop the saved tiles farthest from what is on screen until `bytes` fits
bool TileMap::makeRoom(uint32_t bytes)
{
    const uint64_t cap = capBytes(), reserve = reserveBytes();
    if (bytes > cap)
        return false;
    uint64_t freed = 0; // deleted during this call
    // centre of the current view as a fraction of the world
    const double wz = (double)(256u << cur.z);
    const double cx = (cur.ox + cur.w / 2) / wz, cy = (cur.oy + cur.h / 2) / wz;
    for (;;) {
#if defined(ARDUINO_ARCH_ESP32)
        const uint64_t free = storeFree(freed);
#else
        const uint64_t free = fsFreeHost(savedBytes);
#endif
        if ((uint64_t)savedBytes + bytes <= cap && free >= bytes + reserve)
            return true;
        if (saved.empty())
            return false;
        size_t worst = 0;
        double worstD = -1;
        for (size_t i = 0; i < saved.size(); i++) {
            const Saved &s = saved[i];
            const double n = (double)(1u << s.z);
            const double dx = (s.x + 0.5) / n - cx, dy = (s.y + 0.5) / n - cy;
            // far away first; at the same spot, the most zoomed-in tiles go first (they cover least)
            const double d = dx * dx + dy * dy + s.z * 1e-12;
            if (d > worstD) {
                worstD = d;
                worst = i;
            }
        }
        const Saved s = saved[worst];
        forget(s.z, s.x, s.y);
        freed += s.bytes;
    }
}

// ------------------------------------------------------------------ network
bool TileMap::online(void) const
{
#if defined(ARDUINO_ARCH_ESP32)
    return WiFi.status() == WL_CONNECTED;
#else
    return hostOnline.load();
#endif
}

int TileMap::fetch(uint8_t z, uint32_t x, uint32_t y, const char *etag, Blob &out, char *newEtag, uint32_t &maxAge)
{
    newEtag[0] = 0;
    maxAge = 0;
#if defined(ARDUINO_ARCH_ESP32)
    // one TLS client, kept open between tiles; the server certificate must chain to HOPTICS_MAP_ROOTS
    if (!tls) {
        tls = new WiFiClientSecure();
        tls->setCACert(HOPTICS_MAP_ROOTS);
        tls->setHandshakeTimeout(15);
    }
    if (!http) {
        http = new HTTPClient();
        http->setReuse(true);
        http->setConnectTimeout(8000);
        http->setTimeout(10000);
        http->setUserAgent(kUserAgent);
    }
    char url[96];
    snprintf(url, sizeof(url), HOPTICS_MAP_URL, (unsigned)z, (unsigned)x, (unsigned)y);
    if (!http->begin(*tls, url))
        return -100;
    http->addHeader("Accept", "image/png");
    if (etag && etag[0])
        http->addHeader("If-None-Match", etag);
    const char *keys[] = {"ETag", "Cache-Control"};
    http->collectHeaders(keys, 2);
    int code = http->GET();
    if (code == 200 || code == 304) {
        String et = http->header("ETag");
        if (et.length() > 0 && et.length() < 48)
            strcpy(newEtag, et.c_str());
        String cc = http->header("Cache-Control");
        int p = cc.indexOf("max-age=");
        if (p >= 0)
            maxAge = (uint32_t)strtoul(cc.c_str() + p + 8, nullptr, 10);
    }
    if (code == 200) {
        int len = http->getSize();
        if (len > (int)kMaxTileBytes) {
            http->end();
            return -101;
        }
        if (!out.reserve(len > 0 ? (size_t)len : 32768)) {
            http->end();
            return -103;
        }
        BlobStream s(out);
        int n = http->writeToStream(&s);
        if (n <= 0 || out.overflow || (len > 0 && (int)out.len != len)) {
            http->end();
            return -102;
        }
    }
    http->end();
    if (code < 0) {
        char err[96] = "";
        tls->lastError(err, sizeof(err));
        ILOG_WARN("Hoptics map: tile server failed: %d %s", code, err); // not the address: it holds the tile's place
        tls->stop(); // start over with a fresh connection next time
    }
    return code;
#else
    if (!hostOnline.load())
        return -1;
    char path[512];
    snprintf(path, sizeof(path), "%s/%u/%u/%u.png", hostSource.c_str(), (unsigned)z, (unsigned)x, (unsigned)y);
    FILE *f = fopen(path, "rb");
    if (!f)
        return 404;
    char tag[48];
    snprintf(tag, sizeof(tag), "\"%u-%u-%u\"", (unsigned)z, (unsigned)x, (unsigned)y);
    maxAge = 7 * 24 * 3600;
    strcpy(newEtag, tag);
    if (etag && strcmp(etag, tag) == 0) {
        fclose(f);
        return 304;
    }
    uint8_t b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0)
        if (!out.append(b, n, kMaxTileBytes))
            break;
    fclose(f);
    sleepMs(5); // pretend network
    return out.overflow ? -102 : 200;
#endif
}

void TileMap::hostSetup(const char *cacheDir, const char *sourceDir)
{
#if !defined(ARDUINO_ARCH_ESP32)
    hostRoot = std::string(cacheDir) + "/hmap";
    hostSource = sourceDir;
    mkdir(cacheDir, 0755);
#else
    (void)cacheDir;
    (void)sourceDir;
#endif
}

void TileMap::hostSetOnline(bool on)
{
#if !defined(ARDUINO_ARCH_ESP32)
    hostOnline = on;
#else
    (void)on;
#endif
}

} // namespace hoptics
