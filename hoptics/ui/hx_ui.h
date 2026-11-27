// Hoptics-MC screens (LVGL 9). Landscape 320x240 (T-Deck Plus, ThinkNode M9) and portrait 240x320 (Heltec V4 kits)
// from one layout: a rail on the left in landscape, tabs along the bottom in portrait. MIT licence, see LICENSE.
#pragma once
#include "hx_model.h"
#include "lvgl.h"
#include <string>
#include <vector>

namespace hx
{
// the screen work running last (set as each heavy step starts), so a slow pass can say what it was doing
extern const char *busy;

// keys, after each board's driver has mapped its keyboard / trackball / d-pad onto them
enum Key : uint32_t {
    K_UP = 0x110000, K_DOWN, K_LEFT, K_RIGHT, K_ENTER, K_BACK, K_HOME, K_BKSP, K_TAB,
    K_QUICK,        // dedicated key: the Public channel, ready to type
    K_ADVERT,       // dedicated key: zero-hop advert (neighbours only)
    K_ADVERT_FLOOD, // dedicated key, held: flood advert (the whole mesh)
    K_GOTO = 0x110100 // + tab number: jump straight to a tab (dedicated keys)
};

// the start-up screen (hx_bootscreen.cpp): shown by the board before MeshCore's slow steps, replaced by App::begin()
void bootShow(const char *board, const char *version, int art); // art: which boot picture (any number, kept in turn)
void bootStep(int step);                                        // MeshCore's start-up step that just finished, 0-8
void bootError(const char *what, const char *todo);             // start-up stopped: why, and what to do
void bootDone(void);
const uint8_t *bootArt(int i, int32_t &w, int32_t &h); // the boot pictures (RGB565, this panel's size), or null

class App : public Ui
{
  public:
    explicit App(Model &m);
    void begin(void);      // after lv_init() and the display driver
    void loop(uint32_t ms); // call often from the main loop
    void key(uint32_t k);  // a printable character (< 0x110000) or one of Key
    void touched(void);    // any touch: wake the screen, hide key focus rings

    // Ui
    void onMessage(const Message &m) override;
    void onDelivered(uint32_t id, bool ok) override;
    void onPacket(const Packet &p) override;
    void onContactsChanged() override { dirtyContacts = true; }
    void toast(const char *text) override;
    void onHeard(uint32_t id, uint8_t observers) override;
    void onRemoteLogin(const uint8_t key[6], bool ok, uint8_t perms) override;
    void onRemoteStatus(const uint8_t key[6], const RemoteStatus &s) override;
    void onRemoteNeighbours(const uint8_t key[6], uint16_t total, const RemoteNeighbour *n, int count) override;
    void onRemoteTelemetry(const uint8_t key[6], const uint8_t *lpp, int len) override;
    void onRemoteAccessList(const uint8_t key[6], const RemoteClient *c, int count) override;
    void onRemoteReply(const uint8_t key[6], const char *text) override;

    enum Tab { T_HOME, T_MAP, T_LIVE, T_NODES, T_CHATS, T_MORE, T_COUNT };
    void selectTab(int t);
    bool screenOn(void) const { return awake; }
    // the web page's data as JSON: what the page doesn't have yet (contacts version, last packet and message numbers)
    // plus what the node adds (Model::webExtras)
    void webJson(std::string &out, uint32_t contactsHave, uint32_t packetsHave, uint32_t msgsHave, uint32_t histHave);
    std::string webSend(uint8_t channel, const char *text); // a channel message from the web page: JSON answer
    std::string bandWarning(const Radio &rd) const;          // empty when the radio fits the chosen country band
    lv_obj_t *bandInfo = nullptr;
    lv_obj_t *setVol = nullptr; // More > Sound > Volume
    // More: a hub of six categories, each opening its own page of cards (moreOpen = which one, -1 = the hub)
    enum MoreSec { M_RADIO, M_MESSAGES, M_CONNECT, M_SCREEN, M_LOCATION, M_SYSTEM, M_COUNT };
    void openMore(int s);
    void askApply(void); // the "Apply radio change?" box's Apply

  private:
    Model &m;
    // ---- chat history kept on the screen side
    struct Line {
        std::string from, text;
        uint32_t time = 0, id = 0;
        uint32_t seq = 0; // its number for the web page (which asks for what is newer)
        bool mine = false;
        uint8_t state = 0; // 0 none, 1 sending, 2 delivered (ACK), 3 no ACK, 4 sent (channels)
        int8_t hops = -1; // received: hops it took (-1 unknown, -2 came over the live feed); sent: observers that heard it
        // kept only while running (not saved): the last hop's signal, and for a direct message the wait for its ACK
        int8_t snr4 = -128; // -128 unknown
        uint32_t sentMs = 0, rttMs = 0;
    };
    struct Conv {
        ConvKey key;
        std::string title;
        std::vector<Line> lines;
        uint16_t unread = 0;
        uint32_t last = 0;
    };
    std::vector<Conv> convs;
    Conv &conv(const ConvKey &k);
    // kept across restarts (Model::saveData): written when the screen goes dark, or 2 minutes after a change
    uint32_t chatsChangedAt = 0; // lv tick of the first change not yet saved, 0 = saved
    void chatsChanged(void);
    void saveChats(void);
    void loadChats(void);
    std::string convTitle(const ConvKey &k) const;

    // ---- live packets
    static constexpr int kLive = 48;
    Packet live[kLive];
    int liveHead = 0, liveCount = 0;
    uint16_t perMin[30] = {}; // packets per minute, last 30 minutes (newest last)
    uint32_t minuteStart = 0;

    // ---- widgets
    lv_obj_t *scr = nullptr, *status = nullptr, *stName = nullptr, *stRight = nullptr, *nav = nullptr, *content = nullptr;
    lv_obj_t *navBtn[T_COUNT] = {}, *navBadge = nullptr, *pages[T_COUNT] = {};
    lv_obj_t *toastBox = nullptr, *toastLbl = nullptr;
    uint32_t toastUntil = 0;
    int tab = T_HOME;

    // home
    lv_obj_t *hName = nullptr, *hSub = nullptr, *hTile[4] = {}, *hTileSub[4] = {}, *hChart = nullptr;
    lv_chart_series_t *hSer = nullptr;
    // live
    lv_obj_t *liveList = nullptr, *liveEmpty = nullptr;
    bool dirtyLive = true;
    // nodes
    lv_obj_t *nodeList = nullptr, *nodeChips[5] = {}, *nodeEmpty = nullptr; // nodeEmpty: the "no nodes yet" note
    int nodeFilter = 0;
    bool dirtyContacts = true;
    uint32_t contactsSeen = 0xffffffff;
    // chats
    lv_obj_t *chatList = nullptr;
    bool dirtyChats = true;
    // overlays
    lv_obj_t *chatOv = nullptr, *chatTitle = nullptr, *chatMsgs = nullptr, *chatInput = nullptr;
    // hold a message (or OK on it with the keys): what is known about it, and what can be done with it
    lv_obj_t *msgBox = nullptr, *msgCard = nullptr, *msgInfo = nullptr, *msgBtns = nullptr;
    uint32_t msgSeq = 0;      // the message the box is about
    uint8_t msgFrom[6] = {};  // its sender, when they are a contact
    bool msgFromKnown = false;
    bool keyClick = false;    // the CLICKED being handled came from a key, not a tap
    void openMsgMenu(uint32_t seq);
    void closeMsgMenu(void);
    void msgAction(int a);
    ConvKey chatKey;
    bool chatOpen = false;
    // the last conversation a message came in on (tap its toast, or press R, to open it)
    ConvKey lastIn, toastConv;
    bool haveLastIn = false, toastIsMsg = false;
    lv_obj_t *nodeOv = nullptr, *nodeOvBody = nullptr;
    uint8_t nodeOvKey[6] = {};
    // stat details: a Home tile (or the top card, or the activity chart) opened full screen
    enum Stat { S_HEARD, S_REPEATERS, S_PACKETS, S_NOISE, S_RADIO, S_COUNT };
    lv_obj_t *statOv = nullptr, *statTitle = nullptr, *statBody = nullptr;
    int statOpen = -1;
    uint32_t statDrawn = 0;
    int16_t noiseHist[30] = {}; // noise floor at each minute, last 30 minutes (newest last, 0 = not known)
    // managing a repeater or room server (hx_ui_remote.inc): everything it keeps lives in Remote, made on open
    struct Remote;
    Remote *rm = nullptr;
    lv_obj_t *rmOv = nullptr, *rmTitle = nullptr, *rmTabs = nullptr, *rmBody = nullptr;
    // keys on the menu (the rail in landscape, the tabs in portrait) instead of the page
    bool railFocus = false;
    // map: nodes at their positions, packets animated along their route, links learned from the paths
    lv_obj_t *mapObj = nullptr, *mapStrip = nullptr, *mapEmpty = nullptr;
    lv_timer_t *mapTimer = nullptr;
    // Web Mercator, as the street map's tiles are: world pixels at zoom level mapZ (256 << mapZ across)
    int mapZ = 12;
    double mapCX = 0, mapCY = 0; // the view's centre, in world pixels
    float mapKmPx = 0.1f;        // km per pixel at the centre (follows mapZ and mapCY)
    void mapScaleChanged(void);
    bool mapTiles(void) const;   // the street map is on (Settings > Map)
    bool mapFitted = false;
    std::string mapLast;            // the last packet's route, for the strip
    // settings
    lv_obj_t *setName = nullptr, *setPreset = nullptr, *setTx = nullptr, *setBt = nullptr, *setBtPin = nullptr,
             *setRepeat = nullptr, *setTheme = nullptr, *setBright = nullptr, *setAbout = nullptr;
    lv_obj_t *custFreq = nullptr, *custBw = nullptr, *custSf = nullptr, *custCr = nullptr, *custNote = nullptr;
    void loadCustom(void);
    void applyCustom(void);
    bool loadingSettings = false;
    // the keyed settings (Model::getSetting): one row each, hidden when the radio doesn't have it
    struct SetRow {
        const char *key = nullptr; // "ui:..." = the screen's own pref
        const int16_t *vals = nullptr; // dropdown: the value of each option; nullptr = a switch
        int nvals = 0;
        lv_obj_t *row = nullptr, *ctl = nullptr;
    };
    std::vector<SetRow> setRows;
    lv_obj_t *moreHub = nullptr, *moreSec[M_COUNT] = {}, *moreTile[M_COUNT] = {}, *moreSum[M_COUNT][2] = {};
    int moreOpen = -1;
    lv_obj_t *quickTile[4] = {}, *quickSw[4] = {}; // Wi-Fi, Bluetooth, GPS, Sound: a tap flips, a hold opens the page
    void refreshMoreHub(void);
    lv_obj_t *askBox = nullptr, *askText = nullptr; // "Apply radio change?"
    Radio askRadio{};
    void askRadioChange(const Radio &rd);
    lv_obj_t *setScope = nullptr, *setPin = nullptr;
    // Wi-Fi and the street map (Settings)
    lv_obj_t *wfSwitch = nullptr, *wfStatus = nullptr, *wfList = nullptr, *wfSsid = nullptr, *wfPass = nullptr;
    lv_obj_t *mapTilesSw = nullptr, *mapSaved = nullptr;
    lv_obj_t *feedSw = nullptr, *feedStatus = nullptr, *feedUrl = nullptr; // the live feed
    lv_obj_t *webSw = nullptr, *webStatus = nullptr, *webPin = nullptr;     // the web page
    uint32_t pktSeq = 0, lineSeq = 0;
    void feedServerFromField(void);
    int wfListSeen = -3; // the scan result the list shows
    void wifiConnectFromFields(void);
    void addSetting(lv_obj_t *parent, const char *key, const char *label, const char *opts, const int16_t *vals, int nvals,
                    const char *help = nullptr);
    bool readSetting(const char *key, int32_t &v) const;
    bool writeSetting(const char *key, int32_t v);
    void localTime(uint32_t t, struct tm &out) const;

    // power / keys
    bool awake = true, keysUsed = false;
    uint32_t lastInput = 0, lastTick = 0, lastSlow = 0;
    uint32_t lastTyped = 0; // the keyboard light (T-Deck): last key typed, and the level it is set to (-1 not yet)
    int keyLight = -1;
    void keyLightTick(uint32_t t);
    lv_group_t *group = nullptr;

    // build
    void build(void);
    lv_obj_t *makePage(void);
    lv_obj_t *overlay(const char *title, lv_obj_t **titleOut, lv_event_cb_t back);
    void buildHome(lv_obj_t *p);
    void buildLive(lv_obj_t *p);
    void buildNodes(lv_obj_t *p);
    void buildChats(lv_obj_t *p);
    void buildMap(lv_obj_t *p);
    void buildMore(lv_obj_t *p);
    void buildChatOverlay(void);
    void buildNodeOverlay(void);
    void buildStatOverlay(void);
    void buildRemoteOverlay(void);

    // refresh
    void refreshStatus(void);
    void refreshHome(void);
    void refreshLive(void);
    void refreshNodes(void);
    void refreshChats(void);
    void refreshSettings(void);
    void refreshWifi(void);
    void renderChat(void);
    void appendBubble(const Line &l);

    // actions
    void openChat(const ConvKey &k);
    void closeChat(void);
    void sendFromInput(void);
    uint32_t sendLine(const ConvKey &key, const char *text); // sends, and adds it to the conversation
    void openNode(const uint8_t key[6]);
    void closeNode(void);
    void openStat(int s);
    void renderStat(void);
    void closeStat(void);
    void openRemote(const uint8_t key[6]);
    void closeRemote(void);
    void rmLogin(void);
    void rmAsk(int what);
    void rmCommand(const std::string &text, int row = -1, bool set = false, const std::string &value = "");
    void rmPump(void);
    void rmAnswer(const char *text);
    void rmTick(void);
    void rmRender(void);
    lv_obj_t *rmLoginPage(const std::string &typed);
    void rmStatusPage(void);
    void rmHeardPage(void);
    void rmSettingsPage(void);
    lv_obj_t *rmConsolePage(const std::string &typed);
    void rmReadAll(void);
    void rmReadProgress(void);
    void rmShowValue(int i);
    void rmConsoleChanged(void);
    lv_obj_t *rmTypingTarget(void) const;
    void rmShowPage(int p);
    void rmFree(void);
    void setRail(bool on);
    void mapFit(void);
    void mapCentre(void);
    void mapZoom(float factor);
    void mapPacket(const Packet &pk);
    void mapDraw(lv_event_t *e);
    void mapTap(lv_point_t pt);
    void refreshMapStrip(void);
    bool mapXY(int32_t lat, int32_t lon, int32_t &x, int32_t &y) const;
    void goBack(void);
    void wake(void);
    // wallpaper (a boot picture, dimmed toward the theme's background, in PSRAM) and the lock screen
    lv_image_dsc_t wpDsc = {};
    uint16_t *wpBuf = nullptr;
    int wpPic = -1, wpTheme = -1;
    const lv_image_dsc_t *wallpaper(void);
    lv_obj_t *lockObj = nullptr, *lockTime = nullptr, *lockDate = nullptr, *lockInfo = nullptr;
    void lock(void);
    void unlock(void);
    void refreshLock(void);
    void refocus(lv_obj_t *prefer = nullptr);
    lv_obj_t *nearest(lv_obj_t *from, uint32_t k) const;
    lv_obj_t *topOverlay(void) const;
    void applyTheme(int t);

    // on-screen keyboard, on boards without one: opens when a text field is tapped
    lv_obj_t *kb = nullptr, *kbLifted = nullptr;
    int32_t kbHeight(void) const;

  public:
    // helpers used by the static event callbacks
    void navClicked(int t)
    {
        if (t == T_MORE && tab == T_MORE && moreOpen >= 0)
            return openMore(-1); // More again, deep in a category: back to the six
        selectTab(t);
    }
    bool kbNeeded(void) const { return !m.hasKeyboard(); }
    void kbShow(lv_obj_t *ta);
    void kbHide(void);
    bool kbFor(lv_obj_t *ta) const; // the keyboard is open, typing into ta
    friend struct HarnessAccess; // desktop screenshot harness only
};

} // namespace hx
