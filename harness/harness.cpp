// Hoptics-MC PC harness: renders every screen with demo data into PPM files, no radio needed.
//   harness <W> <H> <outdir>
// MIT licence, see LICENSE.
#include "hx_ui.h"
#include "lvgl.h"
#include "map/hx_tiles.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <array>
#include <functional>
#include <chrono>
#include <map>
#include <string>
#include <vector>

static int WD = 320, HT = 240;
static uint16_t *fb;
static uint32_t fakeMs = 0;
static const uint32_t NOW = 1795640000; // a fixed "now" so screenshots are repeatable

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    const uint16_t *s = (const uint16_t *)px;
    for (int y = a->y1; y <= a->y2; y++)
        for (int x = a->x1; x <= a->x2; x++)
            fb[y * WD + x] = *s++;
    lv_display_flush_ready(d);
}
static uint32_t tick_cb(void) { return fakeMs; }
static hx::App *gApp = nullptr;

class Fake : public hx::Model
{
  public:
    std::vector<hx::Contact> contacts;
    std::vector<hx::Channel> channels;
    std::map<std::string, int> prefs;
    bool bt = true, rep = false;
    hx::Radio rad;
    uint32_t ver = 1, nextId = 100;
    char name[32] = "Bluffs T-Deck";

    Fake()
    {
        struct D {
            const char *n;
            hx::NodeType t;
            int hops;
            int agoS;
            double la, lo;
        } d[] = {{"Folsom Ridge Rptr", hx::NodeType::Repeater, 0, 40, 38.69, -121.17},
                 {"Citrus Ridge Rptr", hx::NodeType::Repeater, 1, 300, 38.62, -121.30},
                 {"Hilltop Base", hx::NodeType::Chat, 0, 90, 38.58, -121.44},
                 {"Auburn W6EK", hx::NodeType::Repeater, 2, 1200, 38.90, -121.07},
                 {"SacMesh Room", hx::NodeType::Room, 1, 2400, 0, 0},
                 {"Trail Tracker", hx::NodeType::Chat, 3, 600, 38.75, -121.10},
                 {"Weather Stn", hx::NodeType::Sensor, 1, 5000, 38.66, -121.20},
                 {"Dennis Mobile", hx::NodeType::Chat, -1, 9000, 0, 0},
                 {"Rocklin Hill", hx::NodeType::Repeater, 2, 700, 38.79, -121.24}};
        for (auto &x : d) {
            hx::Contact c;
            strncpy(c.name, x.n, sizeof(c.name) - 1);
            c.type = x.t;
            c.pathLen = x.hops;
            c.lastHeard = NOW - x.agoS;
            c.lastAdvert = NOW - x.agoS - 30;
            for (int i = 0; i < 6; i++)
                c.key[i] = (uint8_t)(x.n[i] * 31 + i * 17);
            if (x.la != 0) {
                c.hasPos = true;
                c.lat = (int32_t)(x.la * 1e6);
                c.lon = (int32_t)(x.lo * 1e6);
            }
            contacts.push_back(c);
        }
        const char *ch[] = {"Public", "smf", "alerts"};
        for (int i = 0; i < 3; i++) {
            hx::Channel c;
            c.index = i;
            strcpy(c.name, ch[i]);
            channels.push_back(c);
        }
    }
    std::map<std::string, int32_t> settings = {{"hash_mode", 1}, {"multi_acks", 1}, {"rx_boost", 1}, {"cad", 0}, {"add_mode", 1},
                                               {"add_chat", 1}, {"add_rep", 1}, {"add_room", 0}, {"add_sensor", 0}, {"add_hops", 0},
                                               {"add_full", 1}, {"gps", 1}, {"gps_int", 60}, {"adv_loc", 1}, {"tel_base", 1},
                                               {"tel_loc", 0}, {"tel_env", 1}, {"tz", -8}, {"ble_pin", 0}, {"reboot", 0}};
    std::string scope;
    bool getSetting(const char *k, int32_t &v) const override
    {
        auto it = settings.find(k);
        if (it == settings.end())
            return false;
        v = it->second;
        return true;
    }
    bool setSetting(const char *k, int32_t v) override
    {
        if (!settings.count(k))
            return false;
        settings[k] = v;
        return true;
    }
    bool getSettingText(const char *k, char *out, size_t n) const override
    {
        if (strcmp(k, "scope"))
            return false;
        snprintf(out, n, "%s", scope.c_str());
        return true;
    }
    bool setSettingText(const char *k, const char *t) override
    {
        if (strcmp(k, "scope"))
            return false;
        scope = t;
        return true;
    }
    hx::Role role() const override { return hx::Role::Companion; }
    const char *nodeName() const override { return name; }
    void pubKey(uint8_t out[32]) const override
    {
        for (int i = 0; i < 32; i++)
            out[i] = (uint8_t)(i * 37 + 11);
    }
    const char *firmwareVersion() const override { return "0.1.0 (MeshCore 1.18)"; }
    const char *boardName() const override { return WD > HT ? "LilyGO T-Deck Plus" : "Heltec V4 + touch kit"; }
    uint32_t now() const override { return NOW; }
    int batteryPercent() const override { return 82; }
    uint16_t batteryMv() const override { return 3950; }
    bool appConnected() const override { return true; }
    bool gpsFix(int32_t &la, int32_t &lo) const override
    {
        la = 38691000, lo = -121234000;
        return true;
    }
    hx::Radio radio() const override { return rad; }
    hx::Stats stats() const override
    {
        hx::Stats s;
        s.rx = 1834, s.tx = 211, s.uptimeS = 3 * 3600 + 25 * 60, s.noiseFloor = -112;
        return s;
    }
    size_t contactCount() const override { return contacts.size(); }
    bool contactAt(size_t i, hx::Contact &o) const override
    {
        if (i >= contacts.size())
            return false;
        o = contacts[i];
        return true;
    }
    bool findContact(const uint8_t k[6], hx::Contact &o) const override
    {
        for (auto &c : contacts)
            if (!memcmp(c.key, k, 6)) {
                o = c;
                return true;
            }
        return false;
    }
    size_t channelCount() const override { return channels.size(); }
    bool channelAt(size_t i, hx::Channel &o) const override
    {
        if (i >= channels.size())
            return false;
        o = channels[i];
        return true;
    }
    uint32_t contactsVersion() const override { return ver; }
    uint32_t sendText(const hx::ConvKey &, const char *) override { return nextId++; }
    bool sendAdvert(bool) override { return true; }
    bool resetPath(const uint8_t *) override { return true; }
    bool setNodeName(const char *n) override
    {
        strncpy(name, n, 31);
        return true;
    }
    bool setRadio(const hx::Radio &r) override
    {
        rad = r;
        return true;
    }
    int updateAns = -1; // the last answer to the update question: 1 install, 0 keep this build
    void updateAnswer(bool install) override { updateAns = install; }
    bool bluetoothEnabled() const override { return bt; }
    uint32_t bluetoothPin() const override { return 482913; }
    bool setBluetooth(bool on) override
    {
        bt = on;
        return true;
    }
    bool repeatEnabled() const override { return rep; }
    bool setRepeat(bool on) override
    {
        rep = on;
        return true;
    }
    int uiPref(const char *k, int d) const override
    {
        auto it = prefs.find(k);
        return it == prefs.end() ? d : it->second;
    }
    void setUiPref(const char *k, int v) override { prefs[k] = v; }
    void setBacklight(uint8_t) override {}
    bool hasKeyboard() const override { return WD > HT; } // portrait = the Heltec kits: on-screen keyboard
    bool hasKeyLight() const override { return WD > HT; }  // the T-Deck's keyboard light
    bool hasSound() const override { return true; }        // the More > Sound card
    int keyLightLevel = -1;
    void setKeyLight(uint8_t v) override { keyLightLevel = v; }
    // Wi-Fi: connected, with a few networks around
    bool wifiSupported() const override { return true; }
    hx::WifiStatus wifi() const override
    {
        hx::WifiStatus w;
        w.state = hx::WifiStatus::CONNECTED;
        strcpy(w.ssid, "Home-WiFi");
        strcpy(w.ip, "192.168.1.50");
        w.rssi = -58;
        w.clockFromInternet = true;
        return w;
    }
    // the live feed: on, connected
    bool feedOn = true;
    std::string feedSrv = "scope.sacmesh.org";
    bool feedSupported() const override { return true; }
    hx::FeedStatus feed() const override
    {
        hx::FeedStatus f;
        f.state = feedOn ? hx::FeedStatus::LIVE : hx::FeedStatus::OFF;
        snprintf(f.server, sizeof(f.server), "%s", feedSrv.c_str());
        f.packets = 1834;
        f.messages = 2;
        return f;
    }
    void setFeed(bool on) override { feedOn = on; }
    bool setFeedServer(const char *u) override
    {
        feedSrv = u;
        return true;
    }
    int wifiScanResults(hx::WifiNet *out, int max) override
    {
        const char *n[4] = {"Home-WiFi", "Garage-AP", "xfinitywifi", "Neighbors 2.4"};
        const int8_t r[4] = {-58, -66, -79, -86};
        int c = std::min(4, max);
        for (int i = 0; i < c; i++) {
            strcpy(out[i].ssid, n[i]);
            out[i].rssi = r[i];
            out[i].open = i == 2;
        }
        return c;
    }

    // a repeater being managed: answers a moment later, as one would over the air
    std::vector<std::pair<uint32_t, std::function<void()>>> later;
    std::map<std::string, std::string> rset = {{"name", "Folsom Ridge Rptr"}, {"lat", "38.69"}, {"lon", "-121.17"},
                                               {"radio", "909.875,62.5,9,5"}, {"tx", "22"}, {"radio.rxgain", "on"},
                                               {"cad", "off"}, {"repeat", "on"}, {"flood.max", "64"},
                                               {"flood.max.advert", "8"}, {"loop.detect", "minimal"}, {"path.hash.mode", "1"},
                                               {"advert.interval", "120"}, {"flood.advert.interval", "12"}, {"af", "1.0"},
                                               {"rxdelay", "0.0"}, {"txdelay", "0.5"}, {"direct.txdelay", "0.3"},
                                               {"guest.password", "guest"}, {"allow.read.only", "off"}};
    std::map<std::string, std::string> kept, files;
    bool saveData(const char *n, const std::string &d) override
    {
        files[n] = d;
        return true;
    }
    bool loadData(const char *n, std::string &o) const override
    {
        auto it = files.find(n);
        if (it == files.end())
            return false;
        o = it->second;
        return true;
    }
    static std::array<uint8_t, 6> K(const uint8_t *k)
    {
        std::array<uint8_t, 6> a;
        memcpy(a.data(), k, 6);
        return a;
    }
    uint32_t remoteLogin(const uint8_t k[6], const char *pw) override
    {
        auto key = K(k);
        bool ok = strcmp(pw, "wrong") != 0;
        if (ok)
            later.push_back({fakeMs + 700, [=] { gApp->onRemoteLogin(key.data(), true, 3); }});
        return 6000;
    }
    uint32_t remoteStatus(const uint8_t k[6]) override
    {
        auto key = K(k);
        later.push_back({fakeMs + 900, [=] {
                             hx::RemoteStatus s;
                             s.battMv = 4120, s.txQueue = 0, s.noiseFloor = -116, s.lastRssi = -97, s.lastSnr4 = 26;
                             s.rx = 48210, s.tx = 12904, s.airtimeS = 3100, s.uptimeS = 9 * 86400 + 4 * 3600 + 1200;
                             s.sentFlood = 11020, s.sentDirect = 1884, s.recvFlood = 40211, s.recvDirect = 7999;
                             s.dupFlood = 18230, s.dupDirect = 412, s.hasRx = true, s.rxAirtimeS = 21400, s.rxErrors = 311;
                             gApp->onRemoteStatus(key.data(), s);
                         }});
        return 6000;
    }
    uint32_t remoteNeighbours(const uint8_t k[6]) override
    {
        auto key = K(k);
        later.push_back({fakeMs + 900, [=] {
                             hx::RemoteNeighbour n[5];
                             const int idx[5] = {1, 3, 8, -1, 5};
                             const int8_t snr[5] = {38, 21, 6, -14, -31};
                             const uint32_t ago[5] = {95, 640, 1800, 7300, 26000};
                             for (int i = 0; i < 5; i++) {
                                 if (idx[i] >= 0)
                                     memcpy(n[i].key, contacts[idx[i]].key, 6);
                                 else
                                     memcpy(n[i].key, "\xa1\x07\x5c\x33\x90\x12", 6);
                                 n[i].snr4 = snr[i], n[i].heardAgoS = ago[i];
                             }
                             gApp->onRemoteNeighbours(key.data(), 5, n, 5);
                         }});
        return 6000;
    }
    uint32_t remoteTelemetry(const uint8_t k[6]) override
    {
        auto key = K(k);
        later.push_back({fakeMs + 900, [=] {
                             const uint8_t lpp[] = {1, 116, 0x01, 0x9C, 1, 103, 0x01, 0x3B, 2, 104, 90, 2, 115, 0x27, 0x94};
                             gApp->onRemoteTelemetry(key.data(), lpp, sizeof(lpp));
                         }});
        return 6000;
    }
    uint32_t remoteAccessList(const uint8_t k[6]) override
    {
        auto key = K(k);
        later.push_back({fakeMs + 900, [=] {
                             hx::RemoteClient c[2];
                             memcpy(c[0].key, contacts[2].key, 6);
                             c[0].perms = 3;
                             memcpy(c[1].key, "\x5e\x11\x02\xc8\x7a\x44", 6);
                             c[1].perms = 2;
                             gApp->onRemoteAccessList(key.data(), c, 2);
                         }});
        return 6000;
    }
    uint32_t remoteCommand(const uint8_t k[6], const char *cmd) override
    {
        auto key = K(k);
        std::string c = cmd, r;
        if (c.rfind("get ", 0) == 0) {
            auto it = rset.find(c.substr(4));
            r = it != rset.end() ? "> " + it->second : (c == "get owner.info" ? "> " : "??: " + c.substr(4));
        } else if (c.rfind("set ", 0) == 0) {
            size_t sp = c.find(' ', 4);
            rset[c.substr(4, sp - 4)] = c.substr(sp + 1);
            r = "OK";
        } else if (c == "ver")
            r = "v1.11.0 (Build: 12-Nov-2026)";
        else if (c == "clock")
            r = "02:13 - 26/11/2026 UTC";
        else if (c == "neighbors")
            r = "5ad3e1:120:38\n7c0b2a:640:21";
        else
            r = "OK";
        later.push_back({fakeMs + 500, [=] { gApp->onRemoteReply(key.data(), r.c_str()); }});
        return 5000;
    }
    bool savedPassword(const uint8_t k[6], char *out, size_t n) const override
    {
        auto it = kept.find(std::string((const char *)k, 6));
        if (it == kept.end())
            return false;
        snprintf(out, n, "%s", it->second.c_str());
        return true;
    }
    void savePassword(const uint8_t k[6], const char *pw) override
    {
        if (pw && *pw)
            kept[std::string((const char *)k, 6)] = pw;
        else
            kept.erase(std::string((const char *)k, 6));
    }
};

static std::string outDir = "shots";
static Fake *gFake = nullptr;
static void pump(int ms)
{
    for (int i = 0; i < ms; i += 10) {
        fakeMs += 10;
        if (gFake) // answers that are due
            for (size_t j = 0; j < gFake->later.size(); j++)
                if (gFake->later[j].first <= fakeMs) {
                    auto f = gFake->later[j].second;
                    gFake->later.erase(gFake->later.begin() + j--);
                    f();
                }
        if (gApp)
            gApp->loop(fakeMs);
        lv_timer_handler();
    }
    lv_refr_now(nullptr);
}
namespace hx {
struct HarnessAccess {
    static lv_obj_t *cust(App &a, int i) { return i == 0 ? a.custFreq : i == 1 ? a.custBw : i == 2 ? a.custSf : a.custCr; }
    static lv_obj_t *preset(App &a) { return a.setPreset; }
    static void apply(App &a) { a.applyCustom(); }
    static void stat(App &a, int i) { a.openStat(i); }
    static void remotePage(App &a, int p) { a.rmShowPage(p); }
    static void saveChats(App &a) { a.saveChats(); }
    static void lock(App &a) { a.lock(); }
    static lv_obj_t *volume(App &a) { return a.setVol; }
    static void more(App &a, int s) { a.openMore(s); }
    static int tab(App &a) { return a.tab; }
    static int sec(App &a) { return a.moreOpen; }
    static void askApply(App &a) { a.askApply(); }
    static bool asking(App &a) { return a.askBox && !lv_obj_has_flag(a.askBox, LV_OBJ_FLAG_HIDDEN); }
    static lv_obj_t *quick(App &a, int i) { return a.quickTile[i]; }
    static void unlock(App &a) { a.unlock(); }
    static lv_obj_t *feedField(App &a) { return a.feedUrl; }
    static void tapChatInput(App &a) { lv_obj_send_event(a.chatInput, LV_EVENT_CLICKED, nullptr); }
    // the bubble of the newest message (mine or someone else's) in the open chat
    static lv_obj_t *bubble(App &a, bool mine)
    {
        const auto &ls = a.conv(a.chatKey).lines;
        for (auto it = ls.rbegin(); it != ls.rend(); ++it)
            if (it->mine == mine)
                for (uint32_t i = 0; i < lv_obj_get_child_count(a.chatMsgs); i++) {
                    lv_obj_t *b = lv_obj_get_child(lv_obj_get_child(a.chatMsgs, i), 0);
                    if (b && (uint32_t)(uintptr_t)lv_obj_get_user_data(b) == it->seq)
                        return b;
                }
        return nullptr;
    }
    static bool msgOpen(App &a) { return a.msgBox && !lv_obj_has_flag(a.msgBox, LV_OBJ_FLAG_HIDDEN); }
    static bool chatIsOpen(App &a) { return a.chatOpen; }
    static void msgAction(App &a, int act) { a.msgAction(act); }
    static std::string input(App &a) { return lv_textarea_get_text(a.chatInput); }
    static size_t lines(App &a) { return a.conv(a.chatKey).lines.size(); }
    static void refreshNodes(App &a) { a.refreshNodes(); }
    static void refreshChats(App &a) { a.refreshChats(); }
    static void renderChat(App &a) { a.renderChat(); }
    static void clearInput(App &a) { lv_textarea_set_text(a.chatInput, ""); }
    static void sendText(App &a, const char *t) { lv_textarea_set_text(a.chatInput, t); a.sendFromInput(); }
    static void keyOk(App &a, lv_obj_t *o)
    {
        a.keyClick = true;
        lv_obj_send_event(o, LV_EVENT_CLICKED, nullptr);
        a.keyClick = false;
    }
    static void tapRemoteField(App &a) { lv_obj_send_event(a.rmTypingTarget(), LV_EVENT_CLICKED, nullptr); }
    static void node(App &a, const uint8_t *k) { a.openNode(k); }
    static void remote(App &a, const uint8_t *k) { a.openRemote(k); }
    static void remoteAsk(App &a, int what) { a.rmAsk(what); }
    static void remoteScroll(App &a, int dy) { lv_obj_scroll_by_bounded(a.rmBody, 0, -dy, LV_ANIM_OFF); }
    static void channel(App &a, uint8_t ch)
    {
        ConvKey k;
        k.channel = ch;
        a.selectTab(App::T_CHATS);
        a.openChat(k);
    }
    static void history(App &a)
    {
        static const uint16_t pm[30] = {3, 5, 4, 8, 6, 2, 3, 9, 12, 7, 5, 4, 6, 3, 2, 5, 8, 11, 6, 4, 3, 7, 5, 6, 9, 4, 3, 5, 6, 4};
        for (int i = 0; i < 30; i++) {
            a.perMin[i] = pm[i];
            a.noiseHist[i] = (int16_t)(-114 + (i % 5) - (i > 20 ? 0 : 2) + (i == 17 ? 9 : 0));
        }
    }
};
} // namespace hx

static void shot(const char *name)
{
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(nullptr);
    std::string p = outDir + "/" + name + ".ppm";
    FILE *f = fopen(p.c_str(), "wb");
    fprintf(f, "P6\n%d %d\n255\n", WD, HT);
    for (int i = 0; i < WD * HT; i++) {
        uint16_t c = fb[i];
        uint8_t rgb[3] = {(uint8_t)(((c >> 11) & 0x1f) * 255 / 31), (uint8_t)(((c >> 5) & 0x3f) * 255 / 63), (uint8_t)((c & 0x1f) * 255 / 31)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("wrote %s\n", p.c_str());
}

int main(int argc, char **argv)
{
    if (argc > 2) {
        WD = atoi(argv[1]);
        HT = atoi(argv[2]);
    }
    if (argc > 3)
        outDir = argv[3];
    setenv("TZ", "PST8PDT", 1);
    fb = (uint16_t *)calloc(WD * HT, 2);
    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *d = lv_display_create(WD, HT);
    static uint16_t buf[480 * 40];
    lv_display_set_buffers(d, buf, nullptr, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush_cb);

    // the start-up screen: each picture, a step part-way, and how a stop looks
    {
        const char *board = WD > HT ? "LilyGO T-Deck Plus" : "Heltec V4 Expansion Kit";
        const int steps[3] = {0, 2, 5};
        for (int a = 0; a < 3; a++) {
            hx::bootShow(board, "Hoptics-MC v0.1.0-alpha", a);
            hx::bootStep(steps[a]);
            char nm[32];
            snprintf(nm, sizeof(nm), "00_boot_%d", a);
            shot(nm);
            lv_screen_load(lv_obj_create(nullptr));
            hx::bootDone();
        }
        hx::bootShow(board, "Hoptics-MC v0.1.0-alpha", 1);
        hx::bootStep(1);
        hx::bootError("The radio chip didn't start.", "Unplug the radio, wait 10 seconds and plug it back in.");
        shot("00_boot_error"); // App::begin() below replaces it
    }
    // the street map reads the made-up test tiles (make_test_tiles.py) as if they came over Wi-Fi
    hoptics::TileMap::hostSetup((std::string("build/tc_") + (WD > HT ? "land" : "port")).c_str(), "build/tiles");
    hoptics::TileMap::hostSetOnline(true);
    Fake model;
    gFake = &model;
    hx::App app(model);
    gApp = &app;
    app.begin();
    pump(100);
    shot("01_home");

    // traffic
    struct P {
        hx::PktType t;
        bool direct;
        int hops;
        const char *from;
        int rssi, snr4, ago;
        bool mine;
    } ps[] = {{hx::PktType::Advert, false, 2, "Auburn W6EK", -101, -18, 600, false},
              {hx::PktType::GroupText, false, 1, "Trail Tracker", -96, 14, 300, false},
              {hx::PktType::Ack, true, 0, "", -88, 30, 250, false},
              {hx::PktType::Text, true, 1, "", 0, 0, 200, true},
              {hx::PktType::Path, true, 2, "", -109, -30, 150, false},
              {hx::PktType::Advert, false, 0, "Folsom Ridge Rptr", -79, 36, 40, false},
              {hx::PktType::GroupText, false, 3, "Dennis Mobile", -112, -44, 20, false},
              {hx::PktType::Req, true, 1, "", -94, 10, 8, false}};
    for (auto &p : ps) {
        hx::Packet k;
        k.type = p.t, k.direct = p.direct, k.pathLen = p.hops, k.rssi = p.rssi, k.snr4 = p.snr4, k.time = NOW - p.ago, k.mine = p.mine;
        strncpy(k.from, p.from, sizeof(k.from) - 1);
        for (int h = 0; h < p.hops; h++)
            k.path[h] = (uint8_t)(0x3a + h * 0x41);
        k.len = 60;
        app.onPacket(k);
    }
    // messages
    auto msg = [&](bool dm, int ch, const char *from, const char *text, int ago, int hops) {
        hx::Message mm;
        mm.conv.dm = dm;
        mm.conv.channel = ch;
        if (dm)
            memcpy(mm.conv.key, model.contacts[2].key, 6);
        strncpy(mm.from, from, sizeof(mm.from) - 1);
        mm.text = text;
        mm.time = NOW - ago;
        mm.pathLen = hops;
        app.onMessage(mm);
    };
    msg(false, 1, "Trail Tracker", "Heading up the Folsom trail, anyone on?", 1500, 2);
    msg(false, 1, "Dennis Mobile", "Copy, hearing you through Rocklin Hill", 1300, 3);
    msg(false, 0, "Auburn W6EK", "Net tonight at 8 on #smf", 4000, 2);
    msg(true, 0, "Hilltop Base", "Did the new repeater come up?", 200, 0);
    msg(false, 0, "José Müller", "Ça va? Zażółć gęślą jaźń, Ærø, Straße — 5 € „ok“", 150, 1);
    msg(false, 0, "Олена Київ", "Привіт! Як справи? Сәлем, Қазақстан", 120, 2);
    msg(false, 1, "Trail Tracker", "Summit! \xF0\x9F\x8F\x94\xEF\xB8\x8F Signal is great up here \xF0\x9F\x93\xA1\xF0\x9F\x91\x8D\xF0\x9F\x98\x84 \xE2\x9D\xA4\xEF\xB8\x8F\xF0\x9F\x94\xA5", 900, 2);
    pump(4000); // let the message toast go
    app.selectTab(hx::App::T_HOME);
    pump(300);
    shot("01_home");
    { // the web page's data (checked by the run script)
        std::string js;
        app.webJson(js, 0, 0, 0, 0);
        if (FILE *f = fopen("shots/web.json", "w")) {
            fwrite(js.data(), 1, js.size(), f);
            fclose(f);
        }
        std::string again;
        app.webJson(again, model.contactsVersion(), 1u << 30, 1u << 30, 0); // the page already has everything
        if (FILE *f = fopen("shots/web_again.json", "w")) {
            fwrite(again.data(), 1, again.size(), f);
            fclose(f);
        }
    }
    app.selectTab(hx::App::T_LIVE);
    pump(300);
    shot("02_live");
    app.selectTab(hx::App::T_NODES);
    pump(300);
    shot("03_nodes");
    app.key(hx::K_DOWN);
    app.key(hx::K_DOWN);
    app.key(hx::K_DOWN);
    app.key(hx::K_DOWN);
    app.key(hx::K_DOWN);
    app.key(hx::K_DOWN);
    pump(100);
    shot("03b_nodes_keys");
    app.key(hx::K_ENTER);
    pump(300);
    shot("04_node_detail");
    app.key(hx::K_BACK);
    app.touched();
    app.selectTab(hx::App::T_CHATS);
    pump(300);
    shot("05_chats");
    app.key(hx::K_DOWN);
    app.key(hx::K_ENTER); // opens #smf
    pump(300);
    for (const char *c = "On my way"; *c; c++)
        app.key((uint8_t)*c);
    pump(200);
    shot("06_chat_typing");
    app.key(hx::K_ENTER);
    pump(200);
    app.onDelivered(100, true);
    pump(200);
    shot("06b_chat_sent");
    // the keys: Up from the empty message box steps onto the newest message, OK opens its menu, Back closes
    // just the menu
    app.key(hx::K_UP);
    pump(200);
    shot("06c_chat_pick_msg");
    app.key(hx::K_ENTER);
    pump(200);
    if (!hx::HarnessAccess::msgOpen(app)) {
        printf("FAIL OK on a message with the keys\n");
        return 1;
    }
    shot("06d_chat_msg_menu_keys");
    app.key(hx::K_BACK);
    pump(200);
    if (hx::HarnessAccess::msgOpen(app) || hx::HarnessAccess::tab(app) != hx::App::T_CHATS || !hx::HarnessAccess::chatIsOpen(app)) {
        printf("FAIL Back on the message menu must close only the menu\n");
        return 1;
    }
    app.key(hx::K_BACK);
    app.selectTab(hx::App::T_HOME);
    app.key('r'); // R: the conversation the last message came in on
    pump(300);
    shot("06f_chat_r_key");
    app.key(hx::K_BACK);
    app.selectTab(hx::App::T_MORE);
    pump(300);
    shot("07_settings");
    hx::HarnessAccess::more(app, hx::App::M_RADIO);
    pump(100);
    shot("07b_settings_radio");
    // keys: Back returns to the six categories, with the cursor on the one just left
    app.key(hx::K_BACK);
    pump(100);
    if (hx::HarnessAccess::tab(app) != hx::App::T_MORE || hx::HarnessAccess::sec(app) != -1) {
        printf("FAIL Back from a settings page\n");
        return 1;
    }
    // the Sound quick switch: off, then back to the tone it had
    {
        const int before = model.uiPref("snd", 1);
        lv_obj_send_event(hx::HarnessAccess::quick(app, 3), LV_EVENT_CLICKED, nullptr);
        const int off = model.uiPref("snd", 1);
        lv_obj_send_event(hx::HarnessAccess::quick(app, 3), LV_EVENT_CLICKED, nullptr);
        printf("sound quick switch %d -> %d -> %d\n", before, off, model.uiPref("snd", 1));
        if (before < 1 || off != 0 || model.uiPref("snd", 1) != before) {
            printf("FAIL sound quick switch\n");
            return 1;
        }
    }
    pump(4500); // the toasts go
    for (int s = 1; s < hx::App::M_COUNT; s++) {
        hx::HarnessAccess::more(app, s);
        pump(100);
        char nm[32];
        snprintf(nm, sizeof(nm), "07s%d_settings_page", s);
        shot(nm);
    }
    hx::HarnessAccess::more(app, hx::App::M_SCREEN);
    pump(100);
    lv_obj_scroll_to_view_recursive(hx::HarnessAccess::volume(app), LV_ANIM_OFF);
    pump(200);
    shot("07e_settings_sound");
    {
        lv_obj_t *more = lv_obj_get_child(lv_obj_get_child(lv_screen_active(), 2), hx::App::T_MORE);
        hx::HarnessAccess::more(app, hx::App::M_RADIO);
        lv_obj_scroll_to_view_recursive(hx::HarnessAccess::cust(app, 0), LV_ANIM_OFF);
        (void)more;
        pump(100);
        shot("07c_settings_custom");
        // custom entry: type a frequency, pick BW 125 / SF9 / CR6, apply, and check the model got it
        lv_textarea_set_text(hx::HarnessAccess::cust(app, 0), "906.875");
        lv_dropdown_set_selected(hx::HarnessAccess::cust(app, 1), 7);
        lv_dropdown_set_selected(hx::HarnessAccess::cust(app, 2), 4);
        lv_dropdown_set_selected(hx::HarnessAccess::cust(app, 3), 1);
        hx::HarnessAccess::apply(app);
        pump(100);
        hx::Radio r = model.radio();
        printf("custom apply -> %.3f MHz BW %.1f SF%u CR%u\n", r.freq, r.bw, r.sf, r.cr);
        if (fabsf(r.freq - 906.875f) > 0.001f || r.bw != 125.0f || r.sf != 9 || r.cr != 6) {
            printf("FAIL custom radio apply\n");
            return 1;
        }
        // Left/Right step on the frequency box
        lv_group_focus_obj(hx::HarnessAccess::cust(app, 0));
        app.key(hx::K_RIGHT);
        app.key(hx::K_RIGHT);
        printf("stepped -> %s\n", lv_textarea_get_text(hx::HarnessAccess::cust(app, 0)));
        shot("07d_settings_custom_applied");
        // back to the US preset via the dropdown list
        lv_dropdown_set_selected(hx::HarnessAccess::preset(app), 0);
        lv_obj_send_event(hx::HarnessAccess::preset(app), LV_EVENT_VALUE_CHANGED, nullptr);
        pump(100);
        if (!hx::HarnessAccess::asking(app) || model.radio().freq != 906.875f) {
            printf("FAIL a preset must ask before it changes the radio\n");
            return 1;
        }
        shot("07f_settings_ask_radio");
        hx::HarnessAccess::askApply(app);
        pump(100);
        r = model.radio();
        printf("preset 0 -> %.3f MHz BW %.1f SF%u CR%u\n", r.freq, r.bw, r.sf, r.cr);
        if (fabsf(r.freq - 910.525f) > 0.001f || r.sf != 7) {
            printf("FAIL preset apply\n");
            return 1;
        }
        // Cancel leaves the radio alone
        lv_dropdown_set_selected(hx::HarnessAccess::preset(app), 6);
        lv_obj_send_event(hx::HarnessAccess::preset(app), LV_EVENT_VALUE_CHANGED, nullptr);
        app.key(hx::K_BACK);
        pump(100);
        if (hx::HarnessAccess::asking(app) || fabsf(model.radio().freq - 910.525f) > 0.001f ||
            lv_dropdown_get_selected(hx::HarnessAccess::preset(app)) != 0) {
            printf("FAIL Cancel on the radio question\n");
            return 1;
        }
    }
    // a firmware update from the web page asks on the screen: Install and Back answer it, a timeout only closes it
    {
        app.askUpdate(1536);
        pump(100);
        if (!hx::HarnessAccess::asking(app)) {
            printf("FAIL the update question must show\n");
            return 1;
        }
        shot("07g_ask_update");
        hx::HarnessAccess::askApply(app);
        pump(100);
        if (hx::HarnessAccess::asking(app) || model.updateAns != 1) {
            printf("FAIL Install on the update question\n");
            return 1;
        }
        app.askUpdate(1536);
        pump(100);
        app.key(hx::K_BACK);
        pump(100);
        if (hx::HarnessAccess::asking(app) || model.updateAns != 0) {
            printf("FAIL Back on the update question\n");
            return 1;
        }
        model.updateAns = -1;
        app.askUpdate(1536);
        pump(100);
        app.askUpdateGone();
        pump(100);
        if (hx::HarnessAccess::asking(app) || model.updateAns != -1) {
            printf("FAIL the update question going unanswered\n");
            return 1;
        }
        // the radio question afterwards is the radio question again
        hx::HarnessAccess::more(app, hx::App::M_RADIO);
        lv_dropdown_set_selected(hx::HarnessAccess::preset(app), 6);
        lv_obj_send_event(hx::HarnessAccess::preset(app), LV_EVENT_VALUE_CHANGED, nullptr);
        pump(100);
        shot("07h_settings_ask_radio_again");
        app.key(hx::K_BACK);
        pump(4500); // the toasts go
        printf("update question: install, back and no answer all right\n");
    }
    // emoji in a channel chat
    hx::HarnessAccess::channel(app, 1);
    pump(300);
    shot("20_chat_emoji");
    // hold a message: its details and actions. A tap doesn't open it; OK on it with the keys does.
    {
        using HA = hx::HarnessAccess;
        lv_obj_t *b = HA::bubble(app, false);
        if (!b) {
            printf("FAIL no received message to hold\n");
            return 1;
        }
        lv_obj_send_event(b, LV_EVENT_CLICKED, nullptr); // (a touch's CLICKED alone, as after a hold: not again)
        if (HA::msgOpen(app)) {
            printf("FAIL a plain CLICKED opened the message menu\n");
            return 1;
        }
        lv_obj_send_event(b, LV_EVENT_SHORT_CLICKED, nullptr); // a tap
        pump(100);
        if (!HA::msgOpen(app)) {
            printf("FAIL a tap on a message\n");
            return 1;
        }
        HA::msgAction(app, 5); // Close
        lv_obj_send_event(b, LV_EVENT_LONG_PRESSED, nullptr);
        pump(100);
        if (!HA::msgOpen(app)) {
            printf("FAIL holding a message\n");
            return 1;
        }
        shot("20b_message_menu");
        HA::msgAction(app, 0); // Reply on the channel
        pump(100);
        printf("reply puts: [%s]\n", HA::input(app).c_str());
        if (HA::msgOpen(app) || HA::input(app).rfind("@[", 0) != 0) {
            printf("FAIL reply on the channel\n");
            return 1;
        }
        if (HT > WD) {
            app.key(hx::K_BACK); // the keyboard the reply opened
            pump(100);
        }
        HA::clearInput(app);
        // one of mine, opened with the OK key: Send again, then Delete
        HA::sendText(app, "Testing the message menu");
        pump(100);
        const size_t n = HA::lines(app);
        HA::keyOk(app, HA::bubble(app, true));
        pump(100);
        if (!HA::msgOpen(app)) {
            printf("FAIL OK on my message\n");
            return 1;
        }
        shot("20c_message_menu_mine");
        HA::msgAction(app, 3); // Send again
        pump(100);
        if (HA::lines(app) != n + 1) {
            printf("FAIL send again\n");
            return 1;
        }
        HA::keyOk(app, HA::bubble(app, true));
        HA::msgAction(app, 4); // Delete
        pump(100);
        if (HA::lines(app) != n || HA::msgOpen(app)) {
            printf("FAIL delete\n");
            return 1;
        }
        pump(4500); // the toasts go
    }
    if (HT > WD) {
        hx::HarnessAccess::tapChatInput(app);
        pump(100);
        shot("23_keyboard_chat");
        app.key(hx::K_BACK); // closes the keyboard first
        pump(100);
    }
    app.key(hx::K_BACK);
    pump(100);
    // managing a repeater: its node page, Manage, log in, then each page
    {
        using HA = hx::HarnessAccess;
        app.selectTab(hx::App::T_NODES);
        pump(4000); // the last toast goes
        HA::node(app, model.contacts[0].key);
        pump(200);
        shot("21_node_repeater");
        HA::remote(app, model.contacts[0].key);
        pump(200);
        for (const char *c = "hunter2"; *c; c++)
            app.key((uint32_t)*c);
        pump(100);
        shot("21a_manage_login");
        if (HT > WD) { // no keyboard on the Heltec kits: tap the field, the on-screen one comes up
            HA::tapRemoteField(app);
            pump(100);
            shot("23b_keyboard_password");
        }
        app.key(hx::K_ENTER);
        pump(300);
        shot("21b_manage_logging_in");
        pump(1600);
        shot("21c_manage_status");
        HA::remoteAsk(app, 4); // telemetry
        pump(1200);
        HA::remoteScroll(app, 170);
        pump(50);
        shot("21d_manage_status_more");
        HA::remoteScroll(app, 400);
        pump(50);
        shot("21e_manage_actions");
        HA::remotePage(app, 1);
        pump(1200);
        shot("21f_manage_heard");
        HA::remotePage(app, 2);
        pump(16000); // 21 settings, one at a time
        shot("21g_manage_settings");
        for (int i = 1; i < 12; i++) {
            HA::remoteScroll(app, 160);
            pump(50);
            char nm[40];
            snprintf(nm, sizeof(nm), "21h_manage_settings_%02d", i);
            shot(nm);
        }
        HA::remoteAsk(app, 5); // access list
        pump(1200);
        HA::remoteScroll(app, 2000);
        pump(50);
        shot("21i_manage_access");
        HA::remotePage(app, 3);
        pump(200);
        for (const char *c = "ver"; *c; c++)
            app.key((uint32_t)*c);
        app.key(hx::K_ENTER);
        pump(800);
        for (const char *c = "clock"; *c; c++)
            app.key((uint32_t)*c);
        app.key(hx::K_ENTER);
        pump(800);
        for (const char *c = "get flood.max"; *c; c++)
            app.key((uint32_t)*c);
        pump(100);
        shot("21j_manage_console");
        app.key(hx::K_BACK);
        pump(100);
        app.key(hx::K_BACK);
        pump(100);
        app.touched();
    }
    // the fuller settings page, top to bottom
    app.selectTab(hx::App::T_MORE);
    pump(300);
    {
        lv_obj_t *more = lv_obj_get_child(lv_obj_get_child(lv_screen_active(), 2), hx::App::T_MORE);
        for (int s = 0; s < hx::App::M_COUNT; s++) {
            hx::HarnessAccess::more(app, s);
            lv_obj_update_layout(more);
            int32_t total = lv_obj_get_scroll_bottom(more) + lv_obj_get_height(more);
            printf("settings page %d height %d\n", s, (int)total);
            for (int i = 0; i < 12; i++) {
                lv_obj_scroll_to_y(more, i * (lv_obj_get_height(more) - 20), LV_ANIM_OFF);
                pump(50);
                char nm[32];
                snprintf(nm, sizeof(nm), "19_settings_%d_%02d", s, i);
                shot(nm);
                if (lv_obj_get_scroll_bottom(more) <= 0)
                    break;
            }
        }
        hx::HarnessAccess::more(app, -1);
    }
    // stat details from the Home tiles, and the keys' cursor on the menu rail
    app.touched();
    app.selectTab(hx::App::T_HOME);
    hx::HarnessAccess::history(app);
    pump(4500); // let the settings toast go
    static const char *sn[5] = {"11_stat_heard", "12_stat_repeaters", "13_stat_packets", "14_stat_noise", "15_stat_radio"};
    for (int i = 0; i < 5; i++) {
        hx::HarnessAccess::stat(app, i);
        pump(300);
        shot(sn[i]);
        app.key(hx::K_BACK);
        pump(100);
    }
    app.key(hx::K_DOWN);
    app.key(hx::K_DOWN);
    pump(100);
    shot("16_home_tile_focus");
    app.key(hx::K_ENTER);
    pump(300);
    shot("16b_tile_opened_by_keys");
    app.key(hx::K_BACK);
    // the d-pad goes where it points: across the tiles, down, back
    app.key(hx::K_DOWN); // from the top card onto the first tile
    pump(100);
    shot("16c_dpad_start");
    app.key(hx::K_RIGHT);
    pump(100);
    shot("16d_dpad_right");
    app.key(hx::K_DOWN);
    pump(100);
    shot("16e_dpad_down");
    app.key(hx::K_LEFT);
    pump(100);
    shot("16f_dpad_left");
    app.key(hx::K_UP);
    app.key(hx::K_UP);
    pump(100);
    shot("16g_dpad_up_up");
    app.key(hx::K_LEFT); // nothing to the left of the top card: onto the menu
    pump(100);
    shot("17_rail_focus");
    app.key(hx::K_DOWN);
    app.key(hx::K_DOWN);
    pump(300);
    shot("17b_rail_moved");
    app.key(hx::K_RIGHT);
    pump(300);
    shot("17c_rail_entered");
    app.touched();

    // the map: packets flying in along their real routes (2-byte path hashes, as SMF runs)
    auto contactKey = [&](const char *name) -> const uint8_t * {
        for (auto &c : model.contacts)
            if (!strcmp(c.name, name))
                return c.key;
        return nullptr;
    };
    auto fly = [&](hx::PktType t, const char *from, std::vector<const char *> hops, int rssi, int snr4) {
        hx::Packet k;
        k.type = t, k.hashSize = 2, k.pathLen = (uint8_t)hops.size(), k.rssi = rssi, k.snr4 = snr4, k.time = NOW, k.len = 80;
        for (size_t i = 0; i < hops.size(); i++)
            memcpy(k.path + i * 2, contactKey(hops[i]), 2);
        if (const uint8_t *fk = contactKey(from)) {
            k.hasFromKey = true;
            memcpy(k.fromKey, fk, 6);
        }
        strncpy(k.from, from, sizeof(k.from) - 1);
        app.onPacket(k);
    };
    app.selectTab(hx::App::T_MAP);
    pump(300);
    for (int i = 0; i < 400 && !hoptics::TileMap::get().idle(); i++)
        pump(10); // the tiles for this view
    pump(50);
    shot("18_map");
    for (int r = 0; r < 6; r++) { // some history, so the links show
        fly(hx::PktType::Advert, "Auburn W6EK", {"Rocklin Hill", "Citrus Ridge Rptr"}, -104, -12);
        fly(hx::PktType::GroupText, "Trail Tracker", {"Folsom Ridge Rptr"}, -92, 22);
        pump(1600);
    }
    pump(3000);
    fly(hx::PktType::Advert, "Auburn W6EK", {"Rocklin Hill", "Citrus Ridge Rptr"}, -104, -12);
    fly(hx::PktType::GroupText, "Trail Tracker", {"Folsom Ridge Rptr"}, -92, 22);
    pump(450);
    shot("18b_map_flight");
    pump(560);
    shot("18c_map_landing");
    // the arrows move freely on the map too: the zoom buttons are reached like any control, OK presses one
    app.key(hx::K_DOWN); // the first key only shows where the focus is: the + button
    app.key(hx::K_ENTER);
    pump(100);
    shot("18d_map_zoomed_in");
    app.key(hx::K_DOWN); // onto -
    app.key(hx::K_ENTER);
    app.key(hx::K_ENTER);
    pump(100);
    shot("18e_map_zoomed_out");
    // the live feed: a packet only the observers heard, its route ending at the observer, not here
    app.key(hx::K_UP); // back onto +
    app.key(hx::K_ENTER);
    app.key(hx::K_ENTER);
    app.key(hx::K_ENTER);
    pump(100);
    {
        hx::Packet k;
        k.type = hx::PktType::GroupText, k.hashSize = 2, k.pathLen = 2, k.time = NOW, k.len = 72, k.snr4 = 30;
        memcpy(k.path, contactKey("Auburn W6EK"), 2);
        memcpy(k.path + 2, contactKey("Rocklin Hill"), 2);
        strcpy(k.from, "#smf");
        k.viaNet = true;
        memcpy(k.obsKey, contactKey("Folsom Ridge Rptr"), 6);
        strcpy(k.via, "Folsom Ridge Rptr");
        app.onPacket(k);
        pump(500);
        shot("19a_map_feed_flight");
        pump(1200);
        shot("19b_map_feed_landed");
    }
    app.key(hx::K_ENTER);
    app.touched();
    // ... and its message in the chat, marked as such; then one we send, heard by three observers
    hx::HarnessAccess::channel(app, 1);
    pump(200);
    {
        hx::Message mm;
        mm.conv.channel = 1;
        strcpy(mm.from, "Dennis Mobile");
        mm.text = "Anyone copy? I can get out but can't hear a thing down here";
        mm.time = NOW;
        mm.pathLen = 2;
        mm.viaNet = true;
        app.onMessage(mm);
    }
    pump(200);
    for (const char *c = "Copy you Dennis, loud and clear"; *c; c++)
        app.key((uint8_t)*c);
    app.key(hx::K_ENTER);
    const uint32_t sentId = model.nextId - 1;
    pump(200);
    if (WD > HT) { // the keyboard light came on with the typing, and goes off 10 s later
        printf("keyboard light while typing: %d\n", model.keyLightLevel);
        if (model.keyLightLevel <= 0)
            return 1;
        pump(10500);
        printf("keyboard light 10 s later: %d\n", model.keyLightLevel);
        if (model.keyLightLevel != 0)
            return 1;
    }
    app.onHeard(sentId, 1);
    app.onHeard(sentId, 3);
    pump(200);
    shot("19c_chat_via_internet");
    app.key(hx::K_BACK);
    app.selectTab(hx::App::T_MORE);
    hx::HarnessAccess::more(app, hx::App::M_CONNECT);
    pump(200);
    lv_obj_scroll_to_view_recursive(hx::HarnessAccess::feedField(app), LV_ANIM_OFF);
    pump(200);
    shot("19d_settings_feed");
    app.selectTab(hx::App::T_HOME);
    pump(100);
    static const char *tn[10] = {"midnight", "daylight", "nightred", "amber", "glass", "glass_violet", "glass_green", "glass_rose", "glass_gold", "glass_rainbow"};
    for (int t = 1; t < 10; t++) {
        model.setUiPref("theme", t);
        app.begin();
        app.selectTab(hx::App::T_HOME);
        pump(200);
        shot((std::string("10_home_") + tn[t]).c_str());
        if (t == 4 || t == 9) { // Glass and Glass rainbow on the other tabs too
            for (int tab : {(int)hx::App::T_LIVE, (int)hx::App::T_NODES, (int)hx::App::T_CHATS, (int)hx::App::T_MORE}) {
                app.selectTab(tab);
                pump(200);
                shot((std::string("10_") + tn[t] + "_tab" + std::to_string(tab)).c_str());
            }
        }
    }
    // wallpapers (Glass violet with Aurora, Midnight with Ridgelines), the lock screen, large chat text
    model.setUiPref("theme", 5);
    model.setUiPref("wall", 3);
    app.begin();
    app.selectTab(hx::App::T_HOME);
    pump(200);
    shot("24_wallpaper_glass_violet");
    app.selectTab(hx::App::T_CHATS);
    pump(200);
    shot("24b_wallpaper_chats");
    model.setUiPref("theme", 0);
    model.setUiPref("wall", 1);
    app.begin();
    app.selectTab(hx::App::T_HOME);
    pump(200);
    shot("24c_wallpaper_midnight");
    hx::HarnessAccess::lock(app);
    pump(200);
    shot("25_lock_screen");
    hx::HarnessAccess::unlock(app);
    model.setUiPref("wall", 0);
    hx::HarnessAccess::lock(app);
    pump(200);
    shot("25b_lock_plain");
    hx::HarnessAccess::unlock(app);
    model.setUiPref("chattext", 1);
    app.begin();
    hx::HarnessAccess::channel(app, 1);
    pump(300);
    shot("26_chat_large_text");
    hx::HarnessAccess::channel(app, 0);
    pump(300);
    shot("27_international_chat");
    model.setUiPref("chattext", 0);
    app.begin();
    app.selectTab(hx::App::T_HOME);
    pump(100);
    model.setUiPref("theme", 0);

    // reused Live rows: after packets that change every row's type, edge and colours, the list must look exactly like a
    // freshly built one (build_and_run.sh compares 30_live_reused with 30_live_fresh). Glass rainbow, so edges count too.
    model.setUiPref("theme", 9);
    app.begin();
    app.selectTab(hx::App::T_LIVE);
    pump(300);
    for (int i = 0; i < 3; i++) {
        hx::Packet k;
        k.type = i == 1 ? hx::PktType::Text : hx::PktType::Advert, k.direct = i == 2, k.pathLen = i, k.rssi = -90 - i;
        k.snr4 = 30 - 25 * i, k.time = NOW - 2, k.mine = i == 1, k.len = 40;
        if (i != 1)
            snprintf(k.from, sizeof(k.from), "Pool test %d", i);
        app.onPacket(k);
        pump(300);
    }
    shot("30_live_reused");
    app.begin();
    app.selectTab(hx::App::T_LIVE);
    pump(300);
    shot("30_live_fresh");
    model.setUiPref("theme", 0);
    app.begin();
    app.selectTab(hx::App::T_HOME);
    pump(100);
    // timings (with HX_PERF set): a full Nodes list, a full chat, each tab; desktop numbers, for comparing
    if (getenv("HX_PERF")) {
        using HA = hx::HarnessAccess;
        auto ms = [](auto t0) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
        for (int i = 0; i < 130; i++) {
            hx::Contact c;
            snprintf(c.name, sizeof(c.name), "Node %03d Somewhere", i);
            c.type = i % 3 ? hx::NodeType::Chat : hx::NodeType::Repeater;
            c.pathLen = i % 4;
            c.lastHeard = NOW - i * 60;
            for (int k = 0; k < 6; k++)
                c.key[k] = (uint8_t)(i * 7 + k * 13 + 1);
            model.contacts.push_back(c);
        }
        model.ver++;
        app.selectTab(hx::App::T_NODES);
        pump(100);
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 10; i++)
            HA::refreshNodes(app);
        printf("perf: Nodes list (%zu contacts) %.2f ms\n", model.contacts.size(), ms(t0) / 10);
        t0 = std::chrono::steady_clock::now();
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(nullptr);
        printf("perf: Nodes full redraw %.2f ms\n", ms(t0));
        HA::channel(app, 1);
        for (int i = 0; i < 70; i++)
            HA::sendText(app, "A message long enough to wrap onto a second line on the small screens");
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 10; i++)
            HA::renderChat(app);
        printf("perf: chat (%zu lines) %.2f ms\n", HA::lines(app), ms(t0) / 10);
        app.key(hx::K_BACK);
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 10; i++)
            HA::refreshChats(app);
        printf("perf: Chats list %.2f ms\n", ms(t0) / 10);
        for (int t = 0; t < hx::App::T_COUNT; t++) {
            t0 = std::chrono::steady_clock::now();
            app.selectTab(t);
            lv_refr_now(nullptr);
            printf("perf: tab %d %.2f ms\n", t, ms(t0));
        }
        return 0;
    }

    // a restart: a new App on the same radio finds the chats where they were
    hx::HarnessAccess::saveChats(app);
    printf("chat history: %zu bytes\n", model.files["chats"].size());
    {
        hx::App again(model);
        gApp = &again;
        again.begin();
        again.selectTab(hx::App::T_CHATS);
        pump(300);
        shot("22_chats_after_restart");
        hx::HarnessAccess::channel(again, 1);
        pump(300);
        shot("22b_chat_after_restart");
    }
    printf("ok\n");
    return 0;
}
