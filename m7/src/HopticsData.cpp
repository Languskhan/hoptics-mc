#include "HopticsData.h"
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <SPIFFS.h>
#include <time.h>

namespace hx {
namespace {

struct Pkt {
  uint32_t seq, t;
  uint8_t tx, type, route, hops, hsz, len, src;  // src: sender's first byte (channel hash for channel packets)
  int8_t snr4;
  int16_t rssi;
  uint8_t path[8];                                // first byte of each of the last 8 hops
  uint32_t hash;
  uint16_t score, air_ms, raw_len;
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

struct RNode {
  uint8_t pub[4];
  uint8_t type;
  bool loc;
  int32_t lat, lon;
  uint32_t seen, relays;
  char name[24];
};
constexpr int kRegion = 2600;
RNode* region[2];       // [live, being built]
int region_n[2];
uint32_t region_at;     // uptime seconds of the last good refresh
char region_err[48];

constexpr int kPkts = 1024, kRaw = 256, kSeen = 256, kSamples = 8640, kNodes = 256, kNbrs = 64;  // 8640 x 5 min = 30 days

SemaphoreHandle_t mu;
Pkt* pkts;
uint8_t* raws;                                    // kPkts x kRaw packet bytes
uint32_t seen[kSeen];                             // recent flood hashes, to count copies
int seen_head;
uint32_t flood_uniq, flood_dup;
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

// ---- history on flash: two files of up to 15 days each (h0 older, h1 newer). Each starts with a small header;
// records are whole Sample structs. Writes are skipped if the filesystem is getting full, so the node's keys
// and settings (same filesystem) always have room.
constexpr const char* kH0 = "/hx/h0.bin";
constexpr const char* kH1 = "/hx/h1.bin";
constexpr uint32_t kMagic = 0x31535848;  // "HXS1"
constexpr size_t kPerFile = kSamples / 2;
constexpr size_t kKeepFree = 96 * 1024;

struct FileHead { uint32_t magic; uint16_t rec_size; uint16_t reserved; };

void loadFile(const char* path) {
  File f = SPIFFS.open(path, "r");
  if (!f) return;
  FileHead h{};
  if (f.read((uint8_t*)&h, sizeof(h)) != sizeof(h) || h.magic != kMagic || h.rec_size != sizeof(Sample)) { f.close(); return; }
  Sample s;
  size_t n = 0;
  while (n < kPerFile && f.read((uint8_t*)&s, sizeof(s)) == sizeof(s)) {  // a half-written last record is ignored
    samples[sample_head] = s;
    sample_head = (sample_head + 1) % kSamples;
    if (sample_count < kSamples) sample_count++;
    n++;
  }
  f.close();
}

void saveSample(const Sample& s) {
  if (SPIFFS.totalBytes() - SPIFFS.usedBytes() < kKeepFree) return;
  size_t size = 0;
  {
    File f = SPIFFS.open(kH1, "r");
    if (f) { size = f.size(); f.close(); }
  }
  if (size >= sizeof(FileHead) + kPerFile * sizeof(Sample)) {  // h1 full: it becomes h0, start a new h1
    SPIFFS.remove(kH0);
    SPIFFS.rename(kH1, kH0);
    size = 0;
  }
  File f = SPIFFS.open(kH1, size ? "a" : "w");
  if (!f) return;
  if (!size) {
    FileHead h{kMagic, (uint16_t)sizeof(Sample), 0};
    f.write((const uint8_t*)&h, sizeof(h));
  }
  f.write((const uint8_t*)&s, sizeof(s));
  f.close();
}

}  // namespace

void begin() {
  if (mu) return;
  mu = xSemaphoreCreateMutex();
  pkts = (Pkt*)ps(sizeof(Pkt) * kPkts);
  raws = (uint8_t*)ps(kPkts * kRaw);
  samples = (Sample*)ps(sizeof(Sample) * kSamples);
  nodes = (Node*)ps(sizeof(Node) * kNodes);
  region[0] = (RNode*)ps(sizeof(RNode) * kRegion);
  region[1] = (RNode*)ps(sizeof(RNode) * kRegion);
  if (samples) {  // history from before the restart
    Lock l;
    loadFile(kH0);
    loadFile(kH1);
  }
  startRegion();
  alertsBegin();
}

void regionBegin() {
  if (!mu) return;
  Lock l;
  region_n[1] = 0;
}

void addRegionNode(const uint8_t* pub, uint8_t type, const char* name, bool has_loc, int32_t lat, int32_t lon,
                   uint32_t seen, uint32_t relays_24h) {
  if (!mu || !region[1]) return;
  Lock l;
  if (region_n[1] >= kRegion) return;
  RNode& n = region[1][region_n[1]++];
  memcpy(n.pub, pub, 4);
  n.type = type;
  n.loc = has_loc;
  n.lat = lat;
  n.lon = lon;
  n.seen = seen;
  n.relays = relays_24h;
  strlcpy(n.name, name ? name : "", sizeof(n.name));
}

void regionDone(bool ok, int total, const char* err) {
  if (!mu) return;
  Lock l;
  strlcpy(region_err, ok ? "" : (err ? err : "failed"), sizeof(region_err));
  if (!ok || !region[1]) return;  // keep the last good list
  RNode* t = region[0]; region[0] = region[1]; region[1] = t;
  region_n[0] = region_n[1];
  region_n[1] = 0;
  region_at = millis() / 1000;
  (void)total;
}

void writeRegion(Print& out, uint32_t from_i) {
  if (!mu) { out.print("{\"count\":0,\"age\":-1,\"err\":\"\",\"nodes\":[]}"); return; }
  Lock l;
  const int count = region[0] ? region_n[0] : 0;
  out.printf("{\"count\":%d,\"age\":%ld,\"err\":", count,
             region_at ? (long)(millis() / 1000 - region_at) : -1L);
  jsonStr(out, region_err);
  out.print(",\"nodes\":[");
  const int start = from_i < (uint32_t)count ? (int)from_i : count;
  const int end = start + 250 < count ? start + 250 : count;
  for (int i = start; i < end; i++) {
    const RNode& n = region[0][i];
    out.printf("%s[\"%02x%02x%02x%02x\",%u,", i > start ? "," : "", n.pub[0], n.pub[1], n.pub[2], n.pub[3], n.type);
    if (n.loc) out.printf("%.5f,%.5f,", n.lat / 1e6, n.lon / 1e6);
    else out.print("null,null,");
    out.printf("%lu,%lu,", (unsigned long)n.seen, (unsigned long)n.relays);
    jsonStr(out, n.name);
    out.print(']');
  }
  out.print("]}");
}

void setSelf(const uint8_t* pub, const char* name, double lat, double lon) {
  if (!mu) return;
  Lock l;
  memcpy(self_pub, pub, 32);
  strlcpy(self_name, name ? name : "", sizeof(self_name));
  self_lat = lat;
  self_lon = lon;
}

void onPacket(bool tx, const mesh::Packet* p, int len, float snr, float rssi, float score, uint32_t air_ms) {
  if (!mu || !pkts || !raws) return;
  uint8_t h[MAX_HASH_SIZE];
  p->calculatePacketHash(h);
  uint8_t raw[kRaw];
  uint8_t rl = p->writeTo(raw);
  Lock l;
  const uint32_t idx = pkt_seq % kPkts;
  Pkt& k = pkts[idx];
  memcpy(raws + idx * kRaw, raw, rl);
  k.raw_len = rl;
  k.score = tx ? 0 : (uint16_t)(score * 1000);
  k.air_ms = air_ms > 65535 ? 65535 : air_ms;
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
  if (!tx && p->isRouteFlood()) {               // a copy of a flood we already heard, or a new one
    bool again = false;
    for (int i = 0; i < kSeen; i++) if (seen[i] == k.hash) { again = true; break; }
    if (again) flood_dup++;
    else { flood_uniq++; seen[seen_head] = k.hash; seen_head = (seen_head + 1) % kSeen; }
  }
  memset(k.path, 0, sizeof(k.path));
  int first = k.hops > 8 ? k.hops - 8 : 0;
  for (int i = first; i < k.hops && i * k.hsz < MAX_PATH_SIZE; i++) k.path[i - first] = p->path[i * k.hsz];
  k.src = 0;
  if (k.type == PAYLOAD_TYPE_ADVERT && p->payload_len > 0) k.src = p->payload[0];
  else if ((k.type == PAYLOAD_TYPE_GRP_TXT || k.type == PAYLOAD_TYPE_GRP_DATA) && p->payload_len > 0) k.src = p->payload[0];
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
  const bool is_new = slot < 0 || !nodes[slot].seen;
  if (slot < 0) slot = oldest;
  Node& n = nodes[slot];
  memcpy(n.pub, pub, 4);
  n.type = type;
  n.hops = hops;
  n.snr4 = (int8_t)(snr * 4);
  if (has_loc && (lat || lon)) { n.loc = true; n.lat = lat; n.lon = lon; }
  n.seen = now ? now : 1;
  strlcpy(n.name, name ? name : "", sizeof(n.name));
  if (is_new && millis() > 30UL * 60UL * 1000UL && alertsWanted(ALERT_NEW)) {
    bool known = false;
    for (int i = 0; region[0] && i < region_n[0]; i++) if (!memcmp(region[0][i].pub, pub, 4)) { known = true; break; }
    if (!known) {
      static const char* const kinds[] = {"node", "companion", "repeater", "room server", "sensor"};
      char nm[28], msg[160];
      alertName(nm, sizeof(nm), name);
      snprintf(msg, sizeof(msg), "New %s heard: %s, %u hop%s away, SNR %.1f dB", type < 5 ? kinds[type] : "node", nm,
               hops, hops == 1 ? "" : "s", snr);
      alert(ALERT_NEW, msg);
    }
  }
}

void addSample(const Sample& s) {
  if (!mu || !samples) return;
  Sample saved;
  {
    Lock l;
    samples[sample_head] = s;
    samples[sample_head].flood_uniq = flood_uniq;
    samples[sample_head].flood_dup = flood_dup;
    saved = samples[sample_head];
    sample_head = (sample_head + 1) % kSamples;
    if (sample_count < kSamples) sample_count++;
  }
  saveSample(saved);  // flash write outside the lock, so the web server is never held up by it
  static uint32_t n_since = 0;
  if (++n_since >= 288 && alertsWanted(ALERT_DAILY)) {  // every 24 h of samples
    n_since = 0;
    Sample a{}, b{};
    int nodes_seen = 0;
    {
      Lock l;
      if (sample_count < 2) return;
      const int span = sample_count > 288 ? 288 : sample_count - 1;
      b = samples[(sample_head - 1 + kSamples) % kSamples];
      a = samples[(sample_head - 1 - span + kSamples) % kSamples];
      for (int i = 0; nodes && i < kNodes; i++) if (nodes[i].seen && b.t - nodes[i].seen < 86400) nodes_seen++;
    }
    if (b.up <= a.up) return;  // restarted during the day: skip rather than report nonsense
    const float secs = (float)(b.up - a.up);
    char msg[300];
    snprintf(msg, sizeof(msg),
             "Daily summary\nPackets heard %lu, sent %lu\nAirtime %.1f %%, noise floor %d dBm\nNodes heard %d, "
             "repeaters heard well %u\nMQTT uploads %lu ok, %lu failed",
             (unsigned long)(b.recv - a.recv), (unsigned long)(b.sent - a.sent),
             ((b.tx_ms - a.tx_ms) + (b.rx_ms - a.rx_ms)) / (secs * 10.0f), b.noise, nodes_seen, b.nbr_strong,
             (unsigned long)(b.mqtt_ok - a.mqtt_ok), (unsigned long)(b.mqtt_err - a.mqtt_err));
    alert(ALERT_DAILY, msg);
  }
}

void setNeighbours(const Neighbour* n, int count) {
  if (!mu) return;
  static uint8_t quiet[kNbrs][4];  // neighbours already reported as quiet
  static int quiet_n = 0;
  Lock l;
  nbr_count = count > kNbrs ? kNbrs : count;
  memcpy(nbrs, n, sizeof(Neighbour) * nbr_count);
  const uint32_t now = (uint32_t)time(nullptr);
  if (now < 1700000000 || !alertsWanted(ALERT_SILENT)) return;
  for (int i = 0; i < nbr_count; i++) {
    const Neighbour& b = nbrs[i];
    int q = -1;
    for (int k = 0; k < quiet_n; k++) if (!memcmp(quiet[k], b.pub, 4)) { q = k; break; }
    const bool silent = b.heard && now > b.heard && now - b.heard > 6UL * 3600UL;
    if (silent == (q >= 0)) continue;
    char nm[28] = "unnamed", msg[160];
    for (int k = 0; nodes && k < kNodes; k++) if (nodes[k].seen && !memcmp(nodes[k].pub, b.pub, 4)) { alertName(nm, sizeof(nm), nodes[k].name); break; }
    if (silent) {
      if (quiet_n < kNbrs) memcpy(quiet[quiet_n++], b.pub, 4);
      snprintf(msg, sizeof(msg), "Repeater %s %02X has gone quiet: not heard for %lu h", nm, b.pub[0],
               (unsigned long)((now - b.heard) / 3600));
    } else {
      memmove(quiet[q], quiet[q + 1], (quiet_n - q - 1) * 4);
      quiet_n--;
      snprintf(msg, sizeof(msg), "Repeater %s %02X is back, SNR %.1f dB", nm, b.pub[0], b.snr4 / 4.0f);
    }
    alert(ALERT_SILENT, msg);
  }
}

void writePackets(Print& out, uint32_t since) {
  if (!mu) { out.print("{\"seq\":0,\"now\":0,\"p\":[]}"); return; }
  Lock l;
  out.printf("{\"seq\":%lu,\"now\":%lu,\"p\":[", (unsigned long)pkt_seq, (unsigned long)(millis() / 1000));
  // (seq is the newest; the page keeps asking with since = its last packet until it reaches seq)
  uint32_t lo = pkt_seq > kPkts ? pkt_seq - kPkts : 0;
  if (since < lo || since > pkt_seq) since = lo;
  // at most 256 per reply so a response never needs much RAM; the page asks again until it has caught up
  const uint32_t last = pkt_seq - since > 256 ? since + 256 : pkt_seq;
  bool first = true;
  for (uint32_t s = since + 1; s <= last; s++) {
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

void writeHistory(Print& out, uint32_t from_i) {
  // samples are numbered from the oldest kept (0); at most 300 per reply, the page asks for the rest
  if (!mu || !samples) { out.print("{\"interval\":300,\"count\":0,\"s\":[]}"); return; }
  Lock l;
  out.printf("{\"interval\":300,\"count\":%d,\"s\":[", sample_count);
  bool first = true;
  const int start = from_i < (uint32_t)sample_count ? (int)from_i : sample_count;
  const int end = start + 300 < sample_count ? start + 300 : sample_count;
  for (int i = start; i < end; i++) {
    const Sample& s = samples[(sample_head - sample_count + i + kSamples) % kSamples];
    out.printf("%s[%lu,%lu,%d,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%u,%d,%lu,%lu,%lu,%d]", first ? "" : ",",
               (unsigned long)s.t, (unsigned long)s.up, s.noise, (unsigned long)s.tx_ms, (unsigned long)s.rx_ms,
               (unsigned long)s.recv, (unsigned long)s.sent, (unsigned long)s.recv_flood,
               (unsigned long)s.recv_direct, (unsigned long)s.sent_flood, (unsigned long)s.sent_direct,
               (unsigned long)s.rx_err, (unsigned long)s.flood_uniq, (unsigned long)s.flood_dup, s.nbr_strong,
               s.nbr_snr4, (unsigned long)s.mqtt_ok, (unsigned long)s.mqtt_err, (unsigned long)s.heap, s.temp);
    first = false;
  }
  out.print("]}");
}

void writeNodes(Print& out) {
  if (!mu) { out.print("{\"nodes\":[],\"nbrs\":[]}"); return; }
  Lock l;
  out.print("{\"nodes\":[");
  bool first = true;
  for (int i = 0; nodes && i < kNodes; i++) {
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

void writeRaw(Print& out, uint32_t seq) {
  if (!mu || !pkts || !raws) { out.print("{\"error\":\"gone\"}"); return; }
  Lock l;
  uint32_t lo = pkt_seq > kPkts ? pkt_seq - kPkts : 0;
  if (seq == 0 || seq <= lo || seq > pkt_seq) { out.print("{\"error\":\"gone\"}"); return; }
  const uint32_t idx = (seq - 1) % kPkts;
  const Pkt& k = pkts[idx];
  out.printf("{\"seq\":%lu,\"score\":%u,\"air\":%u,\"raw\":\"", (unsigned long)seq, k.score, k.air_ms);
  const uint8_t* r = raws + idx * kRaw;
  for (int i = 0; i < k.raw_len; i++) out.printf("%02x", r[i]);
  out.print("\"}");
}

void writeSelf(Print& out) {
  if (!mu) { out.print("{}"); return; }
  Lock l;
  out.print("{\"name\":");
  jsonStr(out, self_name);
  out.print(",\"id\":\"");
  for (int i = 0; i < 32; i++) out.printf("%02x", self_pub[i]);
  out.printf("\",\"role\":\"%s\",\"lat\":%.6f,\"lon\":%.6f,\"uptime\":%lu,\"psram\":%u,\"version\":\"%s\"}",
#ifdef HOPTICS_M7_ROLE
             HOPTICS_M7_ROLE,
#else
             "repeater",
#endif
             self_lat, self_lon,
             (unsigned long)(millis() / 1000), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
#ifdef HOPTICS_M7_VERSION
             HOPTICS_M7_VERSION
#else
             "dev"
#endif
  );
}

}  // namespace hx
