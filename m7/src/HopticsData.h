#pragma once
// Hoptics M7: what the dashboard shows. The mesh loop records packets, adverts, neighbours and a sample every
// 5 minutes; the web server (async_tcp task) writes them as JSON. One mutex guards it all; the buffers live in PSRAM.
#include <Arduino.h>
#include <Packet.h>

namespace mesh { class Mesh; }

namespace hx {

struct Sample {         // one 5-minute sample; counters are totals since boot (the page takes differences)
  uint32_t t;           // RTC seconds
  uint32_t up;          // uptime seconds
  int16_t noise;        // dBm
  uint32_t tx_ms, rx_ms;
  uint32_t recv, sent, recv_flood, recv_direct, sent_flood, sent_direct, rx_err;
  uint32_t flood_uniq, flood_dup;   // filled in by addSample: flood packets heard once vs again (copies)
  uint16_t nbr_strong;              // neighbours with SNR >= 0 heard in the last 24 h
  int16_t nbr_snr4;                 // their average SNR x4
  uint32_t mqtt_ok, mqtt_err;       // MQTT publishes, all slots, since boot
  uint32_t heap;                    // free internal heap
  int16_t temp;                     // chip temperature, degrees C
};

struct Neighbour { uint8_t pub[4]; int8_t snr4; uint32_t heard; uint32_t advert; };

void begin();
void setSelf(const uint8_t* pub, const char* name, double lat, double lon);
void onPacket(bool tx, const mesh::Packet* p, int len, float snr, float rssi, float score, uint32_t air_ms);
void onAdvert(const uint8_t* pub, uint8_t type, const char* name, bool has_loc, int32_t lat, int32_t lon,
              float snr, uint8_t hops, uint32_t now);
void addSample(const Sample& s);
void setNeighbours(const Neighbour* n, int count);

// JSON writers (call from the web server). Replies are size-capped; the page pages through them.
void writePackets(Print& out, uint32_t since);
void writeHistory(Print& out, uint32_t from_i);
void writeNodes(Print& out);
void writeSelf(Print& out);
void writeRaw(Print& out, uint32_t seq);   // one packet's bytes, for the detail view

// Region nodes from SacMesh CoreScope (HopticsRegion.cpp fills them; a refresh builds a new list, then swaps it in)
void startRegion();
void regionBegin();
void addRegionNode(const uint8_t* pub, uint8_t type, const char* name, bool has_loc, int32_t lat, int32_t lon,
                   uint32_t seen, uint32_t relays_24h);
void regionDone(bool ok, int total, const char* err);
void writeRegion(Print& out, uint32_t from_i);

// Channel chat from the dashboard: the page queues one message, the mesh loop sends it (one at a time,
// at least 5 s apart). Channel packets heard or sent are served with their bytes so the page can decrypt them.
constexpr int kChatMax = 140;
bool queueChannelMsg(const uint8_t key[16], const char* text);
bool takeChannelMsg(uint8_t key[16], char* text, size_t n);
void writeGroup(Print& out, uint32_t since);

// Rollback and stats feed (HopticsSafety.cpp)
void markUpdateInstalled();
void bootCheck();
void bootGood();
void statsBegin();
bool statsOn();
bool statsTokenOk(const char* t);
const char* statsNewToken(bool off);
void writeMetrics(Print& out);

// Discord alerts (HopticsAlerts.cpp)
enum : uint8_t { ALERT_SILENT = 1, ALERT_DAILY = 2, ALERT_NEW = 4 };
void alertsBegin();
bool alertsWanted(uint8_t kind);
void alert(uint8_t kind, const char* text);
void alertName(char* dst, size_t n, const char* name);  // a node name made safe for Discord
bool alertsConfigure(const char* url, uint8_t flags, char* err, size_t err_len);
void alertsTest();
void writeAlerts(Print& out);

// Direct messages from the dashboard (HopticsDm.cpp): a second, chat identity beside the repeater. Off until the
// owner turns it on. The mesh loop sends; the web server only queues and reads.
constexpr int kDmMax = 150;
const uint8_t* selfPub();
void dmBegin();
void dmTick(mesh::Mesh& m);
void dmOnRx(const mesh::Packet* p);
void dmOnAdvert(const uint8_t* pub, uint8_t type, const char* name, uint32_t now);
bool dmQueue(const uint8_t pub[32], const char* text);
bool dmAdvert();
bool dmEnable(bool enable, const char* name);
bool dmSaveContact(const uint8_t pub[32], const char* name, bool keep);
void writeDm(Print& out, uint32_t since);

}  // namespace hx
