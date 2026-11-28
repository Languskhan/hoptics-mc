// Hoptics-MC companion adapter. MIT licence, see LICENSE.
#include "hx_log.h"
#include "UITask.h"
#include "MyMesh.h"
#include "hx_feed.h"
#include "hx_web.h"
#include "hx_ota.h"
#include "hx_parse.h"
#include "hx_sd.h"
#include "map/hx_tiles.h"
#include "hx_hal.h"
#include <Preferences.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <sys/time.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/TxtDataHelpers.h>
#include <helpers/sensors/LocationProvider.h>
#include <SHA256.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>
#include <target.h>
#include <algorithm>
#include <cmath>

// The screen runs in Arduino's loop task. Its 8 KB default ran out in LVGL's layout and focus chains when the
// More tab opened (stack canary, loopTask); 16 KB is the usual fix (arduino-esp32's SET_LOOP_TASK_STACK_SIZE).
// The status line reports what is left, so a creeping need shows before it crashes.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

#ifndef HOPTICS_VERSION
#define HOPTICS_VERSION "0.1.0"
#endif

namespace
{
UITask *gTask = nullptr;
volatile uint8_t wifiReason = 0; // from the Wi-Fi event task: why the last join failed (0 once connected)
Preferences nvs;
bool nvsOpen = false;
void openNvs()
{
    if (!nvsOpen)
        nvsOpen = nvs.begin("hoptics", false);
}
} // namespace

// called by the patched MyMesh::logRxRaw() / logTx() for every packet heard or sent
void hoptics_on_rx(float snr, float rssi, const uint8_t *raw, int len)
{
    if (gTask)
        gTask->rxLog(snr, rssi, raw, len);
}
void hoptics_on_tx(const uint8_t *raw, int len)
{
    if (gTask)
        gTask->txLog(raw, len);
}

UITask::UITask(mesh::MainBoard *board, MultiSerialInterface *serial) : AbstractUITask(board, serial)
{
    gTask = this;
}

namespace
{
// mbedTLS (HTTPS for the street map, the live feed) takes 40 KB or more per connection, and this Arduino core gives it
// internal RAM only, which Wi-Fi and Bluetooth need too. Its buffers go to PSRAM instead (internal RAM when there is none).
void *tlsCalloc(size_t n, size_t size)
{
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
void tlsFree(void *p) { heap_caps_free(p); }
} // namespace

void UITask::begin(DisplayDriver *display, SensorManager *s, NodePrefs *p)
{
    (void)display;
    mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree);
    hxota::bootCheck(); // a newer build flashed over USB after a web update starts instead
    hxsound::begin();   // a buzzer pin held low from the start
    if (hxota::trial())
        hxlog::line("update: this build is on trial, kept after a healthy minute");
    prefs = p;
    sensors = s;
    openNvs();
    if (!hxhal::begin())
        return; // no panel: the radio still works as a companion for the apps
    hxsd::begin(); // the card, if one is in (its bus is the screen's, now started); before the map can look for it
    app = new hx::App(*this);
    app->begin();
    ready = true;
    if (nvsOpen && nvs.getInt("wifi_on", 0) && nvs.isKey("wifi_ssid"))
        wifiStart(); // it was on: join the saved network again
    if (nvsOpen && nvs.getInt("feed_on", 0))
        feedStart();
    {
        uint8_t k[32];
        pubKey(k);
        snprintf(webHost, sizeof(webHost), "hoptics-%02x%02x", k[0], k[1]);
    }
    webOn = nvsOpen && nvs.getInt("web_on", 0);
    // getString counts the end-of-string byte: a good PIN reads back as 7
    if (!nvsOpen || nvs.getString("web_pin", webPin, sizeof(webPin)) != 7 || strspn(webPin, "0123456789") != 6)
        newWebPin();
    hxweb::setPin(webPin);
    if (nvsOpen && nvs.getInt("gps_baud", 0) > 0 && sensors && sensors->getLocationProvider()) {
        gpsBaud = (uint32_t)nvs.getInt("gps_baud", 0); // the speed found to work last time
        Serial1.updateBaudRate(gpsBaud);
        gpsFresh = true;
    }
}

// how hard the main loop works, for the status line: passes per second, the longest pass (the most a key press waits),
// and the share of the time spent on the screen and on the live feed. Counted over each 10 s between status lines
namespace
{
struct Load {
    uint32_t from = 0, prev = 0, passes = 0, worst = 0, ui = 0, lvgl = 0, lvglWorst = 0, feed = 0, feedTask = 0, frames = 0;
    uint32_t refr = 0, draw = 0; // LVGL's own counters at the start of the window
} load;
uint32_t lvNext = 0; // when LVGL next has a timer due
} // namespace

void UITask::loop()
{
    const uint32_t t0 = micros();
    if (load.prev && t0 - load.prev > load.worst)
        load.worst = t0 - load.prev;
    load.prev = t0;
    load.passes++;
    // a status line every 10 s on the USB serial port, for hardware reports (the screen may be what's broken)
    hxlog::tick();
    hxsd::tick(); // card work another task is waiting for (the map, the web page)
    static bool kept = false;
    if (!kept && millis() > 60000) { // a healthy minute: a build on trial keeps itself
        kept = true;
        hxota::confirm();
    }
    static uint32_t askedAt = 0; // a web update waiting for a tap: asked (again if the screen was rebuilt), dropped after two minutes
    if (!hxota::waiting())
        askedAt = 0;
    else if (!askedAt || millis() - askedAt <= 120000) {
        if (!askedAt)
            askedAt = millis() | 1;
        if (app)
            app->askUpdate(hxota::waitingKB()); // no-op while the question shows
    } else {
        hxota::decline();
        hxlog::line("update: nobody answered on the screen");
        if (app)
            app->askUpdateGone();
    }
    static uint32_t restartAt = 0;
    if (hxota::restartWanted) { // an update came in: restart once the page has its answer
        if (!restartAt)
            restartAt = millis() | 1;
        else if (millis() - restartAt > 1500) {
            hxlog::line("restarting into the update");
            writeSaves();
            hxlog::save();
            the_mesh.savePrefs();
            delay(100);
            _board->reboot();
        }
    }
    static uint32_t lastBeat = 0, beats = 0;
    if (millis() - lastBeat > 10000) {
        lastBeat = millis();
        const uint32_t span = std::max<uint32_t>(1, lastBeat - load.from) * 10; // per cent of this many microseconds
        const uint32_t task = hxfeed::busyUs(), frames = hxhal::framesDrawn();
        uint32_t refr, draw;
        hxhal::refreshTimes(refr, draw);
        char busy[200];
        snprintf(busy, sizeof(busy),
                 ", loop %lu/s, longest %lu ms, ui %lu%%, lvgl %lu%% (layout %lu%%, draw %lu%%, %lu frames, longest %lu ms), feed %lu%% + task %lu%%",
                 (unsigned long)(load.passes * 10000ull / span), (unsigned long)(load.worst / 1000), (unsigned long)(load.ui / span),
                 (unsigned long)(load.lvgl / span), (unsigned long)((refr - load.refr - (draw - load.draw)) / span),
                 (unsigned long)((draw - load.draw) / span), (unsigned long)(frames - load.frames), (unsigned long)(load.lvglWorst / 1000),
                 (unsigned long)(load.feed / span), (unsigned long)((task - load.feedTask) / span));
        loadNow.loop = (uint32_t)(load.passes * 10000ull / span);
        loadNow.longest = load.worst / 1000;
        loadNow.ui = load.ui / span;
        loadNow.lvgl = load.lvgl / span;
        loadNow.feed = load.feed / span;
        histLvglUs += load.lvgl;
        load = Load();
        load.from = lastBeat;
        load.feedTask = task;
        load.frames = frames;
        load.refr = refr;
        load.draw = draw;
        char gps[160] = "";
        if (LocationProvider *lp = sensors ? sensors->getLocationProvider() : nullptr) { // no position, only how it is doing
            char d[150];
            lp->formatDiagnostics(d, sizeof(d));
            snprintf(gps, sizeof(gps), ", gps %s", d);
        }
        // every 10 s on USB; every 5 minutes into the kept log too
        (beats++ % 30 ? hxlog::usb : hxlog::line)("%s, up %lus, heap %u, psram %u, stack left %u, batt %u mV, cpu %u MHz%s%s",
                                                 ready ? "ui running" : "ui NOT started (display)", (unsigned long)(millis() / 1000),
                                                 (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(),
                                                 (unsigned)uxTaskGetStackHighWaterMark(nullptr), (unsigned)getBattMilliVolts(),
                                                 (unsigned)getCpuFrequencyMhz(), busy, gps);
        load.prev = micros(); // writing the line out isn't a slow pass
    }
    if (!ready)
        return;
    // inputs
    bool input = false;
    if (hxhal::takeTouched()) {
        app->touched();
        input = true;
    }
    for (int i = 0; i < 4; i++) { // a few keys per pass, so fast typing keeps up
        uint32_t k = hxhal::readKey();
        if (!k)
            break;
        app->key(k);
        input = true;
    }
    // messages that never got an ACK
    uint32_t ms = millis();
    for (auto &pd : pending)
        if (pd.ack && (int32_t)(ms - pd.deadline) > 0) {
            app->onDelivered(pd.ack, false);
            pd.ack = 0;
        }
    if (rreq.kind && (int32_t)(ms - rreq.until) > 0)
        rreq.kind = 0; // a remote-admin answer that never came (the screen has given up too)
    wifiTick();
    uint32_t t = micros();
    feedTick();
    load.feed += micros() - t;
    gpsTick();
    // the screen: LVGL runs when one of its timers is due (it says when the next one is), and at once after a key or a
    // touch so the answer shows on the next frame; it redraws only what changed
    histTick(ms);
    hxweb::Query wq;
    if (hxweb::take(wq)) { // a browser is waiting for data: build it on this pass
        std::string body;
        if (wq.kind == hxweb::Query::LOG) {
            hxlog::text(body);
            if (hxsd::mounted()) { // a copy on the card, as the device's own "save report" does
                char path[64];
                cardName(path, sizeof(path), "/hoptics/logs/report-", ".txt");
                hxsd::writeFile(path, body.data(), body.size());
            }
        } else if (wq.kind == hxweb::Query::BACKUP)
            webBackup(body, (wq.parts & hxweb::Query::KEYS) != 0);
        else if (wq.kind == hxweb::Query::RESTORE) {
            webRestore(wq.data, wq.len, wq.parts, body);
            free(wq.data); // ours once taken (hx_web.h)
        } else if (wq.kind == hxweb::Query::SEND)
            body = app->webSend(wq.channel, wq.text);
        else if (wq.kind == hxweb::Query::JOIN)
            body = app->webJoin(wq.text);
        else
            app->webJson(body, wq.contacts, wq.packets, wq.msgs, wq.hist);
        hxweb::answer(std::move(body));
    }
    t = micros();
    hx::busy = "";
    app->loop(ms);
    const uint32_t t1 = micros();
    load.ui += t1 - t;
    if (t1 - t > 300000) // a slow pass: what the screen was doing, to find the cause
        hxlog::line("ui: %lu ms in the screen loop (%s)", (unsigned long)((t1 - t) / 1000), hx::busy[0] ? hx::busy : "other");
    if (input || (int32_t)(ms - lvNext) >= 0) {
        hx::busy = "";
        const uint32_t wait = lv_timer_handler();
        lvNext = millis() + std::min<uint32_t>(wait, 15);
        const uint32_t d = micros() - t1;
        load.lvgl += d;
        if (d > load.lvglWorst)
            load.lvglWorst = d;
        if (d > 300000)
            hxlog::line("ui: %lu ms drawing or answering a touch (%s)", (unsigned long)(d / 1000), hx::busy[0] ? hx::busy : "drawing");
    }
}

// ------------------------------------------------------------------ events from MeshCore
void UITask::onMessageRecv(mesh::Packet *pkt, const ContactInfo &from, uint8_t txt_type, uint32_t sender_timestamp, const char *text)
{
    if (ready && txt_type == TXT_TYPE_CLI_DATA) { // a repeater's (or room server's) command line answering
        app->onRemoteReply(from.id.pub_key, text);
        return;
    }
    if (!ready || txt_type != TXT_TYPE_PLAIN)
        return;
    hx::Message m;
    m.conv.dm = true;
    memcpy(m.conv.key, from.id.pub_key, 6);
    strncpy(m.from, from.name, sizeof(m.from) - 1);
    m.text = text;
    m.time = sender_timestamp;
    m.pathLen = pkt->isRouteFlood() ? pkt->getPathHashCount() : -1;
    m.snr4 = (int8_t)(pkt->getSNR() * 4);
    app->onMessage(m);
}

void UITask::onChannelMessageRecv(mesh::Packet *pkt, ChannelDetails &ch, const char *text)
{
    if (!ready)
        return;
    if (feedOn) { // the live feed may have brought it already
        Seen *sn = seenAt(*pkt, true);
        if (sn && (sn->flags & SEEN_SHOWN))
            return;
        if (sn)
            sn->flags |= SEEN_SHOWN;
    }
    hx::Message m;
    // which channel index is this?
    ChannelDetails d;
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++)
        if (the_mesh.getChannel(i, d) && memcmp(d.channel.secret, ch.channel.secret, sizeof(d.channel.secret)) == 0) {
            m.conv.channel = (uint8_t)i;
            break;
        }
    // channel text arrives as "sender: message"
    const char *sep = strstr(text, ": ");
    if (sep && sep - text < (int)sizeof(m.from)) {
        memcpy(m.from, text, sep - text);
        m.from[sep - text] = 0;
        m.text = sep + 2;
    } else
        m.text = text;
    m.time = the_mesh.getRTCClock()->getCurrentTime();
    m.pathLen = pkt->isRouteFlood() ? pkt->getPathHashCount() : -1;
    m.snr4 = (int8_t)(pkt->getSNR() * 4);
    app->onMessage(m);
}

void UITask::onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t *path)
{
    contactsVer++;
    if (ready)
        app->onContactsChanged();
}

void UITask::onACKRecv(uint32_t ack_crc)
{
    for (auto &pd : pending)
        if (pd.ack && pd.ack == ack_crc) {
            if (ready)
                app->onDelivered(ack_crc, true);
            pd.ack = 0;
        }
}

// ------------------------------------------------------------------ packets heard and sent
namespace
{
// an advert's signature, checked the way MeshCore checks it before adding a contact (Mesh::onRecvPacket): a name and a
// place go on the map and the web page only when the node's own key signed them. Packets heard over the radio come
// here before MeshCore's own check, and the live feed's never reach it at all, so without this anyone could put a
// made-up node anywhere on the map. Ed25519 takes a few milliseconds: at most 8 are checked a second, and past that
// an advert shows without a name or place, so a flood of fake adverts can't stall the screen
bool advertSigned(const uint8_t *pay, int plen)
{
    static uint32_t windowAt = 0;
    static uint8_t checked = 0;
    const uint32_t ms = millis();
    if (ms - windowAt >= 1000) {
        windowAt = ms;
        checked = 0;
    }
    if (checked >= 8)
        return false;
    checked++;
    const int head = PUB_KEY_SIZE + 4; // [public key 32][timestamp 4][signature 64][app data]
    int alen = plen - head - SIGNATURE_SIZE;
    if (alen < 0)
        return false;
    if (alen > MAX_ADVERT_DATA_SIZE) // MeshCore signs (and reads) only this much
        alen = MAX_ADVERT_DATA_SIZE;
    uint8_t msg[PUB_KEY_SIZE + 4 + MAX_ADVERT_DATA_SIZE];
    memcpy(msg, pay, head);
    memcpy(msg + head, pay + head + SIGNATURE_SIZE, alen);
    mesh::Identity id(pay);
    return id.verify(pay + head, msg, head + alen);
}
} // namespace

void UITask::decode(const uint8_t *raw, int len, hx::Packet &p) const
{
    if (len < 2)
        return;
    uint8_t h = raw[0];
    uint8_t route = h & 0x03, type = (h >> 2) & 0x0F;
    p.direct = route == ROUTE_TYPE_DIRECT || route == ROUTE_TYPE_TRANSPORT_DIRECT;
    static const hx::PktType map[16] = {hx::PktType::Req,      hx::PktType::Resp,    hx::PktType::Text,      hx::PktType::Ack,
                                        hx::PktType::Advert,   hx::PktType::GroupText, hx::PktType::GroupData, hx::PktType::AnonReq,
                                        hx::PktType::Path,     hx::PktType::Trace,   hx::PktType::Multipart, hx::PktType::Control,
                                        hx::PktType::Other,    hx::PktType::Other,   hx::PktType::Other,     hx::PktType::Raw};
    p.type = map[type];
    {
        mesh::Packet mp; // MeshCore's own hash: the web page matches the internet's copies of this packet by it
        if (mp.readFrom(raw, (uint8_t)std::min(len, 255))) {
            mp.calculatePacketHash(p.hash);
            p.hasHash = true;
        }
    }
    int i = 1;
    if (route == ROUTE_TYPE_TRANSPORT_FLOOD || route == ROUTE_TYPE_TRANSPORT_DIRECT)
        i += 4;
    if (i >= len)
        return;
    uint8_t pl = raw[i++];
    p.hashSize = (pl >> 6) + 1;
    p.pathLen = pl & 63;
    int pbytes = p.pathLen * p.hashSize;
    memcpy(p.path, raw + i, std::min<int>(std::min<int>(pbytes, sizeof(p.path)), std::max(0, len - i)));
    i += pbytes;
    if (i > len)
        return;
    const uint8_t *pay = raw + i;
    int plen = len - i;
    // who sent it, where the packet says so in the clear
    if (type == PAYLOAD_TYPE_ADVERT && plen > 32 + 4 + 64 && advertSigned(pay, plen)) {
        memcpy(p.fromKey, pay, sizeof(p.fromKey)); // [public key 32][timestamp 4][signature 64][app data]
        p.hasFromKey = true;
        const uint8_t *app = pay + 32 + 4 + 64;
        int alen = std::min<int>(plen - (32 + 4 + 64), MAX_ADVERT_DATA_SIZE); // only what the signature covers
        uint8_t flags = app[0];
        int j = 1;
        if ((flags & ADV_LATLON_MASK) && alen >= 9) {
            memcpy(&p.lat, app + 1, 4);
            memcpy(&p.lon, app + 5, 4);
            p.hasPos = p.lat != 0 || p.lon != 0;
        }
        if (flags & ADV_LATLON_MASK)
            j += 8;
        if (flags & ADV_FEAT1_MASK)
            j += 2;
        if (flags & ADV_FEAT2_MASK)
            j += 2;
        if ((flags & ADV_NAME_MASK) && j < alen) {
            int n = std::min<int>(alen - j, sizeof(p.from) - 1);
            memcpy(p.from, app + j, n);
            p.from[n] = 0;
        }
    } else if (type == PAYLOAD_TYPE_GRP_TXT || type == PAYLOAD_TYPE_GRP_DATA) {
        if (plen >= 1) { // channel hash: name the channel if it is one of ours
            ChannelDetails d;
            for (int c = 0; c < MAX_GROUP_CHANNELS; c++)
                if (the_mesh.getChannel(c, d) && d.name[0] && d.channel.hash[0] == pay[0]) {
                    snprintf(p.from, sizeof(p.from), "#%s", d.name);
                    break;
                }
        }
    } else if ((type == PAYLOAD_TYPE_TXT_MSG || type == PAYLOAD_TYPE_REQ || type == PAYLOAD_TYPE_RESPONSE || type == PAYLOAD_TYPE_PATH) &&
               plen >= 2) {
        // [dest hash][src hash]: name the sender when exactly one contact matches its first key byte
        int matches = 0;
        ContactInfo c, found;
        for (int k = MAX_ANON_CONTACTS; k < the_mesh.getTotalContactSlots() && matches < 2; k++)
            if (the_mesh.getContactByIdx(k, c) && c.id.pub_key[0] == pay[1]) {
                found = c;
                matches++;
            }
        if (matches == 1)
            strncpy(p.from, found.name, sizeof(p.from) - 1);
    }
}

void UITask::rxLog(float snr, float rssi, const uint8_t *raw, int len)
{
    if (!ready)
        return;
    if (feedOn) {
        Seen *sn = seenAt(raw, len, true);
        if (sn)
            sn->flags |= SEEN_RF;
    }
    hx::Packet p;
    p.time = now();
    p.snr4 = (int8_t)(snr * 4);
    p.rssi = (int16_t)rssi;
    p.len = (uint16_t)len;
    decode(raw, len, p);
    rssiSum += (int32_t)rssi;
    rssiCount++;
    app->onPacket(p);
}

void UITask::txLog(const uint8_t *raw, int len)
{
    if (!ready)
        return;
    hx::Packet p;
    p.time = now();
    p.len = (uint16_t)len;
    p.mine = true;
    decode(raw, len, p);
    if (!p.from[0] || p.type == hx::PktType::Advert)
        strncpy(p.from, prefs->node_name, sizeof(p.from) - 1);
    if (feedOn) { // so its copies on the live feed can be counted ("heard by 3 observers")
        Seen *sn = seenAt(raw, len, true);
        if (sn) {
            sn->flags |= SEEN_MINE;
            if (p.type == hx::PktType::GroupText && chanSendId) {
                sn->sentId = chanSendId;
                chanSendId = 0;
            }
        }
    }
    app->onPacket(p);
}

// ------------------------------------------------------------------ hx::Model
const char *UITask::nodeName() const { return prefs ? prefs->node_name : ""; }
void UITask::pubKey(uint8_t out[32]) const { memcpy(out, the_mesh.self_id.pub_key, 32); }
const char *UITask::firmwareVersion() const
{
    static char v[48];
    snprintf(v, sizeof(v), HOPTICS_VERSION " (MeshCore %s)", FIRMWARE_VERSION);
    return v;
}
const char *UITask::boardName() const { return hxhal::boardName(); }
// MeshCore's clock can be an I2C chip (the ThinkNode M9's: six register reads each time, about a millisecond), and the
// screens ask for the time on every pass of the main loop: read it once a second and count the rest with millis()
uint32_t UITask::now() const
{
    static uint32_t at = 0, base = 0;
    const uint32_t ms = millis();
    if (!at || ms - at >= 1000) {
        base = the_mesh.getRTCClock()->getCurrentTime();
        at = ms;
    }
    const uint32_t t = base + (ms - at) / 1000;
    return t > 1600000000 ? t : 0;
}
uint16_t UITask::batteryMv() const { return getBattMilliVolts(); }
int UITask::batteryPercent() const
{
    uint16_t mv = getBattMilliVolts();
    if (mv < 2500)
        return -1;
    // single-cell Li-ion, rough curve: 3.30 V empty, 4.15 V full
    int pct = (int)((mv - 3300) * 100L / (4150 - 3300));
    return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}
bool UITask::gpsFix(int32_t &lat, int32_t &lon) const
{
    if (!sensors || (sensors->node_lat == 0 && sensors->node_lon == 0))
        return false;
    lat = (int32_t)(sensors->node_lat * 1e6);
    lon = (int32_t)(sensors->node_lon * 1e6);
    return true;
}
int UITask::gpsSatellites(bool &on, bool &fix) const
{
    LocationProvider *lp = sensors ? sensors->getLocationProvider() : nullptr;
    if (!lp)
        return -1;
    on = lp->isEnabled();
    fix = lp->isValid();
    return (int)lp->satellitesCount();
}

hx::Radio UITask::radio() const
{
    hx::Radio r;
    if (prefs) {
        r.freq = prefs->freq, r.bw = prefs->bw, r.sf = prefs->sf, r.cr = prefs->cr, r.txPower = prefs->tx_power_dbm;
    }
    return r;
}
hx::Stats UITask::stats() const
{
    hx::Stats s;
    s.rx = radio_driver.getPacketsRecv();
    s.tx = radio_driver.getPacketsSent();
    s.noiseFloor = (int16_t)radio_driver.getNoiseFloor();
    s.airtimeTxMs = the_mesh.getTotalAirTime();
    s.uptimeS = millis() / 1000;
    s.lastRssi = (int16_t)radio_driver.getLastRSSI();
    s.lastSnr4 = (int8_t)(radio_driver.getLastSNR() * 4);
    return s;
}

namespace
{
void toContact(const ContactInfo &c, hx::Contact &o)
{
    memcpy(o.key, c.id.pub_key, 6);
    strncpy(o.name, c.name, sizeof(o.name) - 1);
    o.name[sizeof(o.name) - 1] = 0;
    o.type = (hx::NodeType)(c.type <= 4 ? c.type : 0);
    o.pathLen = c.out_path_len == OUT_PATH_UNKNOWN ? -1 : (int8_t)(c.out_path_len & 63);
    o.lastAdvert = c.last_advert_timestamp;
    o.lastHeard = c.lastmod;
    o.hasPos = c.gps_lat != 0 || c.gps_lon != 0;
    o.lat = c.gps_lat;
    o.lon = c.gps_lon;
    o.favourite = c.isFav();
}
} // namespace

size_t UITask::contactCount() const { return (size_t)the_mesh.getNumContacts(); }
bool UITask::contactAt(size_t i, hx::Contact &out) const
{
    ContactInfo c;
    // slots 0..MAX_ANON_CONTACTS-1 are reserved blanks for anonymous requests
    if (!the_mesh.getContactByIdx(MAX_ANON_CONTACTS + i, c))
        return false;
    toContact(c, out);
    return true;
}
bool UITask::findContact(const uint8_t key[6], hx::Contact &out) const
{
    ContactInfo *c = the_mesh.lookupContactByPubKey(key, 6);
    if (!c)
        return false;
    toContact(*c, out);
    return true;
}
size_t UITask::channelCount() const { return MAX_GROUP_CHANNELS; }
bool UITask::channelAt(size_t i, hx::Channel &out) const
{
    ChannelDetails d;
    if (!the_mesh.getChannel((int)i, d) || !d.name[0])
        return false;
    out.index = (uint8_t)i;
    strncpy(out.name, d.name, sizeof(out.name) - 1);
    return true;
}

uint32_t UITask::sendText(const hx::ConvKey &to, const char *text)
{
    uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
    if (to.dm) {
        ContactInfo *c = the_mesh.lookupContactByPubKey(to.key, 6);
        if (!c)
            return 0;
        uint32_t ack = 0, timeout = 0;
        int r = the_mesh.sendMessage(*c, ts, 0, text, ack, timeout);
        if (r == MSG_SEND_FAILED)
            return 0;
        if (!ack)
            ack = ts; // nothing to wait for: show it as sent
        for (auto &pd : pending)
            if (!pd.ack) {
                pd.ack = ack;
                pd.deadline = millis() + (timeout ? timeout : 15000) + 5000;
                break;
            }
        return ack;
    }
    ChannelDetails d;
    if (!the_mesh.getChannel(to.channel, d))
        return 0;
    chanSendId = ts | 1; // txLog() ties the packet to this id when it goes out
    if (!the_mesh.sendGroupMessage(ts, d.channel, prefs->node_name, text, strlen(text)))
        return chanSendId = 0;
    return chanSendId; // no ACK on channels: the screen marks it as sent
}

bool UITask::sendAdvert(bool flood)
{
    mesh::Packet *pkt = prefs->advert_loc_policy == ADVERT_LOC_NONE
                            ? the_mesh.createSelfAdvert(prefs->node_name)
                            : the_mesh.createSelfAdvert(prefs->node_name, sensors ? sensors->node_lat : 0, sensors ? sensors->node_lon : 0);
    if (!pkt)
        return false;
    if (flood) {
        TransportKey scope;
        memcpy(&scope.key, prefs->default_scope_key, sizeof(scope.key));
        the_mesh.sendFloodScoped(scope, pkt, 0);
    } else
        the_mesh.sendZeroHop(pkt);
    return true;
}

bool UITask::resetPath(const uint8_t key[6])
{
    ContactInfo *c = the_mesh.lookupContactByPubKey(key, 6);
    if (!c)
        return false;
    c->out_path_len = OUT_PATH_UNKNOWN;
    the_mesh.dirty_contacts_expiry = the_mesh.futureMillis(5000); // saved with the next lazy contacts write
    contactsVer++;
    return true;
}

bool UITask::setNodeName(const char *name)
{
    if (!name || !name[0])
        return false;
    strncpy(prefs->node_name, name, sizeof(prefs->node_name) - 1);
    prefs->node_name[sizeof(prefs->node_name) - 1] = 0;
    the_mesh.savePrefs();
    return true;
}

bool UITask::setRadio(const hx::Radio &r)
{
    if (r.freq < 150 || r.freq > 2500 || r.sf < 5 || r.sf > 12 || r.cr < 5 || r.cr > 8 || r.bw < 7 || r.bw > 500)
        return false;
    if (r.txPower < -9 || r.txPower > MAX_LORA_TX_POWER)
        return false;
    if (prefs->isRepeatEn() && !the_mesh.isValidClientRepeatFreq((uint32_t)(r.freq * 1000)))
        prefs->setRepeatEn(false); // repeating is only allowed on the off-grid frequencies
    prefs->freq = r.freq, prefs->bw = r.bw, prefs->sf = r.sf, prefs->cr = r.cr, prefs->tx_power_dbm = r.txPower;
    the_mesh.savePrefs();
    radio_driver.setParams(prefs->freq, prefs->bw, prefs->sf, prefs->cr);
    radio_driver.setTxPower(prefs->tx_power_dbm);
    return true;
}

bool UITask::bluetoothEnabled() const { return isBluetoothEnabled(); }
uint32_t UITask::bluetoothPin() const { return the_mesh.getBLEPin(); }
bool UITask::setBluetooth(bool on)
{
    on ? enableBluetooth() : disableBluetooth();
    return true;
}
bool UITask::repeatEnabled() const { return prefs && prefs->isRepeatEn(); }
bool UITask::setRepeat(bool on)
{
    if (on && !the_mesh.isValidClientRepeatFreq((uint32_t)(prefs->freq * 1000))) {
        if (app)
            app->toast("MeshCore only lets a companion repeat on the off-grid frequencies (433.000, 869.495 or 918.000 MHz). "
                       "For your mesh's main frequency, flash the Hoptics repeater.");
        return false;
    }
    prefs->setRepeatEn(on);
    the_mesh.savePrefs();
    return true;
}

// ------------------------------------------------------------------ the rest of the settings, by key (see hx_model.h)
namespace
{
// MeshCore's companion auto-add flags (companion_radio/MyMesh.cpp)
constexpr uint8_t ADD_OVERWRITE = 1 << 0, ADD_CHAT = 1 << 1, ADD_REP = 1 << 2, ADD_ROOM = 1 << 3, ADD_SENSOR = 1 << 4;
constexpr uint8_t ADD_TYPES = ADD_CHAT | ADD_REP | ADD_ROOM | ADD_SENSOR;
uint8_t addBit(const char *k)
{
    return !strcmp(k, "add_chat") ? ADD_CHAT : !strcmp(k, "add_rep") ? ADD_REP : !strcmp(k, "add_room") ? ADD_ROOM : !strcmp(k, "add_sensor") ? ADD_SENSOR : 0;
}
} // namespace

bool UITask::getSetting(const char *k, int32_t &v) const
{
    if (!prefs)
        return false;
    if (!strcmp(k, "hash_mode"))
        v = prefs->path_hash_mode;
    else if (!strcmp(k, "multi_acks"))
        v = prefs->multi_acks ? 1 : 0;
    else if (!strcmp(k, "rx_boost"))
        v = prefs->rx_boosted_gain ? 1 : 0;
    else if (!strcmp(k, "cad"))
        v = prefs->cad_enabled ? 1 : 0;
    else if (!strcmp(k, "add_mode"))
        v = (prefs->manual_add_contacts & 1) == 0 ? 0 : (prefs->autoadd_config & ADD_TYPES) ? 1 : 2;
    else if (uint8_t b = addBit(k))
        v = (prefs->autoadd_config & b) ? 1 : 0;
    else if (!strcmp(k, "add_full"))
        v = (prefs->autoadd_config & ADD_OVERWRITE) ? 1 : 0;
    else if (!strcmp(k, "add_hops"))
        v = prefs->autoadd_max_hops;
    else if (!strcmp(k, "gps") || !strcmp(k, "gps_int")) {
        if (!sensors || !sensors->isGPSDetected())
            return false; // no GPS on this radio (or it isn't answering): the screen hides these
        v = !strcmp(k, "gps") ? (prefs->gps_enabled ? 1 : 0) : (int32_t)std::max<uint32_t>(1, prefs->gps_interval);
    } else if (!strcmp(k, "adv_loc"))
        v = prefs->advert_loc_policy == ADVERT_LOC_NONE ? 0 : 1;
    else if (!strcmp(k, "tel_base"))
        v = prefs->telemetry_mode_base;
    else if (!strcmp(k, "tel_loc"))
        v = prefs->telemetry_mode_loc;
    else if (!strcmp(k, "tel_env"))
        v = prefs->telemetry_mode_env;
    else if (!strcmp(k, "tz"))
        v = prefs->tz_offset;
    else if (!strcmp(k, "ble_pin"))
        v = (int32_t)prefs->ble_pin;
    else if (!strcmp(k, "reboot"))
        v = 0;
    else
        return false;
    return true;
}

bool UITask::setSetting(const char *k, int32_t v)
{
    if (!prefs)
        return false;
    if (!strcmp(k, "hash_mode")) {
        if (v < 0 || v > 2)
            return false;
        prefs->path_hash_mode = (uint8_t)v;
    } else if (!strcmp(k, "multi_acks"))
        prefs->multi_acks = v ? 1 : 0;
    else if (!strcmp(k, "rx_boost")) {
        prefs->rx_boosted_gain = v ? 1 : 0;
        radio_driver.setRxBoostedGainMode(prefs->rx_boosted_gain);
    } else if (!strcmp(k, "cad"))
        prefs->cad_enabled = v ? 1 : 0;
    else if (!strcmp(k, "add_mode")) {
        if (v == 0)
            prefs->manual_add_contacts &= ~1;
        else {
            prefs->manual_add_contacts |= 1;
            if (v == 1 && !(prefs->autoadd_config & ADD_TYPES))
                prefs->autoadd_config |= ADD_TYPES; // start from every type ticked
            if (v == 2)
                prefs->autoadd_config &= ~ADD_TYPES;
        }
    } else if (uint8_t b = addBit(k)) {
        if (v)
            prefs->autoadd_config |= b;
        else
            prefs->autoadd_config &= ~b;
    } else if (!strcmp(k, "add_full")) {
        if (v)
            prefs->autoadd_config |= ADD_OVERWRITE;
        else
            prefs->autoadd_config &= ~ADD_OVERWRITE;
    } else if (!strcmp(k, "add_hops"))
        prefs->autoadd_max_hops = (uint8_t)std::min<int32_t>(64, std::max<int32_t>(0, v));
    else if (!strcmp(k, "gps")) {
        if (!sensors || !sensors->isGPSDetected() || !sensors->setSettingValue("gps", v ? "1" : "0"))
            return false;
        prefs->gps_enabled = v ? 1 : 0;
    } else if (!strcmp(k, "gps_int")) {
        char b[12];
        snprintf(b, sizeof(b), "%ld", (long)std::max<int32_t>(1, v));
        if (!sensors || !sensors->setSettingValue("gps_interval", b))
            return false;
        prefs->gps_interval = (uint32_t)std::max<int32_t>(1, v);
    } else if (!strcmp(k, "adv_loc"))
        prefs->advert_loc_policy = v ? ADVERT_LOC_SHARE : ADVERT_LOC_NONE;
    else if (!strcmp(k, "tel_base") || !strcmp(k, "tel_loc") || !strcmp(k, "tel_env")) {
        if (v < TELEM_MODE_DENY || v > TELEM_MODE_ALLOW_ALL)
            return false;
        (!strcmp(k, "tel_base") ? prefs->telemetry_mode_base : !strcmp(k, "tel_loc") ? prefs->telemetry_mode_loc : prefs->telemetry_mode_env) =
            (uint8_t)v;
    } else if (!strcmp(k, "tz")) {
        if (v < -12 || v > 14)
            return false;
        prefs->tz_offset = (int8_t)v;
    } else if (!strcmp(k, "ble_pin")) {
        if (v != 0 && (v < 100000 || v > 999999))
            return false; // six digits, or 0 for a new one each start
        prefs->ble_pin = (uint32_t)v;
    } else if (!strcmp(k, "reboot")) {
        hxlog::line("restarting (asked from the screen)");
        writeSaves(); // what the screens were still writing
        hxlog::save();
        the_mesh.savePrefs();
        delay(100);
        _board->reboot();
        return true;
    } else
        return false;
    the_mesh.savePrefs();
    return true;
}

bool UITask::getSettingText(const char *k, char *out, size_t n) const
{
    if (!prefs || !n)
        return false;
    if (!strcmp(k, "scope")) {
        snprintf(out, n, "%s", prefs->default_scope_name);
        return true;
    }
    return false;
}

bool UITask::setSettingText(const char *k, const char *t)
{
    if (!prefs || !t)
        return false;
    if (!strcmp(k, "scope")) {
        // a region: MeshCore's key for a public "#name" region is the first 16 bytes of SHA-256("#name")
        while (*t == ' ')
            t++;
        if (!*t) {
            memset(prefs->default_scope_name, 0, sizeof(prefs->default_scope_name));
            memset(prefs->default_scope_key, 0, sizeof(prefs->default_scope_key));
        } else {
            char name[sizeof(prefs->default_scope_name)];
            snprintf(name, sizeof(name), "%s%s", t[0] == '#' ? "" : "#", t);
            if (strlen(name) < 2 || strlen(name) >= sizeof(prefs->default_scope_name))
                return false;
            SHA256 sha;
            sha.update(name, strlen(name));
            sha.finalize(prefs->default_scope_key, sizeof(prefs->default_scope_key));
            strcpy(prefs->default_scope_name, name);
        }
        the_mesh.savePrefs();
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ remote admin (repeaters, room servers)
// The same requests the MeshCore apps send (BaseChatMesh::sendLogin / sendRequest / sendCommandData). Their answers
// reach MyMesh::onContactResponse(), which hands the ones it isn't waiting for itself to onUnhandledResponse() below.
namespace
{
enum : uint8_t { R_NONE, R_LOGIN, R_STATUS, R_NEIGH, R_TELEM, R_ACL };
constexpr uint8_t REQ_NEIGHBOURS = 0x06, REQ_ACCESS_LIST = 0x05, REQ_TELEMETRY = 0x03, REQ_STATUS = 0x01;
constexpr uint32_t MARGIN_MS = 2500; // the node waits a little before it answers
template <typename T> T rd(const uint8_t *p, int at)
{
    T v;
    memcpy(&v, p + at, sizeof(T));
    return v;
}
} // namespace

uint32_t UITask::remoteSend(const uint8_t key[6], uint8_t kind, const uint8_t *req, uint8_t len)
{
    ContactInfo *c = the_mesh.lookupContactByPubKey(key, 6);
    if (!c)
        return 0;
    uint32_t tag = 0, timeout = 0;
    int r = len == 1 ? the_mesh.sendRequest(*c, req[0], tag, timeout) : the_mesh.sendRequest(*c, req, len, tag, timeout);
    if (r == MSG_SEND_FAILED)
        return 0;
    rreq.kind = kind;
    memcpy(rreq.key, key, 6);
    rreq.tag = tag;
    rreq.until = millis() + timeout + MARGIN_MS;
    return timeout + MARGIN_MS;
}

uint32_t UITask::remoteLogin(const uint8_t key[6], const char *password)
{
    ContactInfo *c = the_mesh.lookupContactByPubKey(key, 6);
    if (!c)
        return 0;
    uint32_t timeout = 0;
    if (the_mesh.sendLogin(*c, password ? password : "", timeout) == MSG_SEND_FAILED)
        return 0;
    rreq.kind = R_LOGIN;
    memcpy(rreq.key, key, 6);
    rreq.tag = 0; // a login's answer carries the node's clock, not our tag: matched by who sent it
    rreq.until = millis() + timeout + MARGIN_MS;
    return timeout + MARGIN_MS;
}

uint32_t UITask::remoteStatus(const uint8_t key[6])
{
    const uint8_t q[1] = {REQ_STATUS};
    return remoteSend(key, R_STATUS, q, 1);
}

uint32_t UITask::remoteTelemetry(const uint8_t key[6])
{
    const uint8_t q[1] = {REQ_TELEMETRY}; // the reserved bytes go as 0: everything we're allowed to read
    return remoteSend(key, R_TELEM, q, 1);
}

uint32_t UITask::remoteNeighbours(const uint8_t key[6])
{
    // version 0, as many as fit, from the first, strongest first, 6-byte keys, then 4 random bytes
    uint8_t q[11] = {REQ_NEIGHBOURS, 0, 20, 0, 0, 2, 6};
    the_mesh.getRNG()->random(&q[7], 4);
    return remoteSend(key, R_NEIGH, q, sizeof(q));
}

uint32_t UITask::remoteAccessList(const uint8_t key[6])
{
    uint8_t q[8] = {REQ_ACCESS_LIST, 0, 0, 0};
    the_mesh.getRNG()->random(&q[4], 4);
    return remoteSend(key, R_ACL, q, sizeof(q));
}

uint32_t UITask::remoteCommand(const uint8_t key[6], const char *cmd)
{
    ContactInfo *c = the_mesh.lookupContactByPubKey(key, 6);
    if (!c || !cmd || !*cmd)
        return 0;
    uint32_t timeout = 0;
    // the radio's own clock, always rising: the node drops anything older than the last thing it got from us
    uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
    if (the_mesh.sendCommandData(*c, ts, 0, TXT_TYPE_CLI_DATA, cmd, timeout) == MSG_SEND_FAILED)
        return 0;
    return timeout + MARGIN_MS;
}

bool UITask::onUnhandledResponse(const ContactInfo &from, uint32_t tag, const uint8_t *d, uint8_t len)
{
    if (!ready || rreq.kind == R_NONE || memcmp(from.id.pub_key, rreq.key, 6) != 0)
        return false;
    const uint8_t kind = rreq.kind;
    if (kind == R_LOGIN) {
        // new nodes: 0 (OK), keep-alive, is-admin, permissions, 4 random, firmware level; old repeaters: "OK"
        const bool legacy = len >= 2 && d[0] == 'O' && d[1] == 'K';
        const bool ok = legacy || (len >= 3 && d[0] == 0);
        uint8_t perms = 0;
        if (legacy)
            perms = 3; // old firmware didn't say; the commands will tell
        else if (ok)
            perms = (len >= 4 ? (d[3] & 3) : 0) | (d[2] == 1 ? 3 : 0);
        rreq.kind = R_NONE;
        app->onRemoteLogin(from.id.pub_key, ok, perms);
        return true;
    }
    if (tag != rreq.tag)
        return false;
    rreq.kind = R_NONE;
    if (kind == R_STATUS && len >= 48) { // RepeaterStats / ServerStats (simple_repeater, simple_room_server)
        hx::RemoteStatus s;
        s.battMv = rd<uint16_t>(d, 0), s.txQueue = rd<uint16_t>(d, 2);
        s.noiseFloor = rd<int16_t>(d, 4), s.lastRssi = rd<int16_t>(d, 6);
        s.rx = rd<uint32_t>(d, 8), s.tx = rd<uint32_t>(d, 12), s.airtimeS = rd<uint32_t>(d, 16), s.uptimeS = rd<uint32_t>(d, 20);
        s.sentFlood = rd<uint32_t>(d, 24), s.sentDirect = rd<uint32_t>(d, 28);
        s.recvFlood = rd<uint32_t>(d, 32), s.recvDirect = rd<uint32_t>(d, 36);
        s.errFlags = rd<uint16_t>(d, 40), s.lastSnr4 = (int8_t)rd<int16_t>(d, 42);
        s.dupDirect = rd<uint16_t>(d, 44), s.dupFlood = rd<uint16_t>(d, 46);
        if (len >= 56 && from.type == ADV_TYPE_REPEATER) {
            s.hasRx = true;
            s.rxAirtimeS = rd<uint32_t>(d, 48), s.rxErrors = rd<uint32_t>(d, 52);
        }
        app->onRemoteStatus(from.id.pub_key, s);
    } else if (kind == R_NEIGH && len >= 4) {
        hx::RemoteNeighbour n[12];
        int cnt = 0;
        for (int at = 4; at + 11 <= len && cnt < 12; at += 11, cnt++) {
            memcpy(n[cnt].key, d + at, 6);
            n[cnt].heardAgoS = rd<uint32_t>(d, at + 6);
            n[cnt].snr4 = (int8_t)d[at + 10];
        }
        app->onRemoteNeighbours(from.id.pub_key, rd<uint16_t>(d, 0), n, cnt);
    } else if (kind == R_TELEM) {
        app->onRemoteTelemetry(from.id.pub_key, d, len);
    } else if (kind == R_ACL) {
        hx::RemoteClient c[24];
        int cnt = 0;
        for (int at = 0; at + 7 <= len && cnt < 24; at += 7, cnt++) {
            memcpy(c[cnt].key, d + at, 6);
            c[cnt].perms = d[at + 6];
        }
        app->onRemoteAccessList(from.id.pub_key, c, cnt);
    }
    return true;
}

bool UITask::contactKeyHex(const uint8_t key[6], char out[65]) const
{
    ContactInfo *c = the_mesh.lookupContactByPubKey(key, 6);
    if (!c)
        return false;
    mesh::Utils::toHex(out, c->id.pub_key, PUB_KEY_SIZE);
    return true;
}

// passwords the user chose to keep: NVS keys "rp" + the node's 6-byte key prefix in hex
static void pwKey(const uint8_t key[6], char out[16])
{
    snprintf(out, 16, "rp%02x%02x%02x%02x%02x%02x", key[0], key[1], key[2], key[3], key[4], key[5]);
}
bool UITask::savedPassword(const uint8_t key[6], char *out, size_t n) const
{
    char k[16];
    pwKey(key, k);
    if (!nvsOpen || !n || !nvs.isKey(k))
        return false;
    return nvs.getString(k, out, n) > 0;
}
void UITask::savePassword(const uint8_t key[6], const char *password)
{
    openNvs();
    if (!nvsOpen)
        return;
    char k[16];
    pwKey(key, k);
    if (password && *password)
        nvs.putString(k, password);
    else if (nvs.isKey(k))
        nvs.remove(k);
}

// ------------------------------------------------------------------ Wi-Fi
// Station mode next to Bluetooth (ESP32-S3 shares the antenna between them; the phone app keeps working). The
// network is kept in NVS ("wifi_ssid", "wifi_pass", "wifi_on"). Once connected, the clock is set from the internet.
void UITask::wifiStart(void)
{
    char ssid[33] = "", pass[65] = "";
    if (!nvsOpen || nvs.getString("wifi_ssid", ssid, sizeof(ssid)) == 0)
        return;
    if (nvs.isKey("wifi_pass"))
        nvs.getString("wifi_pass", pass, sizeof(pass));
    static bool hooked = false;
    if (!hooked) { // why a join fails (shown in Settings, and on USB)
        hooked = true;
        WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
            wifiReason = info.wifi_sta_disconnected.reason;
            hxlog::line("wifi dropped, reason %u, %d dBm", (unsigned)wifiReason, (int)info.wifi_sta_disconnected.rssi);
        }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
        WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t) { wifiReason = 0; }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
    }
    wifiReason = 0;
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("hoptics-mc");
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid, pass[0] ? pass : nullptr);
    wifiOn = true;
    wifiDownSince = millis();
    hxlog::line("wifi joining the saved network (heap %u)", (unsigned)ESP.getFreeHeap()); // no network names in the log
}

void UITask::setWifi(bool on)
{
    openNvs();
    if (nvsOpen)
        nvs.putInt("wifi_on", on ? 1 : 0);
    if (on && !wifiOn)
        wifiStart();
    if (!on && WiFi.getMode() != WIFI_OFF) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        wifiOn = false;
        ntpStarted = false;
        scanState = -2;
    }
}

bool UITask::wifiJoin(const char *ssid, const char *password)
{
    if (!ssid || !*ssid || strlen(ssid) > 32 || (password && strlen(password) > 64))
        return false;
    openNvs();
    if (!nvsOpen)
        return false;
    nvs.putString("wifi_ssid", ssid);
    nvs.putString("wifi_pass", password ? password : "");
    nvs.putInt("wifi_on", 1);
    if (wifiOn)
        WiFi.disconnect(false);
    wifiOn = false;
    ntpStarted = false;
    wifiStart();
    return true;
}

void UITask::wifiForget()
{
    openNvs();
    if (nvsOpen) {
        if (nvs.isKey("wifi_ssid"))
            nvs.remove("wifi_ssid");
        if (nvs.isKey("wifi_pass"))
            nvs.remove("wifi_pass");
        nvs.putInt("wifi_on", 0);
    }
    setWifi(false);
}

hx::WifiStatus UITask::wifi() const
{
    hx::WifiStatus w;
    if (nvsOpen && nvs.isKey("wifi_ssid"))
        nvs.getString("wifi_ssid", w.ssid, sizeof(w.ssid));
    w.clockFromInternet = clockSynced;
    w.reason = wifiReason;
    if (!wifiOn) {
        w.state = hx::WifiStatus::OFF;
        return w;
    }
    switch (WiFi.status()) {
    case WL_CONNECTED:
        w.state = hx::WifiStatus::CONNECTED;
        snprintf(w.ip, sizeof(w.ip), "%s", WiFi.localIP().toString().c_str());
        w.rssi = (int8_t)WiFi.RSSI();
        break;
    case WL_NO_SSID_AVAIL:
        w.state = hx::WifiStatus::NO_NETWORK;
        break;
    case WL_CONNECT_FAILED:
        w.state = hx::WifiStatus::FAILED;
        break;
    default:
        w.state = hx::WifiStatus::CONNECTING;
        break;
    }
    return w;
}

void UITask::wifiScan()
{
    if (WiFi.getMode() == WIFI_OFF)
        WiFi.mode(WIFI_STA); // the radio has to be on to listen; it stays on until Wi-Fi is turned off
    if (WiFi.scanComplete() == WIFI_SCAN_RUNNING)
        return;
    WiFi.scanDelete();
    scanFound.clear();
    scanState = WiFi.scanNetworks(true /* async */, false) == WIFI_SCAN_FAILED ? -2 : -1;
}

int UITask::wifiScanResults(hx::WifiNet *out, int max)
{
    if (scanState == -1) { // collect when the scan is done
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING)
            return -1;
        scanFound.clear();
        for (int i = 0; i < n; i++) {
            String id = WiFi.SSID(i);
            if (!id.length() || id.length() > 32)
                continue; // hidden networks
            bool dup = false;
            for (auto &f : scanFound)
                if (!strcmp(f.ssid, id.c_str())) {
                    dup = true;
                    f.rssi = std::max<int8_t>(f.rssi, (int8_t)WiFi.RSSI(i));
                }
            if (dup)
                continue;
            hx::WifiNet w;
            snprintf(w.ssid, sizeof(w.ssid), "%s", id.c_str());
            w.rssi = (int8_t)WiFi.RSSI(i);
            w.open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
            scanFound.push_back(w);
        }
        WiFi.scanDelete();
        std::sort(scanFound.begin(), scanFound.end(), [](const hx::WifiNet &a, const hx::WifiNet &b) { return a.rssi > b.rssi; });
        scanState = (int)scanFound.size();
    }
    if (scanState < 0)
        return scanState;
    int n = std::min<int>(max, (int)scanFound.size());
    for (int i = 0; i < n; i++)
        out[i] = scanFound[i];
    return n;
}

// the clock: from the internet once Wi-Fi is up; otherwise the system time follows MeshCore's clock (GPS, phone app),
// so files written with a date (saved map tiles) get one
void UITask::wifiTick(void)
{
    const uint32_t ms = millis();
    const bool up = wifiOn && WiFi.status() == WL_CONNECTED;
    if (up != wifiWasUp) {
        wifiWasUp = up;
        if (up)
            hxlog::line("wifi connected, %d dBm, heap %u", (int)WiFi.RSSI(), (unsigned)ESP.getFreeHeap()); // no addresses in the log
        wifiDownSince = ms;
    }
    if (up && !ntpStarted) {
        ntpStarted = true;
        configTime(0, 0, "pool.ntp.org", "time.google.com");
    }
    if (ms - lastClockCheck < 5000)
        return;
    lastClockCheck = ms;
    webTick();
    // the Wi-Fi library gives up after some failures (a refused password, even a passing one, when the router is busy):
    // while it is on and not joined, start again each minute
    if (wifiOn && !up && ms - wifiDownSince > 60000) {
        wifiDownSince = ms;
        hxlog::line("wifi not joined for a minute: trying again");
        wifiStart();
    }
    const time_t sys = time(nullptr);
    if (ntpStarted && !clockSynced && sys > 1704067200) { // NTP answered
        the_mesh.getRTCClock()->setCurrentTime((uint32_t)sys);
        clockSynced = true;
        hxlog::line("clock set from the internet");
    } else if (sys < 1704067200) {
        const uint32_t mc = the_mesh.getRTCClock()->getCurrentTime();
        if (mc > 1704067200) {
            struct timeval tv = {(time_t)mc, 0};
            settimeofday(&tv, nullptr);
        }
    }
}

// ------------------------------------------------------------------ GPS speed and power
bool UITask::gpsCanSleep() const
{
#ifdef PIN_GPS_EN // boards that can cut the GPS's power (the T-Deck can't: it shares a power line)
    return sensors && sensors->getLocationProvider();
#else
    return false;
#endif
}

// "gps_sleep" minutes (0: always on): the GPS wakes, gets a fix (or gives up after 3 minutes), keeps it 30 s, then
// sleeps that long. The last position and the clock stay as they were. Only while the GPS setting is on.
void UITask::gpsSchedule(LocationProvider *lp, uint32_t ms)
{
    const int every = gpsCanSleep() ? uiPref("gps_sleep", 0) : 0;
    if (!prefs->gps_enabled || !every) {
        if (gpsAsleep) { // the schedule was turned off: back on for good
            gpsAsleep = false;
            sensors->setSettingValue("gps", "1");
            gpsFresh = true;
        }
        return;
    }
    if (gpsAsleep) {
        if (ms - gpsSince >= (uint32_t)every * 60000) {
            gpsAsleep = false;
            gpsSince = ms, gpsFixAt = 0;
            sensors->setSettingValue("gps", "1");
            gpsFresh = true;
            hxlog::line("gps waking");
        }
        return;
    }
    if (!gpsSince)
        gpsSince = ms; // first pass: count the time it has been on from now
    if (lp->isValid() && ms - gpsSince > 20000 && !gpsFixAt)
        gpsFixAt = ms;
    const bool done = gpsFixAt && ms - gpsFixAt > 30000;
    if (done || ms - gpsSince > 180000) {
        hxlog::line("gps sleeping %d min (%s)", every, done ? "fix kept" : "no fix this time");
        sensors->setSettingValue("gps", "0");
        gpsAsleep = true;
        gpsSince = ms;
    }
}

void UITask::gpsTick(void)
{
    const uint32_t ms = millis();
    if ((int32_t)(ms - gpsCheckAt) < 0)
        return;
    gpsCheckAt = ms + 3000;
    LocationProvider *lp = sensors ? sensors->getLocationProvider() : nullptr;
    if (!lp)
        return;
    gpsSchedule(lp, ms);
    if (gpsAsleep || !lp->isEnabled())
        return;
    char d[150];
    lp->formatDiagnostics(d, sizeof(d)); // "... ub:N ... ok:N ..." (MeshCore's GPS reader counts bytes and good sentences)
    const char *u = strstr(d, "ub:"), *o = strstr(d, " ok:");
    if (!u || !o)
        return;
    const uint32_t bytes = strtoul(u + 3, nullptr, 10), ok = strtoul(o + 4, nullptr, 10);
    const uint32_t dOk = ok - gpsOk, dBytes = bytes - gpsBytes;
    gpsOk = ok, gpsBytes = bytes;
    if (gpsFresh) { // the speed just changed: what came before was read at the old one
        gpsFresh = false;
        return;
    }
    if (dOk >= 3) { // it understands the GPS: keep this speed
        if (gpsBaud && nvsOpen && nvs.getInt("gps_baud", 0) != (int)gpsBaud) {
            nvs.putInt("gps_baud", (int)gpsBaud);
            hxlog::line("gps understood at %u baud, kept", (unsigned)gpsBaud);
        }
        return;
    }
    if (dOk > 0)
        return; // a sentence or two can come through by chance: undecided
    if (dBytes < 300)
        return; // quiet, or too little to judge (a GPS sends several sentences a second, fix or not)
    static const uint32_t speeds[] = {9600, 38400, 115200};
    uint32_t next = speeds[gpsBaudTry++ % 3];
    if (next == gpsBaud)
        next = speeds[gpsBaudTry++ % 3];
    hxlog::line("gps garbled, trying %u baud", (unsigned)next);
    gpsBaud = next;
    Serial1.updateBaudRate(next);
    gpsFresh = true;
}

// ------------------------------------------------------------------ live feed
// Packets the region's observers hear, from a CoreScope server's WebSocket (hx_feed.cpp runs the connection). Each one
// is decoded here as if the radio had heard it, but only for the screens: nothing is passed on to the mesh or the
// phone app. Channel messages are decrypted with this radio's own channel keys. Off unless turned on ("feed_on");
// the server is "feed_url".
namespace
{
const char *kFeedDefault = "scope.sacmesh.org"; // SacMesh's CoreScope
}

void UITask::feedStart(void)
{
    char url[96] = "";
    if (!nvsOpen || !nvs.isKey("feed_url") || nvs.getString("feed_url", url, sizeof(url)) == 0)
        strcpy(url, kFeedDefault);
    hxweb::setFeed(url);
    char ua[48];
    snprintf(ua, sizeof(ua), "HopticsMC/%s (%s)", HOPTICS_VERSION, hxhal::boardName());
    hxfeed::start(url, ua);
    feedOn = true;
}

void UITask::setFeed(bool on)
{
    openNvs();
    if (nvsOpen)
        nvs.putInt("feed_on", on ? 1 : 0);
    if (on)
        feedStart();
    else {
        hxfeed::stop();
        feedOn = false;
    }
}

bool UITask::setFeedServer(const char *url)
{
    bool tls;
    char host[64], path[64];
    uint16_t port;
    if (!url || strlen(url) >= 96 || !hxfeed::splitUrl(url, tls, host, sizeof(host), port, path, sizeof(path)))
        return false;
    openNvs();
    if (!nvsOpen)
        return false;
    nvs.putString("feed_url", url);
    hxweb::setFeed(url); // the page's own WebSocket may go there now (and nowhere else)
    if (feedOn)
        feedStart(); // reconnect to the new one
    return true;
}

hx::FeedStatus UITask::feed() const
{
    hx::FeedStatus f;
    if (!nvsOpen || !nvs.isKey("feed_url") || nvs.getString("feed_url", f.server, sizeof(f.server)) == 0)
        strcpy(f.server, kFeedDefault);
    static const hx::FeedStatus::State map[] = {hx::FeedStatus::OFF, hx::FeedStatus::NO_WIFI, hx::FeedStatus::CONNECTING,
                                                hx::FeedStatus::LIVE, hx::FeedStatus::FAILED};
    f.state = map[(int)hxfeed::state()];
    f.packets = hxfeed::packets();
    f.messages = feedMsgs;
    snprintf(f.error, sizeof(f.error), "%s", hxfeed::lastError());
    return f;
}

UITask::Seen *UITask::seenAt(const mesh::Packet &pkt, bool add)
{
    uint8_t h[MAX_HASH_SIZE];
    pkt.calculatePacketHash(h); // the same for every copy of a packet, whatever path it took
    for (auto &s : seen)
        if (s.flags && !memcmp(s.hash, h, sizeof(s.hash)))
            return &s;
    if (!add)
        return nullptr;
    Seen &s = seen[seenNext];
    seenNext = (seenNext + 1) % (int)(sizeof(seen) / sizeof(seen[0]));
    memset(&s, 0, sizeof(s));
    memcpy(s.hash, h, sizeof(s.hash));
    s.flags = SEEN_USED;
    return &s;
}

UITask::Seen *UITask::seenAt(const uint8_t *raw, int len, bool add)
{
    static mesh::Packet pkt; // big (path + payload): kept off the stack
    if (len < 2 || len > MAX_TRANS_UNIT || !pkt.readFrom(raw, (uint8_t)len))
        return nullptr;
    return seenAt(pkt, add);
}

bool UITask::feedChannel(const mesh::Packet &pkt)
{
    // as MeshCore does it (Mesh::onRecvPacket, BaseChatMesh::onGroupDataRecv): [channel hash][MAC + encrypted text]
    if (pkt.payload_len < 4)
        return false;
    static uint8_t data[MAX_PACKET_PAYLOAD + 1];
    ChannelDetails d;
    for (int c = 0; c < MAX_GROUP_CHANNELS; c++) {
        if (!the_mesh.getChannel(c, d) || !d.name[0] || d.channel.hash[0] != pkt.payload[0])
            continue;
        int len = mesh::Utils::MACThenDecrypt(d.channel.secret, data, &pkt.payload[1], pkt.payload_len - 1);
        if (len < 5 || (data[4] >> 2) != 0) // [timestamp 4][text type 1][text]: plain text only
            continue;
        data[len] = 0;
        const char *text = (const char *)&data[5];
        hx::Message m;
        m.conv.channel = (uint8_t)c;
        const char *sep = strstr(text, ": ");
        if (sep && sep - text < (int)sizeof(m.from)) {
            memcpy(m.from, text, sep - text);
            m.text = sep + 2;
        } else
            m.text = text;
        m.time = the_mesh.getRTCClock()->getCurrentTime();
        m.pathLen = pkt.isRouteFlood() ? pkt.getPathHashCount() : -1;
        m.viaNet = true;
        app->onMessage(m);
        return true;
    }
    return false;
}

void UITask::feedTick(void)
{
    if (!feedOn)
        return;
    static hxfeed::Item it; // ~300 bytes: off the stack
    static mesh::Packet pkt;
    for (int n = 0; n < 6 && hxfeed::pop(it); n++) { // a few per pass; the rest wait for the next
        if (!pkt.readFrom(it.raw, it.len))
            continue;
        Seen *sn = seenAt(pkt, true);
        if (!sn)
            continue;
        const bool first = !(sn->flags & SEEN_NET);
        sn->flags |= SEEN_NET;
        // count the observers that heard it (for what we sent)
        const uint16_t ob = (uint16_t)(it.obsKey[0] << 8 | it.obsKey[1]);
        bool known = !ob;
        for (int i = 0; i < sn->nObs && !known; i++)
            known = sn->obs[i] == ob;
        if (!known && sn->nObs < 8) {
            sn->obs[sn->nObs++] = ob;
            if (sn->sentId)
                app->onHeard(sn->sentId, sn->nObs);
        }
        if (!first || (sn->flags & SEEN_RF))
            continue; // the map shows each packet once, and not again when this radio heard it too
        hx::Packet p;
        p.time = now();
        p.len = it.len;
        p.snr4 = it.snr4;
        p.viaNet = true;
        p.mine = (sn->flags & SEEN_MINE) != 0;
        memcpy(p.obsKey, it.obsKey, sizeof(p.obsKey));
        snprintf(p.via, sizeof(p.via), "%s", it.obsName);
        decode(it.raw, it.len, p);
        app->onPacket(p);
        if (pkt.getPayloadType() == PAYLOAD_TYPE_GRP_TXT && !(sn->flags & (SEEN_MINE | SEEN_SHOWN)) && feedChannel(pkt)) {
            sn->flags |= SEEN_SHOWN;
            feedMsgs++;
        }
    }
}

// the screens' own files, next to MeshCore's on SPIFFS ("/hx_<name>"); written to a new file first and swapped in,
// so a reset part-way through leaves the old one. A flash write can take a second or two (SPIFFS cleans up as it
// goes), so they are written by a background task and the main loop doesn't stall
namespace
{
SemaphoreHandle_t saveLock = nullptr;
TaskHandle_t saveTask = nullptr;
std::vector<std::pair<std::string, std::string>> toSave; // name, contents: the newest for each name
} // namespace

bool UITask::saveData(const char *name, const std::string &data)
{
    if (!saveLock) {
        saveLock = xSemaphoreCreateMutex();
        xTaskCreatePinnedToCore(
            [](void *self) {
                for (;;) {
                    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                    ((UITask *)self)->writeSaves();
                }
            },
            "hoptics-save", 4096, this, 1, &saveTask, tskNO_AFFINITY);
    }
    xSemaphoreTake(saveLock, portMAX_DELAY);
    bool found = false;
    for (auto &f : toSave)
        if (f.first == name) {
            f.second = data;
            found = true;
        }
    if (!found)
        toSave.emplace_back(name, data);
    xSemaphoreGive(saveLock);
    xTaskNotifyGive(saveTask);
    return true;
}

// writes what is waiting (the background task, or the main loop before a restart)
void UITask::writeSaves(void)
{
    if (!saveLock)
        return;
    for (;;) {
        std::pair<std::string, std::string> f;
        xSemaphoreTake(saveLock, portMAX_DELAY);
        if (!toSave.empty()) {
            f = std::move(toSave.front());
            toSave.erase(toSave.begin());
        }
        xSemaphoreGive(saveLock);
        if (f.first.empty())
            return;
        writeData(f.first.c_str(), f.second);
    }
}

bool UITask::writeData(const char *name, const std::string &data)
{
    char path[32], tmp[36];
    snprintf(path, sizeof(path), "/hx_%s", name);
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    File f = SPIFFS.open(tmp, FILE_WRITE);
    if (!f)
        return false;
    const bool ok = f.write((const uint8_t *)data.data(), data.size()) == data.size();
    f.close();
    if (!ok) {
        SPIFFS.remove(tmp);
        return false;
    }
    if (SPIFFS.exists(path))
        SPIFFS.remove(path);
    return SPIFFS.rename(tmp, path);
}

bool UITask::loadData(const char *name, std::string &out) const
{
    char path[32];
    snprintf(path, sizeof(path), "/hx_%s", name);
    if (!SPIFFS.exists(path))
        return false;
    File f = SPIFFS.open(path, FILE_READ);
    if (!f)
        return false;
    out.resize(f.size());
    const bool ok = f.read((uint8_t *)&out[0], out.size()) == out.size();
    f.close();
    return ok;
}

// the screens' settings, read from NVS once and then from this copy: some are asked for on every pass of the main loop,
// and each NVS read is a flash read, which stops both processor cores' caches while it runs
namespace
{
struct CachedPref {
    char key[16]; // NVS keys are at most 15 characters
    bool stored;  // false: not in NVS, so the caller's default applies
    int value;
};
CachedPref prefCache[32];
int prefCount = 0;
CachedPref *cachedPref(const char *key)
{
    for (int i = 0; i < prefCount; i++)
        if (!strcmp(prefCache[i].key, key))
            return &prefCache[i];
    if (prefCount == (int)(sizeof(prefCache) / sizeof(prefCache[0])) || strlen(key) >= sizeof(prefCache[0].key))
        return nullptr; // full (or a key NVS couldn't hold): read NVS directly
    CachedPref &c = prefCache[prefCount++];
    strcpy(c.key, key);
    c.stored = nvs.isKey(key);
    c.value = c.stored ? nvs.getInt(key, 0) : 0;
    return &c;
}
} // namespace

int UITask::uiPref(const char *key, int def) const
{
    if (!nvsOpen)
        return def;
    const CachedPref *c = cachedPref(key);
    if (!c)
        return nvs.getInt(key, def);
    return c->stored ? c->value : def;
}
void UITask::setUiPref(const char *key, int value)
{
    openNvs();
    if (!nvsOpen)
        return;
    nvs.putInt(key, value);
    if (CachedPref *c = cachedPref(key)) {
        c->stored = true;
        c->value = value;
    }
}
// The screen off means nothing to draw: the processor drops to 80 MHz (plenty for the radio, Wi-Fi and Bluetooth) and
// comes back to its start-up speed before the screen lights again
void UITask::setBacklight(uint8_t level)
{
    static uint32_t fullMhz = 0;
    if (!fullMhz)
        fullMhz = getCpuFrequencyMhz();
    const uint32_t want = level ? fullMhz : std::min<uint32_t>(fullMhz, 80);
    if (getCpuFrequencyMhz() != want)
        setCpuFrequencyMhz(want);
    hxhal::setBacklight(level);
}

bool UITask::sendLog()
{
    hxlog::dump();
    return true;
}

bool UITask::saveReport()
{
    if (!hxsd::mounted())
        return false;
    char name[48];
    cardName(name, sizeof(name), "report-", ".txt");
    return hxlog::saveReport(name);
}

// "<folder>2026-11-27-140205<ext>" by the node's clock (UTC), or "<folder>up<seconds><ext>" while it isn't set
void UITask::cardName(char *out, size_t n, const char *folder, const char *ext) const
{
    const time_t t = (time_t)now();
    struct tm tm;
    if (t > 1704067200 && gmtime_r(&t, &tm))
        snprintf(out, n, "%s%04d-%02d-%02d-%02d%02d%02d%s", folder, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                 tm.tm_sec, ext);
    else
        snprintf(out, n, "%sup%lu%s", folder, (unsigned long)(millis() / 1000), ext);
}

// ------------------------------------------------------------------ backup and restore (the web page; hx_web.cpp)
// A backup is JSON: the node's settings (name, position, radio and how it adds contacts and shares telemetry), its
// channels with their keys (a channel can't be joined again without its key), and its contacts' public keys, names,
// types and favourites. Never the node's own private key or the Bluetooth PIN. One channel or contact per line.
namespace
{
void hexTo(std::string &o, const uint8_t *b, size_t n)
{
    static const char *H = "0123456789abcdef";
    o += '"';
    for (size_t i = 0; i < n; i++) {
        o += H[b[i] >> 4];
        o += H[b[i] & 15];
    }
    o += '"';
}
} // namespace

void UITask::webBackup(std::string &out, bool keys)
{
    std::string b;
    char t[400];
    snprintf(t, sizeof(t), "{\"hoptics_backup\":1,\"fw\":\"%s\",\"made\":%lu,\"board\":", HOPTICS_VERSION, (unsigned long)now());
    b += t;
    hxparse::jsonString(b, hxhal::boardName());
    b += ",\n\"settings\":{\"name\":";
    hxparse::jsonString(b, prefs->node_name);
    snprintf(t, sizeof(t),
             ",\"lat\":%.6f,\"lon\":%.6f,\"freq\":%.4f,\"bw\":%g,\"sf\":%u,\"cr\":%u,\"tx\":%d,\"adv_loc\":%u,\"man_add\":%u,"
             "\"auto_add\":%u,\"auto_max\":%u,\"multi_ack\":%u,\"tel_base\":%u,\"tel_loc\":%u,\"tel_env\":%u},",
             sensors ? sensors->node_lat : prefs->node_lat, sensors ? sensors->node_lon : prefs->node_lon, (double)prefs->freq,
             (double)prefs->bw, prefs->sf, prefs->cr, (int)prefs->tx_power_dbm, prefs->advert_loc_policy, prefs->manual_add_contacts,
             prefs->autoadd_config, prefs->autoadd_max_hops, prefs->multi_acks, prefs->telemetry_mode_base, prefs->telemetry_mode_loc,
             prefs->telemetry_mode_env);
    b += t;
    b += "\n\"channels\":[";
    int nCh = 0, nCt = 0, nNoKey = 0;
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
        ChannelDetails d;
        if (!the_mesh.getChannel(i, d) || !d.name[0])
            continue;
        static const uint8_t zero[16] = {};
        snprintf(t, sizeof(t), "%s\n{\"i\":%d,\"name\":", nCh++ ? "," : "", i);
        b += t;
        hxparse::jsonString(b, d.name);
        // a private channel's key only when asked for: the page is plain HTTP, so anyone on the same Wi-Fi could
        // read it on the way (Public's and #hashtag channels' keys anyone can work out, so they always come)
        char known[33];
        if (keys || channelWebKey(i, known)) {
            b += ",\"key\":";
            hexTo(b, d.channel.secret, memcmp(d.channel.secret + 16, zero, 16) ? 32 : 16); // a 128-bit key, or 256
        } else {
            b += ",\"key\":null";
            nNoKey++;
        }
        b += '}';
    }
    b += "],\n\"contacts\":[";
    ContactInfo c;
    for (int i = MAX_ANON_CONTACTS; the_mesh.getContactByIdx(i, c); i++) {
        if (c.type == ADV_TYPE_NONE)
            continue;
        b += nCt++ ? ",\n{\"key\":" : "\n{\"key\":";
        hexTo(b, c.id.pub_key, PUB_KEY_SIZE);
        b += ",\"name\":";
        hxparse::jsonString(b, c.name);
        snprintf(t, sizeof(t), ",\"type\":%u,\"flags\":%u,\"lat\":%ld,\"lon\":%ld,\"heard\":%lu,\"mod\":%lu}", c.type, c.flags,
                 (long)c.gps_lat, (long)c.gps_lon, (unsigned long)c.last_advert_timestamp, (unsigned long)c.lastmod);
        b += t;
    }
    b += "]}\n";
    // on the card too, when there is one
    char path[64] = "";
    if (hxsd::mounted()) {
        cardName(path, sizeof(path), "/hoptics/backup/", ".json");
        if (!hxsd::writeFile(path, b.data(), b.size()))
            path[0] = 0;
    }
    hxlog::line("backup made: %d channels (%d without their key), %d contacts%s", nCh, nNoKey, nCt, path[0] ? ", saved on the card" : "");
    snprintf(t, sizeof(t), "{\"ok\":true,\"noKey\":%d,\"file\":", nNoKey);
    out = t;
    if (path[0])
        hxparse::jsonString(out, path);
    else
        out += "null";
    out += ",\"backup\":";
    out += b;
    out += '}';
}

void UITask::webRestore(const char *data, size_t len, uint8_t parts, std::string &out)
{
    const char *p = data, *end = data + len;
    hxparse::Value settings, channels, contacts;
    bool ours = false;
    if (!hxparse::object(p, end, [&](const hxparse::Value &k, const hxparse::Value &v) {
            if (hxparse::is(k, "hoptics_backup"))
                ours = hxparse::number(v, 0) == 1;
            else if (hxparse::is(k, "settings") && v.kind == 'o')
                settings = v;
            else if (hxparse::is(k, "channels") && v.kind == 'a')
                channels = v;
            else if (hxparse::is(k, "contacts") && v.kind == 'a')
                contacts = v;
        }) ||
        !ours) {
        out = "{\"error\":\"That isn't a Hoptics backup\"}";
        return;
    }
    int ctAdded = 0, ctHad = 0, ctFull = 0, chAdded = 0, chHad = 0, chFull = 0, chNoKey = 0;
    bool setOk = false;
    if ((parts & hxweb::Query::CONTACTS) && contacts.p) { // contacts the node doesn't have yet are added; known ones stay as they are
        const char *q = contacts.p;
        hxparse::array(q, contacts.p + contacts.n, [&](const hxparse::Value &e) {
            if (e.kind != 'o')
                return;
            ContactInfo c{}; // zeroed (its key class has a constructor, so not by memset)
            bool key = false;
            const char *r = e.p;
            hxparse::object(r, e.p + e.n, [&](const hxparse::Value &k, const hxparse::Value &v) {
                if (hxparse::is(k, "key"))
                    key = hxparse::hex(v, c.id.pub_key, PUB_KEY_SIZE);
                else if (hxparse::is(k, "name"))
                    hxparse::text(v, c.name, sizeof(c.name));
                else if (hxparse::is(k, "type"))
                    c.type = (uint8_t)hxparse::integer(v, 0, 255, 0);
                else if (hxparse::is(k, "flags"))
                    c.flags = (uint8_t)hxparse::integer(v, 0, 255, 0);
                else if (hxparse::is(k, "lat")) // degrees x 1e6
                    c.gps_lat = (int32_t)hxparse::integer(v, -90000000L, 90000000L, 0);
                else if (hxparse::is(k, "lon"))
                    c.gps_lon = (int32_t)hxparse::integer(v, -180000000L, 180000000L, 0);
                else if (hxparse::is(k, "heard"))
                    c.last_advert_timestamp = (uint32_t)hxparse::integer(v, 0, 4294967295L, 0);
                else if (hxparse::is(k, "mod"))
                    c.lastmod = (uint32_t)hxparse::integer(v, 0, 4294967295L, 0);
            });
            if (!key || c.type < ADV_TYPE_CHAT || c.type > ADV_TYPE_SENSOR)
                return;
            if (the_mesh.lookupContactByPubKey(c.id.pub_key, PUB_KEY_SIZE)) {
                ctHad++;
                return;
            }
            c.out_path_len = OUT_PATH_UNKNOWN; // the route is found again (paths change)
            the_mesh.addContact(c) ? ctAdded++ : ctFull++;
        });
        if (ctAdded) {
            the_mesh.saveContacts();
            contactsVer++;
            app->onContactsChanged();
        }
    }
    if ((parts & hxweb::Query::CHANNELS) && channels.p) { // a channel the node has already (same key) stays as it is
        const char *q = channels.p;
        hxparse::array(q, channels.p + channels.n, [&](const hxparse::Value &e) {
            if (e.kind != 'o')
                return;
            ChannelDetails d;
            memset(&d, 0, sizeof(d));
            int want = -1;
            bool key = false;
            const char *r = e.p;
            hxparse::object(r, e.p + e.n, [&](const hxparse::Value &k, const hxparse::Value &v) {
                if (hxparse::is(k, "i"))
                    want = (int)hxparse::integer(v, 0, MAX_GROUP_CHANNELS - 1, -1);
                else if (hxparse::is(k, "name"))
                    hxparse::text(v, d.name, sizeof(d.name));
                else if (hxparse::is(k, "key"))
                    key = hxparse::hex(v, d.channel.secret, 16) || hxparse::hex(v, d.channel.secret, 32);
            });
            if (!d.name[0])
                return;
            if (!key) { // saved without its key: the node keeps whatever it has, nothing is wiped
                chNoKey++;
                return;
            }
            int slot = -1;
            for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
                ChannelDetails c;
                const bool used = the_mesh.getChannel(i, c) && c.name[0];
                if (used && !memcmp(c.channel.secret, d.channel.secret, sizeof(d.channel.secret))) {
                    chHad++;
                    return;
                }
                if (!used && (slot < 0 || i == want))
                    slot = i; // its old place if that is free, else the first free one
            }
            if (slot < 0 || !the_mesh.setChannel(slot, d))
                chFull++;
            else
                chAdded++;
        });
        if (chAdded)
            the_mesh.saveChannels();
    }
    if ((parts & hxweb::Query::SETTINGS) && settings.p) {
        char name[32] = "";
        hx::Radio rad;
        rad.freq = prefs->freq, rad.bw = prefs->bw, rad.sf = prefs->sf, rad.cr = prefs->cr, rad.txPower = prefs->tx_power_dbm;
        double lat = 999, lon = 999;
        const char *q = settings.p;
        hxparse::object(q, settings.p + settings.n, [&](const hxparse::Value &k, const hxparse::Value &v) {
            double n = hxparse::number(v, -1);
            if (!std::isfinite(n)) // setRadio checks the ranges; a NaN would pass every "out of range" test
                n = -1;
            const uint8_t u = (uint8_t)hxparse::integer(v, 0, 255, 0);
            if (hxparse::is(k, "name"))
                hxparse::text(v, name, sizeof(name));
            else if (hxparse::is(k, "lat"))
                lat = hxparse::number(v, 999);
            else if (hxparse::is(k, "lon"))
                lon = hxparse::number(v, 999);
            else if (hxparse::is(k, "freq"))
                rad.freq = (float)n;
            else if (hxparse::is(k, "bw"))
                rad.bw = (float)n;
            else if (hxparse::is(k, "sf"))
                rad.sf = u;
            else if (hxparse::is(k, "cr"))
                rad.cr = u;
            else if (hxparse::is(k, "tx"))
                rad.txPower = (int8_t)hxparse::integer(v, -128, 127, prefs->tx_power_dbm);
            else if (hxparse::is(k, "adv_loc"))
                prefs->advert_loc_policy = u ? ADVERT_LOC_SHARE : ADVERT_LOC_NONE;
            else if (hxparse::is(k, "man_add"))
                prefs->manual_add_contacts = u;
            else if (hxparse::is(k, "auto_add"))
                prefs->autoadd_config = u;
            else if (hxparse::is(k, "auto_max"))
                prefs->autoadd_max_hops = std::min<uint8_t>(u, 64);
            else if (hxparse::is(k, "multi_ack"))
                prefs->multi_acks = u;
            else if (hxparse::is(k, "tel_base"))
                prefs->telemetry_mode_base = std::min<uint8_t>(u, TELEM_MODE_ALLOW_ALL);
            else if (hxparse::is(k, "tel_loc"))
                prefs->telemetry_mode_loc = std::min<uint8_t>(u, TELEM_MODE_ALLOW_ALL);
            else if (hxparse::is(k, "tel_env"))
                prefs->telemetry_mode_env = std::min<uint8_t>(u, TELEM_MODE_ALLOW_ALL);
        });
        if (lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180 && sensors) {
            sensors->node_lat = lat;
            sensors->node_lon = lon;
        }
        if (name[0])
            setNodeName(name);
        setOk = setRadio(rad); // checked as on the screen; saves the rest of the settings too
        if (!setOk)
            the_mesh.savePrefs();
    }
    hxlog::line("restore: %d contacts and %d channels added%s", ctAdded, chAdded,
                (parts & hxweb::Query::SETTINGS) ? (setOk ? ", settings" : ", settings without the radio") : "");
    char b[280];
    snprintf(b, sizeof(b),
             "{\"ok\":true,\"contacts\":{\"added\":%d,\"had\":%d,\"full\":%d},\"channels\":{\"added\":%d,\"had\":%d,\"full\":%d,"
             "\"noKey\":%d},\"settings\":%s,\"radio\":%s}",
             ctAdded, ctHad, ctFull, chAdded, chHad, chFull, chNoKey, (parts & hxweb::Query::SETTINGS) ? "true" : "false",
             setOk ? "true" : "false");
    out = b;
}

// ------------------------------------------------------------------ the web page
void UITask::webTick(void)
{
    const bool up = wifiOn && WiFi.status() == WL_CONNECTED;
    if (webOn && up && !hxweb::running()) {
        hxweb::setFeed(feed().server); // the feed's server, for the page's policy (the default when none is set)
        hxweb::start(80);
        if (!mdnsUp && MDNS.begin(webHost)) { // hoptics-xxxx.local, for phones and computers that look names up that way
            MDNS.addService("http", "tcp", 80);
            mdnsUp = true;
        }
    } else if (!webOn && hxweb::running()) {
        hxweb::stop();
        if (mdnsUp) {
            MDNS.end();
            mdnsUp = false;
        }
    }
}

hx::WebStatus UITask::web() const
{
    hx::WebStatus w;
    w.on = webOn;
    w.running = hxweb::running() && wifiOn && WiFi.status() == WL_CONNECTED;
    if (w.running)
        snprintf(w.addr, sizeof(w.addr), "http://%s", WiFi.localIP().toString().c_str());
    snprintf(w.host, sizeof(w.host), "%s.local", webHost);
    memcpy(w.pin, webPin, sizeof(w.pin));
    w.sessions = hxweb::sessions();
    return w;
}

void UITask::setWeb(bool on)
{
    webOn = on;
    openNvs();
    if (nvsOpen)
        nvs.putInt("web_on", on ? 1 : 0);
    lastClockCheck = 0; // start or stop on the next pass
}

void UITask::updateAnswer(bool install)
{
    const char *err = nullptr;
    if (!install) {
        hxota::decline();
        hxlog::line("update: turned down on the screen");
    } else if (!hxota::accept(err) && app)
        app->toast(err);
}

void UITask::newWebPin(void)
{
    uint32_t r;
    do // every PIN equally likely: 2^32 isn't a multiple of a million, so the top few values are thrown back
        r = esp_random();
    while (r >= 4294000000u);
    snprintf(webPin, sizeof(webPin), "%06lu", (unsigned long)(r % 1000000));
    openNvs();
    if (nvsOpen)
        nvs.putString("web_pin", webPin);
    hxweb::setPin(webPin); // signs every browser out
}

int UITask::joinHashChannel(const char *name)
{
    char n[32];
    while (*name == ' ')
        name++;
    snprintf(n, sizeof(n), "%s%s", name[0] == '#' ? "" : "#", name);
    for (char *p = n + strlen(n); p > n && p[-1] == ' ';)
        *--p = 0;
    const size_t len = strlen(n);
    if (len < 2 || len >= sizeof(n) - 1)
        return -1;
    for (size_t i = 1; i < len; i++) // the apps' rule: letters, digits and dashes, lower case
        if (!(isalnum((unsigned char)n[i]) || n[i] == '-') || isupper((unsigned char)n[i]))
            return -1;
    ChannelDetails d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, n, sizeof(d.name) - 1);
    uint8_t h[32];
    SHA256 sha;
    sha.update(n, len);
    sha.finalize(h, sizeof(h));
    memcpy(d.channel.secret, h, 16);
    int freeSlot = -1;
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
        ChannelDetails c;
        if (!the_mesh.getChannel(i, c) || !c.name[0]) {
            if (freeSlot < 0)
                freeSlot = i;
        } else if (!memcmp(c.channel.secret, d.channel.secret, 16))
            return i; // already joined
    }
    if (freeSlot < 0 || !the_mesh.setChannel(freeSlot, d))
        return -1;
    the_mesh.saveChannels();
    hxlog::line("joined a #channel"); // no names in the log
    return freeSlot;
}

// Public's key is published, and a #hashtag channel's key is the start of SHA-256 of its name: anyone can work them out,
// so the page may have them. Any other channel's key stays on the radio
bool UITask::channelWebKey(size_t i, char out[33]) const
{
    ChannelDetails d;
    if (!the_mesh.getChannel((int)i, d) || !d.name[0])
        return false;
    for (int b = 16; b < 32; b++)
        if (d.channel.secret[b])
            return false; // a 256-bit key: never one of those
    static const uint8_t PUBLIC_KEY[16] = {0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a, 0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72};
    bool open = !memcmp(d.channel.secret, PUBLIC_KEY, 16);
    if (!open && d.name[0] == '#') {
        uint8_t h[32];
        SHA256 sha;
        sha.update(d.name, strlen(d.name));
        sha.finalize(h, sizeof(h));
        open = !memcmp(d.channel.secret, h, 16);
    }
    if (!open)
        return false;
    for (int b = 0; b < 16; b++)
        snprintf(out + b * 2, 3, "%02x", d.channel.secret[b]);
    return true;
}

void UITask::histTick(uint32_t ms)
{
    if (ms - histAt < 300000 || !ready)
        return;
    const bool first = !histAt;
    histAt = ms;
    const hx::Stats s = stats();
    if (first) { // the counters start here; the first sample comes 5 minutes later
        histRx = s.rx;
        histTx = s.tx;
        histLvglFrom = ms;
        histLvglUs = 0;
        return;
    }
    if (!hist) {
        hist = (Sample *)heap_caps_calloc(288, sizeof(Sample), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!hist)
            return;
    }
    Sample &x = hist[histHead];
    histHead = (histHead + 1) % 288;
    if (histN < 288)
        histN++;
    x.battMv = (uint16_t)getBattMilliVolts();
    x.rx = (uint16_t)std::min<uint32_t>(s.rx - histRx, 65535);
    x.tx = (uint16_t)std::min<uint32_t>(s.tx - histTx, 65535);
    histRx = s.rx;
    histTx = s.tx;
    x.heapK = (uint16_t)(ESP.getFreeHeap() / 1024);
    x.noise = s.noiseFloor;
    x.rssi = rssiCount ? (int16_t)(rssiSum / rssiCount) : 0;
    rssiSum = 0;
    rssiCount = 0;
    x.lvgl = (uint8_t)std::min<uint32_t>(histLvglUs / std::max<uint32_t>(1, (ms - histLvglFrom) * 10), 100);
    histLvglUs = 0;
    histLvglFrom = ms;
    const uint32_t t = now();
    uint16_t heard = 0;
    if (t) {
        ContactInfo c;
        for (int i = MAX_ANON_CONTACTS; the_mesh.getContactByIdx(i, c); i++)
            heard += c.lastmod && t - c.lastmod <= 3600;
    }
    x.nodes = heard;
    histEnd = t ? t : ms / 1000;
}

void UITask::webExtras(std::string &o, uint32_t histHave) const
{
    char b[300];
    bool gpsOn = false, fix = false;
    const int sats = gpsSatellites(gpsOn, fix);
    snprintf(b, sizeof(b), ",\"hw\":{\"cpu\":%u,\"heap\":%u,\"heapTotal\":%u,\"psram\":%u,\"psramTotal\":%u,\"rssi\":%d",
             (unsigned)getCpuFrequencyMhz(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getHeapSize(), (unsigned)ESP.getFreePsram(),
             (unsigned)ESP.getPsramSize(), wifiOn && WiFi.status() == WL_CONNECTED ? (int)WiFi.RSSI() : 0);
    o += b;
    snprintf(b, sizeof(b), ",\"built\":\"%s %s\",\"slot\":\"%s\",\"trial\":%s,\"update\":\"%s\"", __DATE__, __TIME__,
             hxota::slot(), hxota::trial() ? "true" : "false",
             hxota::waiting() ? "waiting" : hxota::declined() ? "declined" : hxota::failed() ? "failed" : hxota::restartWanted ? "restarting" : "");
    o += b;
    const hoptics::MapStatus ms = hoptics::TileMap::get().status();
    snprintf(b, sizeof(b), ",\"tiles\":%u,\"tilesKB\":%u,\"tilesCapKB\":%u,\"tilesCard\":%s", (unsigned)ms.savedTiles, (unsigned)ms.savedKB,
             (unsigned)ms.capKB, ms.onCard ? "true" : "false");
    o += b;
    if (hxsd::mounted()) { // the microSD card: its size and free space (for Files and the tile pack check)
        snprintf(b, sizeof(b), ",\"sd\":{\"totalKB\":%lu,\"freeKB\":%lu}", (unsigned long)(hxsd::totalBytes() >> 10),
                 (unsigned long)(hxsd::freeBytes() >> 10));
        o += b;
    }
    if (sats >= 0) {
        snprintf(b, sizeof(b), ",\"gps\":{\"on\":%s,\"fix\":%s,\"sats\":%d}", gpsOn ? "true" : "false", fix ? "true" : "false", sats);
        o += b;
    }
    snprintf(b, sizeof(b), "},\"load\":{\"loop\":%lu,\"longest\":%lu,\"ui\":%lu,\"lvgl\":%lu,\"feed\":%lu},\"feed\":{\"device\":%s,\"url\":\"",
             (unsigned long)loadNow.loop, (unsigned long)loadNow.longest, (unsigned long)loadNow.ui, (unsigned long)loadNow.lvgl,
             (unsigned long)loadNow.feed, feedOn ? "true" : "false");
    o += b;
    const hx::FeedStatus f = feed();
    for (const char *p = f.server; *p; p++) // an address typed on the radio: only its plain characters
        if (*p > ' ' && *p != '"' && *p != '\\' && *p < 0x7f)
            o += *p;
    o += "\"";
    if (f.state == hx::FeedStatus::FAILED) { // why the node's own feed is not connected
        o += ",\"error\":\"";
        for (const char *p = f.error; *p; p++)
            if (*p >= ' ' && *p != '"' && *p != '\\' && *p < 0x7f)
                o += *p;
        o += "\"";
    }
    o += "}";
    if (!hist || !histN || histEnd == histHave)
        return;
    snprintf(b, sizeof(b), ",\"hist\":{\"step\":300,\"end\":%lu", (unsigned long)histEnd);
    o += b;
    struct Col {
        const char *name;
        int (*get)(const Sample &);
    };
    static const Col cols[] = {{"batt", [](const Sample &x) { return (int)x.battMv; }}, {"rx", [](const Sample &x) { return (int)x.rx; }},
                               {"tx", [](const Sample &x) { return (int)x.tx; }},     {"heap", [](const Sample &x) { return (int)x.heapK; }},
                               {"noise", [](const Sample &x) { return (int)x.noise; }}, {"rssi", [](const Sample &x) { return (int)x.rssi; }},
                               {"lvgl", [](const Sample &x) { return (int)x.lvgl; }},  {"nodes", [](const Sample &x) { return (int)x.nodes; }}};
    for (const Col &c : cols) {
        snprintf(b, sizeof(b), ",\"%s\":[", c.name);
        o += b;
        for (uint16_t i = 0; i < histN; i++) { // oldest first
            snprintf(b, sizeof(b), i ? ",%d" : "%d", c.get(hist[(histHead + 288 - histN + i) % 288]));
            o += b;
        }
        o += ']';
    }
    o += '}';
}
