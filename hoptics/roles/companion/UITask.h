// Hoptics-MC companion: replaces MeshCore's companion_radio/ui-new UITask (found first on the include path).
// It is the screens' Model: it reads MeshCore's contacts, channels, prefs and radio, sends on the screen's behalf,
// and forwards MeshCore's events (messages, ACKs, every packet heard or sent) to the screens. MIT licence.
#pragma once

#include <Arduino.h>
#include <vector>
#include <MeshCore.h>
#include <helpers/MultiSerialInterface.h>
#include <helpers/SensorManager.h>
#include <helpers/ui/DisplayDriver.h>

#include "AbstractUITask.h" // MeshCore companion_radio (on the include path)
#include "NodePrefs.h"
#include "hx_hal.h"
#include "hx_ui.h"

class UITask : public AbstractUITask, public hx::Model
{
  public:
    UITask(mesh::MainBoard *board, MultiSerialInterface *serial);
    void begin(DisplayDriver *display, SensorManager *sensors, NodePrefs *node_prefs);

    // AbstractUITask
    void notify(UIEventType t = UIEventType::none) override {}
    void loop() override;

    // MyMesh::Listener
    void onMessageRecv(mesh::Packet *pkt, const ContactInfo &from, uint8_t txt_type, uint32_t sender_timestamp, const char *text) override;
    void onChannelMessageRecv(mesh::Packet *pkt, ChannelDetails &channel_details, const char *text) override;
    void onQueueSizeChanged(int msgcount) override {}
    void onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t *path) override;
    void onACKRecv(uint32_t ack_crc) override;
    bool onUnhandledResponse(const ContactInfo &from, uint32_t tag, const uint8_t *data, uint8_t len) override;

    // hx::Model
    hx::Role role() const override { return hx::Role::Companion; }
    const char *nodeName() const override;
    void pubKey(uint8_t out[32]) const override;
    const char *firmwareVersion() const override;
    const char *boardName() const override;
    uint32_t now() const override;
    int batteryPercent() const override;
    uint16_t batteryMv() const override;
    bool appConnected() const override { return hasConnection(); }
    bool gpsFix(int32_t &lat, int32_t &lon) const override;
    int gpsSatellites(bool &on, bool &fix) const override;
    bool gpsCanSleep() const override;
    hx::Radio radio() const override;
    hx::Stats stats() const override;
    size_t contactCount() const override;
    bool contactAt(size_t i, hx::Contact &out) const override;
    bool findContact(const uint8_t key[6], hx::Contact &out) const override;
    size_t channelCount() const override;
    bool channelAt(size_t i, hx::Channel &out) const override;
    uint32_t contactsVersion() const override { return contactsVer; }
    uint32_t sendText(const hx::ConvKey &to, const char *text) override;
    bool sendAdvert(bool flood) override;
    bool resetPath(const uint8_t key[6]) override;
    bool setNodeName(const char *name) override;
    bool setRadio(const hx::Radio &r) override;
    bool bluetoothEnabled() const override;
    uint32_t bluetoothPin() const override;
    bool setBluetooth(bool on) override;
    bool repeatEnabled() const override;
    bool setRepeat(bool on) override;
    bool getSetting(const char *key, int32_t &value) const override;
    bool setSetting(const char *key, int32_t value) override;
    bool getSettingText(const char *key, char *out, size_t n) const override;
    bool setSettingText(const char *key, const char *text) override;
    uint32_t remoteLogin(const uint8_t key[6], const char *password) override;
    uint32_t remoteStatus(const uint8_t key[6]) override;
    uint32_t remoteNeighbours(const uint8_t key[6]) override;
    uint32_t remoteTelemetry(const uint8_t key[6]) override;
    uint32_t remoteAccessList(const uint8_t key[6]) override;
    uint32_t remoteCommand(const uint8_t key[6], const char *cmd) override;
    bool contactKeyHex(const uint8_t key[6], char out[65]) const override;
    bool savedPassword(const uint8_t key[6], char *out, size_t n) const override;
    void savePassword(const uint8_t key[6], const char *password) override;
    bool wifiSupported() const override { return true; }
    hx::WifiStatus wifi() const override;
    void setWifi(bool on) override;
    bool wifiJoin(const char *ssid, const char *password) override;
    void wifiForget() override;
    void wifiScan() override;
    int wifiScanResults(hx::WifiNet *out, int max) override;
    bool feedSupported() const override { return true; }
    hx::FeedStatus feed() const override;
    void setFeed(bool on) override;
    bool setFeedServer(const char *url) override;
    bool webSupported() const override { return true; }
    hx::WebStatus web() const override;
    void setWeb(bool on) override;
    void newWebPin() override;
    bool channelWebKey(size_t i, char out[33]) const override;
    void webExtras(std::string &out, uint32_t histHave) const override;
    bool saveData(const char *name, const std::string &data) override;
    bool loadData(const char *name, std::string &out) const override;
    int uiPref(const char *key, int def) const override;
    void setUiPref(const char *key, int value) override;
    void setBacklight(uint8_t level) override;
    bool hasKeyboard() const override { return hxhal::hasKeyboard(); }
    bool hasKeyLight() const override { return hxhal::hasKeyLight(); }
    bool sendLog() override;
    void setKeyLight(uint8_t level) override { hxhal::setKeyLight(level); }

    // packet log hooks (patched MeshCore, see patches/meshcore.patch)
    void rxLog(float snr, float rssi, const uint8_t *raw, int len);
    void txLog(const uint8_t *raw, int len);

  private:
    NodePrefs *prefs = nullptr;
    SensorManager *sensors = nullptr;
    hx::App *app = nullptr;
    bool ready = false;
    uint32_t contactsVer = 1;
    struct Pending {
        uint32_t ack = 0, deadline = 0;
    } pending[8];
    void decode(const uint8_t *raw, int len, hx::Packet &p) const;
    // the one remote-admin request waiting for its answer (MeshCore matches answers by tag, a login by its sender)
    struct RemoteReq {
        uint8_t kind = 0, key[6] = {};
        uint32_t tag = 0, until = 0;
    } rreq;
    uint32_t remoteSend(const uint8_t key[6], uint8_t kind, const uint8_t *req, uint8_t len);
    // Wi-Fi
    bool wifiOn = false, ntpStarted = false, clockSynced = false;
    int scanState = -2; // as wifiScanResults()
    std::vector<hx::WifiNet> scanFound;
    uint32_t lastClockCheck = 0;
    void wifiStart(void);
    void wifiTick(void);
    bool wifiWasUp = false;
    uint32_t wifiDownSince = 0;
    // GPS: some boards come with either of two GPS chips that talk at different speeds (T-Deck Plus: u-blox at 38400 or
    // Quectel L76K at 9600). When everything the GPS sends fails its checksum, the next speed is tried and the one that
    // works is kept ("gps_baud")
    uint32_t gpsCheckAt = 0, gpsOk = 0, gpsBytes = 0, gpsBaud = 0; // gpsBaud: the speed we set (0 = the board's own)
    uint8_t gpsBaudTry = 0;
    bool gpsFresh = false; // the speed just changed: the next reading is only a starting point
    void gpsTick(void);
    bool gpsAsleep = false; // the power schedule has it off
    uint32_t gpsSince = 0, gpsFixAt = 0;
    void gpsSchedule(LocationProvider *lp, uint32_t ms);
    // live feed. Recent packets by hash: what the radio heard itself, what the feed showed, what we sent and which
    // observers heard it (to say "heard by 3 observers")
    struct Seen {
        uint8_t hash[6];
        uint8_t flags, nObs;
        uint16_t obs[8];
        uint32_t sentId;
    };
    enum : uint8_t { SEEN_RF = 1, SEEN_NET = 2, SEEN_SHOWN = 4, SEEN_MINE = 8, SEEN_USED = 0x80 };
    Seen seen[96] = {};
    int seenNext = 0;
    // the web page (hoptics/net/hx_web): on/off and the PIN are kept in NVS ("web_on", "web_pin")
    bool webOn = false, mdnsUp = false;
    char webPin[8] = "", webHost[24] = "";
    void webTick(void);
public:
    void writeSaves(void); // the files waiting to be written (saveData), now
private:
    bool writeData(const char *name, const std::string &data);
    // a day of 5-minute samples for the web page's charts (in PSRAM)
    struct Sample {
        uint16_t battMv, rx, tx, heapK, nodes;
        int16_t noise, rssi;
        uint8_t lvgl;
    };
    Sample *hist = nullptr;
    uint16_t histN = 0, histHead = 0;
    uint32_t histEnd = 0, histAt = 0, histRx = 0, histTx = 0, histLvglUs = 0, histLvglFrom = 0;
    int32_t rssiSum = 0;
    uint16_t rssiCount = 0;
    void histTick(uint32_t ms);
    struct LoadNow {
        uint32_t loop = 0, longest = 0, ui = 0, lvgl = 0, feed = 0;
    } loadNow;
    bool feedOn = false;
    uint32_t chanSendId = 0, feedMsgs = 0;
    Seen *seenAt(const uint8_t *raw, int len, bool add); // by the packet's hash; add: a new entry when not there
    Seen *seenAt(const mesh::Packet &pkt, bool add);
    void feedStart(void);
    void feedTick(void);
    bool feedChannel(const mesh::Packet &pkt);
};
