// Hoptics-MC: what the screens need from the radio, and what they can ask it to do.
// The screens never touch MeshCore directly: each role (companion, repeater, room server, observer) implements
// this interface, and the PC harness implements it with demo data. MIT licence, see LICENSE.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace hx
{

enum class NodeType : uint8_t { Unknown = 0, Chat = 1, Repeater = 2, Room = 3, Sensor = 4 };

// a conversation: a group channel (index) or a direct chat with a contact (first 6 bytes of its public key)
struct ConvKey {
    bool dm = false;
    uint8_t channel = 0;
    uint8_t key[6] = {};
    bool operator==(const ConvKey &o) const;
};

struct Contact {
    uint8_t key[6] = {};     // public key prefix
    char name[32] = {};
    NodeType type = NodeType::Unknown;
    int8_t pathLen = -1;     // hops on the known route, -1 = no route yet (flood)
    uint32_t lastAdvert = 0; // unix time of its last advert (its clock)
    uint32_t lastHeard = 0;  // unix time we last heard it (our clock)
    bool hasPos = false;
    int32_t lat = 0, lon = 0; // degrees * 1e6
    bool favourite = false;
};

struct Channel {
    uint8_t index = 0;
    char name[32] = {};
};

// one packet as heard by the radio (the Live tab)
enum class PktType : uint8_t {
    Req = 0, Resp, Text, Ack, Advert, GroupText, GroupData, AnonReq, Path, Trace, Multipart, Control, Raw, Other
};
struct Packet {
    uint32_t time = 0; // unix
    PktType type = PktType::Other;
    bool direct = false;     // direct route (false: flood)
    uint8_t pathLen = 0;     // hops so far
    uint8_t path[16] = {};   // first path hashes (as heard): 16 one-byte or 8 two-byte hops
    uint8_t hashSize = 1;    // bytes per path hash
    int16_t rssi = 0;        // dBm
    int8_t snr4 = 0;         // SNR * 4
    uint16_t len = 0;        // bytes on air
    char from[24] = {};      // sender name, when known (adverts, channel messages)
    bool mine = false;       // sent by this radio
    bool hasFromKey = false; // adverts: the sender's public key prefix
    uint32_t seq = 0;        // its number in the screen's list (the web page asks for what is newer)
    bool hasHash = false;    // MeshCore's packet hash: the same for every copy of a packet, whatever its path
    uint8_t hash[8] = {};
    uint8_t fromKey[6] = {};
    bool hasPos = false;     // adverts that carry a location
    int32_t lat = 0, lon = 0; // degrees * 1e6
    bool viaNet = false;     // from the live feed: another radio (an observer) heard it, not this one
    uint8_t obsKey[6] = {};  // that observer's public key prefix
    char via[20] = {};       // and its name
};

struct Radio {
    float freq = 910.525f; // MHz
    float bw = 62.5f;      // kHz
    uint8_t sf = 7, cr = 5;
    int8_t txPower = 22; // dBm
};

struct Stats {
    uint32_t rx = 0, tx = 0, rxFlood = 0, rxDirect = 0, dups = 0;
    uint32_t airtimeTxMs = 0, uptimeS = 0;
    int16_t noiseFloor = 0; // dBm, 0 = unknown
    int16_t lastRssi = 0;
    int8_t lastSnr4 = 0;
};

enum class Role : uint8_t { Companion = 0, Repeater, RoomServer, Observer, RepeaterObserver, FieldStation };

// managing a repeater or room server you have the password for (MeshCore's remote admin)
struct RemoteStatus {     // its answer to a status request (repeaters send all of it, room servers up to dupFlood)
    uint16_t battMv = 0, txQueue = 0;
    int16_t noiseFloor = 0, lastRssi = 0;
    int8_t lastSnr4 = 0;
    uint32_t rx = 0, tx = 0, airtimeS = 0, uptimeS = 0;
    uint32_t sentFlood = 0, sentDirect = 0, recvFlood = 0, recvDirect = 0;
    uint16_t errFlags = 0, dupDirect = 0, dupFlood = 0;
    bool hasRx = false; // the two below (repeaters only)
    uint32_t rxAirtimeS = 0, rxErrors = 0;
};
struct RemoteNeighbour { // a node the repeater hears directly
    uint8_t key[6] = {};
    uint32_t heardAgoS = 0;
    int8_t snr4 = 0;
};
struct RemoteClient {    // an entry on its access list
    uint8_t key[6] = {};
    uint8_t perms = 0;   // low 2 bits: 0 guest, 1 read-only, 2 read-write, 3 admin
};

// Wi-Fi (for the street map and the clock; Bluetooth stays on for the phone app)
struct WifiNet {
    char ssid[33] = {};
    int8_t rssi = 0;
    bool open = false; // no password
};
struct WifiStatus {
    enum State : uint8_t { OFF, CONNECTING, CONNECTED, NO_NETWORK, FAILED } state = OFF;
    char ssid[33] = {}; // the saved network
    char ip[16] = {};
    int8_t rssi = 0;
    bool clockFromInternet = false; // the clock was set from the internet this session
    uint8_t reason = 0; // why the last try to join failed (Wi-Fi reason code: 15/202/204 password, 201 not found), 0 none
};

// the live feed: what the region's observers hear, over the internet from a CoreScope server (receive only)
struct FeedStatus {
    enum State : uint8_t { OFF, NO_WIFI, CONNECTING, LIVE, FAILED } state = OFF;
    char server[96] = {};
    uint32_t packets = 0;  // received since it was turned on
    uint32_t messages = 0; // channel messages it brought that the radio had not heard
    char error[64] = {};   // why it is not connected
};

// the web page: the node serves its dashboard on the Wi-Fi it is on (hoptics/net/hx_web)
struct WebStatus {
    bool on = false;      // switched on in settings
    bool running = false; // serving now (it needs Wi-Fi)
    char addr[24] = {};   // http://192.168.1.20
    char host[32] = {};   // hoptics-1a2b.local
    char pin[8] = {};     // what signs a browser in
    uint8_t sessions = 0; // browsers signed in
};

// incoming message for the chat screens
struct Message {
    ConvKey conv;
    char from[32] = {};
    const char *text = nullptr; // valid during the call
    uint32_t time = 0;
    int8_t pathLen = -1; // hops it took, -1 unknown
    int8_t snr4 = 0;
    bool viaNet = false; // from the live feed: an observer heard it, this radio didn't
};

class Model
{
  public:
    virtual ~Model() {}

    // who we are
    virtual Role role() const = 0;
    virtual const char *nodeName() const = 0;
    virtual void pubKey(uint8_t out[32]) const = 0;
    virtual const char *firmwareVersion() const = 0;
    virtual const char *boardName() const = 0;
    virtual uint32_t now() const = 0; // unix time, 0 when the clock is not set
    virtual int batteryPercent() const = 0; // -1 unknown, 101 = external power
    virtual uint16_t batteryMv() const = 0;
    virtual bool appConnected() const = 0; // a phone or computer app is connected
    virtual bool gpsFix(int32_t &lat, int32_t &lon) const = 0;
    virtual int gpsSatellites(bool &on, bool &fix) const { (void)on, (void)fix; return -1; } // -1: no GPS on this board
    virtual bool gpsCanSleep() const { return false; } // the GPS's power can be cut between fixes (ui pref "gps_sleep")

    // radio and mesh
    virtual Radio radio() const = 0;
    virtual Stats stats() const = 0;

    // contacts and channels
    virtual size_t contactCount() const = 0;
    virtual bool contactAt(size_t i, Contact &out) const = 0;
    virtual bool findContact(const uint8_t key[6], Contact &out) const = 0;
    virtual size_t channelCount() const = 0;
    virtual bool channelAt(size_t i, Channel &out) const = 0;
    virtual uint32_t contactsVersion() const = 0; // changes when the list changes

    // actions (return false when not possible right now)
    virtual uint32_t sendText(const ConvKey &to, const char *text) = 0; // returns an id for setDelivered(), 0 = failed
    virtual bool sendAdvert(bool flood) = 0;
    virtual bool resetPath(const uint8_t key[6]) = 0;

    // settings
    virtual bool setNodeName(const char *name) = 0;
    virtual bool setRadio(const Radio &r) = 0;
    virtual bool bluetoothEnabled() const = 0;
    virtual uint32_t bluetoothPin() const = 0;
    virtual bool setBluetooth(bool on) = 0;
    virtual bool repeatEnabled() const = 0;
    virtual bool setRepeat(bool on) = 0;

    // the rest of the radio's settings, by key; each role answers for the ones it has (false: not on this radio,
    // and the screen hides that row). Keys (MeshCore companion): hash_mode 0-2, multi_acks, rx_boost, cad,
    // add_mode 0 auto / 1 picked types / 2 manual, add_chat, add_rep, add_room, add_sensor, add_hops, add_full,
    // gps, gps_int (s), adv_loc, tel_base / tel_loc / tel_env 0 nobody / 1 favourites / 2 everyone, tz (hours),
    // ble_pin (0 = new each start), reboot (action). Text: scope (region name, "" = none).
    virtual bool getSetting(const char *key, int32_t &value) const
    {
        (void)key, (void)value;
        return false;
    }
    virtual bool setSetting(const char *key, int32_t value)
    {
        (void)key, (void)value;
        return false;
    }
    virtual bool getSettingText(const char *key, char *out, size_t n) const
    {
        (void)key, (void)out, (void)n;
        return false;
    }
    virtual bool setSettingText(const char *key, const char *text)
    {
        (void)key, (void)text;
        return false;
    }

    // remote admin of a repeater or room server. Each call sends one request and returns how long to wait for the
    // answer in ms (0 = not sent); the answer comes back through Ui::onRemote...(), or never (wrong password, out
    // of range): the screen gives up after the wait. key: the node's contact key prefix.
    virtual uint32_t remoteLogin(const uint8_t key[6], const char *password)
    {
        (void)key, (void)password;
        return 0;
    }
    virtual uint32_t remoteStatus(const uint8_t key[6])
    {
        (void)key;
        return 0;
    }
    virtual uint32_t remoteNeighbours(const uint8_t key[6])
    {
        (void)key;
        return 0;
    }
    virtual uint32_t remoteTelemetry(const uint8_t key[6])
    {
        (void)key;
        return 0;
    }
    virtual uint32_t remoteAccessList(const uint8_t key[6])
    {
        (void)key;
        return 0;
    }
    virtual uint32_t remoteCommand(const uint8_t key[6], const char *cmd) // its command line ("get name", "reboot"...)
    {
        (void)key, (void)cmd;
        return 0;
    }
    // a node's full public key as hex (64 chars), for commands that need it; false if it is not a contact
    virtual bool contactKeyHex(const uint8_t key[6], char out[65]) const
    {
        (void)key, (void)out;
        return false;
    }
    // its password, kept on this radio when the user asks ("" forgets it)
    virtual bool savedPassword(const uint8_t key[6], char *out, size_t n) const
    {
        (void)key, (void)out, (void)n;
        return false;
    }
    virtual void savePassword(const uint8_t key[6], const char *password) { (void)key, (void)password; }

    // Wi-Fi: false = this radio has none (the screens hide it)
    virtual bool wifiSupported() const { return false; }
    virtual WifiStatus wifi() const { return WifiStatus(); }
    virtual void setWifi(bool on) { (void)on; }
    virtual bool wifiJoin(const char *ssid, const char *password) // saves the network and connects
    {
        (void)ssid, (void)password;
        return false;
    }
    virtual void wifiForget() {}
    virtual void wifiScan() {}
    // networks found by the last scan, strongest first: count, -1 still scanning, -2 none started
    virtual bool feedSupported() const { return false; }
    virtual FeedStatus feed() const { return FeedStatus(); }
    // the web page
    virtual bool webSupported() const { return false; }
    virtual WebStatus web() const { return WebStatus(); }
    virtual void setWeb(bool on) { (void)on; }
    virtual void newWebPin() {}
    // a firmware update sent from the web page, answered on the screen: true installs it, false keeps this build
    virtual void updateAnswer(bool install) { (void)install; }
    // a channel whose key anyone can work out (Public, or a #hashtag channel whose key comes from its name): its key in
    // hex, for the page to read that channel off the live feed. Private channels' keys never leave the radio
    virtual bool channelWebKey(size_t i, char out[33]) const { (void)i, (void)out; return false; }
    // join a #hashtag channel (its key is the start of SHA-256 of its name, as in the MeshCore apps): its index, or -1
    // when the name is no good or every channel slot is taken. Already joined: its index
    virtual int joinHashChannel(const char *name) { (void)name; return -1; }
    // what the node adds to the page's data: hardware, load, the live feed's server, and the 24-hour history (only when
    // it has a newer sample than the page's, whose end time is 'histHave')
    virtual void webExtras(std::string &out, uint32_t histHave) const { (void)out, (void)histHave; }
    virtual void setFeed(bool on) { (void)on; }
    virtual bool setFeedServer(const char *url) { (void)url; return false; } // false: not an address it can use
    virtual int wifiScanResults(WifiNet *out, int max)
    {
        (void)out, (void)max;
        return -2;
    }

    // a file the screens keep their own data in (chat history), written whole each time; false: no storage
    virtual bool saveData(const char *name, const std::string &data)
    {
        (void)name, (void)data;
        return false;
    }
    virtual bool loadData(const char *name, std::string &out) const
    {
        (void)name, (void)out;
        return false;
    }

    // the screen's own settings (theme, brightness...), kept by the role in flash
    virtual int uiPref(const char *key, int def) const = 0;
    virtual void setUiPref(const char *key, int value) = 0;
    virtual void setBacklight(uint8_t level) = 0; // 0..255
    virtual bool hasKeyboard() const { return true; } // false: the screens show an on-screen keyboard to type
    virtual bool hasKeyLight() const { return false; } // a keyboard backlight (T-Deck)
    virtual bool hasSound() const { return false; }    // a buzzer or speaker for message sounds
    virtual void playSound(int tone, int volume) { (void)tone, (void)volume; } // tone 1-3, volume 0-100
    virtual bool sendLog() { return false; } // print the kept log to USB (for a problem report); false: none kept
    virtual bool saveReport() { return false; } // the same as a file on the microSD card; false: no card
    virtual void setKeyLight(uint8_t level) { (void)level; } // 0 = off
};

// the screens, as seen by a role (events arrive on the main loop)
class Ui
{
  public:
    virtual ~Ui() {}
    virtual void onMessage(const Message &m) = 0;
    virtual void onDelivered(uint32_t id, bool ok) = 0;
    virtual void onPacket(const Packet &p) = 0;
    virtual void onContactsChanged() = 0;
    virtual void toast(const char *text) = 0;
    virtual void onHeard(uint32_t id, uint8_t observers) { (void)id, (void)observers; } // a channel message you sent, on the live feed
    // answers from a repeater or room server being managed (Model::remote...)
    virtual void onRemoteLogin(const uint8_t key[6], bool ok, uint8_t perms) { (void)key, (void)ok, (void)perms; }
    virtual void onRemoteStatus(const uint8_t key[6], const RemoteStatus &s) { (void)key, (void)s; }
    virtual void onRemoteNeighbours(const uint8_t key[6], uint16_t total, const RemoteNeighbour *n, int count)
    {
        (void)key, (void)total, (void)n, (void)count;
    }
    virtual void onRemoteTelemetry(const uint8_t key[6], const uint8_t *lpp, int len) { (void)key, (void)lpp, (void)len; }
    virtual void onRemoteAccessList(const uint8_t key[6], const RemoteClient *c, int count) { (void)key, (void)c, (void)count; }
    virtual void onRemoteReply(const uint8_t key[6], const char *text) { (void)key, (void)text; } // its command line
};

const char *nodeTypeName(NodeType t);
const char *pktTypeName(PktType t);

} // namespace hx
