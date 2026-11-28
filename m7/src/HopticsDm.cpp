// Hoptics M7: direct messages from the dashboard. A second identity (a "chat" node, like a phone companion) lives
// beside the repeater. It only handles packets addressed to it, answers with the same ACKs and path replies as the
// companion firmware, and never changes how the repeater forwards anything. Off until the owner turns it on.
#include "HopticsData.h"
#include <Mesh.h>
#include <Utils.h>
#include <helpers/AdvertDataHelpers.h>
#include <Preferences.h>
#include <esp_heap_caps.h>

namespace hx {
namespace {

constexpr int kContacts = 64, kSaved = 16, kMsgs = 128, kSeenDm = 32;
constexpr uint32_t kAckDelay = 200;  // same as the companion firmware

struct Contact {
  uint8_t pub[32];
  char name[32];
  uint8_t secret[32];
  bool has_secret, saved;
  uint8_t path[64];
  int16_t path_len;   // -1: unknown, send by flood
  uint32_t heard;     // RTC seconds, 0 = never
};

struct Msg {
  uint32_t seq, t;
  uint8_t pub[32];
  uint8_t out;        // 1 = sent from the page
  uint8_t status;     // received 0; sent: 1 waiting, 2 delivered, 3 no reply
  uint8_t hops;       // received: hops it took (0xFF direct)
  char text[kDmMax + 1];
};

struct Pending {      // the one message being sent
  bool busy;
  int contact;
  uint32_t msg_seq, ts, acks[3], due;
  uint8_t attempt;
  char text[kDmMax + 1];
};

SemaphoreHandle_t mu;
mesh::Mesh* the_mesh;
mesh::LocalIdentity me;
bool on, want_advert;
char my_name[32];
Contact* contacts;
Msg* msgs;
uint32_t msg_seq;
Pending pend;
uint8_t q_pub[32];
char q_text[kDmMax + 1];
bool q_full;
uint32_t last_send_ms, last_advert_ms;
uint8_t seen[kSeenDm][MAX_HASH_SIZE];
int seen_head;

struct Trace {        // one trace route at a time
  uint8_t path[kTraceMax], n;
  uint32_t tag, sent_ms;
  bool queued, waiting, done;
  int8_t snr4[kTraceMax];   // what each hop heard (x4)
  int8_t last4;             // what this node heard on the way back
} tr;

struct Lock {
  Lock() { xSemaphoreTake(mu, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mu); }
};

void cleanText(char* dst, size_t n, const char* src, size_t len) {   // printable text only, always terminated
  size_t j = 0;
  for (size_t i = 0; i < len && src[i] && j + 1 < n; i++) dst[j++] = (uint8_t)src[i] < 0x20 ? ' ' : src[i];
  dst[j] = 0;
}

void jstr(Print& out, const char* s) {
  out.print('"');
  for (; *s; s++) {
    const uint8_t c = (uint8_t)*s;
    if (c == '"' || c == '\\') { out.print('\\'); out.print((char)c); }
    else if (c < 0x20) out.printf("\\u%04x", c);
    else out.print((char)c);
  }
  out.print('"');
}

void hexOut(Print& out, const uint8_t* b, int n) {
  for (int i = 0; i < n; i++) out.printf("%02x", b[i]);
}

int findContact(const uint8_t* pub) {
  for (int i = 0; i < kContacts; i++)
    if ((contacts[i].heard || contacts[i].saved) && memcmp(contacts[i].pub, pub, 32) == 0) return i;
  return -1;
}

// a slot for a new contact: a free one, else the longest-unheard contact that wasn't saved by the owner
int newSlot() {
  int best = -1;
  for (int i = 0; i < kContacts; i++) {
    const Contact& c = contacts[i];
    if (!c.heard && !c.saved) return i;
    if (!c.saved && (best < 0 || c.heard < contacts[best].heard)) best = i;
  }
  return best;
}

const uint8_t* secretFor(int i) {
  Contact& c = contacts[i];
  if (!c.has_secret) { me.calcSharedSecret(c.secret, c.pub); c.has_secret = true; }
  return c.secret;
}

Msg& addMsg(const uint8_t* pub, bool out, uint8_t status, uint8_t hops, const char* text) {
  Msg& m = msgs[msg_seq % kMsgs];
  m.seq = ++msg_seq;
  m.t = the_mesh ? the_mesh->getRTCClock()->getCurrentTime() : 0;
  memcpy(m.pub, pub, 32);
  m.out = out;
  m.status = status;
  m.hops = hops;
  cleanText(m.text, sizeof(m.text), text, kDmMax);
  return m;
}

Msg* findMsg(uint32_t seq) {
  Msg& m = msgs[(seq - 1) % kMsgs];
  return m.seq == seq ? &m : nullptr;
}

bool seenBefore(const mesh::Packet* p) {
  uint8_t h[MAX_HASH_SIZE];
  p->calculatePacketHash(h);
  for (int i = 0; i < kSeenDm; i++) if (memcmp(seen[i], h, MAX_HASH_SIZE) == 0) return true;
  memcpy(seen[seen_head], h, MAX_HASH_SIZE);
  seen_head = (seen_head + 1) % kSeenDm;
  return false;
}

// the same packets Mesh::createDatagram / createPathReturn build, but from the chat identity
mesh::Packet* datagram(uint8_t type, int ci, const uint8_t* data, size_t len) {
  if (len + CIPHER_MAC_SIZE + CIPHER_BLOCK_SIZE - 1 > MAX_PACKET_PAYLOAD) return nullptr;
  mesh::Packet* p = the_mesh->obtainNewPacket();
  if (!p) return nullptr;
  p->header = (type << PH_TYPE_SHIFT);
  int n = 0;
  p->payload[n++] = contacts[ci].pub[0];
  p->payload[n++] = me.pub_key[0];
  n += mesh::Utils::encryptThenMAC(secretFor(ci), &p->payload[n], data, len);
  p->payload_len = n;
  return p;
}

mesh::Packet* pathReturn(int ci, const uint8_t* path, uint8_t path_len, uint8_t extra_type, const uint8_t* extra,
                         size_t extra_len) {
  const int hs = (path_len >> 6) + 1, hc = path_len & 63;
  if (hc * hs + extra_len + 5 > MAX_PACKET_PAYLOAD - 2 - CIPHER_BLOCK_SIZE) return nullptr;
  uint8_t d[MAX_PACKET_PAYLOAD];
  int n = 0;
  d[n++] = path_len;
  memcpy(&d[n], path, hc * hs); n += hc * hs;
  if (extra_len) { d[n++] = extra_type; memcpy(&d[n], extra, extra_len); n += extra_len; }
  else { d[n++] = 0xFF; the_mesh->getRNG()->random(&d[n], 4); n += 4; }
  return datagram(PAYLOAD_TYPE_PATH, ci, d, n);
}

void ackMatched(uint32_t crc) {
  if (!pend.busy) return;
  for (int a = 0; a <= pend.attempt && a < 3; a++) {
    if (pend.acks[a] != crc) continue;
    if (Msg* m = findMsg(pend.msg_seq)) m->status = 2;
    pend.busy = false;
    return;
  }
}

bool transmit() {   // the pending message, attempt pend.attempt; the last attempt always floods
  Contact& c = contacts[pend.contact];
  uint8_t d[5 + kDmMax + 1];
  memcpy(d, &pend.ts, 4);
  d[4] = pend.attempt & 3;
  const size_t tl = strlen(pend.text);
  memcpy(&d[5], pend.text, tl);
  mesh::Utils::sha256((uint8_t*)&pend.acks[pend.attempt], 4, d, 5 + tl, me.pub_key, 32);
  mesh::Packet* p = datagram(PAYLOAD_TYPE_TXT_MSG, pend.contact, d, 5 + tl);
  if (!p) return false;
  const bool direct = c.path_len >= 0 && pend.attempt < 2;
  if (direct) {
    the_mesh->sendDirect(p, c.path, (uint8_t)c.path_len);
    pend.due = millis() + 6000 + 2500UL * (c.path_len & 63);
  } else {
    the_mesh->sendFlood(p);
    pend.due = millis() + 20000;
  }
  return true;
}

void loadSaved() {
  Preferences p;
  if (!p.begin("hxdm", true)) return;
  on = p.getBool("on", false);
  uint8_t key[PRV_KEY_SIZE + PUB_KEY_SIZE];
  if (p.getBytes("id", key, sizeof(key)) == sizeof(key)) me.readFrom(key, sizeof(key));
  else on = false;
  memset(key, 0, sizeof(key));
  p.getString("name", my_name, sizeof(my_name));
  struct Saved { uint8_t pub[32]; char name[24]; } s[kSaved];
  const size_t got = p.getBytes("saved", s, sizeof(s));
  p.end();
  for (size_t i = 0; i < got / sizeof(Saved) && i < (size_t)kSaved; i++) {
    Contact& c = contacts[i];
    memcpy(c.pub, s[i].pub, 32);
    cleanText(c.name, sizeof(c.name), s[i].name, sizeof(s[i].name));
    c.saved = true;
    c.path_len = -1;
  }
}

void storeSaved() {   // only on an owner change, so the flash isn't worn by traffic
  struct Saved { uint8_t pub[32]; char name[24]; } s[kSaved];
  int n = 0;
  for (int i = 0; i < kContacts && n < kSaved; i++) {
    if (!contacts[i].saved) continue;
    memcpy(s[n].pub, contacts[i].pub, 32);
    strlcpy(s[n].name, contacts[i].name, sizeof(s[n].name));
    n++;
  }
  Preferences p;
  if (!p.begin("hxdm", false)) return;
  p.putBytes("saved", s, n * sizeof(Saved));
  p.end();
}

}  // namespace

void dmBegin() {
  if (mu) return;
  mu = xSemaphoreCreateMutex();
  contacts = (Contact*)heap_caps_calloc(kContacts, sizeof(Contact), MALLOC_CAP_SPIRAM);
  msgs = (Msg*)heap_caps_calloc(kMsgs, sizeof(Msg), MALLOC_CAP_SPIRAM);
  if (!contacts || !msgs) return;
  for (int i = 0; i < kContacts; i++) contacts[i].path_len = -1;
  loadSaved();
}

// mesh loop: send what the page queued, retry, and advertise when asked
void dmTick(mesh::Mesh& m) {
  if (!mu || !contacts || !msgs) return;
  the_mesh = &m;
  Lock l;
  if (tr.queued) {
    tr.queued = false;
    if (mesh::Packet* p = m.createTrace(tr.tag, 0, 0)) {
      m.sendDirect(p, tr.path, tr.n);
      tr.waiting = true;
      tr.sent_ms = millis();
    }
  }
  if (tr.waiting && millis() - tr.sent_ms > 30000UL) tr.waiting = false;   // no answer
  if (!on) return;
  if (want_advert) {
    want_advert = false;
    uint8_t app[MAX_ADVERT_DATA_SIZE];
    AdvertDataBuilder b(ADV_TYPE_CHAT, my_name);
    const uint8_t n = b.encodeTo(app);
    if (mesh::Packet* p = m.createAdvert(me, app, n)) m.sendFlood(p);
  }
  if (pend.busy && (int32_t)(millis() - pend.due) >= 0) {
    if (pend.attempt >= 2) {
      if (Msg* x = findMsg(pend.msg_seq)) x->status = 3;
      pend.busy = false;
    } else {
      pend.attempt++;
      if (pend.attempt == 2) contacts[pend.contact].path_len = -1;   // the path may be stale: find a new one
      if (!transmit()) { if (Msg* x = findMsg(pend.msg_seq)) x->status = 3; pend.busy = false; }
    }
  }
  if (q_full && !pend.busy) {
    q_full = false;
    const int ci = findContact(q_pub);
    if (ci < 0) return;
    pend = {};
    pend.busy = true;
    pend.contact = ci;
    pend.ts = m.getRTCClock()->getCurrentTimeUnique();
    strlcpy(pend.text, q_text, sizeof(pend.text));
    pend.msg_seq = addMsg(q_pub, true, 1, 0, q_text).seq;
    if (!transmit()) { if (Msg* x = findMsg(pend.msg_seq)) x->status = 3; pend.busy = false; }
  }
}

// every packet heard (before the repeater handles it). Only packets whose last hop is this node and whose
// destination byte is the chat identity get looked at; the rest cost one compare.
void dmOnRx(const mesh::Packet* pk) {
  if (!mu) return;
  if (pk->getPayloadType() == PAYLOAD_TYPE_TRACE && pk->isRouteDirect() && pk->payload_len >= 9) {
    uint32_t tag;
    memcpy(&tag, pk->payload, 4);
    Lock l;
    const int hashes = pk->payload_len - 9;
    if (!tr.waiting || tag != tr.tag || (pk->payload[8] & 3) != 0 || hashes != tr.n || pk->path_len < hashes) return;
    for (int i = 0; i < hashes; i++) tr.snr4[i] = (int8_t)pk->path[i];
    tr.last4 = (int8_t)(pk->getSNR() * 4);
    tr.waiting = false;
    tr.done = true;
    return;
  }
  if (!on || !the_mesh || !contacts) return;
  const uint8_t type = pk->getPayloadType();
  if (pk->isRouteDirect() && pk->getPathHashCount() > 0) return;   // still on its way to someone else
  if (type == PAYLOAD_TYPE_ACK) {
    if (pk->payload_len < 4) return;
    uint32_t crc;
    memcpy(&crc, pk->payload, 4);
    Lock l;
    ackMatched(crc);
    return;
  }
  if (type != PAYLOAD_TYPE_TXT_MSG && type != PAYLOAD_TYPE_PATH) return;
  if (pk->payload_len < 2 + CIPHER_MAC_SIZE + 1 || pk->payload[0] != me.pub_key[0]) return;
  Lock l;
  if (seenBefore(pk)) return;
  const uint8_t src = pk->payload[1];
  for (int i = 0; i < kContacts; i++) {
    Contact& c = contacts[i];
    if ((!c.heard && !c.saved) || c.pub[0] != src) continue;
    uint8_t d[MAX_PACKET_PAYLOAD + 1];
    const int len = mesh::Utils::MACThenDecrypt(secretFor(i), d, &pk->payload[2], pk->payload_len - 2);
    if (len <= 0) continue;
    d[len] = 0;
    if (type == PAYLOAD_TYPE_TXT_MSG) {
      if (len <= 5 || (d[4] >> 2) != 0) return;   // plain text only
      const size_t tl = strnlen((const char*)&d[5], len - 5);
      addMsg(c.pub, false, 0, pk->isRouteFlood() ? pk->getPathHashCount() : 0xFF, (const char*)&d[5]);
      uint8_t ack[6];
      mesh::Utils::sha256(ack, 4, d, 5 + tl, c.pub, 32);
      ack[4] = (5 + tl + 1 < (size_t)len) ? d[5 + tl + 1] : 0;
      the_mesh->getRNG()->random(&ack[5], 1);
      if (pk->isRouteFlood()) {   // tell them the way here, with the ACK inside
        if (mesh::Packet* r = pathReturn(i, pk->path, pk->path_len, PAYLOAD_TYPE_ACK, ack, 6)) the_mesh->sendFlood(r, kAckDelay);
      } else if (mesh::Packet* a = the_mesh->createAck(ack, 6)) {
        if (c.path_len >= 0) the_mesh->sendDirect(a, c.path, (uint8_t)c.path_len, kAckDelay);
        else the_mesh->sendFlood(a, kAckDelay);
      }
    } else {   // PATH: the way to them, maybe with an ACK for our message
      const uint8_t pl = d[0];
      if (!mesh::Packet::isValidPathLen(pl)) return;
      const int bytes = ((pl >> 6) + 1) * (pl & 63);
      if (1 + bytes + 1 > len || bytes > (int)sizeof(c.path)) return;
      memcpy(c.path, &d[1], bytes);
      c.path_len = pl;
      const uint8_t extra_type = d[1 + bytes] & 0x0F;
      if (extra_type == PAYLOAD_TYPE_ACK && len - (2 + bytes) >= 4) {
        uint32_t crc;
        memcpy(&crc, &d[2 + bytes], 4);
        ackMatched(crc);
      }
      if (pk->isRouteFlood())   // send them our way back, directly
        if (mesh::Packet* r = pathReturn(i, pk->path, pk->path_len, 0, nullptr, 0)) the_mesh->sendDirect(r, c.path, pl, 500);
    }
    c.heard = the_mesh->getRTCClock()->getCurrentTime();
    return;
  }
}

// chat nodes heard advertising become contacts (not saved to flash)
void dmOnAdvert(const uint8_t* pub, uint8_t type, const char* name, uint32_t now) {
  if (!mu || !contacts || type != ADV_TYPE_CHAT) return;
  Lock l;
  if (on && memcmp(pub, me.pub_key, 32) == 0) return;
  int i = findContact(pub);
  if (i < 0) {
    i = newSlot();
    if (i < 0) return;
    contacts[i] = Contact{};
    memcpy(contacts[i].pub, pub, 32);
    contacts[i].path_len = -1;
  }
  if (name && *name) cleanText(contacts[i].name, sizeof(contacts[i].name), name, sizeof(contacts[i].name) - 1);
  contacts[i].heard = now ? now : 1;
}

bool dmQueue(const uint8_t pub[32], const char* text) {
  if (!mu) return false;
  Lock l;
  if (!on || q_full || pend.busy || millis() - last_send_ms < 5000 || findContact(pub) < 0) return false;
  memcpy(q_pub, pub, 32);
  strlcpy(q_text, text, sizeof(q_text));
  q_full = true;
  last_send_ms = millis();
  return true;
}

bool dmAdvert() {
  if (!mu) return false;
  Lock l;
  if (!on || (last_advert_ms && millis() - last_advert_ms < 60000)) return false;
  last_advert_ms = millis();
  want_advert = true;
  return true;
}

// turn on (makes a key the first time, never the same first byte as the repeater) or off
bool dmEnable(bool enable, const char* name) {
  const uint8_t* repeater_pub = selfPub();
  if (!mu || !contacts) return false;
  Lock l;
  Preferences p;
  if (!p.begin("hxdm", false)) return false;
  if (enable) {
    if (p.getBytesLength("id") != PRV_KEY_SIZE + PUB_KEY_SIZE) {
      if (!the_mesh) { p.end(); return false; }
      for (int tries = 0; tries < 50; tries++) {
        me = mesh::LocalIdentity(the_mesh->getRNG());
        const uint8_t b = me.pub_key[0];
        if (b != 0x00 && b != 0xFF && (!repeater_pub || b != repeater_pub[0])) break;
      }
      uint8_t key[PRV_KEY_SIZE + PUB_KEY_SIZE];
      me.writeTo(key, sizeof(key));
      p.putBytes("id", key, sizeof(key));
      memset(key, 0, sizeof(key));
    }
    if (name && *name) { cleanText(my_name, sizeof(my_name), name, sizeof(my_name) - 1); p.putString("name", my_name); }
    if (!my_name[0]) strlcpy(my_name, "Hoptics chat", sizeof(my_name));
    for (int i = 0; i < kContacts; i++) contacts[i].has_secret = false;
    on = true;
    want_advert = true;   // let people add it
  } else {
    on = false;
    pend.busy = q_full = false;
  }
  p.putBool("on", on);
  p.end();
  return true;
}

bool dmSaveContact(const uint8_t pub[32], const char* name, bool keep) {
  if (!mu || !contacts) return false;
  Lock l;
  int i = findContact(pub);
  if (keep) {
    if (on && memcmp(pub, me.pub_key, 32) == 0) return false;
    if (i < 0) {
      int saved = 0;
      for (int k = 0; k < kContacts; k++) saved += contacts[k].saved;
      if (saved >= kSaved || (i = newSlot()) < 0) return false;
      contacts[i] = Contact{};
      memcpy(contacts[i].pub, pub, 32);
      contacts[i].path_len = -1;
    } else if (!contacts[i].saved) {
      int saved = 0;
      for (int k = 0; k < kContacts; k++) saved += contacts[k].saved;
      if (saved >= kSaved) return false;
    }
    if (name && *name) cleanText(contacts[i].name, sizeof(contacts[i].name), name, sizeof(contacts[i].name) - 1);
    contacts[i].saved = true;
  } else {
    if (i < 0) return true;
    contacts[i].saved = false;
  }
  storeSaved();
  return true;
}

bool traceStart(const uint8_t* path, int n) {
  if (!mu || n < 1 || n > kTraceMax || !the_mesh) return false;
  Lock l;
  if (tr.queued || (tr.waiting && millis() - tr.sent_ms < 10000UL)) return false;
  memcpy(tr.path, path, n);
  tr.n = n;
  the_mesh->getRNG()->random((uint8_t*)&tr.tag, 4);
  tr.queued = true;
  tr.waiting = tr.done = false;
  return true;
}

void writeTrace(Print& out) {
  if (!mu) { out.print("{}"); return; }
  Lock l;
  out.printf("{\"busy\":%s,\"done\":%s,\"ago\":%lu,\"path\":[", (tr.queued || tr.waiting) ? "true" : "false",
             tr.done ? "true" : "false", (unsigned long)((millis() - tr.sent_ms) / 1000));
  for (int i = 0; i < tr.n; i++) out.printf("%s\"%02x\"", i ? "," : "", tr.path[i]);
  out.print("],\"snr\":[");
  for (int i = 0; tr.done && i < tr.n; i++) out.printf("%s%.2f", i ? "," : "", tr.snr4[i] / 4.0f);
  out.printf("],\"back\":%.2f}", tr.done ? tr.last4 / 4.0f : 0.0f);
}

void writeDm(Print& out, uint32_t since) {
  if (!mu || !contacts || !msgs) { out.print("{\"on\":false,\"contacts\":[],\"msgs\":[]}"); return; }
  Lock l;
  out.printf("{\"on\":%s,\"name\":", on ? "true" : "false");
  jstr(out, my_name);
  out.print(",\"pub\":\"");
  if (on) hexOut(out, me.pub_key, 32);
  out.printf("\",\"busy\":%s,\"seq\":%lu,\"contacts\":[", pend.busy ? "true" : "false", (unsigned long)msg_seq);
  bool first = true;
  for (int i = 0; i < kContacts; i++) {
    const Contact& c = contacts[i];
    if (!c.heard && !c.saved) continue;
    out.print(first ? "[\"" : ",[\"");
    first = false;
    hexOut(out, c.pub, 32);
    out.print("\",");
    jstr(out, c.name);
    out.printf(",%lu,%d,%d]", (unsigned long)c.heard, c.path_len < 0 ? -1 : (c.path_len & 63), c.saved ? 1 : 0);
  }
  out.print("],\"msgs\":[");
  first = true;
  const uint32_t lo = msg_seq > 48 ? msg_seq - 48 : 0;   // newest 48 at most, so a reply stays small
  for (uint32_t s = (since > lo ? since : lo) + 1; s <= msg_seq; s++) {
    const Msg* m = findMsg(s);
    if (!m) continue;
    out.printf("%s[%lu,%lu,\"", first ? "" : ",", (unsigned long)m->seq, (unsigned long)m->t);
    first = false;
    hexOut(out, m->pub, 32);
    out.printf("\",%u,%u,%u,", m->out, m->status, m->hops);
    jstr(out, m->text);
    out.print(']');
  }
  out.print("]}");
}

}  // namespace hx

// ---- room posts (room roles only): the mesh loop mirrors the room's posts here every few seconds, and posts
// what the page typed as the room itself (addSystemPost), one at a time.
namespace hx {
namespace {
SemaphoreHandle_t room_mu;
bool room_on;
RoomPost* room_posts;
int room_n;
char room_q[kRoomMax + 1];
bool room_q_full;
uint32_t room_last_ms;
struct RLock {
  RLock() { xSemaphoreTake(room_mu, portMAX_DELAY); }
  ~RLock() { xSemaphoreGive(room_mu); }
};
}  // namespace

void roomSetPosts(const RoomPost* p, int n) {
  if (!room_mu) {
    room_mu = xSemaphoreCreateMutex();
    room_posts = (RoomPost*)heap_caps_calloc(kRoomPosts, sizeof(RoomPost), MALLOC_CAP_SPIRAM);
  }
  if (!room_mu || !room_posts) return;
  RLock l;
  room_on = true;
  room_n = n > kRoomPosts ? kRoomPosts : n;
  for (int i = 0; i < room_n; i++) {
    room_posts[i] = p[i];
    cleanText(room_posts[i].text, sizeof(room_posts[i].text), p[i].text, sizeof(p[i].text) - 1);
  }
}

bool roomQueuePost(const char* text) {
  if (!room_mu) return false;
  RLock l;
  if (!room_on || room_q_full || millis() - room_last_ms < 5000) return false;
  strlcpy(room_q, text, sizeof(room_q));
  room_q_full = true;
  room_last_ms = millis();
  return true;
}

bool roomTakePost(char* out, size_t n) {
  if (!room_mu) return false;
  RLock l;
  if (!room_q_full) return false;
  strlcpy(out, room_q, n);
  room_q_full = false;
  return true;
}

void writeRoom(Print& out) {
  if (!room_mu || !room_posts) { out.print("{\"on\":false,\"posts\":[]}"); return; }
  RLock l;
  out.print("{\"on\":true,\"posts\":[");
  for (int i = 0; i < room_n; i++) {
    const RoomPost& p = room_posts[i];
    out.printf("%s[\"%02x%02x%02x%02x\",%lu,%d,", i ? "," : "", p.author[0], p.author[1], p.author[2], p.author[3],
               (unsigned long)p.t, p.mine ? 1 : 0);
    jstr(out, p.text);
    out.print(']');
  }
  out.print("]}");
}
}  // namespace hx
