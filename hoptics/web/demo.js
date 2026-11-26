/* Hoptics-MC web dashboard: demo data for the preview (not part of the firmware). Every node, name, message and
 * position here is made up. Packets are real MeshCore packets built on the fly, so the page's decoding is exercised. */
"use strict";
(() => {
  let seed = 7;
  const rnd = () => ((seed = (seed * 16807) % 2147483647) / 2147483647);
  const pick = (a) => a[Math.floor(rnd() * a.length)];
  const bytes = (n) => { const b = new Uint8Array(n); for (let i = 0; i < n; i++) b[i] = Math.floor(rnd() * 256); return b; };
  const C = { lat: 38.5816, lon: -121.4944 };
  const t0 = nowS();
  const nodes = [];
  const add = (name, type, dLat, dLon, extra) => {
    const key = bytes(32);
    key[0] = 0x10 + nodes.length * 7; // distinct first bytes: one-byte path hashes resolve to one node
    const n = Object.assign({ name, type, key, k: hex(key), lat: C.lat + dLat, lon: C.lon + dLon, last: t0 - Math.floor(rnd() * 3000), hops: Math.floor(rnd() * 4) }, extra || {});
    nodes.push(n); return n;
  };
  const R = [["Foothill Ridge RPT", 0.17, 0.28], ["Folsom Lake RPT", 0.11, 0.33], ["Midtown Roof", 0.01, 0.02], ["River City RPT", -0.06, -0.05], ["Arden Tower", 0.06, 0.1],
    ["Elk Grove South", -0.17, 0.04], ["Roseville North", 0.21, 0.17], ["Davis West RPT", -0.03, -0.25], ["Rancho Hill", -0.01, 0.2], ["Fair Oaks Bluff", 0.08, 0.21],
    ["Natomas Relay", 0.1, -0.03], ["Cameron Peak", 0.1, 0.55], ["Lodi Water Tower", -0.45, 0.22], ["Placerville Grade", 0.13, 0.72]];
  for (const [n, a, b] of R) add(n, 2, a, b, { hops: Math.floor(rnd() * 3) });
  for (const [n, a, b] of [["Kayak Kit", 0.07, 0.31], ["Trail Pack", 0.19, 0.4], ["Jeep Node", -0.11, 0.09], ["Garage Base", 0.03, 0.12], ["Handheld K", 0.05, -0.08], ["Bike Mount", -0.02, 0.05], ["Office Desk", 0.0, -0.01], ["Car Node 2", 0.14, 0.06]]) add(n, 1, a, b);
  add("Mesh Chat Room", 3, 0.04, 0.03); add("Weather Mast", 4, 0.09, 0.19); add("River Gauge", 4, -0.04, -0.02);
  add("Bay Gateway", 2, -0.2, -1.1, { hops: 4 });
  { const k = bytes(32); nodes.push({ name: "Unknown solar", type: 2, key: k, k: hex(k), lat: undefined, last: t0 - 9000 }); }
  const reps = nodes.filter((n) => n.type === 2 && n.lat !== undefined);
  const observers = ["Midtown Roof", "Natomas Relay", "Rancho Hill", "Elk Grove South"].map((n) => nodes.find((x) => x.name === n));
  const myKey = bytes(32); myKey[0] = 0x0c;
  const chans = [{ i: 0, n: "Public", k: "8b3387e9c5cdea6ac9e5edbaa115cd72" }, { i: 1, n: "#sacmesh" }, { i: 2, n: "#emcomm" }, { i: 3, n: "Family" }];
  for (const c of chans.slice(1, 3)) c.k = hex(SHA.sha256(new TextEncoder().encode(c.n)).subarray(0, 16));
  const lines = {
    0: ["Good morning mesh! Signal check from the foothills", "Copy, hearing you 3 hops out", "Anyone near the lake? Testing a new repeater", "Loud and clear here, -92 dBm",
      "New antenna is up on the roof, how am I getting out?", "Heard 14 repeaters overnight", "First time on the mesh, hello everyone", "Nice, that came through direct"],
    1: ["Meetup Saturday at the park, bring your nodes", "Who runs the Davis link? It has been rock solid", "Map update: three new repeaters this week", "Flashing the new firmware tonight",
      "Solar repeater survived the storm", "Anyone have a spare 915 MHz antenna?"],
    2: ["Net check-in at 19:00 on this channel", "Weather mast reading 12 mph gusts", "Practice message, please acknowledge", "Check-in: Fair Oaks, all good"],
  };
  const keys = channelKeys(chans);
  // ---- build real packets
  function pathTo(from, to) { // repeaters roughly along the line, nearest first
    const d = distKm(from, to), out = [];
    for (const r of reps) { if (r === from || r === to) continue; const a = distKm(from, r), b = distKm(r, to); if (a + b < d * 1.25 && rnd() < 0.7) out.push([a, r]); }
    return out.sort((x, y) => x[0] - y[0]).slice(0, 4).map((x) => x[1]);
  }
  function frame(typeNum, flood, path, payload) {
    const b = [(typeNum << 2) | (flood ? 1 : 2), path.length & 63, ...path.map((r) => r.key[0]), ...payload];
    return new Uint8Array(b);
  }
  function advert(n, path) {
    const app = [n.type | 0x80 | (n.lat !== undefined ? 0x10 : 0)];
    if (n.lat !== undefined) { const dv = new DataView(new ArrayBuffer(8)); dv.setInt32(0, Math.round(n.lat * 1e6), true); dv.setInt32(4, Math.round(n.lon * 1e6), true); app.push(...new Uint8Array(dv.buffer)); }
    app.push(...new TextEncoder().encode(n.name));
    const ts = new Uint8Array(4); new DataView(ts.buffer).setUint32(0, nowS(), true);
    return frame(4, true, path, [...n.key, ...ts, ...bytes(64), ...app]);
  }
  function group(chIdx, from, text, path) {
    const ch = keys.find((k) => k.i === chIdx), msg = new TextEncoder().encode(from + ": " + text);
    const pt = new Uint8Array(Math.ceil((5 + msg.length) / 16) * 16); new DataView(pt.buffer).setUint32(0, nowS(), true); pt.set(msg, 5);
    const ct = AES.encryptECB(ch.key, pt), mac = SHA.hmac(ch.secret, ct);
    return frame(5, true, path, [ch.hash, mac[0], mac[1], ...ct]);
  }
  const other = (path) => { const t = pick([2, 2, 3, 3, 3, 8, 8, 9, 0, 1]); return frame(t, t !== 3 && t !== 1, t === 3 ? [] : path, [...bytes(4), ...bytes(t === 3 ? 0 : 16 + Math.floor(rnd() * 3) * 16)]); };
  function randomPacket(to) {
    const from = pick(nodes.filter((n) => n.lat !== undefined && n !== to)), path = pathTo(from, to), r = rnd();
    if (r < 0.25) return { raw: advert(from, path), from };
    if (r < 0.5) { const c = pick([0, 0, 0, 1, 1, 2]); return { raw: group(c, from.name, pick(lines[c]), path), from, chat: c }; }
    return { raw: other(path), from };
  }
  // ---- what the node reports
  const mePos = { lat: C.lat + 0.005, lon: C.lon + 0.012 };
  const rf = [], msgs = [];
  let seq = 1, mseq = 1, rx = 812, tx = 64;
  function heardByRadio(raw, from, chat, t) {
    const p = decodePacket(raw), hops = p.path.length, rssi = Math.round(-70 - hops * 9 - rnd() * 18), snr = Math.round((10 - hops * 3.2 - rnd() * 5) * 4) / 4;
    rf.push({ s: seq++, t, id: p.id, ty: p.type, fl: p.flood, h: hops, hs: 1, p: p.path, rssi, snr, len: raw.length, f: from && (p.type === "ADVERT" || chat !== undefined) ? from.name : "", k: p.type === "ADVERT" ? from.k.slice(0, 12) : undefined, lat: p.lat, lon: p.lon, nt: p.nodeType });
    if (rf.length > 48) rf.shift();
    if (chat !== undefined) { const m = decryptGroup(p, keys); if (m) { msgs.push({ s: mseq++, t: m.t, c: m.ch, f: m.from, x: m.text, h: hops, snr }); if (msgs.length > 120) msgs.shift(); } }
    rx++;
  }
  // a past hour of traffic, so the charts have something in them
  for (let i = 60; i > 0; i--) { const { raw, from, chat } = randomPacket({ lat: mePos.lat, lon: mePos.lon }); heardByRadio(raw, from, chat, t0 - i * 55); }
  const old = [["Midtown Roof", 0, "Morning all, the roof repeater is back up"], ["Trail Pack", 0, "Heading up the trail, will check in from the top"], ["Kayak Kit", 1, "On the water today, signal is great"], ["Weather Mast", 2, "Gusts 14 mph, temp 54 F"]];
  old.forEach(([f, c, x], i) => msgs.push({ s: mseq++, t: t0 - 5400 + i * 900, c, f, x, h: 1 + (i % 3), snr: 6 - i }));
  msgs.push({ s: mseq++, t: t0 - 1200, c: 0, f: "Demo T-Deck", x: "Thanks all, hearing everyone fine from downtown", me: true });
  const hist = { step: 300, batt: [], rx: [], tx: [], noise: [], rssi: [], heap: [], lvgl: [], nodes: [] };
  for (let i = 0; i < 288; i++) {
    const hr = ((t0 / 3600 - (287 - i) / 12) % 24 + 24) % 24, busy = 0.55 + 0.45 * Math.sin(((hr - 8) / 24) * Math.PI * 2);
    hist.batt.push(Math.round(4120 - i * 0.55 + Math.sin(i / 9) * 6 + (i > 200 ? (i - 200) * 0.9 : 0)));
    hist.rx.push(Math.round(14 + busy * 30 + rnd() * 9)); hist.tx.push(Math.round(1 + rnd() * 3 + busy * 2));
    hist.noise.push(Math.round(-114 + busy * 4 + rnd() * 3)); hist.rssi.push(Math.round(-101 + busy * 3 + rnd() * 5));
    hist.heap.push(Math.round(148 - rnd() * 3 - (i % 40 === 0 ? 8 : 0))); hist.lvgl.push(Math.round(3 + rnd() * 4 + busy * 3)); hist.nodes.push(Math.round(14 + busy * 8));
  }
  const perMin = Array.from({ length: 30 }, () => Math.round(3 + rnd() * 6));
  function snapshot() {
    const now = nowS();
    return {
      v: 1, now, up: 3 * 86400 + 7 * 3600 + (now - t0),
      node: { name: "Demo T-Deck", key: hex(myKey).slice(0, 12), fw: "Hoptics-MC 0.2.0-beta", board: "LilyGO T-Deck Plus", lat: mePos.lat, lon: mePos.lon, posSrc: "gps",
        batt: { mv: hist.batt[287], pct: 78 }, cpu: 240, heap: 148200, heapTotal: 327680, psram: 7690000, psramTotal: 8388608, rssi: -61, gps: { on: true, sats: 9, fix: true } },
      radio: { freq: 910.525, bw: 62.5, sf: 7, cr: 5, tx: 22 },
      stats: { rx, tx, flood: Math.round(rx * 0.8), direct: Math.round(rx * 0.2), dups: Math.round(rx * 0.6), airTxS: 214 + Math.floor((now - t0) / 40), noise: -113, lastRssi: rf.length ? rf[rf.length - 1].rssi : -95, lastSnr: rf.length ? rf[rf.length - 1].snr : 4 },
      load: { loop: 9800, longest: 18, ui: 1, lvgl: 6, draw: 4, feed: 0 },
      feed: { url: "wss://demo.invalid/ws", device: false },
      hist: Object.assign({ end: now }, hist), perMin,
      contacts: nodes.filter((n) => n.type).map((n) => ({ k: n.k.slice(0, 12), n: n.name, t: n.type, h: n.hops, last: n.last, lat: n.lat, lon: n.lon, fav: n.name === "Midtown Roof" })),
      channels: chans.map((c) => ({ i: c.i, n: c.n, k: c.k })), packets: rf.slice(), msgs: msgs.slice(),
    };
  }
  let lastTick = Date.now();
  window.HX_DEMO = {
    autologin: false,
    api: {
      async login(pin) { await new Promise((r) => setTimeout(r, 500)); this.ok = /^\d{6}$/.test(pin); return this.ok ? { ok: true } : { ok: false, error: "Enter all six digits" }; },
      async state() {
        if (!this.ok) return null;
        // new packets since the last look: about one every 3 s
        const now = Date.now();
        while (now - lastTick > 3000) {
          lastTick += 3000;
          const { raw, from, chat } = randomPacket(mePos); heardByRadio(raw, from, chat, nowS());
          perMin[29]++; if (rnd() < 0.05) perMin.push(perMin.shift() * 0 + 1);
          for (const n of nodes) if (rnd() < 0.08) n.last = nowS() - Math.floor(rnd() * 60);
        }
        return JSON.parse(JSON.stringify(snapshot()));
      },
      async logout() { this.ok = false; },
      async log() {
        return ["===== Hoptics-MC log: this session =====", "[0] hoptics: starting (reset: power on)", "[1] hoptics: display init (LilyGO T-Deck Plus)", "[2] hoptics: start 8/8 running (3412 ms)",
          "[3] hoptics: wifi connected, -61 dBm, heap 150112", "[4] hoptics: web page on, port 80", "[300] hoptics: ui running, up 300s, heap 148200, psram 7690000, batt 4120 mV, cpu 240 MHz, loop 9800/s, longest 18 ms"].join("\n");
      },
    },
    feed(cb) {
      const iv = setInterval(() => {
        const ob = pick(observers), { raw, from } = randomPacket(ob), copies = 1 + Math.floor(rnd() * 3);
        const others = [ob, ...observers.filter((o) => o !== ob).sort(() => rnd() - 0.5)].slice(0, copies);
        others.forEach((o, i) => setTimeout(() => cb({ type: "packet", data: { raw_hex: hex(raw), observer_id: o.k, observer_name: o.name, snr: Math.round((8 - rnd() * 14) * 4) / 4 } }), i * 350));
        void from;
      }, 1100);
      return { stop: () => clearInterval(iv) };
    },
    terrain(N) { // a valley, a ridge part way, then foothills
      const out = [];
      for (let i = 0; i < N; i++) {
        const f = i / (N - 1);
        out.push(18 + 40 * f + 210 * Math.exp(-(((f - 0.58) / 0.07) ** 2)) + 260 * Math.max(0, f - 0.8) ** 1.4 * 4 + 9 * Math.sin(f * 37) + 5 * Math.sin(f * 91));
      }
      return out;
    },
  };
})();
