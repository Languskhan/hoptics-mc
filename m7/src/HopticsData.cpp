#include "HopticsData.h"
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace hx {
namespace {

struct Pkt {
  uint32_t seq, t;
  uint8_t tx, type, route, hops, hsz, len, src;  // src: first byte of the sender, 0 if unknown
  int8_t snr4;
  int16_t rssi;
  uint8_t path[8];                                // first byte of each of the last 8 hops
  uint32_t hash;
};

struct Node {
  uint8_t pub[4];
  uint8_t type, hops;
  int8_t snr4;
  bool loc;
  int32_t lat, lon;
  uint32_t seen;
  char name[24];
};

constexpr int kPkts = 512, kSamples = 8640, kNodes = 256, kNbrs = 64;  // 8640 x 5 min = 30 days

SemaphoreHandle_t mu;
Pkt* pkts;
Sample* samples;
Node* nodes;
Neighbour nbrs[kNbrs];
int nbr_count;
uint32_t pkt_seq;
int sample_head, sample_count;
uint8_t self_pub[32];
char self_name[33];
double self_lat, self_lon;

struct Lock {
  Lock() { xSemaphoreTake(mu, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mu); }
};

void *ps(size_t n) {
  void* p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
  return p ? p : calloc(1, n);
}

void jsonStr(Print& out, const char* s) {
  out.print('"');
  for (; *s; s++) {
    uint8_t c = (uint8_t)*s;
    if (c == '"' || c == '\\') { out.print('\\'); out.print((char)c); }
    else if (c < 0x20) out.printf("\\u%04x", c);
    else out.print((char)c);
  }
  out.print('"');
}

}  // namespace

void begin() {
  if (mu) return;
  mu = xSemaphoreCreateMutex();
  pkts = (Pkt*)ps(sizeof(Pkt) * kPkts);
  samples = (Sample*)ps(sizeof(Sample) * kSamples);
  nodes = (Node*)ps(sizeof(Node) * kNodes);
}

void setSelf(const uint8_t* pub, const char* name, double lat, double lon) {
  if (!mu) return;
  Lock l;
  memcpy(self_pub, pub, 32);
  strlcpy(self_name, name ? name : "", sizeof(self_name));
  self_lat = lat;
  self_lon = lon;
}

void onPacket(bool tx, const mesh::Packet* p, int len, float snr, float rssi) {
  if (!mu || !pkts) return;
  uint8_t h[MAX_HASH_SIZE];
  p->calculatePacketHash(h);
  Lock l;
  Pkt& k = pkts[pkt_seq % kPkts];
  k.seq = ++pkt_seq;
  k.t = millis() / 1000;
  k.tx = tx;
  k.type = p->getPayloadType();
  k.route = p->getRouteType();
  k.hsz = p->getPathHashSize();
  k.hops = p->getPathHashCount();
  k.len = len > 255 ? 255 : len;
  k.snr4 = tx ? 0 : (int8_t)(snr * 4);
  k.rssi = tx ? 0 : (int16_t)rssi;
  memcpy(&k.hash, h, 4);
  memset(k.path, 0, sizeof(k.path));
  int first = k.hops > 8 ? k.hops - 8 : 0;
  for (int i = first; i < k.hops; i++) k.path[i - first] = p->path[i * k.hsz];
  k.src = 0;
  if (k.type == PAYLOAD_TYPE_ADVERT && p->payload_len > 0) k.src = p->payload[0];
  else if ((k.type == PAYLOAD_TYPE_PATH || k.type == PAYLOAD_TYPE_REQ || k.type == PAYLOAD_TYPE_RESPONSE ||
            k.type == PAYLOAD_TYPE_TXT_MSG) && p->payload_len > 1) k.src = p->payload[1];
}

void onAdvert(const uint8_t* pub, uint8_t type, const char* name, bool has_loc, int32_t lat, int32_t lon,
              float snr, uint8_t hops, uint32_t now) {
  if (!mu || !nodes) return;
  Lock l;
  int slot = -1, oldest = 0;
  for (int i = 0; i < kNodes; i++) {
    if (nodes[i].seen && memcmp(nodes[i].pub, pub, 4) == 0) { slot = i; break; }
    if (!nodes[i].seen) { if (slot < 0) slot = i; }
    else if (nodes[i].seen < nodes[oldest].seen) oldest = i;
  }
  if (slot < 0) slot = oldest;
  Node& n = nodes[slot];
  memcpy(n.pub, pub, 4);
  n.type = type;
  n.hops = hops;
  n.snr4 = (int8_t)(snr * 4);
  if (has_loc && (lat || lon)) { n.loc = true; n.lat = lat; n.lon = lon; }
  n.seen = now ? now : 1;
  strlcpy(n.name, name ? name : "", sizeof(n.name));
}

void addSample(const Sample& s) {
  if (!mu || !samples) return;
  Lock l;
  samples[sample_head] = s;
  sample_head = (sample_head + 1) % kSamples;
  if (sample_count < kSamples) sample_count++;
}

void setNeighbours(const Neighbour* n, int count) {
  if (!mu) return;
  Lock l;
  nbr_count = count > kNbrs ? kNbrs : count;
  memcpy(nbrs, n, sizeof(Neighbour) * nbr_count);
}

void writePackets(Print& out, uint32_t since) {
  Lock l;
  out.printf("{\"seq\":%lu,\"now\":%lu,\"p\":[", (unsigned long)pkt_seq, (unsigned long)(millis() / 1000));
  uint32_t lo = pkt_seq > kPkts ? pkt_seq - kPkts : 0;
  if (since < lo) since = lo;
  bool first = true;
  for (uint32_t s = since + 1; s <= pkt_seq; s++) {
    const Pkt& k = pkts[(s - 1) % kPkts];
    out.printf("%s[%lu,%lu,%u,%u,%u,%u,%u,%u,%d,%d,\"%08lx\",\"", first ? "" : ",", (unsigned long)k.seq,
               (unsigned long)k.t, k.tx, k.type, k.route, k.hops, k.len, k.src, k.snr4, k.rssi,
               (unsigned long)k.hash);
    int n = k.hops > 8 ? 8 : k.hops;
    for (int i = 0; i < n; i++) out.printf("%02x", k.path[i]);
    out.print("\"]");
    first = false;
  }
  out.print("]}");
}

void writeHistory(Print& out, uint32_t from_t) {
  Lock l;
  out.print("{\"interval\":300,\"s\":[");
  bool first = true;
  for (int i = 0; i < sample_count; i++) {
    const Sample& s = samples[(sample_head - sample_count + i + kSamples) % kSamples];
    if (s.t < from_t) continue;
    out.printf("%s[%lu,%lu,%d,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu]", first ? "" : ",", (unsigned long)s.t,
               (unsigned long)s.up, s.noise, (unsigned long)s.tx_ms, (unsigned long)s.rx_ms,
               (unsigned long)s.recv, (unsigned long)s.sent, (unsigned long)s.recv_flood,
               (unsigned long)s.recv_direct, (unsigned long)s.sent_flood, (unsigned long)s.sent_direct,
               (unsigned long)s.rx_err);
    first = false;
  }
  out.print("]}");
}

void writeNodes(Print& out) {
  Lock l;
  out.print("{\"nodes\":[");
  bool first = true;
  for (int i = 0; i < kNodes; i++) {
    const Node& n = nodes[i];
    if (!n.seen) continue;
    out.printf("%s{\"id\":\"%02x%02x%02x%02x\",\"type\":%u,\"hops\":%u,\"snr\":%.2f,\"seen\":%lu,", first ? "" : ",",
               n.pub[0], n.pub[1], n.pub[2], n.pub[3], n.type, n.hops, n.snr4 / 4.0f, (unsigned long)n.seen);
    if (n.loc) out.printf("\"lat\":%.6f,\"lon\":%.6f,", n.lat / 1e6, n.lon / 1e6);
    out.print("\"name\":");
    jsonStr(out, n.name);
    out.print('}');
    first = false;
  }
  out.print("],\"nbrs\":[");
  for (int i = 0; i < nbr_count; i++) {
    const Neighbour& b = nbrs[i];
    out.printf("%s{\"id\":\"%02x%02x%02x%02x\",\"snr\":%.2f,\"heard\":%lu,\"advert\":%lu}", i ? "," : "", b.pub[0],
               b.pub[1], b.pub[2], b.pub[3], b.snr4 / 4.0f, (unsigned long)b.heard, (unsigned long)b.advert);
  }
  out.print("]}");
}

void writeSelf(Print& out) {
  Lock l;
  out.print("{\"name\":");
  jsonStr(out, self_name);
  out.print(",\"id\":\"");
  for (int i = 0; i < 32; i++) out.printf("%02x", self_pub[i]);
  out.printf("\",\"lat\":%.6f,\"lon\":%.6f,\"uptime\":%lu,\"psram\":%u,\"version\":\"%s\"}", self_lat, self_lon,
             (unsigned long)(millis() / 1000), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
#ifdef HOPTICS_M7_VERSION
             HOPTICS_M7_VERSION
#else
             "dev"
#endif
  );
}

}  // namespace hx
