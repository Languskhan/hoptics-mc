/* Hoptics-MC web dashboard: helpers, crypto, MeshCore packets, charts. MIT licence.
 * Everything from the radio or the internet is shown with textContent or drawn on canvas, never as HTML. */
"use strict";
const $ = (id) => document.getElementById(id);
function el(tag, cls, text) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined && text !== null) e.textContent = String(text);
  return e;
}
const css = (v) => getComputedStyle(document.body).getPropertyValue(v).trim();
const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
const hex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");
const unhex = (s) => { const o = new Uint8Array(s.length >> 1); for (let i = 0; i < o.length; i++) o[i] = parseInt(s.substr(i * 2, 2), 16); return o; };
const utf8 = (b) => { try { return new TextDecoder().decode(b); } catch (e) { return ""; } };
const nowS = () => Math.floor(Date.now() / 1000);
function ago(t, now) {
  if (!t) return "-";
  const d = (now || nowS()) - t;
  if (d < 5) return "now";
  if (d < 60) return d + "s";
  if (d < 3600) return Math.floor(d / 60) + "m";
  if (d < 86400) return Math.floor(d / 3600) + "h";
  return Math.floor(d / 86400) + "d";
}
const clock = (t) => new Date(t * 1000).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
function km(d) { return d < 1 ? Math.round(d * 1000) + " m" : d < 10 ? d.toFixed(1) + " km" : Math.round(d) + " km"; }
function distKm(a, b) {
  const R = 6371, r = Math.PI / 180, dLa = (b.lat - a.lat) * r, dLo = (b.lon - a.lon) * r;
  const h = Math.sin(dLa / 2) ** 2 + Math.cos(a.lat * r) * Math.cos(b.lat * r) * Math.sin(dLo / 2) ** 2;
  return 2 * R * Math.asin(Math.sqrt(h));
}
function bearing(a, b) {
  const r = Math.PI / 180, y = Math.sin((b.lon - a.lon) * r) * Math.cos(b.lat * r);
  const x = Math.cos(a.lat * r) * Math.sin(b.lat * r) - Math.sin(a.lat * r) * Math.cos(b.lat * r) * Math.cos((b.lon - a.lon) * r);
  return (Math.atan2(y, x) / r + 360) % 360;
}
const compass = (deg) => ["N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"][Math.round(deg / 22.5) % 16];

// a node's colour from its key, the same idea as the device: stable, spread around the wheel
function keyColor(key) {
  let h = 2166136261;
  for (let i = 0; i < key.length; i++) h = Math.imul(h ^ key.charCodeAt(i), 16777619);
  const hue = (h >>> 0) % 360, day = document.body.classList.contains("day");
  return `hsl(${hue} ${day ? 70 : 65}% ${day ? 42 : 62}%)`;
}
function initials(name) {
  const w = String(name || "?").replace(/[^\p{L}\p{N} ]/gu, "").trim().split(/\s+/);
  return ((w[0] || "?")[0] + (w.length > 1 ? w[1][0] : (w[0] || "")[1] || "")).toUpperCase();
}
function avatar(key, name) {
  const a = el("span", "av", initials(name));
  a.style.background = keyColor(key || name || "?");
  return a;
}

// ------------------------------------------------------------------ crypto (the page may be plain http, where
// crypto.subtle is missing, so SHA-256, HMAC and AES-128 are done here)
const SHA = (() => {
  const K = new Uint32Array([0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2]);
  const rotr = (x, n) => (x >>> n) | (x << (32 - n));
  function sha256(data) {
    const len = data.length, bl = ((len + 9 + 63) >> 6) << 6, m = new Uint8Array(bl);
    m.set(data); m[len] = 0x80;
    const dv = new DataView(m.buffer);
    dv.setUint32(bl - 4, len * 8); dv.setUint32(bl - 8, Math.floor(len / 0x20000000));
    const H = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]), W = new Uint32Array(64);
    for (let o = 0; o < bl; o += 64) {
      for (let i = 0; i < 16; i++) W[i] = dv.getUint32(o + i * 4);
      for (let i = 16; i < 64; i++) {
        const s0 = rotr(W[i - 15], 7) ^ rotr(W[i - 15], 18) ^ (W[i - 15] >>> 3), s1 = rotr(W[i - 2], 17) ^ rotr(W[i - 2], 19) ^ (W[i - 2] >>> 10);
        W[i] = (W[i - 16] + s0 + W[i - 7] + s1) | 0;
      }
      let [a, b, c, d, e, f, g, h] = H;
      for (let i = 0; i < 64; i++) {
        const t1 = (h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + W[i]) | 0;
        const t2 = ((rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c))) | 0;
        h = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
      }
      H[0] += a; H[1] += b; H[2] += c; H[3] += d; H[4] += e; H[5] += f; H[6] += g; H[7] += h;
    }
    const out = new Uint8Array(32), ov = new DataView(out.buffer);
    for (let i = 0; i < 8; i++) ov.setUint32(i * 4, H[i]);
    return out;
  }
  function hmac(key, msg) {
    const k = new Uint8Array(64);
    k.set(key.length > 64 ? sha256(key) : key);
    const ip = new Uint8Array(64 + msg.length), op = new Uint8Array(96);
    for (let i = 0; i < 64; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
    ip.set(msg, 64); op.set(sha256(ip), 64);
    return sha256(op);
  }
  return { sha256, hmac };
})();

const AES = (() => {
  const S = new Uint8Array(256), Si = new Uint8Array(256);
  const rotl8 = (x, s) => ((x << s) | (x >> (8 - s))) & 255;
  let p = 1, q = 1;
  do {
    p = (p ^ (p << 1) ^ (p & 0x80 ? 0x1b : 0)) & 255;
    q ^= q << 1; q ^= q << 2; q ^= q << 4; q &= 255; if (q & 0x80) q ^= 0x09;
    S[p] = (q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63) & 255;
  } while (p !== 1);
  S[0] = 0x63;
  for (let i = 0; i < 256; i++) Si[S[i]] = i;
  const xt = (a) => ((a << 1) ^ (a & 0x80 ? 0x1b : 0)) & 255;
  const mul = (a, b) => { let r = 0; while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; } return r; };
  function expand(key) {
    const w = new Uint8Array(176); w.set(key);
    let rc = 1;
    for (let i = 16; i < 176; i += 4) {
      let t = [w[i - 4], w[i - 3], w[i - 2], w[i - 1]];
      if (i % 16 === 0) { t = [S[t[1]] ^ rc, S[t[2]], S[t[3]], S[t[0]]]; rc = xt(rc); }
      for (let j = 0; j < 4; j++) w[i + j] = w[i - 16 + j] ^ t[j];
    }
    return w;
  }
  function decryptBlock(w, inp, out, off) {
    const s = new Uint8Array(16);
    for (let i = 0; i < 16; i++) s[i] = inp[off + i] ^ w[160 + i];
    for (let r = 9; r >= 0; r--) {
      // inverse shift rows (state is column-major: s[row + 4 * col])
      const t = s.slice();
      for (let row = 1; row < 4; row++) for (let c = 0; c < 4; c++) s[row + 4 * ((c + row) % 4)] = t[row + 4 * c];
      for (let i = 0; i < 16; i++) s[i] = Si[s[i]] ^ w[r * 16 + i];
      if (r) for (let c = 0; c < 4; c++) {
        const a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
        s[4 * c] = mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9);
        s[4 * c + 1] = mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13);
        s[4 * c + 2] = mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11);
        s[4 * c + 3] = mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14);
      }
    }
    out.set(s, off);
  }
  function decryptECB(key, data) {
    const w = expand(key), out = new Uint8Array(data.length);
    for (let o = 0; o + 16 <= data.length; o += 16) decryptBlock(w, data, out, o);
    return out;
  }
  function encryptECB(key, data) { // only the demo uses this (the page never sends)
    const w = expand(key), out = new Uint8Array(data.length);
    for (let o = 0; o + 16 <= data.length; o += 16) {
      const s = new Uint8Array(16);
      for (let i = 0; i < 16; i++) s[i] = data[o + i] ^ w[i];
      for (let r = 1; r <= 10; r++) {
        for (let i = 0; i < 16; i++) s[i] = S[s[i]];
        const t = s.slice();
        for (let row = 1; row < 4; row++) for (let c = 0; c < 4; c++) s[row + 4 * c] = t[row + 4 * ((c + row) % 4)];
        if (r < 10) for (let c = 0; c < 4; c++) {
          const a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
          s[4 * c] = mul(a0, 2) ^ mul(a1, 3) ^ a2 ^ a3; s[4 * c + 1] = a0 ^ mul(a1, 2) ^ mul(a2, 3) ^ a3;
          s[4 * c + 2] = a0 ^ a1 ^ mul(a2, 2) ^ mul(a3, 3); s[4 * c + 3] = mul(a0, 3) ^ a1 ^ a2 ^ mul(a3, 2);
        }
        for (let i = 0; i < 16; i++) s[i] ^= w[r * 16 + i];
      }
      out.set(s, o);
    }
    return out;
  }
  return { decryptECB, encryptECB };
})();

// ------------------------------------------------------------------ MeshCore packets
const PT = ["REQ", "RESP", "TEXT", "ACK", "ADVERT", "GROUP", "GDATA", "ANON", "PATH", "TRACE", "MULTI", "CTRL", "T12", "T13", "T14", "RAW"];
const PT_NAME = { REQ: "Request", RESP: "Response", TEXT: "Direct message", ACK: "Ack", ADVERT: "Advert", GROUP: "Channel message", GDATA: "Channel data", ANON: "Anonymous request", PATH: "Path", TRACE: "Trace", MULTI: "Multipart", CTRL: "Control", RAW: "Raw" };
const PT_COLOR = { ADVERT: "--green", GROUP: "--cyan", TEXT: "--blue", ACK: "--dim", PATH: "--amber", TRACE: "--pink", REQ: "--violet", RESP: "--violet", ANON: "--orange", CTRL: "--orange" };
const ptColor = (t) => css(PT_COLOR[t] || "--muted");
const NT = ["Node", "Companion", "Repeater", "Room", "Sensor"];
const NT_COLOR = ["--muted", "--blue", "--green", "--violet", "--amber"];

function decodePacket(b) {
  if (!b || b.length < 2) return null;
  const h = b[0], route = h & 3, type = PT[(h >> 2) & 15];
  let i = 1;
  if (route === 0 || route === 3) i += 4; // transport codes
  if (i >= b.length) return null;
  const pl = b[i++], hs = (pl >> 6) + 1, hc = pl & 63, path = [];
  if (i + hc * hs > b.length) return null;
  for (let k = 0; k < hc; k++) path.push(hex(b.subarray(i + k * hs, i + (k + 1) * hs)));
  i += hc * hs;
  const pay = b.subarray(i);
  // MeshCore's packet hash: every copy of a packet has it, whatever path it took
  const hb = new Uint8Array(pay.length + 2); let n = 0;
  hb[n++] = (h >> 2) & 15;
  if (type === "TRACE") hb[n++] = pl;
  hb.set(pay, n);
  const p = { type, flood: route < 2, path, hashSize: hs, len: b.length, id: hex(SHA.sha256(hb.subarray(0, n + pay.length)).subarray(0, 8)) };
  if (type === "ADVERT" && pay.length >= 101) {
    p.key = hex(pay.subarray(0, 32));
    const app = pay.subarray(100), f = app[0];
    p.nodeType = f & 15;
    let j = 1;
    if (f & 0x10 && app.length >= j + 8) {
      const dv = new DataView(app.buffer, app.byteOffset + j, 8);
      const lat = dv.getInt32(0, true) / 1e6, lon = dv.getInt32(4, true) / 1e6;
      if (lat || lon) { p.lat = lat; p.lon = lon; }
      j += 8;
    }
    if (f & 0x20) j += 2;
    if (f & 0x40) j += 2;
    if (f & 0x80) p.name = utf8(app.subarray(j)).replace(/\0.*$/, "");
  } else if ((type === "GROUP" || type === "GDATA") && pay.length > 3) {
    p.chHash = pay[0]; p.mac = pay.subarray(1, 3); p.ct = pay.subarray(3);
  }
  return p;
}
// a channel message: try each channel this node shares with the page
function decryptGroup(p, channels) {
  if (!p.ct || p.ct.length % 16) return null;
  for (const ch of channels) {
    if (ch.hash !== p.chHash) continue;
    const mac = SHA.hmac(ch.secret, p.ct);
    if (mac[0] !== p.mac[0] || mac[1] !== p.mac[1]) continue;
    const pt = AES.decryptECB(ch.key, p.ct);
    const t = new DataView(pt.buffer).getUint32(0, true);
    const s = utf8(pt.subarray(5)).replace(/\0[\s\S]*$/, "");
    const c = s.indexOf(": ");
    return { ch: ch.i, t, from: c > 0 ? s.slice(0, c) : "", text: c > 0 ? s.slice(c + 2) : s };
  }
  return null;
}
function channelKeys(list) {
  return list.filter((c) => c.k).map((c) => {
    const key = unhex(c.k), secret = new Uint8Array(32);
    secret.set(key);
    return { i: c.i, name: c.n, key, secret, hash: SHA.sha256(key)[0] };
  });
}

// ------------------------------------------------------------------ charts (canvas, with hover read-outs)
function fitCanvas(c, h) {
  const r = c.getBoundingClientRect(), dpr = window.devicePixelRatio || 1;
  const w = Math.max(10, r.width);
  if (c.width !== Math.round(w * dpr) || c.height !== Math.round(h * dpr)) { c.width = Math.round(w * dpr); c.height = Math.round(h * dpr); }
  c.style.height = h + "px";
  const g = c.getContext("2d");
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  return [g, w, h];
}
function niceStep(range, n) {
  const raw = range / Math.max(1, n), p = Math.pow(10, Math.floor(Math.log10(raw))), f = raw / p;
  return (f < 1.5 ? 1 : f < 3 ? 2 : f < 7 ? 5 : 10) * p;
}
function fmtNum(v, d) { return Math.abs(v) >= 10000 ? (v / 1000).toFixed(0) + "k" : Math.abs(v) >= 1000 ? (v / 1000).toFixed(1) + "k" : v.toFixed(d === undefined ? (Math.abs(v) < 10 && v % 1 ? 1 : 0) : d); }
function fmtTimeTick(t, span) {
  const d = new Date(t * 1000);
  return span > 2 * 86400 ? d.toLocaleDateString([], { weekday: "short" }) : d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
}
const charts = new Set();
// opts: {height, type: "line"|"area"|"bar"|"scatter", stacked, yMin, yMax, unit, xTime, xFmt, yFmt, series: [{name, color, data: [[x,y]...], hidden}]}
class Chart {
  constructor(host, opts) {
    this.o = Object.assign({ height: 180, type: "line", unit: "", xTime: true }, opts);
    this.host = host; host.classList.add("chart");
    this.c = el("canvas"); this.tip = el("div", "tip"); this.tip.style.opacity = 0;
    host.append(this.c, this.tip);
    if (this.o.legend !== false) { this.leg = el("div", "legend"); host.append(this.leg); }
    this.hover = null;
    const move = (e) => { const r = this.c.getBoundingClientRect(); this.hover = [e.clientX - r.left, e.clientY - r.top]; this.draw(); };
    this.c.addEventListener("pointermove", move);
    this.c.addEventListener("pointerdown", move);
    this.c.addEventListener("pointerleave", () => { this.hover = null; this.draw(); });
    charts.add(this);
  }
  set(series, patch) { this.o.series = series; if (patch) Object.assign(this.o, patch); this.legend(); this.draw(); }
  legend() {
    if (!this.leg) return;
    this.leg.replaceChildren();
    if ((this.o.series || []).length < 2) return;
    for (const s of this.o.series) {
      const sp = el("span", s.hidden ? "off" : ""), i = el("i");
      i.style.background = css(s.color) || s.color;
      sp.append(i, document.createTextNode(s.name));
      sp.onclick = () => { s.hidden = !s.hidden; this.legend(); this.draw(); };
      this.leg.append(sp);
    }
  }
  draw() {
    const o = this.o, [g, w, h] = fitCanvas(this.c, o.height);
    g.clearRect(0, 0, w, h);
    const ser = (o.series || []).filter((s) => !s.hidden && s.data && s.data.length);
    const L = 38, R = 10, T = 10, B = 22, pw = w - L - R, ph = h - T - B;
    g.font = "11px system-ui"; g.textBaseline = "middle";
    if (!ser.length) { g.fillStyle = css("--dim"); g.textAlign = "center"; g.fillText(o.empty || "No data yet", w / 2, h / 2); return; }
    let x0 = Infinity, x1 = -Infinity, y0 = o.yMin !== undefined ? o.yMin : Infinity, y1 = o.yMax !== undefined ? o.yMax : -Infinity;
    const xs = new Set();
    for (const s of ser) for (const [x] of s.data) { x0 = Math.min(x0, x); x1 = Math.max(x1, x); xs.add(x); }
    if (o.stacked) {
      const sum = new Map();
      for (const s of ser) for (const [x, y] of s.data) sum.set(x, (sum.get(x) || 0) + (y || 0));
      for (const v of sum.values()) { if (o.yMax === undefined) y1 = Math.max(y1, v); if (o.yMin === undefined) y0 = Math.min(y0, 0); }
    } else for (const s of ser) for (const [, y] of s.data) if (y !== null && y !== undefined) { if (o.yMin === undefined) y0 = Math.min(y0, y); if (o.yMax === undefined) y1 = Math.max(y1, y); }
    if (o.type === "bar" && o.yMin === undefined) y0 = Math.min(0, y0);
    if (y0 === y1) { y1 += 1; if (o.yMin === undefined) y0 -= 1; }
    if (o.yPad && o.yMax === undefined) y1 += (y1 - y0) * o.yPad;
    const step = niceStep(y1 - y0, 4);
    if (o.yMin === undefined) y0 = Math.floor(y0 / step) * step;
    if (o.yMax === undefined) y1 = Math.ceil(y1 / step) * step;
    const sorted = [...xs].sort((a, b) => a - b);
    const bw = o.type === "bar" ? Math.max(2, (pw / Math.max(1, sorted.length)) * 0.72) : 0;
    const X = (x) => L + bw / 2 + (x1 === x0 ? pw / 2 : ((x - x0) / (x1 - x0)) * (pw - bw));
    const Y = (y) => T + ph - ((y - y0) / (y1 - y0)) * ph;
    // grid and axes
    g.strokeStyle = css("--quiet"); g.lineWidth = 1; g.fillStyle = css("--dim"); g.textAlign = "right";
    for (let v = y0; v <= y1 + step / 2; v += step) {
      const y = Math.round(Y(v)) + 0.5;
      g.beginPath(); g.moveTo(L, y); g.lineTo(w - R, y); g.stroke();
      g.fillText((o.yFmt ? o.yFmt(v) : fmtNum(v)), L - 6, y);
    }
    g.textAlign = "center"; g.textBaseline = "alphabetic";
    const nt = Math.max(2, Math.floor(pw / 90));
    for (let i = 0; i <= nt; i++) {
      const x = x0 + ((x1 - x0) * i) / nt;
      const lab = o.xFmt ? o.xFmt(x) : o.xTime ? fmtTimeTick(x, x1 - x0) : fmtNum(x);
      g.fillText(lab, clamp(X(x), L + 16, w - R - 16), h - 5);
    }
    // series
    const base = new Map();
    ser.forEach((s, si) => {
      const col = css(s.color) || s.color;
      if (o.type === "bar") {
        const n = o.stacked ? 1 : ser.length, sw = bw / n;
        for (const [x, y] of s.data) {
          if (!y) continue;
          const b0 = o.stacked ? base.get(x) || 0 : 0, b1 = b0 + y;
          if (o.stacked) base.set(x, b1);
          const bx = X(x) - bw / 2 + (o.stacked ? 0 : si * sw), top = Y(b1), bot = Y(b0);
          g.fillStyle = col;
          const r = Math.min(3, sw / 2, (bot - top) / 2);
          g.beginPath(); g.roundRect ? g.roundRect(bx, top, sw - 1, bot - top, [r, r, 0, 0]) : g.rect(bx, top, sw - 1, bot - top); g.fill();
        }
      } else if (o.type === "scatter") {
        g.fillStyle = col; g.globalAlpha = 0.75;
        for (const [x, y] of s.data) { g.beginPath(); g.arc(X(x), Y(y), 3.2, 0, 7); g.fill(); }
        g.globalAlpha = 1;
      } else {
        g.beginPath(); let pen = false, first = null, last = null;
        for (const [x, y] of s.data) {
          if (y === null || y === undefined) { pen = false; continue; }
          const px = X(x), py = Y(clamp(y, y0, y1));
          if (pen) g.lineTo(px, py); else { g.moveTo(px, py); pen = true; if (!first) first = px; }
          last = px;
        }
        g.strokeStyle = col; g.lineWidth = 2; g.lineJoin = "round"; g.shadowColor = col; g.shadowBlur = 10; g.stroke(); g.shadowBlur = 0;
        if ((o.type === "area" || s.fill) && first !== null) {
          g.lineTo(last, Y(Math.max(y0, 0))); g.lineTo(first, Y(Math.max(y0, 0))); g.closePath();
          const gr = g.createLinearGradient(0, T, 0, T + ph);
          gr.addColorStop(0, col.replace(/^hsl\((.*)\)$/, "hsl($1 / .35)").replace(/^#(\w{6})$/, "#$155"));
          gr.addColorStop(1, col.replace(/^hsl\((.*)\)$/, "hsl($1 / 0)").replace(/^#(\w{6})$/, "#$100"));
          g.fillStyle = gr; g.fill();
        }
      }
    });
    // hover: nearest x (or nearest point for scatter), crosshair and a read-out
    this.tip.style.opacity = 0;
    if (!this.hover) return;
    const [hx, hy] = this.hover;
    if (hx < L || hx > w - R) return;
    let rows = [], title = "", px = hx, py = hy;
    if (o.type === "scatter") {
      let best = null, bd = 400;
      for (const s of ser) for (const pt of s.data) { const d = (X(pt[0]) - hx) ** 2 + (Y(pt[1]) - hy) ** 2; if (d < bd) { bd = d; best = [s, pt]; } }
      if (!best) return;
      px = X(best[1][0]); py = Y(best[1][1]);
      g.strokeStyle = css("--text"); g.lineWidth = 2; g.beginPath(); g.arc(px, py, 6, 0, 7); g.stroke();
      title = best[1][2] || best[0].name;
      rows = (o.tipRows ? o.tipRows(best[1]) : [[best[0].name, fmtNum(best[1][1]) + o.unit]]).map((r) => [best[0].color, ...r]);
    } else {
      let bx = sorted[0], bd = Infinity;
      for (const x of sorted) { const d = Math.abs(X(x) - hx); if (d < bd) { bd = d; bx = x; } }
      px = X(bx);
      g.strokeStyle = css("--line2"); g.setLineDash([3, 3]); g.beginPath(); g.moveTo(px, T); g.lineTo(px, T + ph); g.stroke(); g.setLineDash([]);
      title = o.xFmt ? o.xFmt(bx) : o.xTime ? new Date(bx * 1000).toLocaleString([], { weekday: "short", hour: "2-digit", minute: "2-digit" }) : fmtNum(bx);
      let acc = 0;
      for (const s of ser) {
        const pt = s.data.find((d) => d[0] === bx);
        if (!pt || pt[1] === null || pt[1] === undefined) continue;
        rows.push([s.color, s.name, (o.yFmt ? o.yFmt(pt[1]) : fmtNum(pt[1])) + o.unit]);
        if (o.type !== "bar") { g.fillStyle = css(s.color) || s.color; g.beginPath(); g.arc(px, Y(o.stacked ? (acc += pt[1]) : pt[1]), 4, 0, 7); g.fill(); }
      }
      if (o.stacked && rows.length > 1) rows.push(["--text", "Total", fmtNum(rows.reduce((a, r) => a + parseFloat(r[2]), 0)) + o.unit]);
    }
    if (!rows.length) return;
    this.tip.replaceChildren(el("b", "", title));
    for (const [c, n, v] of rows) { const r = el("div", "row"), sw = el("i", "sw"); sw.style.background = css(c) || c; r.append(sw, el("span", "", n), el("span", "", v)); this.tip.append(r); }
    const tw = this.tip.offsetWidth || 140;
    this.tip.style.left = (px + 14 + tw > w ? px - tw - 14 : px + 14) + "px";
    this.tip.style.top = clamp(py - 20, 0, h - 60) + "px";
    this.tip.style.opacity = 1;
  }
}
// a ring of shares, with the hovered slice lifted
class Donut {
  constructor(host, opts) {
    this.o = Object.assign({ size: 170 }, opts);
    host.classList.add("chart");
    const wrap = el("div"); wrap.style.cssText = "display:flex;align-items:center;gap:18px;flex-wrap:wrap";
    this.c = el("canvas"); this.c.style.width = this.o.size + "px"; this.c.style.flex = "none";
    this.list = el("div"); this.list.style.cssText = "flex:1;min-width:140px;font-size:12.5px";
    wrap.append(this.c, this.list); host.append(wrap);
    this.hi = -1;
    this.c.addEventListener("pointermove", (e) => {
      const r = this.c.getBoundingClientRect(), x = e.clientX - r.left - r.width / 2, y = e.clientY - r.top - r.height / 2;
      const d = Math.hypot(x, y), R = r.width / 2;
      let a = Math.atan2(y, x) + Math.PI / 2; if (a < 0) a += Math.PI * 2;
      let hi = -1;
      if (d > R * 0.55 && d < R) { let acc = 0; this.items.forEach((it, i) => { const s = (it.v / this.total) * Math.PI * 2; if (a >= acc && a < acc + s) hi = i; acc += s; }); }
      if (hi !== this.hi) { this.hi = hi; this.draw(); }
    });
    this.c.addEventListener("pointerleave", () => { this.hi = -1; this.draw(); });
    charts.add(this);
  }
  set(items) { this.items = items.filter((i) => i.v > 0); this.total = this.items.reduce((a, i) => a + i.v, 0); this.draw(); }
  draw() {
    const s = this.o.size, [g] = fitCanvas(this.c, s), R = s / 2;
    g.clearRect(0, 0, s, s);
    this.list.replaceChildren();
    if (!this.items || !this.total) { g.fillStyle = css("--dim"); g.textAlign = "center"; g.fillText("No data yet", R, R); return; }
    let a = -Math.PI / 2;
    this.items.forEach((it, i) => {
      const sw = (it.v / this.total) * Math.PI * 2, lift = i === this.hi ? 5 : 0, mid = a + sw / 2;
      g.save(); g.translate(Math.cos(mid) * lift, Math.sin(mid) * lift);
      g.beginPath(); g.arc(R, R, R - 6, a + 0.012, a + sw - 0.012); g.arc(R, R, R * 0.62, a + sw - 0.012, a + 0.012, true); g.closePath();
      g.fillStyle = css(it.c) || it.c; g.globalAlpha = this.hi < 0 || i === this.hi ? 1 : 0.45; g.fill(); g.restore();
      a += sw;
      const row = el("div"); row.style.cssText = "display:flex;align-items:center;gap:8px;margin:4px 0;cursor:default" + (i === this.hi ? ";font-weight:650" : "");
      const sq = el("i"); sq.style.cssText = `width:10px;height:10px;border-radius:3px;background:${css(it.c) || it.c}`;
      const n = el("span", "", it.n); n.style.flex = "1";
      row.append(sq, n, el("span", "muted", Math.round((it.v * 100) / this.total) + "%"), el("span", "dim", String(it.v)));
      row.onpointerenter = () => { this.hi = i; this.draw(); };
      this.list.append(row);
    });
    g.globalAlpha = 1; g.textAlign = "center"; g.textBaseline = "middle";
    const it = this.items[this.hi];
    g.fillStyle = css("--text"); g.font = "700 22px system-ui"; g.fillText(it ? Math.round((it.v * 100) / this.total) + "%" : String(this.total), R, R - 6);
    g.fillStyle = css("--muted"); g.font = "11px system-ui"; g.fillText(it ? it.n : this.o.label || "total", R, R + 14);
  }
}
function sparkline(c, vals, color) {
  const [g, w, h] = fitCanvas(c, 46);
  g.clearRect(0, 0, w, h);
  const v = vals.filter((x) => x !== null && x !== undefined);
  if (v.length < 2) return;
  const lo = Math.min(...v), hi = Math.max(...v), sp = hi - lo || 1;
  const col = css(color);
  g.beginPath();
  vals.forEach((y, i) => { const x = (i / (vals.length - 1)) * w, yy = h - 4 - ((y - lo) / sp) * (h - 10); i ? g.lineTo(x, yy) : g.moveTo(x, yy); });
  g.strokeStyle = col; g.lineWidth = 1.6; g.globalAlpha = 0.9; g.stroke();
  g.lineTo(w, h); g.lineTo(0, h); g.closePath();
  const gr = g.createLinearGradient(0, 0, 0, h); gr.addColorStop(0, col + "40"); gr.addColorStop(1, col + "00");
  g.fillStyle = gr; g.globalAlpha = 1; g.fill();
}
function hbars(host, items, color) {
  host.replaceChildren();
  host.classList.add("bars");
  if (!items.length) { host.append(el("div", "empty", "Nothing yet")); return; }
  const max = Math.max(...items.map((i) => i.v));
  for (const it of items) {
    const b = el("div", "b"), bar = el("div", "bar"), fill = el("i");
    fill.style.width = (it.v * 100) / max + "%";
    b.style.setProperty("--c", css(it.c || color));
    bar.append(fill);
    const t = el("span", "t", it.n);
    if (it.title) t.title = it.title;
    b.append(t, bar, el("span", "n", String(it.v)));
    host.append(b);
  }
}
let redrawTimer = 0;
window.addEventListener("resize", () => { clearTimeout(redrawTimer); redrawTimer = setTimeout(() => charts.forEach((c) => c.host === undefined || c.host.offsetParent ? c.draw() : 0), 80); });
