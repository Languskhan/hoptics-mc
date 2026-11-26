#pragma once
/**
 * Hoptics street map - OpenStreetMap raster tiles drawn into one RGB565 view buffer. The same engine as Hoptics MT
 * (same author), with the flash cache on MeshCore's SPIFFS.
 *
 * The map tab hands this engine a view (zoom level + a window in world pixels). A worker task
 * fills the buffer: tiles already saved on flash first, then - only while Wi-Fi is up - downloads
 * of the tiles that are actually in view (HTTPS, server checked against pinned root CAs), each one
 * saved so the same area still shows later without Wi-Fi.
 *
 * tile.openstreetmap.org usage policy, which this follows: no prefetching or "download an area for
 * offline", only tiles in view; a distinct User-Agent; keep tiles at least 7 days (or what the
 * server's Cache-Control says) and re-check expired ones with a conditional request; show
 * "(c) OpenStreetMap" on the map. https://operations.osmfoundation.org/policies/tiles/
 *
 * The engine never touches LVGL objects: the UI shows the buffer as an image and polls takeDirty().
 * Apart from a thin platform layer it is plain C++, so the PC harness runs the same code. MIT licence.
 */
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#ifndef HOPTICS_MAP_URL
#define HOPTICS_MAP_URL "https://tile.openstreetmap.org/%u/%u/%u.png" // z, x, y
#endif
#ifndef HOPTICS_MAP_CACHE_KB
#define HOPTICS_MAP_CACHE_KB 2048 // flash used for saved tiles at most
#endif
#ifndef HOPTICS_MAP_FS_RESERVE_KB
#define HOPTICS_MAP_FS_RESERVE_KB 1024 // the node's own files always keep at least this much free space
#endif
#ifndef HOPTICS_MAP_MIN_ZOOM
#define HOPTICS_MAP_MIN_ZOOM 3
#endif
#ifndef HOPTICS_MAP_MAX_ZOOM
#define HOPTICS_MAP_MAX_ZOOM 18
#endif

class PNG;
struct png_draw_tag;

namespace hoptics
{

enum class MapStyle : uint8_t { Light, Dark, Red, Amber };

// One window onto the map. World size at zoom z is (256 << z) pixels square (Web Mercator).
struct MapView {
    uint8_t z = 0;
    int32_t ox = 0, oy = 0; // world pixel of the view's top-left corner
    int16_t w = 0, h = 0;
    MapStyle style = MapStyle::Dark;
    uint16_t bg = 0; // RGB565 colour where there is no tile
    bool same(const MapView &o) const
    {
        return z == o.z && ox == o.ox && oy == o.oy && w == o.w && h == o.h && style == o.style && bg == o.bg;
    }
};

// growable byte buffer for one tile (PSRAM when there is some)
struct Blob {
    uint8_t *data = nullptr;
    size_t len = 0, cap = 0;
    bool overflow = false;
    Blob() = default;
    Blob(const Blob &) = delete;
    Blob &operator=(const Blob &) = delete;
    ~Blob();
    bool reserve(size_t n);
    bool append(const uint8_t *p, size_t n, size_t max);
    void reset(void);
};

struct MapStatus {
    enum Problem : uint8_t { NONE, OFFLINE, BUSY, NO_LINK };
    bool enabled = true;
    bool online = false;
    uint8_t need = 0;    // tiles covering the view
    uint8_t shown = 0;   // ...of those drawn
    uint8_t pending = 0; // ...still to download
    Problem problem = NONE;
    bool storageFull = false; // the last tile could not be saved (still shown)
    uint32_t savedTiles = 0;
    uint32_t savedKB = 0;
};

class TileMap
{
  public:
    static TileMap &get(void);

    // ---- UI task ----
    // allocator for the two long-lived blocks (view buffer, PNG decoder), e.g. lv_malloc; default malloc
    void setAllocator(void *(*alloc)(size_t), void (*release)(void *));
    // the view buffer: RGB565, row stride = w. Allocated on first use and again only if the size changes.
    uint16_t *buffer(int16_t w, int16_t h);
    // draw this view (cheap when nothing changed)
    void request(const MapView &v);
    // after a drag: move what is drawn by (dx, dy), fill the uncovered edges, then draw view v
    void shiftAndRequest(int32_t dx, int32_t dy, const MapView &v);
    // true once each time the worker changed the buffer
    bool takeDirty(void) { return dirty.exchange(false); }
    void setEnabled(bool on);
    bool isEnabled(void) const { return enabled.load(); }
    MapStatus status(void);
    void clearSaved(void);
    // start the worker (it reads the saved-tile index) without drawing anything yet
    void wake(void) { startWorker(); }

    // ---- any task (the web page) ----
    // a tile from the user's own tile pack (z/x/y.png), saved as if downloaded and kept a year. Waits for the
    // worker to write it (up to waitMs).
    enum ImportResult { IMPORTED, FULL, BAD, BUSY };
    ImportResult importTile(uint8_t z, uint32_t x, uint32_t y, const uint8_t *png, size_t len, uint32_t waitMs);

    // ---- PC harness ----
    // tiles are read from sourceDir/z/x/y.png instead of the internet; saved ones go to cacheDir
    static void hostSetup(const char *cacheDir, const char *sourceDir);
    static void hostSetOnline(bool on);
    // the worker has finished everything it can do for the current view
    bool idle(void) const { return idleGen.load() == gen.load() && !working.load(); }

  private:
    struct Tile {
        int32_t tx, ty;   // unwrapped tile coordinates (screen position = tx * 256 - ox)
        uint32_t x, y;    // wrapped server coordinates
        bool inWorld;     // ty is inside the map (no tile above the north / below the south edge)
        bool shown;       // drawn for this view
        bool gone;        // the server has no such tile (404) - nothing to wait for
        bool stale;       // drawn from flash but due for a re-check
        char etag[48];
    };
    struct Saved {
        uint8_t z;
        uint32_t x, y;
        uint32_t bytes; // space on flash, rounded up to whole blocks
    };
    struct DrawCtx {
        TileMap *self;
        const MapView *v;
        int32_t dx, dy; // tile's top-left corner in the view
        uint32_t gen;
        bool aborted;
        bool complete;
    };

    TileMap() = default;
    void startWorker(void);
    void workerLoop(void);
    bool retire(uint32_t g);
    void renderView(uint32_t g);
    void downloadMissing(uint32_t g);
    void revalidate(uint32_t g);
    bool drawPng(const uint8_t *data, size_t len, const Tile &t, const MapView &v, uint32_t g);
    static int drawRow(png_draw_tag *d);
    void fillTile(const Tile &t, const MapView &v, uint32_t g);
    void setProblem(MapStatus::Problem p);
    void publishStatus(void);

    // flash cache
    void loadIndex(void);
    bool readSaved(uint8_t z, uint32_t x, uint32_t y, Blob &png, uint32_t &fetched, uint32_t &maxAge, char *etag);
    bool save(uint8_t z, uint32_t x, uint32_t y, const uint8_t *png, size_t len, uint32_t maxAge, const char *etag);
    struct Import {
        uint8_t z;
        uint32_t x, y;
        const uint8_t *png;
        size_t len;
        ImportResult result;
    };
    std::mutex impLock; // guards imp; the worker writes it while holding the lock
    Import *imp = nullptr;
    std::atomic<bool> impWaiting{false};
    void takeImport(void);
    void touchSaved(uint8_t z, uint32_t x, uint32_t y, uint32_t maxAge, const char *etag);
    void forget(uint8_t z, uint32_t x, uint32_t y);
    bool makeRoom(uint32_t bytes);
    void wipeSaved(void);

    // network
    bool online(void) const;
    int fetch(uint8_t z, uint32_t x, uint32_t y, const char *etag, Blob &out, char *newEtag, uint32_t &maxAge);

    // state shared with the UI task
    std::mutex bufLock; // guards buf/bufW/bufH and every write into the buffer
    uint16_t *buf = nullptr;
    int16_t bufW = 0, bufH = 0;
    std::mutex reqLock; // guards req
    MapView req;
    std::atomic<uint32_t> gen{0};     // bumped by every new view
    std::atomic<uint32_t> idleGen{0}; // last generation the worker finished
    std::atomic<bool> working{false};
    std::atomic<bool> dirty{false};
    std::atomic<bool> enabled{true};
    std::atomic<bool> wipeRequested{false};
    std::atomic<bool> started{false}; // a worker is running (it ends itself when idle, to give its memory back)
    std::atomic<bool> incomplete{false}; // it ended with tiles still to fetch (waiting for Wi-Fi)
    std::mutex statLock;
    MapStatus stat;
    void *(*longAlloc)(size_t) = nullptr;
    void (*longFree)(void *) = nullptr;

    // worker-only state
    PNG *png = nullptr;
    uint16_t row[256] = {}; // one decoded tile line (scratch)
    MapView cur;             // view being drawn
    std::vector<Tile> tiles; // tiles of cur
    std::vector<Saved> saved;
    bool indexLoaded = false;
    uint32_t savedBytes = 0;
    uint32_t dirMade = 0; // bit per zoom level whose directory exists
    uint32_t backoffUntil = 0, backoffMs = 0;
    std::vector<uint64_t> notFound; // z/x/y the server answered 404 for (not asked again this boot)
};

} // namespace hoptics
