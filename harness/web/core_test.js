// The web page's own crypto and packet decoder (hoptics/web/core.js), checked against Node's crypto.
// Run: node harness/web/core_test.js   (exits non-zero on the first failure)
"use strict";
const fs = require("fs"), path = require("path"), vm = require("vm"), crypto = require("crypto"), assert = require("assert");

// core.js is a browser script: give it the few globals it touches at load time
const ctx = { window: { addEventListener() {} }, document: {}, getComputedStyle: () => ({ getPropertyValue: () => "" }), TextDecoder, TextEncoder, Uint8Array, Uint32Array, DataView, Math };
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(path.join(__dirname, "../../hoptics/web/core.js"), "utf8") + "\n;this.T = { SHA, AES, decodePacket, decryptGroup, channelKeys, hex, unhex, cardPath, fmtSize };", ctx);
const { SHA, AES, decodePacket, decryptGroup, channelKeys, hex, unhex, cardPath, fmtSize } = ctx.T;
const u8 = (b) => new Uint8Array(b);
const sha = (b) => u8(crypto.createHash("sha256").update(b).digest());
let n = 0;
const ok = (cond, what) => { assert.ok(cond, what); n++; };

// SHA-256 and HMAC, every length across the padding edges
for (let len = 0; len < 300; len++) {
  const d = u8(crypto.randomBytes(len));
  ok(hex(SHA.sha256(d)) === hex(sha(d)), "sha256 len " + len);
  const k = u8(crypto.randomBytes((len % 80) + 1));
  ok(hex(SHA.hmac(k, d)) === crypto.createHmac("sha256", k).update(d).digest("hex"), "hmac len " + len);
}

// AES-128-ECB both ways
for (let blocks = 1; blocks <= 8; blocks++) {
  const key = u8(crypto.randomBytes(16)), pt = u8(crypto.randomBytes(16 * blocks));
  const c = crypto.createCipheriv("aes-128-ecb", key, null); c.setAutoPadding(false);
  const ct = u8(Buffer.concat([c.update(pt), c.final()]));
  ok(hex(AES.encryptECB(key, pt)) === hex(ct), "aes encrypt " + blocks);
  ok(hex(AES.decryptECB(key, ct)) === hex(pt), "aes decrypt " + blocks);
}

// channel keys: Public's published PSK, and a #hashtag channel (key = first 16 bytes of SHA-256 of its name)
const pub = "8b3387e9c5cdea6ac9e5edbaa115cd72";
const tag = hex(sha(Buffer.from("#test")).subarray(0, 16));
const chans = channelKeys([{ i: 0, n: "Public", k: pub }, { i: 3, n: "#test", k: tag }, { i: 5, n: "private" }]);
ok(chans.length === 2, "a channel without a key is left out");
ok(chans[0].hash === sha(unhex(pub))[0] && chans[1].hash === sha(unhex(tag))[0], "channel hash byte");

// a channel message made the way MeshCore makes one, then read back
function groupPacket(ch, t, text, route = 1) {
  const body = Buffer.concat([Buffer.alloc(5), Buffer.from(text)]);
  body.writeUInt32LE(t, 0);
  const pt = Buffer.concat([body, Buffer.alloc((16 - (body.length % 16)) % 16)]);
  const c = crypto.createCipheriv("aes-128-ecb", ch.key, null); c.setAutoPadding(false);
  const ct = Buffer.concat([c.update(pt), c.final()]);
  const mac = crypto.createHmac("sha256", ch.secret).update(ct).digest().subarray(0, 2);
  return u8(Buffer.concat([Buffer.from([(5 << 2) | route, 0x00, ch.hash]), mac, ct]));
}
for (const ch of chans) {
  const p = decodePacket(groupPacket(ch, 1764000000, "Alice: hi there, testing 1 2 3"));
  ok(p && p.type === "GROUP" && p.flood && p.path.length === 0, "group header");
  const m = decryptGroup(p, chans);
  ok(m && m.ch === ch.i && m.t === 1764000000 && m.from === "Alice" && m.text === "hi there, testing 1 2 3", "group decrypt on " + ch.name);
}
const wrong = decodePacket(groupPacket(chans[0], 1, "Bob: x"));
wrong.mac = u8([wrong.mac[0] ^ 1, wrong.mac[1]]);
ok(decryptGroup(wrong, chans) === null, "a bad MAC is refused");

// an advert: path of two 1-byte hashes, position and name
const key = crypto.randomBytes(32), app = Buffer.alloc(9);
app[0] = 0x91; // companion, has position, has name
app.writeInt32LE(Math.round(38.6782 * 1e6), 1); app.writeInt32LE(Math.round(-121.1761 * 1e6), 5);
const advPay = Buffer.concat([key, Buffer.alloc(4), crypto.randomBytes(64), app, Buffer.from("Test node")]);
const adv = decodePacket(u8(Buffer.concat([Buffer.from([(4 << 2) | 1, 0x02, 0xaa, 0xbb]), advPay])));
ok(adv.type === "ADVERT" && adv.path.join() === "aa,bb" && adv.hashSize === 1, "advert path");
ok(adv.key === key.toString("hex") && adv.nodeType === 1 && adv.name === "Test node", "advert key, type, name");
ok(Math.abs(adv.lat - 38.6782) < 1e-6 && Math.abs(adv.lon + 121.1761) < 1e-6, "advert position");
ok(adv.id === hex(sha(Buffer.concat([Buffer.from([4]), advPay])).subarray(0, 8)), "packet hash = SHA-256(type + payload)");

// 2-byte path hashes, transport codes (route 0), and TRACE hashing its path_len byte
const two = decodePacket(u8([(2 << 2) | 1, 0x42, 1, 2, 3, 4, 9, 9]));
ok(two.hashSize === 2 && two.path.join() === "0102,0304", "2-byte path hashes");
const tr = decodePacket(u8([(9 << 2) | 0, 0xde, 0xad, 0xbe, 0xef, 0x01, 0x77, 5, 6, 7]));
ok(tr.type === "TRACE" && tr.flood && tr.path.join() === "77", "transport codes are skipped");
ok(tr.id === hex(sha(Buffer.from([9, 0x01, 5, 6, 7])).subarray(0, 8)), "TRACE hash includes path_len");

// broken input never throws
ok(decodePacket(u8([])) === null && decodePacket(u8([0x15])) === null, "too short");
ok(decodePacket(u8([(5 << 2) | 1, 0x05, 1, 2])) === null, "path longer than the packet");
ok(decodePacket(u8([(9 << 2) | 0, 1, 2, 3])) === null, "cut inside the transport codes");
for (let i = 0; i < 2000; i++) decodePacket(u8(crypto.randomBytes(1 + (i % 120))));
n++;

// the Files tab's paths on the card (the node checks them again, harness/parse) and sizes
ok(cardPath.join("/", "maps") === "/maps" && cardPath.join("/maps", "osm") === "/maps/osm", "join a name to a folder");
ok(cardPath.up("/maps/osm") === "/maps" && cardPath.up("/maps") === "/" && cardPath.up("/") === "/", "up a folder");
for (const good of ["notes.txt", "Café map", ".hidden", "a".repeat(255), "é".repeat(127)]) ok(cardPath.okName(good), "a good name: " + good.slice(0, 12));
for (const bad of ["", ".", "..", "a.", "a ", "a/b", "a\\b", "c:", "a*", "a?", 'a"', "a<", "a>", "a|", "a\n", "a\x7f", "a".repeat(256), "é".repeat(128), null, 5])
  ok(!cardPath.okName(bad), "a refused name: " + String(bad).slice(0, 12));
ok(fmtSize(0) === "0 B" && fmtSize(1536) === "1.5 KB" && fmtSize(20480) === "20 KB" && fmtSize(3 * 1048576) === "3.0 MB" && fmtSize(31154688 * 1024) === "29.7 GB", "sizes");

console.log("core.js: " + n + " checks passed");
