#pragma once
// Hoptics M7: what the dashboard shows. The mesh loop records packets, adverts, neighbours and a sample every
// 5 minutes; the web server (async_tcp task) writes them as JSON. One mutex guards it all; the buffers live in PSRAM.
#include <Arduino.h>
#include <Packet.h>

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

// JSON writers (call from the web server). since = last sequence number the page has.
void writePackets(Print& out, uint32_t since);
void writeHistory(Print& out, uint32_t from_t);
void writeNodes(Print& out);
void writeSelf(Print& out);
void writeRaw(Print& out, uint32_t seq);   // one packet's bytes, for the detail view

}  // namespace hx
