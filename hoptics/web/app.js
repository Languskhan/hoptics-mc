/* Hoptics-MC web dashboard: data, live feed, views. MIT licence. Needs core.js first. */
"use strict";
const DEMO = window.HX_DEMO || null;
const store = (k, v) => { try { if (v === undefined) return localStorage.getItem("hx_" + k); localStorage.setItem("hx_" + k, v); } catch (e) { return null; } };

// ------------------------------------------------------------------ talking to the node
const API = DEMO ? DEMO.api : {
  async login(pin) {
    const r = await fetch("/api/login", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ pin }) });
    return r.ok ? { ok: true } : Object.assign({ ok: false }, await r.json().catch(() => ({ error: "No answer (" + r.status + ")" })));
  },
  async state() { // only what the page doesn't have yet: contacts version, last packet and message, history end
    const r = await fetch(`/api/state?cv=${S.cv}&ps=${S.rfSeq}&ms=${S.msgSeq}&hv=${S.histEnd}`, { cache: "no-store" });
    if (r.status === 401) return null;
    if (!r.ok) throw new Error("HTTP " + r.status);
    return r.json();
  },
  async logout() { await fetch("/api/logout", { method: "POST" }).catch(() => 0); },
  async log() { const r = await fetch("/api/log", { cache: "no-store" }); if (!r.ok) throw new Error("HTTP " + r.status); return r.text(); },
  async tile(z, x, y, blob) { // one tile of a tile pack; retried while the node is busy
    for (let i = 0; ; i++) {
      const r = await fetch(`/api/tile?z=${z}&x=${x}&y=${y}`, { method: "POST", headers: { "Content-Type": "image/png", "X-Hoptics": "1" }, body: blob });
      if (r.ok) return { ok: true };
      const j = await r.json().catch(() => ({}));
      if (r.status !== 503 || i >= 4) return { ok: false, full: r.status === 507, error: j.error || "HTTP " + r.status };
      await new Promise((ok) => setTimeout(ok, 600));
    }
  },
  async send(c, text) {
    const r = await fetch("/api/send", { method: "POST", headers: { "Content-Type": "application/json", "X-Hoptics": "1" }, body: JSON.stringify({ c, text }) });
    const j = await r.json().catch(() => ({}));
    return r.ok ? { ok: true } : { ok: false, error: j.error || "No answer (" + r.status + ")" };
  },
  async clearTiles() { const r = await fetch("/api/tiles/clear", { method: "POST", headers: { "X-Hoptics": "1" } }); return r.ok; },
  update(file, progress) { // XHR: fetch has no upload progress
    return new Promise((done) => {
      const x = new XMLHttpRequest();
      x.open("POST", "/api/update");
      x.setRequestHeader("Content-Type", "application/octet-stream");
      x.setRequestHeader("X-Hoptics", "1");
      x.upload.onprogress = (e) => e.lengthComputable && progress(e.loaded / e.total);
      x.onload = () => { let j = {}; try { j = JSON.parse(x.responseText); } catch (e) { } done(x.status === 200 ? { ok: true } : { ok: false, error: j.error || "No answer (" + x.status + ")" }); };
      x.onerror = () => done({ ok: false, error: "The connection dropped" });
      x.send(file);
    });
  },
};

// ------------------------------------------------------------------ what the page knows
const S = {
  snap: null, chKeys: [], chKeySig: "", cv: 0, histEnd: 0, hist: null, lastUp: 0,
  packets: [], seenPk: new Map(), // newest first; seen: packet id -> entry (merges the internet's copies)
  rfSeq: 0, msgSeq: 0,
  msgs: [], seenMsg: new Set(),
  nodes: new Map(), // 12-hex key -> {key, name, type, lat, lon, last, hops, contact, fav}
  observers: new Map(), // observer name -> {key, n}
  netMin: new Map(), rfMin: new Map(), // unix minute -> count
  unread: new Map(), feedState: "off", feedCount: 0,
  beams: [], paused: false,
};
const me = () => (S.snap && S.snap.node) || {};
const myPos = () => { const n = me(); return n.lat !== undefined && n.lat !== null ? { lat: n.lat, lon: n.lon } : null; };
function nodeFor(prefix) { // a path hash (1-3 bytes) or a key prefix: the best known node it can be
  if (!prefix) return null;
  let best = null;
  for (const n of S.nodes.values()) if (n.key.startsWith(prefix)) { if (!best || (n.type === 2 && best.type !== 2) || (n.lat !== undefined && best.lat === undefined) || (n.last || 0) > (best.last || 0)) best = n; }
  return best;
}
function upsertNode(key, patch) {
  key = key.slice(0, 12);
  let n = S.nodes.get(key);
  if (!n) { n = { key, name: "", type: 0 }; S.nodes.set(key, n); }
  for (const k in patch) if (patch[k] !== undefined && patch[k] !== null && patch[k] !== "") n[k] = patch[k];
  return n;
}

function bump(map, t) { const m = Math.floor(t / 60); map.set(m, (map.get(m) || 0) + 1); if (map.size > 180) map.delete(Math.min(...map.keys())); }

function addPacket(p, src, extra) {
  // the same packet from the radio and from the internet is one entry: the internet's copies add observers
  const old = S.seenPk.get(p.id);
  if (old) {
    if (src === "net" && extra.obs && !old.obs.has(extra.obs)) { old.obs.add(extra.obs); old.fresh = Date.now(); }
    if (src === "rf" && old.src === "net") { old.src = "both"; Object.assign(old, extra); }
    return old;
  }
  const { obs: oname, ...rest } = extra;
  const e = Object.assign({ src, fresh: Date.now() }, p, rest);
  e.obs = new Set(oname ? [oname] : []);
  S.packets.unshift(e);
  S.seenPk.set(p.id, e);
  if (S.packets.length > 600) S.seenPk.delete(S.packets.pop().id);
  bump(src === "rf" ? S.rfMin : S.netMin, e.t);
  if (p.type === "ADVERT" && p.key) upsertNode(p.key, { name: p.name, type: p.nodeType, lat: p.lat, lon: p.lon, last: e.t, advert: true });
  queueBeam(e);
  return e;
}
function addMsg(m) {
  const id = m.c + "|" + m.f + "|" + m.x + "|" + Math.round(m.t / 30);
  if (S.seenMsg.has(id)) return;
  S.seenMsg.add(id);
  S.msgs.push(m);
  if (S.msgs.length > 800) S.msgs.shift();
  if (!(UI.view === "messages" && UI.chan === m.c) && !m.old) S.unread.set(m.c, (S.unread.get(m.c) || 0) + 1);
}

// a fresh snapshot from the node
function takeSnapshot(d) {
  if (d.up < S.lastUp) { S.cv = 0; S.rfSeq = 0; S.msgSeq = 0; S.histEnd = 0; } // the node restarted: its numbers did too
  S.lastUp = d.up || 0;
  if (d.hw) Object.assign(d.node, d.hw);
  if (d.cv !== undefined) S.cv = d.cv;
  if (d.hist) { S.hist = d.hist; S.histEnd = d.hist.end; }
  d.hist = S.hist || {};
  S.snap = d;
  const sig = (d.channels || []).map((c) => c.i + c.k).join(",");
  if (sig !== S.chKeySig) { S.chKeySig = sig; S.chKeys = channelKeys(d.channels || []); }
  for (const c of d.contacts || []) upsertNode(c.k, { name: c.n, type: c.t, hops: c.h, last: c.last, lat: c.lat, lon: c.lon, fav: c.fav, contact: true });
  const first = !S.rfSeq;
  for (const r of (d.packets || []).slice().sort((a, b) => a.s - b.s)) {
    if (r.s <= S.rfSeq) continue;
    S.rfSeq = r.s;
    addPacket({ id: r.id || "rf" + r.s, type: r.ty, flood: r.fl, path: r.p || [], hashSize: r.hs || 1, len: r.len, name: r.f, key: r.k, lat: r.lat, lon: r.lon, nodeType: r.nt },
      "rf", { t: r.t, rssi: r.rssi, snr: r.snr, mine: r.me, from: r.f, quiet: first });
  }
  for (const m of (d.msgs || []).slice().sort((a, b) => a.s - b.s)) {
    if (m.s <= S.msgSeq) continue;
    S.msgSeq = m.s;
    addMsg({ c: m.c, f: m.f, x: m.x, t: m.t, h: m.h, snr: m.snr, me: m.me, net: m.net, old: first });
  }
}

// ------------------------------------------------------------------ the live feed, straight from the browser
const Feed = {
  ws: null, url: "", backoff: 3000, timer: 0, sim: null, fails: 0, opened: false,
  want() { return store("feed") !== "0"; },
  start(url) {
    if (!this.want() || !url) { this.stop(); return; }
    if (this.url === url) return; // already on it: connected, waiting to retry, or refused
    this.stop(); this.url = url;
    if (DEMO) { this.sim = DEMO.feed((m) => this.onMsg(m)); S.feedState = "live"; return; }
    this.connect();
  },
  connect() {
    const url = this.url;
    let u = url.replace(/^https?:/, (m) => (m === "https:" ? "wss:" : "ws:"));
    if (!/^wss?:\/\//.test(u)) u = "wss://" + u;
    if (!/\/[^/]/.test(u.replace(/^wss?:\/\//, ""))) u = u.replace(/\/?$/, "/ws");
    S.feedState = "connecting"; UI.chips();
    try { this.ws = new WebSocket(u); } catch (e) { this.retry(); return; }
    this.ws.onopen = () => { S.feedState = "live"; this.backoff = 3000; this.fails = 0; this.opened = true; UI.chips(); };
    this.ws.onmessage = (e) => { try { this.onMsg(JSON.parse(e.data)); } catch (x) {} };
    this.ws.onclose = () => { this.ws = null; if (this.url) this.retry(); };
    this.ws.onerror = () => { S.feedState = "failed"; UI.chips(); };
  },
  retry() {
    // a server that never once let this browser in (CoreScope turns away pages from other sites) is left alone,
    // instead of the chip flicking between Connecting and Internet down every few seconds
    if (++this.fails >= 3 && !this.opened) { S.feedState = "refused"; this.timer = 0; UI.chips(); return; }
    S.feedState = "failed"; UI.chips(); clearTimeout(this.timer);
    this.timer = setTimeout(() => { this.timer = 0; if (this.url) this.connect(); }, this.backoff); this.backoff = Math.min(this.backoff * 2, 120000);
  },
  stop() { this.url = ""; clearTimeout(this.timer); this.timer = 0; this.fails = 0; this.opened = false; this.backoff = 3000; if (this.ws) { this.ws.onclose = null; this.ws.close(); } this.ws = null; if (this.sim) this.sim.stop(); this.sim = null; S.feedState = "off"; },
  onMsg(j) {
    const pick = (o, k, d = 0) => { if (!o || typeof o !== "object" || d > 4) return undefined; if (k in o) return o[k]; for (const v of Object.values(o)) { const r = pick(v, k, d + 1); if (r !== undefined) return r; } };
    const raw = pick(j, "raw_hex");
    if (typeof raw !== "string" || raw.length < 4) return;
    const p = decodePacket(unhex(raw));
    if (!p) return;
    const obs = String(pick(j, "observer_name") || "").slice(0, 32), oid = String(pick(j, "observer_id") || "");
    if (obs && oid.length >= 12) { S.observers.set(obs, { key: oid.slice(0, 12).toLowerCase() }); upsertNode(oid, { observer: true }); }
    const snr = pick(j, "snr");
    S.feedCount++;
    const e = addPacket(p, "net", { t: nowS(), obs, snr: typeof snr === "number" ? snr : null });
    if (p.ct && e.obs.size === 1) {
      const m = decryptGroup(p, S.chKeys);
      if (m) addMsg({ c: m.ch, f: m.from, x: m.text, t: m.t || nowS(), net: true });
    }
    UI.soon();
  },
};

// ------------------------------------------------------------------ the map (OpenStreetMap tiles, canvas on top)
class SlippyMap {
  constructor(host, o) {
    this.o = Object.assign({ lat: 38.6, lon: -121.3, z: 10 }, o);
    this.host = host; host.classList.add("mapwrap"); host.replaceChildren();
    this.tl = el("div", "tiles"); this.cv = el("canvas"); host.append(this.tl, this.cv);
    this.att = el("div", "attrib"); this.att.append("© ", Object.assign(el("a", "", "OpenStreetMap"), { href: "https://www.openstreetmap.org/copyright", target: "_blank", rel: "noopener" }), " contributors");
    host.append(this.att);
    this.lat = this.o.lat; this.lon = this.o.lon; this.z = this.o.z; this.imgs = new Map();
    this.ptrs = new Map(); this.moved = false; this.anim = 0;
    host.addEventListener("pointerdown", (e) => { host.setPointerCapture(e.pointerId); this.ptrs.set(e.pointerId, [e.clientX, e.clientY]); this.moved = false; this.pinch = null; });
    host.addEventListener("pointermove", (e) => {
      if (!this.ptrs.has(e.pointerId)) { if (this.o.onHover) { const r = host.getBoundingClientRect(); this.o.onHover(e.clientX - r.left, e.clientY - r.top); } return; }
      const prev = this.ptrs.get(e.pointerId); this.ptrs.set(e.pointerId, [e.clientX, e.clientY]);
      if (this.ptrs.size === 2) {
        const [a, b] = [...this.ptrs.values()], d = Math.hypot(a[0] - b[0], a[1] - b[1]);
        if (!this.pinch) this.pinch = d;
        else if (d / this.pinch > 1.6 || d / this.pinch < 0.62) { const r = host.getBoundingClientRect(); this.zoomAt(d > this.pinch ? 1 : -1, (a[0] + b[0]) / 2 - r.left, (a[1] + b[1]) / 2 - r.top); this.pinch = d; }
        this.moved = true; return;
      }
      const dx = e.clientX - prev[0], dy = e.clientY - prev[1];
      if (Math.abs(dx) + Math.abs(dy) > 0) { if (Math.hypot(dx, dy) > 1) this.moved = true; this.panBy(-dx, -dy); }
    });
    const up = (e) => {
      if (this.ptrs.size === 1 && !this.moved && this.o.onClick) { const r = host.getBoundingClientRect(); this.o.onClick(e.clientX - r.left, e.clientY - r.top); }
      this.ptrs.delete(e.pointerId);
    };
    host.addEventListener("pointerup", up); host.addEventListener("pointercancel", (e) => this.ptrs.delete(e.pointerId));
    host.addEventListener("wheel", (e) => { e.preventDefault(); const r = host.getBoundingClientRect(); this.zoomAt(e.deltaY < 0 ? 1 : -1, e.clientX - r.left, e.clientY - r.top); }, { passive: false });
    host.addEventListener("dblclick", (e) => { const r = host.getBoundingClientRect(); this.zoomAt(1, e.clientX - r.left, e.clientY - r.top); });
    new ResizeObserver(() => this.render()).observe(host);
  }
  world(lat, lon, z = this.z) {
    const n = 256 * 2 ** z, s = Math.sin(clamp(lat, -85, 85) * Math.PI / 180);
    return [((lon + 180) / 360) * n, (0.5 - Math.log((1 + s) / (1 - s)) / (4 * Math.PI)) * n];
  }
  unworld(x, y, z = this.z) {
    const n = 256 * 2 ** z, lon = (x / n) * 360 - 180, k = Math.PI - (2 * Math.PI * y) / n;
    return { lat: (180 / Math.PI) * Math.atan(Math.sinh(k)), lon };
  }
  size() { const r = this.host.getBoundingClientRect(); return [r.width, r.height]; }
  xy(lat, lon) { const [w, h] = this.size(), [cx, cy] = this.world(this.lat, this.lon), [x, y] = this.world(lat, lon); return [x - cx + w / 2, y - cy + h / 2]; }
  ll(x, y) { const [w, h] = this.size(), [cx, cy] = this.world(this.lat, this.lon); return this.unworld(cx + x - w / 2, cy + y - h / 2); }
  panBy(dx, dy) { const [cx, cy] = this.world(this.lat, this.lon), p = this.unworld(cx + dx, cy + dy); this.lat = p.lat; this.lon = p.lon; this.render(); }
  zoomAt(dz, x, y) {
    const z = clamp(this.z + dz, 3, 17); if (z === this.z) return;
    const p = this.ll(x, y); this.z = z;
    const [w, h] = this.size(), [px, py] = this.world(p.lat, p.lon), c = this.unworld(px - x + w / 2, py - y + h / 2);
    this.lat = c.lat; this.lon = c.lon; this.render();
  }
  view(lat, lon, z) { this.lat = lat; this.lon = lon; if (z) this.z = z; this.render(); }
  fit(pts, maxZ = 14) {
    if (!pts.length) return;
    const [w, h] = this.size();
    let z = maxZ;
    for (; z > 3; z--) {
      const xy = pts.map((p) => this.world(p.lat, p.lon, z)), xs = xy.map((p) => p[0]), ys = xy.map((p) => p[1]);
      if (Math.max(...xs) - Math.min(...xs) < w - 80 && Math.max(...ys) - Math.min(...ys) < h - 80) break;
    }
    const xy = pts.map((p) => this.world(p.lat, p.lon, z)), cx = (Math.min(...xy.map((p) => p[0])) + Math.max(...xy.map((p) => p[0]))) / 2, cy = (Math.min(...xy.map((p) => p[1])) + Math.max(...xy.map((p) => p[1]))) / 2;
    const c = this.unworld(cx, cy, z); this.view(c.lat, c.lon, z);
  }
  render() {
    if (this.raf) return;
    this.raf = requestAnimationFrame(() => { this.raf = 0; this.paint(); });
  }
  paint() {
    const [w, h] = this.size(); if (!w) return;
    const [cx, cy] = this.world(this.lat, this.lon), z = this.z, n = 2 ** z;
    const x0 = Math.floor((cx - w / 2) / 256), x1 = Math.floor((cx + w / 2) / 256), y0 = Math.max(0, Math.floor((cy - h / 2) / 256)), y1 = Math.min(n - 1, Math.floor((cy + h / 2) / 256));
    const want = new Set();
    for (let ty = y0; ty <= y1; ty++) for (let tx = x0; tx <= x1; tx++) {
      const wx = ((tx % n) + n) % n, k = `${z}/${tx}/${ty}`;
      want.add(k);
      let im = this.imgs.get(k);
      if (!im) {
        im = new Image(); im.decoding = "async"; im.alt = ""; im.referrerPolicy = "strict-origin-when-cross-origin";
        im.onload = () => im.classList.add("ok");
        im.src = `https://tile.openstreetmap.org/${z}/${wx}/${ty}.png`;
        this.imgs.set(k, im); this.tl.append(im);
      }
      im.style.transform = `translate(${Math.round(tx * 256 - cx + w / 2)}px,${Math.round(ty * 256 - cy + h / 2)}px)`;
    }
    for (const [k, im] of this.imgs) if (!want.has(k)) { im.remove(); this.imgs.delete(k); }
    this.overlay();
  }
  overlay() { // only the canvas: markers and beams move, the tiles don't
    const [w, h] = this.size(); if (!w) return;
    const [g] = fitCanvas(this.cv, h);
    g.clearRect(0, 0, w, h);
    if (this.o.onDraw) this.o.onDraw(g, this, w, h);
  }
}

// ------------------------------------------------------------------ beams: a packet's hops drawn as light along the path
function queueBeam(e) {
  if (e.quiet) return;
  S.beams.push({ e, t0: performance.now() });
  if (S.beams.length > 60) S.beams.shift();
  if (UI.view === "map" && UI.map) UI.map.render();
}
function beamPoints(e) {
  const pts = [];
  if (e.type === "ADVERT" && e.lat !== undefined) pts.push({ lat: e.lat, lon: e.lon });
  for (const h of e.path || []) { const n = nodeFor(h); if (n && n.lat !== undefined) pts.push(n); }
  let end = null;
  if (e.src !== "net") end = myPos();
  else if (e.obs.size) { const o = S.observers.get([...e.obs][0]); const n = o && nodeFor(o.key); if (n && n.lat !== undefined) end = n; }
  if (end) pts.push(end);
  return pts.filter((p, i) => !i || p.lat !== pts[i - 1].lat || p.lon !== pts[i - 1].lon);
}

// ------------------------------------------------------------------ the page
const ICON = {
  overview: "M3 13h8V3H3zm0 8h8v-6H3zm10 0h8V11h-8zm0-18v6h8V3z",
  live: "M2 12h3l3-8 4 16 3-8h7",
  map: "M9 4 3 6v14l6-2 6 2 6-2V4l-6 2zm0 0v14m6-12v14",
  messages: "M4 4h16v12H8l-4 4z",
  los: "M3 20 9 9l4 6 3-4 5 9zM16 4l5 3",
  nodes: "M12 3a3 3 0 1 1 0 6 3 3 0 0 1 0-6zM5 15a3 3 0 1 1 0 6 3 3 0 0 1 0-6zm14 0a3 3 0 1 1 0 6 3 3 0 0 1 0-6zM12 9v3m0 0-5 4m5-4 5 4",
  device: "M7 3h10v18H7zM10 18h4",
};
const VIEWS = [["overview", "Overview"], ["live", "Live"], ["map", "Map"], ["messages", "Messages"], ["los", "Line of sight"], ["nodes", "Nodes"], ["device", "Device"]];
function svgIcon(d) {
  const s = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  s.setAttribute("viewBox", "0 0 24 24"); s.setAttribute("fill", "none"); s.setAttribute("stroke", "currentColor"); s.setAttribute("stroke-width", "1.8"); s.setAttribute("stroke-linecap", "round"); s.setAttribute("stroke-linejoin", "round");
  const p = document.createElementNS("http://www.w3.org/2000/svg", "path"); p.setAttribute("d", d); s.append(p); return s;
}
function toast(t) { const e = $("toast"); e.textContent = t; e.classList.add("on"); clearTimeout(toast.t); toast.t = setTimeout(() => e.classList.remove("on"), 2600); }

const UI = {
  view: "overview", chan: 0, built: new Set(), soonT: 0, map: null,
  soon() { if (this.soonT) return; this.soonT = setTimeout(() => { this.soonT = 0; this.render(); }, 400); },
  go(v) {
    if (!VIEWS.some((x) => x[0] === v)) v = "overview";
    this.view = v;
    document.querySelectorAll("nav button").forEach((b) => b.classList.toggle("on", b.dataset.v === v));
    document.querySelectorAll(".view").forEach((s) => s.classList.toggle("on", s.id === "v-" + v));
    if (location.hash !== "#" + v) history.replaceState(null, "", "#" + v);
    if (!this.built.has(v)) { this.built.add(v); V[v].build(); }
    if (v === "messages") S.unread.delete(this.chan);
    this.render();
    if (v === "map" && this.map) this.map.render();
  },
  render() { if (!S.snap) return; this.chips(); this.badges(); const v = V[this.view]; if (v && this.built.has(this.view)) v.render(); },
  chips() {
    const n = me(), c = $("chips"); if (!c) return;
    c.replaceChildren();
    const chip = (cls, text, opt) => { const e = el("span", "chip " + cls + (opt ? " opt" : "")); e.append(el("i"), text); c.append(e); };
    if (n.batt) chip(n.batt.pct >= 101 ? "ok" : n.batt.pct > 30 ? "ok" : n.batt.pct > 15 ? "warn" : "bad", n.batt.pct >= 101 ? "USB power" : (n.batt.pct >= 0 ? n.batt.pct + "% " : "") + (n.batt.mv / 1000).toFixed(2) + " V");
    chip(S.feedState === "live" ? "ok" : S.feedState === "off" ? "" : S.feedState === "connecting" ? "warn" : "bad", S.feedState === "live" ? "Internet live" : S.feedState === "off" ? "Internet off" : S.feedState === "connecting" ? "Connecting" : S.feedState === "refused" ? "Internet refused" : "Internet down", true);
    if (n.rssi) chip(n.rssi > -67 ? "ok" : n.rssi > -78 ? "warn" : "bad", "Wi-Fi " + n.rssi + " dBm", true);
    $("hdrName").textContent = n.name || "Hoptics-MC";
    const r = S.snap.radio || {};
    $("hdrSub").textContent = [n.board, r.freq ? r.freq.toFixed(3) + " MHz" : "", r.sf ? "SF" + r.sf + " BW" + r.bw : ""].filter(Boolean).join("  ·  ");
  },
  badges() {
    const total = [...S.unread.values()].reduce((a, b) => a + b, 0), b = $("msgBadge");
    if (b) { b.textContent = total; b.classList.toggle("hidden", !total); }
  },
};

// ------------------------------------------------------------------ views
const V = {};
const card = (title, cls, extra) => { const c = el("section", "card " + (cls || "")); const h = el("h3"); h.append(title); if (extra) { h.append(el("span", "grow")); h.append(extra); } c.append(h); return c; };
function seg(opts, cur, on) { const s = el("div", "seg"); for (const [v, t] of opts) { const b = el("button", v === cur ? "on" : "", t); b.onclick = () => { s.querySelectorAll("button").forEach((x) => x.classList.toggle("on", x === b)); on(v); }; s.append(b); } return s; }
const lastHour = () => S.packets.filter((p) => nowS() - p.t < 3600);

V.overview = {
  range: "30m",
  build() {
    const v = $("v-overview");
    const tiles = el("div", "tiles"); tiles.id = "ovTiles";
    const T = (id, k, c) => { const t = el("div", "tile"); t.style.setProperty("--c", css(c)); t.append(el("div", "k", k), Object.assign(el("div", "v", "-"), { id: id + "V" }), Object.assign(el("div", "s", ""), { id: id + "S" }), Object.assign(el("canvas"), { id: id + "C" })); tiles.append(t); };
    T("tNodes", "Nodes heard", "--cyan"); T("tRate", "Packets / min", "--green"); T("tNoise", "Noise floor", "--violet"); T("tBatt", "Battery", "--amber"); T("tAir", "Airtime sent", "--pink");
    const grid = el("div", "grid"); grid.style.marginTop = "14px";
    const tr = card("Traffic", "c8", seg([["30m", "30 min"], ["24h", "24 h"]], this.range, (r) => { this.range = r; this.render(); }));
    this.traffic = new Chart(tr.appendChild(el("div")), { height: 220, type: "bar", stacked: true, unit: "" });
    const mix = card("Packet types", "c4", el("span", "dim", "last hour"));
    this.mix = new Donut(mix.appendChild(el("div")), { size: 160, label: "packets" });
    const sig = card("Signal of packets this radio heard", "c6");
    this.sig = new Chart(sig.appendChild(el("div")), { height: 230, type: "scatter", xTime: false, unit: " dB", xFmt: (x) => Math.round(x) + " dBm", yFmt: (y) => fmtNum(y), legend: true,
      tipRows: (p) => [["RSSI", Math.round(p[0]) + " dBm"], ["SNR", p[1].toFixed(1) + " dB"]] });
    const hops = card("Hops travelled", "c3");
    this.hops = new Chart(hops.appendChild(el("div")), { height: 230, type: "bar", xTime: false, unit: " packets", xFmt: (x) => (x >= 8 ? "8+" : x === 0 ? "direct" : x + " hop" + (x > 1 ? "s" : "")), legend: false });
    const busy = card("Busiest repeaters", "c3", el("span", "dim", "last hour"));
    this.busy = busy.appendChild(el("div"));
    const noise = card("Noise floor and signal, 24 h", "c8");
    this.noise = new Chart(noise.appendChild(el("div")), { height: 200, type: "line", unit: " dBm" });
    const rm = card("Latest messages", "c4", Object.assign(el("button", "btn sm", "Open"), { onclick: () => UI.go("messages") }));
    this.recent = rm.appendChild(el("div"));
    grid.append(tr, mix, sig, hops, busy, noise, rm);
    v.append(tiles, grid);
  },
  render() {
    const d = S.snap, now = nowS(), st = d.stats || {}, h = d.hist || {};
    const heard = [...S.nodes.values()].filter((n) => n.last && now - n.last < 86400).length;
    $("tNodesV").textContent = heard; $("tNodesS").textContent = "in 24 h · " + S.nodes.size + " known";
    const m = Math.floor(now / 60), rf = d.perMin ? d.perMin[d.perMin.length - 1] || 0 : S.rfMin.get(m) || 0, net = S.netMin.get(m - 1) || 0;
    $("tRateV").replaceChildren(String(rf), el("small", "", net ? "+" + net + " net" : "")); $("tRateS").textContent = (st.rx || 0) + " heard · " + (st.tx || 0) + " sent";
    $("tNoiseV").replaceChildren(st.noise ? String(st.noise) : "-", el("small", "", "dBm")); $("tNoiseS").textContent = st.noise ? (st.noise < -110 ? "quiet" : st.noise < -100 ? "some noise" : "noisy") : "measuring";
    const b = d.node.batt || {};
    $("tBattV").replaceChildren(b.pct >= 101 ? "USB" : b.pct >= 0 ? String(b.pct) : "-", el("small", "", b.pct >= 0 && b.pct < 101 ? "%" : "")); $("tBattS").textContent = b.mv ? (b.mv / 1000).toFixed(2) + " V" : "";
    const air = d.up ? (st.airTxS || 0) / d.up * 100 : 0;
    $("tAirV").replaceChildren(air.toFixed(air < 10 ? 2 : 1), el("small", "", "%")); $("tAirS").textContent = Math.round((st.airTxS || 0) / 60) + " min since start";
    sparkline($("tNodesC"), (h.nodes || []).slice(-72), "--cyan"); sparkline($("tRateC"), (h.rx || []).slice(-72), "--green");
    sparkline($("tNoiseC"), (h.noise || []).slice(-72), "--violet"); sparkline($("tBattC"), (h.batt || []).slice(-72), "--amber"); sparkline($("tAirC"), (h.tx || []).slice(-72), "--pink");
    // traffic
    if (this.range === "30m") {
      const rfS = [], netS = [], pm = d.perMin || [];
      for (let i = 0; i < 30; i++) { const mm = m - 29 + i; rfS.push([mm * 60, pm.length === 30 ? pm[i] : S.rfMin.get(mm) || 0]); netS.push([mm * 60, S.netMin.get(mm) || 0]); }
      this.traffic.set([{ name: "This radio", color: "--cyan", data: rfS }, { name: "Internet (other observers)", color: "--violet", data: netS }], { type: "bar", stacked: true, unit: "" });
    } else {
      const step = h.step || 300, end = h.end || now, n = (h.rx || []).length, rx = [], tx = [];
      for (let i = 0; i < n; i++) { const t = end - (n - 1 - i) * step; rx.push([t, h.rx[i]]); tx.push([t, (h.tx || [])[i] || 0]); }
      this.traffic.set([{ name: "Heard", color: "--cyan", data: rx }, { name: "Sent", color: "--pink", data: tx }], { type: "area", stacked: false, unit: " / 5 min" });
    }
    // mix
    const cnt = {};
    for (const p of lastHour()) cnt[p.type] = (cnt[p.type] || 0) + 1;
    this.mix.set(Object.entries(cnt).sort((a, b) => b[1] - a[1]).map(([t, v]) => ({ n: PT_NAME[t] || t, v, c: PT_COLOR[t] || "--muted" })));
    // signal scatter by type
    const by = {};
    for (const p of S.packets) if (p.src !== "net" && p.rssi && p.snr !== null && p.snr !== undefined) { (by[p.type] = by[p.type] || []).push([p.rssi, p.snr, (p.from || PT_NAME[p.type] || p.type) + "  ·  " + clock(p.t)]); if (Object.values(by).flat().length > 300) break; }
    this.sig.set(Object.entries(by).map(([t, data]) => ({ name: PT_NAME[t] || t, color: PT_COLOR[t] || "--muted", data })));
    // hops
    const hc = new Array(9).fill(0);
    for (const p of lastHour()) if (p.flood) hc[Math.min(8, p.path.length)]++;
    this.hops.set([{ name: "Packets", color: "--green", data: hc.map((v, i) => [i, v]) }]);
    // busiest repeaters
    const rep = new Map();
    for (const p of lastHour()) for (const x of p.path) rep.set(x, (rep.get(x) || 0) + 1);
    hbars(this.busy, [...rep.entries()].sort((a, b) => b[1] - a[1]).slice(0, 7).map(([x, v]) => { const n = nodeFor(x); return { n: n && n.name ? n.name : x.toUpperCase(), v, c: "--green", title: x }; }), "--green");
    // noise 24 h
    const step = h.step || 300, end = h.end || now, nz = [], rs = [];
    (h.noise || []).forEach((v, i, a) => nz.push([end - (a.length - 1 - i) * step, v || null]));
    (h.rssi || []).forEach((v, i, a) => rs.push([end - (a.length - 1 - i) * step, v || null]));
    this.noise.set([{ name: "Noise floor", color: "--violet", data: nz }, { name: "Average signal heard", color: "--cyan", data: rs }]);
    // messages
    this.recent.replaceChildren();
    const last = S.msgs.slice(-5).reverse();
    if (!last.length) this.recent.append(el("div", "empty", "No channel messages yet"));
    for (const x of last) {
      const r = el("div", "who"); r.style.cssText = "margin:8px 0;align-items:flex-start";
      const t = el("div"); t.style.minWidth = "0";
      const top = el("div"); top.style.cssText = "display:flex;gap:6px;align-items:center;font-size:12px";
      top.append(el("b", "", x.f || "?"), el("span", "dim", chanName(x.c) + " · " + ago(x.t)));
      if (x.net) top.append(el("span", "tag net", "internet"));
      const tx = el("div", "", x.x); tx.style.cssText = "overflow:hidden;text-overflow:ellipsis;white-space:nowrap";
      t.append(top, tx); r.append(avatar(x.f, x.f), t); this.recent.append(r);
    }
  },
};
function chanName(i) { const c = (S.snap.channels || []).find((x) => x.i === i); return c ? c.n : "Channel " + i; }

V.live = {
  src: "all", type: "", q: "", open: null,
  build() {
    const v = $("v-live"), c = card("Live traffic");
    const tb = el("div", "toolbar");
    tb.append(seg([["all", "All"], ["rf", "This radio"], ["net", "Internet"]], this.src, (s) => { this.src = s; this.render(); }));
    const types = el("div", "seg");
    for (const [t, n] of [["", "Any type"], ["ADVERT", "Adverts"], ["GROUP", "Channel"], ["TEXT", "Direct"], ["PATH", "Path"], ["TRACE", "Trace"]]) {
      const b = el("button", t === this.type ? "on" : "", n);
      b.onclick = () => { this.type = t; types.querySelectorAll("button").forEach((x) => x.classList.toggle("on", x === b)); this.render(); };
      types.append(b);
    }
    const q = el("input", "search"); q.placeholder = "Search name, path or observer"; q.oninput = () => { this.q = q.value.toLowerCase(); this.render(); };
    const pause = el("button", "btn sm", "Pause");
    pause.onclick = () => { S.paused = !S.paused; pause.textContent = S.paused ? "Resume" : "Pause"; pause.classList.toggle("on", S.paused); if (!S.paused) this.render(); };
    this.count = el("span", "dim");
    tb.append(types, q, pause, el("span", "grow"), this.count);
    const sc = el("div", "scroll"); sc.style.maxHeight = "calc(100vh - 250px)";
    const t = el("table", "tbl"), th = el("thead"), hr = el("tr");
    for (const [n, cls] of [["Type", ""], ["From", ""], ["Route", ""], ["Heard by", "hide-s"], ["Signal", "hide-s"], ["Time", ""]]) hr.append(el("th", cls, n));
    th.append(hr); this.tb = el("tbody"); t.append(th, this.tb); sc.append(t);
    c.append(tb, sc); v.append(c);
  },
  render() {
    if (S.paused) return;
    const q = this.q, rows = S.packets.filter((p) => (this.src === "all" || (this.src === "rf" ? p.src !== "net" : p.src !== "rf")) && (!this.type || p.type === this.type) &&
      (!q || (p.from || p.name || "").toLowerCase().includes(q) || p.path.join(" ").includes(q) || [...p.obs].join(" ").toLowerCase().includes(q))).slice(0, 200);
    this.count.textContent = rows.length + " shown";
    this.tb.replaceChildren();
    const now = Date.now();
    for (const p of rows) {
      const tr = el("tr", now - p.fresh < 1500 ? "new" : "");
      const pill = el("span", "pill", p.type); pill.style.setProperty("--c", ptColor(p.type));
      const td1 = el("td"); td1.append(pill);
      const who = el("div", "who"), name = p.from || p.name || (p.type === "ADVERT" && p.key ? p.key.slice(0, 8) : "");
      if (name) who.append(avatar(p.key || name, name), el("b", "", name)); else who.append(el("span", "dim", p.mine ? "this radio" : "-"));
      const td2 = el("td"); td2.append(who);
      const td3 = el("td"), hp = el("span", "hops");
      td3.append(el("span", "muted", (p.flood ? "flood " : "direct ") + p.path.length + " hop" + (p.path.length === 1 ? "" : "s") + " "));
      for (const x of p.path.slice(0, 5)) { const n = nodeFor(x), s = el("span", "", n && n.name ? n.name.slice(0, 10) : x.toUpperCase()); s.title = x; hp.append(s); }
      if (p.path.length > 5) hp.append(el("span", "", "+" + (p.path.length - 5)));
      td3.append(hp);
      const td4 = el("td", "hide-s");
      if (p.src === "rf") td4.append(el("span", "tag", "this radio"));
      else { if (p.src === "both") td4.append(el("span", "tag", "this radio"), " "); const t = el("span", "tag net", p.obs.size + " observer" + (p.obs.size === 1 ? "" : "s")); t.title = [...p.obs].join(", "); td4.append(t); }
      const td5 = el("td", "hide-s mono", p.rssi ? p.rssi + " dBm / " + (p.snr > 0 ? "+" : "") + (p.snr || 0).toFixed(1) + " dB" : p.snr !== null && p.snr !== undefined ? "SNR " + p.snr.toFixed(1) : "-");
      const td6 = el("td", "muted", clock(p.t));
      tr.append(td1, td2, td3, td4, td5, td6);
      tr.onclick = () => { this.open = this.open === p.id ? null : p.id; this.render(); };
      this.tb.append(tr);
      if (this.open === p.id) {
        const dr = el("tr"), dd = el("td"); dd.colSpan = 6;
        const box = el("div", "detail");
        const kv = el("div", "kvlist");
        const add = (k, v) => kv.append(el("span", "", k), el("span", "", v));
        add("Type", PT_NAME[p.type] || p.type); add("Size", p.len + " bytes"); add("Route", (p.flood ? "Flood" : "Direct") + ", " + p.hashSize + "-byte hop hashes");
        add("Path", p.path.length ? p.path.map((x) => { const n = nodeFor(x); return x.toUpperCase() + (n && n.name ? " (" + n.name + ")" : ""); }).join(" → ") : "none");
        if (p.obs.size) add("Observers", [...p.obs].join(", "));
        if (p.lat !== undefined) add("Position", p.lat.toFixed(4) + ", " + p.lon.toFixed(4));
        box.append(kv); dd.append(box); dr.append(dd); this.tb.append(dr);
      }
    }
    if (!rows.length) { const tr = el("tr"), td = el("td", "empty", "Waiting for packets..."); td.colSpan = 6; tr.append(td); this.tb.append(tr); }
  },
};

V.map = {
  show: { rep: true, comp: true, room: true, beams: true, labels: true }, sel: null, fitted: false,
  build() {
    const v = $("v-map"), host = el("div");
    v.append(host);
    this.m = UI.map = new SlippyMap(host, { onDraw: (g, m, w, h) => this.draw(g, m, w, h), onClick: (x, y) => this.click(x, y) });
    const ctl = el("div", "mapctl");
    const zin = el("button", "btn", "+"), zout = el("button", "btn", "−"), fit = el("button", "btn"), mine = el("button", "btn");
    fit.append(svgIcon("M4 9V4h5M20 9V4h-5M4 15v5h5M20 15v5h-5")); mine.append(svgIcon("M12 2v3m0 14v3M2 12h3m14 0h3M12 8a4 4 0 1 1 0 8 4 4 0 0 1 0-8z"));
    zin.onclick = () => { const [w, h] = this.m.size(); this.m.zoomAt(1, w / 2, h / 2); }; zout.onclick = () => { const [w, h] = this.m.size(); this.m.zoomAt(-1, w / 2, h / 2); };
    fit.onclick = () => this.fit(); mine.onclick = () => { const p = myPos(); if (p) this.m.view(p.lat, p.lon, Math.max(this.m.z, 12)); else toast("This node has no position yet"); };
    ctl.append(zin, zout, fit, mine);
    const lay = el("details", "layers"), sum = el("summary", "", "Layers");
    lay.open = innerWidth > 720; lay.append(sum);
    for (const [k, n, c] of [["rep", "Repeaters", "--green"], ["comp", "Companions", "--blue"], ["room", "Rooms and sensors", "--violet"], ["beams", "Live beams", "--cyan"], ["labels", "Names", "--muted"]]) {
      const l = el("label"), cb = el("input"); cb.type = "checkbox"; cb.checked = this.show[k];
      cb.onchange = () => { this.show[k] = cb.checked; this.m.render(); };
      const i = el("i"); i.style.background = css(c); l.append(cb, i, n); lay.append(l);
    }
    this.stats = el("div", "mapstats");
    this.cardEl = el("div", "mapcard hidden");
    host.append(ctl, lay, this.stats, this.cardEl);
  },
  nodesShown() {
    return [...S.nodes.values()].filter((n) => n.lat !== undefined && n.lat !== null && (n.lat || n.lon) &&
      (n.type === 2 ? this.show.rep : n.type === 1 || !n.type ? this.show.comp : this.show.room));
  },
  fit() { // the recent nodes, leaving out the farthest few so one far-off node doesn't shrink everything
    let pts = this.nodesShown().filter((n) => !n.last || nowS() - n.last < 7 * 86400);
    const p = myPos();
    if (p && pts.length > 6) pts = pts.sort((a, b) => distKm(p, a) - distKm(p, b)).slice(0, Math.ceil(pts.length * 0.85));
    if (p) pts.push(p);
    if (pts.length) this.m.fit(pts);
  },
  render() {
    if (!this.fitted && S.snap) { this.fitted = true; const p = myPos(); if (p) this.m.view(p.lat, p.lon, 11); this.fit(); }
    this.stats.replaceChildren();
    const ns = this.nodesShown(), live = S.beams.filter((b) => performance.now() - b.t0 < 6000).length;
    for (const [c, t] of [["ok", ns.length + " on the map"], [live ? "ok" : "", live + " beams now"]]) { const e = el("span", "chip " + c); e.append(el("i"), t); this.stats.append(e); }
    this.m.render();
  },
  draw(g, m, w, h) {
    const now = nowS(), t = performance.now(), mp = myPos();
    // beams
    if (this.show.beams) {
      S.beams = S.beams.filter((b) => t - b.t0 < 6500);
      for (const b of S.beams) {
        const pts = beamPoints(b.e); if (pts.length < 2) continue;
        const xy = pts.map((p) => m.xy(p.lat, p.lon)), k = (t - b.t0) / 6500, col = ptColor(b.e.type);
        // segments light up one after another, then the whole route fades
        const segs = xy.length - 1, prog = Math.min(1, (t - b.t0) / 1800) * segs;
        g.lineCap = "round";
        for (let i = 0; i < segs; i++) {
          const f = clamp(prog - i, 0, 1); if (!f) break;
          const [x0, y0] = xy[i], [x1, y1] = xy[i + 1], x = x0 + (x1 - x0) * f, y = y0 + (y1 - y0) * f;
          g.globalAlpha = (1 - k) * 0.35; g.strokeStyle = col; g.lineWidth = 7; g.beginPath(); g.moveTo(x0, y0); g.lineTo(x, y); g.stroke();
          g.globalAlpha = 1 - k; g.lineWidth = 2.2; g.beginPath(); g.moveTo(x0, y0); g.lineTo(x, y); g.stroke();
          if (f < 1) { g.fillStyle = "#fff"; g.globalAlpha = 1 - k; g.beginPath(); g.arc(x, y, 3.5, 0, 7); g.fill(); }
        }
        const [sx, sy] = xy[0];
        g.globalAlpha = Math.max(0, 1 - k * 2.2); g.strokeStyle = col; g.lineWidth = 2; g.beginPath(); g.arc(sx, sy, 6 + k * 50, 0, 7); g.stroke();
        g.globalAlpha = 1;
      }
    }
    // nodes
    const label = this.show.labels && m.z >= 9, placed = [];
    g.font = "600 11px system-ui"; g.textBaseline = "middle";
    // repeaters and recent nodes first, so their names win where labels would overlap
    const order = this.nodesShown().sort((a, b) => (b.type === 2) - (a.type === 2) || (b.last || 0) - (a.last || 0));
    for (const n of order) {
      const [x, y] = m.xy(n.lat, n.lon); if (x < -20 || y < -20 || x > w + 20 || y > h + 20) continue;
      const old = !n.last || now - n.last > 86400 * 3;
      g.globalAlpha = old ? 0.45 : 1;
      const col = css(NT_COLOR[n.type] || "--muted");
      g.fillStyle = col; g.strokeStyle = css("--bg"); g.lineWidth = 2;
      g.beginPath();
      if (n.type === 2) { g.moveTo(x, y - 8); g.lineTo(x + 7, y + 5); g.lineTo(x - 7, y + 5); g.closePath(); }
      else if (n.type === 3 || n.type === 4) g.rect(x - 5.5, y - 5.5, 11, 11);
      else g.arc(x, y, 5.5, 0, 7);
      g.fill(); g.stroke();
      if (this.sel === n) { g.strokeStyle = css("--text"); g.lineWidth = 2; g.beginPath(); g.arc(x, y, 11, 0, 7); g.stroke(); }
      const tw = label && n.name ? g.measureText(n.name).width : 0, box = [x + 9, y - 9, x + 19 + tw, y + 9];
      if (tw && !placed.some((p) => box[0] < p[2] && box[2] > p[0] && box[1] < p[3] && box[3] > p[1])) {
        placed.push(box);
        g.fillStyle = css("--bar"); g.globalAlpha *= 0.85; g.beginPath(); g.roundRect ? g.roundRect(x + 9, y - 9, tw + 10, 18, 6) : g.rect(x + 9, y - 9, tw + 10, 18); g.fill();
        g.globalAlpha = old ? 0.45 : 1; g.fillStyle = css("--text"); g.fillText(n.name, x + 14, y + 0.5);
      }
    }
    g.globalAlpha = 1;
    if (mp) {
      const [x, y] = m.xy(mp.lat, mp.lon), pulse = (t % 2000) / 2000;
      g.fillStyle = css("--cyan"); g.globalAlpha = 0.25 * (1 - pulse); g.beginPath(); g.arc(x, y, 8 + pulse * 22, 0, 7); g.fill();
      g.globalAlpha = 1; g.shadowColor = g.fillStyle; g.shadowBlur = 14; g.beginPath(); g.arc(x, y, 7, 0, 7); g.fill(); g.shadowBlur = 0;
      g.strokeStyle = "#fff"; g.lineWidth = 2; g.stroke();
    }
    if (this.sel) this.place();
    if (UI.view === "map" && (S.beams.length || mp) && !m.anim) m.anim = requestAnimationFrame(() => { m.anim = 0; m.overlay(); });
  },
  click(x, y) {
    let best = null, bd = 400;
    for (const n of this.nodesShown()) { const [nx, ny] = this.m.xy(n.lat, n.lon), d = (nx - x) ** 2 + (ny - y) ** 2; if (d < bd) { bd = d; best = n; } }
    this.sel = best;
    if (!best) { this.cardEl.classList.add("hidden"); this.m.render(); return; }
    const c = this.cardEl; c.replaceChildren(); c.classList.remove("hidden");
    const hd = el("div", "who"); hd.append(avatar(best.key, best.name)); const tt = el("div"); tt.append(el("h4", "", best.name || best.key.toUpperCase()), el("div", "muted", NT[best.type] || "Node")); hd.append(tt);
    const kv = el("div", "kv"), add = (k, v) => kv.append(el("span", "", k), el("span", "", v));
    add("Heard", best.last ? ago(best.last) + " ago" : "-"); if (best.hops !== undefined && best.hops !== null) add("Route", best.hops < 0 ? "flood" : best.hops === 0 ? "direct" : best.hops + " hops");
    const p = myPos(); if (p) { add("Distance", km(distKm(p, best))); add("Bearing", Math.round(bearing(p, best)) + "° " + compass(bearing(p, best))); }
    add("Key", best.key.toUpperCase());
    const bt = el("div"); bt.style.cssText = "display:flex;gap:6px";
    const los = el("button", "btn sm primary", "Line of sight"); los.onclick = () => { V.los.preset = { b: best.key }; UI.go("los"); };
    const close = el("button", "btn sm", "Close"); close.onclick = () => { this.sel = null; c.classList.add("hidden"); this.m.render(); };
    bt.append(los, close); c.append(hd, kv, bt); this.place();
  },
  place() {
    const [x, y] = this.m.xy(this.sel.lat, this.sel.lon), [w, h] = this.m.size(), c = this.cardEl;
    c.style.left = clamp(x + 16, 8, w - 260) + "px"; c.style.top = clamp(y - 40, 8, h - (c.offsetHeight || 200) - 8) + "px";
  },
};

V.messages = {
  build() {
    const v = $("v-messages"), c = card("Channel messages", "", el("span", "dim hide-s", "Channels this node shares with the page; sent from here, a message goes out from the node. Direct messages stay on the radio."));
    const wrap = el("div", "chat"); this.list = el("div", "chans"); this.box = el("div", "msgs");
    const right = el("div"); right.style.cssText = "display:flex;flex-direction:column;min-width:0;min-height:0";
    const form = el("form"); form.style.cssText = "display:flex;gap:8px;padding:10px 0 0";
    this.input = el("input"); this.input.maxLength = 140; this.input.placeholder = "Message"; this.input.autocomplete = "off";
    this.input.style.cssText = "flex:1;min-width:0;background:var(--card2);border:1px solid var(--line);border-radius:10px;color:var(--text);padding:9px 12px;font:inherit";
    const go = el("button", "btn primary", "Send"); go.type = "submit";
    form.onsubmit = async (e) => {
      e.preventDefault();
      const text = this.input.value.trim(); if (!text) return;
      if (DEMO) { toast("Not in the demo"); return; }
      go.disabled = true;
      const r = await API.send(UI.chan, text);
      go.disabled = false;
      if (r.ok) { this.input.value = ""; this.scrolled = false; Poll.tick(); } else toast("Not sent: " + r.error);
      this.input.focus();
    };
    form.append(this.input, go);
    this.box.style.flex = "1"; right.append(this.box, form);
    wrap.append(this.list, right); c.append(wrap); v.append(c);
  },
  render() {
    const chans = (S.snap.channels || []);
    if (!chans.some((x) => x.i === UI.chan) && chans.length) UI.chan = chans[0].i;
    this.list.replaceChildren();
    for (const ch of chans) {
      const b = el("button", ch.i === UI.chan ? "on" : ""), u = S.unread.get(ch.i);
      const last = S.msgs.filter((m) => m.c === ch.i).slice(-1)[0];
      const t = el("div"); t.style.minWidth = "0"; t.append(el("div", "", ch.n)); const sub = el("div", "dim", last ? (last.f ? last.f + ": " : "") + last.x : ch.k ? "no messages yet" : "private: stays on the radio"); sub.style.cssText = "font-size:12px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:150px"; t.append(sub);
      b.append(el("span", "hash", ch.n.startsWith("#") ? "#" : ch.n[0] || "?"), t);
      if (u) b.append(el("span", "u", String(u)));
      b.onclick = () => { UI.chan = ch.i; S.unread.delete(ch.i); UI.badges(); this.render(); };
      this.list.append(b);
    }
    const cur = chans.find((x) => x.i === UI.chan);
    this.input.placeholder = cur ? "Message " + cur.n : "Message";
    const ms = S.msgs.filter((m) => m.c === UI.chan).sort((a, b) => a.t - b.t);
    const atEnd = this.box.scrollHeight - this.box.scrollTop - this.box.clientHeight < 60;
    this.box.replaceChildren();
    if (!ms.length) this.box.append(el("div", "empty", "No messages on this channel yet."));
    let day = "";
    for (const m of ms) {
      const d = new Date(m.t * 1000).toDateString();
      if (d !== day) { day = d; this.box.append(el("div", "daysep", new Date(m.t * 1000).toLocaleDateString([], { weekday: "long", month: "short", day: "numeric" }))); }
      const r = el("div", "m" + (m.me ? " mine" : "")), bub = el("div", "bub"), meta = el("div", "meta");
      const nm = el("b", "", m.me ? "You" : m.f || "?"); nm.style.color = keyColor(m.f || "?");
      meta.append(nm, el("span", "", clock(m.t)));
      if (m.net) meta.append(el("span", "tag net", "via internet"));
      else if (m.h !== undefined && m.h !== null && m.h >= 0) meta.append(el("span", "tag", m.h === 0 ? "direct" : m.h + " hop" + (m.h > 1 ? "s" : "")));
      bub.append(meta, el("div", "txt", m.x));
      r.append(avatar(m.f, m.me ? me().name : m.f), bub); this.box.append(r);
    }
    if (atEnd || !this.scrolled) { this.box.scrollTop = this.box.scrollHeight; this.scrolled = true; }
  },
};

V.los = {
  a: "me", b: "", ha: 2, hb: 10, ft: false, result: null, preset: null, pick: "",
  points() {
    const list = [];
    const p = myPos(); if (p) list.push({ id: "me", name: (me().name || "This node") + " (this node)", lat: p.lat, lon: p.lon });
    for (const n of [...S.nodes.values()].filter((n) => n.lat !== undefined && (n.lat || n.lon)).sort((a, b) => (b.type === 2) - (a.type === 2) || (a.name || "").localeCompare(b.name || ""))) list.push({ id: n.key, name: (n.name || n.key.toUpperCase()) + (n.type === 2 ? "  ▲" : ""), lat: n.lat, lon: n.lon, type: n.type });
    if (this.custom.a) list.push(this.custom.a); if (this.custom.b) list.push(this.custom.b);
    return list;
  },
  custom: {},
  build() {
    const v = $("v-los"), wrap = el("div", "los");
    const left = card("Line of sight"), right = el("div");
    left.append(el("p", "muted", "Pick two points, or click the map. The terrain between them comes from Open-Meteo (Copernicus 90 m elevation); the two positions are sent there to get it."));
    left.firstChild.nextSibling.style.cssText = "margin:0 0 14px;font-size:12.5px";
    const fld = (label, inp) => { const f = el("div", "field"); f.append(el("label", "", label), inp); return f; };
    this.selA = el("select"); this.selB = el("select");
    this.selA.onchange = () => { this.a = this.selA.value; this.mark(); }; this.selB.onchange = () => { this.b = this.selB.value; this.mark(); };
    const pa = el("button", "btn sm", "Pick A on map"), pb = el("button", "btn sm", "Pick B on map");
    pa.onclick = () => { this.pick = "a"; toast("Click the map to place A"); }; pb.onclick = () => { this.pick = "b"; toast("Click the map to place B"); };
    this.inA = el("input"); this.inB = el("input"); this.inA.type = this.inB.type = "number"; this.inA.min = this.inB.min = "0"; this.inA.step = this.inB.step = "0.5";
    this.inA.value = this.ha; this.inB.value = this.hb;
    this.inA.oninput = () => { this.ha = this.toM(+this.inA.value); if (this.result) this.compute(); }; this.inB.oninput = () => { this.hb = this.toM(+this.inB.value); if (this.result) this.compute(); };
    const units = seg([["m", "metres"], ["ft", "feet"]], "m", (u) => { this.ft = u === "ft"; this.inA.value = this.fromM(this.ha).toFixed(1); this.inB.value = this.fromM(this.hb).toFixed(1); this.compute(); });
    this.gain = el("input"); this.gain.type = "number"; this.gain.value = store("losGain") || 3; this.gain.step = "0.5"; this.gain.oninput = () => { store("losGain", this.gain.value); this.compute(); };
    this.go = el("button", "btn primary wide", "Check line of sight"); this.go.onclick = () => this.run();
    const pr = el("div"); pr.style.cssText = "display:flex;gap:6px;margin:-4px 0 12px"; pr.append(pa, pb);
    const hr = el("div", "row2"); hr.append(fld("Antenna A height", this.inA), fld("Antenna B height", this.inB));
    const ur = el("div"); ur.style.cssText = "margin:-4px 0 12px"; ur.append(units);
    left.append(fld("Point A", this.selA), fld("Point B", this.selB), pr, hr, ur, fld("Antenna gain, each end (dBi)", this.gain), this.go);
    this.out = el("div"); this.out.style.marginTop = "14px"; left.append(this.out);
    const mc = card("Path"), mh = el("div"); mh.style.height = "300px";
    mc.append(mh);
    this.m = new SlippyMap(mh, { z: 10, onDraw: (g, m) => this.drawMap(g, m), onClick: (x, y) => this.mapClick(x, y) });
    mh.style.minHeight = "0";
    const pc = card("Terrain profile", "", el("span", "dim", "earth curvature included (k = 4/3)"));
    this.prof = el("div", "chart"); this.pcv = el("canvas"); this.ptip = el("div", "tip"); this.ptip.style.opacity = 0; this.prof.append(this.pcv, this.ptip); pc.append(this.prof);
    const lg = el("div", "legend");
    for (const [c, n] of [["--amber", "Terrain"], ["--cyan", "Line of sight"], ["--violet", "Fresnel zone (60% kept clear)"], ["--red", "Blocking"]]) { const s = el("span"), i = el("i"); i.style.background = css(c); s.append(i, n); lg.append(s); }
    pc.append(lg);
    this.pcv.addEventListener("pointermove", (e) => { const r = this.pcv.getBoundingClientRect(); this.hx = e.clientX - r.left; this.drawProfile(); });
    this.pcv.addEventListener("pointerleave", () => { this.hx = null; this.drawProfile(); });
    charts.add({ draw: () => this.drawProfile() });
    right.style.cssText = "display:flex;flex-direction:column;gap:14px;min-width:0";
    right.append(mc, pc);
    wrap.append(left, right); v.append(wrap);
  },
  toM(v) { return this.ft ? v * 0.3048 : v; }, fromM(v) { return this.ft ? v / 0.3048 : v; },
  render() {
    if (this.preset) { if (this.preset.b) { this.b = this.preset.b; if (!myPos()) this.a = ""; } this.preset = null; this.result = null; }
    const pts = this.points();
    if (!this.a && pts[0]) this.a = pts[0].id;
    if (!this.b) { const r = pts.find((p) => p.id !== this.a && p.type === 2) || pts.find((p) => p.id !== this.a); if (r) this.b = r.id; }
    for (const [sel, cur] of [[this.selA, this.a], [this.selB, this.b]]) {
      if (sel.matches(":focus")) continue;
      sel.replaceChildren(); for (const p of pts) { const o = el("option", "", p.name); o.value = p.id; sel.append(o); } sel.value = cur;
    }
    if (!this.placed) { this.placed = true; this.mark(); }
    if (this.result && !this.drawn) this.drawProfile();
  },
  pt(id) { return this.points().find((p) => p.id === id); },
  mark() { const A = this.pt(this.a), B = this.pt(this.b); this.result = null; this.out.replaceChildren(); this.drawProfile(); if (A && B) this.m.fit([A, B], 15); else if (A) this.m.view(A.lat, A.lon, 11); else this.m.render(); },
  mapClick(x, y) {
    if (!this.pick) return;
    const p = this.m.ll(x, y), id = "pt_" + this.pick;
    this.custom[this.pick] = { id, name: "Map point " + this.pick.toUpperCase() + " (" + p.lat.toFixed(4) + ", " + p.lon.toFixed(4) + ")", lat: p.lat, lon: p.lon };
    this[this.pick] = id; this.pick = ""; this.render(); this.selA.value = this.a; this.selB.value = this.b; this.mark();
  },
  drawMap(g, m) {
    const A = this.pt(this.a), B = this.pt(this.b);
    if (!A || !B) return;
    const [ax, ay] = m.xy(A.lat, A.lon), [bx, by] = m.xy(B.lat, B.lon);
    const r = this.result, col = r ? css(r.verdict.c) : css("--cyan");
    g.strokeStyle = col; g.lineWidth = 3; g.setLineDash(r ? [] : [6, 6]); g.beginPath(); g.moveTo(ax, ay); g.lineTo(bx, by); g.stroke(); g.setLineDash([]);
    if (r) for (const s of r.samples) if (s.ratio < 0) { const [x, y] = m.xy(s.lat, s.lon); g.fillStyle = css("--red"); g.beginPath(); g.arc(x, y, 3, 0, 7); g.fill(); }
    for (const [x, y, t] of [[ax, ay, "A"], [bx, by, "B"]]) { g.fillStyle = css("--bg"); g.beginPath(); g.arc(x, y, 11, 0, 7); g.fill(); g.strokeStyle = col; g.lineWidth = 2.5; g.stroke(); g.fillStyle = css("--text"); g.font = "700 12px system-ui"; g.textAlign = "center"; g.textBaseline = "middle"; g.fillText(t, x, y + 0.5); }
    g.textAlign = "start";
  },
  async run() {
    const A = this.pt(this.a), B = this.pt(this.b);
    if (!A || !B || A.id === B.id) { toast("Pick two different points"); return; }
    this.go.disabled = true; this.go.textContent = "Getting the terrain...";
    const N = 160, lats = [], lons = [];
    for (let i = 0; i < N; i++) { const f = i / (N - 1); lats.push(A.lat + (B.lat - A.lat) * f); lons.push(A.lon + (B.lon - A.lon) * f); }
    let elev = null, demo = false;
    try {
      elev = [];
      for (let o = 0; o < N; o += 80) {
        const q = `https://api.open-meteo.com/v1/elevation?latitude=${lats.slice(o, o + 80).map((v) => v.toFixed(5)).join(",")}&longitude=${lons.slice(o, o + 80).map((v) => v.toFixed(5)).join(",")}`;
        const r = await fetch(q, { referrerPolicy: "no-referrer" }); if (!r.ok) throw new Error(r.status);
        elev.push(...(await r.json()).elevation);
      }
      if (elev.length !== N) throw new Error("short");
    } catch (e) {
      if (!DEMO) { this.go.disabled = false; this.go.textContent = "Check line of sight"; toast("Couldn't get the terrain (" + e.message + "). The page needs internet for this."); return; }
      elev = DEMO.terrain(N); demo = true;
    }
    this.go.disabled = false; this.go.textContent = "Check line of sight";
    this.raw = { A, B, lats, lons, elev, demo };
    this.compute();
  },
  compute() {
    if (!this.raw) return;
    const { A, B, lats, lons, elev, demo } = this.raw, N = elev.length;
    const D = distKm(A, B) * 1000, f = ((S.snap.radio || {}).freq || 910.525) * 1e6, lam = 299792458 / f, k = 4 / 3, Re = 6371000;
    const hA = elev[0] + this.ha, hB = elev[N - 1] + this.hb;
    let worst = Infinity, wi = 0;
    const samples = elev.map((g, i) => {
      const d1 = (D * i) / (N - 1), d2 = D - d1, bulge = (d1 * d2) / (2 * k * Re), ground = g + bulge;
      const los = hA + ((hB - hA) * d1) / D, F1 = Math.sqrt((lam * d1 * d2) / Math.max(D, 1));
      const clear = los - ground, ratio = F1 > 0.01 ? clear / F1 : clear >= 0 ? 9 : -9;
      if (i > 0 && i < N - 1 && ratio < worst) { worst = ratio; wi = i; }
      return { d: d1, g, ground, los, F1, clear, ratio, lat: lats[i], lon: lons[i] };
    });
    if (!isFinite(worst)) worst = 9;
    // knife-edge loss at the worst point (ITU-R P.526 approximation)
    const v = -worst * Math.SQRT2, J = v > -0.78 ? 6.9 + 20 * Math.log10(Math.sqrt((v - 0.1) ** 2 + 1) + v - 0.1) : 0;
    const fspl = 20 * Math.log10(D / 1000) + 20 * Math.log10(f / 1e6) + 32.44;
    const r = S.snap.radio || {}, sens = { 7: -124, 8: -127, 9: -130, 10: -133, 11: -135.5, 12: -137 }[r.sf || 8] + 10 * Math.log10((r.bw || 62.5) / 125);
    const gain = +this.gain.value || 0, rx = (r.tx || 22) + 2 * gain - fspl - Math.max(0, J), margin = rx - sens;
    const verdict = worst >= 0.6 ? { c: "--green", t: "Clear line of sight", s: "The Fresnel zone is at least 60% clear the whole way.", i: "✓" }
      : worst >= 0 ? { c: "--amber", t: "Partly blocked", s: "You can see it, but terrain is in the Fresnel zone: expect a few dB of loss.", i: "!" }
      : { c: "--red", t: "Blocked by terrain", s: "The ground rises above the straight line. A relay or taller masts would help.", i: "✕" };
    this.result = { samples, D, worst, wi, J: Math.max(0, J), fspl, rx, sens, margin, verdict, demo };
    this.out.replaceChildren();
    const vd = el("div", "verdict"); vd.style.setProperty("--c", css(verdict.c));
    const tx = el("div"); tx.append(el("b", "", verdict.t), el("div", "muted", verdict.s)); tx.lastChild.style.fontSize = "12.5px";
    vd.append(el("div", "ic", verdict.i), tx);
    const kp = el("div", "kpis"), K = (k, v, s) => { const e = el("div", "kpi"); e.append(el("div", "k", k), el("div", "v", v), el("div", "s", s || "")); kp.append(e); };
    K("Distance", km(D / 1000), Math.round(bearing(A, B)) + "° " + compass(bearing(A, B)));
    const wpt = samples[wi], U = (m) => (this.ft ? Math.round(m / 0.3048) + " ft" : Math.round(m) + " m");
    if (worst < 0) K("Blocked by", U(-wpt.clear), "of ground above the line, " + km(wpt.d / 1000) + " from A");
    else K("Clearance", Math.round(Math.min(worst, 9.99) * 100) + "%", "of the first Fresnel zone kept clear");
    K("Path loss", Math.round(fspl + this.result.J) + " dB", this.result.J > 0.5 ? Math.round(this.result.J) + " dB from terrain" : "free space");
    K("Link margin", (margin > 0 ? "+" : "") + Math.round(margin) + " dB", "signal " + Math.round(rx) + " dBm at SF" + (r.sf || 8));
    kp.style.gridTemplateColumns = "repeat(2,1fr)";
    this.out.append(vd, kp);
    if (demo) this.out.append(Object.assign(el("div", "hint", "Demo terrain: the preview can't reach the elevation service, so this profile is made up."), {}));
    this.drawProfile(); this.m.render();
  },
  drawProfile() {
    const c = this.pcv; if (!c) return;
    const [g, w, h] = fitCanvas(c, 260); g.clearRect(0, 0, w, h);
    const r = this.result;
    if (!r) { g.fillStyle = css("--dim"); g.textAlign = "center"; g.font = "13px system-ui"; g.fillText("Pick two points and press Check line of sight", w / 2, h / 2); return; }
    this.drawn = true;
    const L = 46, R = 12, T = 12, B = 24, pw = w - L - R, ph = h - T - B, s = r.samples;
    let lo = Infinity, hi = -Infinity;
    for (const x of s) { lo = Math.min(lo, x.g, x.ground); hi = Math.max(hi, x.ground, x.los + x.F1); }
    const pad = (hi - lo) * 0.08 + 5; lo -= pad; hi += pad;
    const step = niceStep(hi - lo, 4); lo = Math.floor(lo / step) * step; hi = Math.ceil(hi / step) * step;
    const X = (d) => L + (d / r.D) * pw, Y = (v) => T + ph - ((v - lo) / (hi - lo)) * ph, U = (m) => (this.ft ? Math.round(m / 0.3048) + " ft" : Math.round(m) + " m");
    g.font = "11px system-ui"; g.fillStyle = css("--dim"); g.strokeStyle = css("--quiet"); g.textAlign = "right"; g.textBaseline = "middle";
    for (let v = lo; v <= hi + 1; v += step) { const y = Math.round(Y(v)) + 0.5; g.beginPath(); g.moveTo(L, y); g.lineTo(w - R, y); g.stroke(); g.fillText(U(v), L - 6, y); }
    g.textAlign = "center"; g.textBaseline = "alphabetic";
    for (let i = 0; i <= 5; i++) g.fillText(km((r.D * i) / 5000), clamp(X((r.D * i) / 5), L + 14, w - R - 14), h - 6);
    // Fresnel zone
    g.beginPath(); s.forEach((x, i) => { const px = X(x.d), py = Y(x.los + x.F1); i ? g.lineTo(px, py) : g.moveTo(px, py); });
    for (let i = s.length - 1; i >= 0; i--) g.lineTo(X(s[i].d), Y(s[i].los - s[i].F1));
    g.closePath(); g.fillStyle = css("--violet") + "26"; g.fill();
    g.setLineDash([4, 4]); g.strokeStyle = css("--violet"); g.lineWidth = 1; g.beginPath(); s.forEach((x, i) => { const px = X(x.d), py = Y(x.los - 0.6 * x.F1); i ? g.lineTo(px, py) : g.moveTo(px, py); }); g.stroke(); g.setLineDash([]);
    // terrain (with the earth's bulge)
    g.beginPath(); g.moveTo(X(0), Y(lo)); s.forEach((x) => g.lineTo(X(x.d), Y(x.ground))); g.lineTo(X(r.D), Y(lo)); g.closePath();
    const gr = g.createLinearGradient(0, T, 0, T + ph); gr.addColorStop(0, css("--amber") + "cc"); gr.addColorStop(1, css("--amber") + "22");
    g.fillStyle = gr; g.fill(); g.strokeStyle = css("--amber"); g.lineWidth = 1.5; g.beginPath(); s.forEach((x, i) => (i ? g.lineTo(X(x.d), Y(x.ground)) : g.moveTo(X(x.d), Y(x.ground)))); g.stroke();
    // blocked stretches
    g.strokeStyle = css("--red"); g.lineWidth = 3;
    for (let i = 1; i < s.length; i++) if (s[i].ratio < 0 && s[i - 1].ratio < 0) { g.beginPath(); g.moveTo(X(s[i - 1].d), Y(s[i - 1].ground)); g.lineTo(X(s[i].d), Y(s[i].ground)); g.stroke(); }
    // masts and the line of sight
    const mast = (i, top) => { g.strokeStyle = css("--text"); g.lineWidth = 2; g.beginPath(); g.moveTo(X(s[i].d), Y(s[i].ground)); g.lineTo(X(s[i].d), Y(top)); g.stroke(); };
    mast(0, s[0].los); mast(s.length - 1, s[s.length - 1].los);
    g.strokeStyle = css(r.verdict.c); g.lineWidth = 2.5; g.beginPath(); g.moveTo(X(0), Y(s[0].los)); g.lineTo(X(r.D), Y(s[s.length - 1].los)); g.stroke();
    for (const [i, t] of [[0, "A"], [s.length - 1, "B"]]) { const x = X(s[i].d), y = Y(s[i].los); g.fillStyle = css(r.verdict.c); g.beginPath(); g.arc(x, y, 5, 0, 7); g.fill(); g.fillStyle = css("--text"); g.font = "700 12px system-ui"; g.textAlign = i ? "right" : "left"; g.fillText(t, x + (i ? -9 : 9), y - 8); }
    // worst point
    const wp = s[r.wi]; g.fillStyle = css(r.worst < 0 ? "--red" : r.worst < 0.6 ? "--amber" : "--green"); g.beginPath(); g.arc(X(wp.d), Y(wp.ground), 4.5, 0, 7); g.fill();
    // hover read-out
    this.ptip.style.opacity = 0;
    if (this.hx === null || this.hx === undefined || this.hx < L || this.hx > w - R) return;
    const i = clamp(Math.round(((this.hx - L) / pw) * (s.length - 1)), 0, s.length - 1), x = s[i], px = X(x.d);
    g.strokeStyle = css("--line2"); g.setLineDash([3, 3]); g.lineWidth = 1; g.beginPath(); g.moveTo(px, T); g.lineTo(px, T + ph); g.stroke(); g.setLineDash([]);
    g.fillStyle = css("--text"); g.beginPath(); g.arc(px, Y(x.ground), 4, 0, 7); g.fill();
    this.ptip.replaceChildren(el("b", "", km(x.d / 1000) + " from A"));
    for (const [c, n, v] of [["--amber", "Ground", U(x.g)], ["--cyan", "Line of sight", U(x.los)], ["--violet", "Clearance", U(x.clear)], [x.ratio < 0 ? "--red" : x.ratio < 0.6 ? "--amber" : "--green", "Fresnel clear", Math.round(clamp(x.ratio, -9, 9) * 100) + "%"]]) {
      const row = el("div", "row"), sw = el("i", "sw"); sw.style.background = css(c); row.append(sw, el("span", "", n), el("span", "", v)); this.ptip.append(row);
    }
    const tw = this.ptip.offsetWidth || 150;
    this.ptip.style.left = (px + 14 + tw > w ? px - tw - 14 : px + 14) + "px"; this.ptip.style.top = "10px"; this.ptip.style.opacity = 1;
  },
};

V.nodes = {
  sort: "last", dir: -1, q: "", type: -1,
  build() {
    const v = $("v-nodes"), c = card("Nodes");
    const tb = el("div", "toolbar"), q = el("input", "search"); q.placeholder = "Search by name or key"; q.oninput = () => { this.q = q.value.toLowerCase(); this.render(); };
    tb.append(q, seg([["-1", "All"], ["2", "Repeaters"], ["1", "Companions"], ["3", "Rooms"], ["4", "Sensors"]], "-1", (t) => { this.type = +t; this.render(); }));
    this.count = el("span", "dim"); tb.append(el("span", "grow"), this.count);
    const sc = el("div", "scroll"); sc.style.maxHeight = "calc(100vh - 250px)";
    const t = el("table", "tbl"), th = el("thead"), hr = el("tr");
    this.heads = [];
    for (const [k, n, cls] of [["name", "Name", ""], ["type", "Type", ""], ["hops", "Route", "hide-s"], ["dist", "Distance", ""], ["last", "Heard", ""], ["key", "Key", "hide-s"]]) {
      const h = el("th", cls, n); h.onclick = () => { if (this.sort === k) this.dir = -this.dir; else { this.sort = k; this.dir = k === "name" || k === "dist" || k === "key" || k === "type" ? 1 : -1; } this.render(); };
      h.dataset.k = k; this.heads.push(h); hr.append(h);
    }
    th.append(hr); this.tb = el("tbody"); t.append(th, this.tb); sc.append(t); c.append(tb, sc); v.append(c);
  },
  render() {
    const p = myPos(), q = this.q, now = nowS();
    let list = [...S.nodes.values()].filter((n) => (n.contact || n.advert) && (this.type < 0 || n.type === this.type) && (!q || (n.name || "").toLowerCase().includes(q) || n.key.includes(q)));
    for (const n of list) n.dist = p && n.lat !== undefined && (n.lat || n.lon) ? distKm(p, n) : null;
    list.sort((a, b) => { const x = a[this.sort], y = b[this.sort]; if (x === null || x === undefined) return 1; if (y === null || y === undefined) return -1; return (x > y ? 1 : x < y ? -1 : 0) * this.dir; });
    this.heads.forEach((h) => { h.classList.toggle("sorted", h.dataset.k === this.sort); h.classList.toggle("up", this.dir > 0); });
    this.count.textContent = list.length + " nodes";
    this.tb.replaceChildren();
    for (const n of list.slice(0, 400)) {
      const tr = el("tr"), who = el("div", "who"); who.append(avatar(n.key, n.name), el("b", "", n.name || "(no name)")); if (n.fav) who.append(el("span", "", "★"));
      const td1 = el("td"); td1.append(who);
      const pill = el("span", "pill", NT[n.type] || "Node"); pill.style.setProperty("--c", css(NT_COLOR[n.type] || "--muted"));
      const td2 = el("td"); td2.append(pill);
      tr.append(td1, td2, el("td", "hide-s muted", n.hops === undefined || n.hops === null ? "-" : n.hops < 0 ? "flood" : n.hops === 0 ? "direct" : n.hops + " hops"),
        el("td", "", n.dist === null ? "-" : km(n.dist)), el("td", n.last && now - n.last < 3600 ? "" : "muted", ago(n.last, now)), el("td", "hide-s mono", n.key.toUpperCase()));
      if (!n.last || now - n.last > 86400 * 3) tr.style.opacity = 0.6;
      tr.onclick = () => { if (n.lat !== undefined && (n.lat || n.lon)) { UI.go("map"); V.map.m.view(n.lat, n.lon, 13); V.map.sel = n; V.map.click(...V.map.m.xy(n.lat, n.lon)); } else toast("No position for " + (n.name || "this node")); };
      this.tb.append(tr);
    }
    if (!list.length) { const tr = el("tr"), td = el("td", "empty", "No nodes yet"); td.colSpan = 6; tr.append(td); this.tb.append(tr); }
  },
};

V.device = {
  build() {
    const v = $("v-device"), grid = el("div", "grid");
    const id = card("This node", "c4"); this.id = id.appendChild(el("div", "kvlist"));
    const radio = card("Radio", "c4"); this.radio = radio.appendChild(el("div", "kvlist"));
    const sys = card("System", "c4"); this.sys = sys.appendChild(el("div"));
    const bat = card("Battery, 24 h", "c6"); this.bat = new Chart(bat.appendChild(el("div")), { height: 190, type: "area", unit: " V", yFmt: (v) => v.toFixed(2) });
    const mem = card("Free memory, 24 h", "c6"); this.mem = new Chart(mem.appendChild(el("div")), { height: 190, type: "line", unit: " KB" });
    const load = card("Screen load, 24 h", "c6", el("span", "dim", "share of the main loop")); this.load = new Chart(load.appendChild(el("div")), { height: 190, type: "area", unit: "%", yMin: 0 });
    const net = card("Internet feed", "c6"); this.net = net.appendChild(el("div"));
    const sw = el("div"); sw.style.cssText = "display:flex;gap:8px;flex-wrap:wrap;margin-top:12px";
    this.feedBtn = el("button", "btn sm"); this.feedBtn.onclick = () => { store("feed", Feed.want() ? "0" : "1"); if (Feed.want()) Feed.start(S.snap.feed && S.snap.feed.url); else Feed.stop(); this.render(); UI.chips(); };
    const logBtn = el("button", "btn sm", "Download the device log"); logBtn.onclick = async () => {
      try { const t = await API.log(); const a = el("a"); a.href = URL.createObjectURL(new Blob([t], { type: "text/plain" })); a.download = "hoptics-mc-log-" + new Date().toISOString().slice(0, 16).replace(/[:T]/g, "-") + ".txt"; a.click(); toast("Log downloaded. It holds no messages, names or positions."); } catch (e) { toast("Couldn't get the log: " + e.message); }
    };
    sw.append(this.feedBtn, logBtn); net.append(sw);
    const fw = card("Firmware update", "c6", el("span", "dim", "over Wi-Fi, on trial"));
    this.fwInfo = fw.appendChild(el("div", "kvlist"));
    const note = el("p", "muted", "Choose the file ending in .update.bin for this board. The node checks it, restarts into it and keeps it after a healthy minute; if it doesn't start properly, the node goes back to the build it has now by itself.");
    note.style.cssText = "margin:10px 0;font-size:12.5px";
    const row = el("div"); row.style.cssText = "display:flex;gap:8px;flex-wrap:wrap;align-items:center";
    const pick = el("input"); pick.type = "file"; pick.accept = ".bin"; pick.style.cssText = "flex:1;min-width:0;font-size:12.5px;color:var(--muted)";
    const go = el("button", "btn sm", "Install"); go.disabled = true;
    const bar = el("div", "meter"), bi = el("i"); bar.append(bi); bar.style.cssText = "display:none;margin-top:10px"; bi.style.setProperty("--c", css("--cyan"));
    this.fwMsg = el("p", "muted"); this.fwMsg.style.cssText = "margin:8px 0 0;font-size:12.5px";
    let armed = false;
    pick.onchange = () => {
      const f = pick.files[0]; armed = false; go.textContent = "Install";
      go.disabled = !f || /factory|full|merged/i.test(f.name);
      this.fwMsg.textContent = !f ? "" : go.disabled ? "That is the full image for a USB install. Choose the .update.bin." : f.name + ", " + (f.size / 1048576).toFixed(2) + " MB";
    };
    go.onclick = async () => {
      const f = pick.files[0]; if (!f) return;
      if (DEMO) { toast("Not in the demo"); return; }
      if (!armed) { armed = true; go.textContent = "Tap again to install"; return; } // no pop-up confirm: a second tap
      armed = false; go.disabled = pick.disabled = true; go.textContent = "Sending..."; bar.style.display = "";
      Poll.hold = Date.now() + 600000;
      const r = await API.update(f, (p) => { bi.style.width = Math.round(p * 100) + "%"; this.fwMsg.textContent = "Sending " + Math.round(p * 100) + "%"; });
      go.textContent = "Install"; pick.disabled = false;
      if (r.ok) { this.fwMsg.textContent = "Checked and installed. The node is restarting into it; this page reconnects by itself."; Poll.hold = Date.now() + 12000; toast("Update installed, restarting"); }
      else { Poll.hold = 0; go.disabled = false; bar.style.display = "none"; this.fwMsg.textContent = "Not installed: " + r.error; }
    };
    row.append(pick, go); fw.append(note, row, bar, this.fwMsg);
    const tl = card("Offline map tiles", "c6", el("span", "dim", "for the node's own map"));
    this.tlInfo = tl.appendChild(el("div", "kvlist"));
    const tnote = el("p", "muted", "Add a folder of tiles laid out as zoom/x/y.png (for example from Mobile Atlas Creator) and the node's map shows them without internet. Use a tile source that allows keeping tiles offline: OpenStreetMap's own servers don't allow bulk downloads.");
    tnote.style.cssText = "margin:10px 0;font-size:12.5px";
    const trow = el("div"); trow.style.cssText = "display:flex;gap:8px;flex-wrap:wrap;align-items:center";
    const dir = el("input"); dir.type = "file"; dir.webkitdirectory = true; dir.multiple = true; dir.style.cssText = "flex:1;min-width:0;font-size:12.5px;color:var(--muted)";
    const send = el("button", "btn sm", "Send"); send.disabled = true;
    const clear = el("button", "btn sm", "Clear saved tiles");
    const tbar = el("div", "meter"), tbi = el("i"); tbar.append(tbi); tbar.style.cssText = "display:none;margin-top:10px"; tbi.style.setProperty("--c", css("--green"));
    this.tlMsg = el("p", "muted"); this.tlMsg.style.cssText = "margin:8px 0 0;font-size:12.5px";
    let pack = [], clearArmed = false;
    dir.onchange = () => {
      pack = [];
      for (const f of dir.files) { const m = (f.webkitRelativePath || f.name).match(/(\d{1,2})\/(\d+)\/(\d+)\.png$/i); if (m && +m[1] <= 19) pack.push({ z: +m[1], x: +m[2], y: +m[3], f }); }
      pack.sort((a, b) => a.z - b.z); // the overview first: a pack cut short still shows something everywhere
      const kb = Math.round(pack.reduce((t, p) => t + p.f.size + 64, 0) / 1024), cap = (S.snap && S.snap.node.tilesCapKB) || 2048;
      send.disabled = !pack.length;
      this.tlMsg.textContent = !pack.length ? "No zoom/x/y.png tiles in that folder." : pack.length + " tiles, " + kb + " KB" + (kb > cap ? ": more than the node keeps (" + cap + " KB), so the most zoomed-in ones won't all fit. Choose fewer zoom levels." : "");
    };
    send.onclick = async () => {
      if (DEMO) { toast("Not in the demo"); return; }
      send.disabled = dir.disabled = true; tbar.style.display = ""; let ok = 0, err = "";
      for (let i = 0; i < pack.length; i++) {
        const p = pack[i], r = await API.tile(p.z, p.x, p.y, p.f);
        if (r.ok) ok++; else if (r.full) { err = r.error; break; } else err = r.error;
        tbi.style.width = Math.round(((i + 1) / pack.length) * 100) + "%";
        this.tlMsg.textContent = "Sending " + (i + 1) + " of " + pack.length + (ok < i + 1 ? " (" + (i + 1 - ok) + " not taken)" : "");
      }
      this.tlMsg.textContent = ok + " of " + pack.length + " tiles saved on the node" + (err ? ". Last problem: " + err : ".");
      send.disabled = dir.disabled = false; toast(ok + " map tiles saved");
    };
    clear.onclick = async () => {
      if (!clearArmed) { clearArmed = true; clear.textContent = "Tap again to clear"; setTimeout(() => { clearArmed = false; clear.textContent = "Clear saved tiles"; }, 4000); return; }
      clearArmed = false; clear.textContent = "Clear saved tiles";
      toast(DEMO ? "Not in the demo" : (await API.clearTiles()) ? "Saved tiles cleared" : "Couldn't clear them");
    };
    trow.append(dir, send, clear); tl.append(tnote, trow, tbar, this.tlMsg);
    grid.append(id, radio, sys, bat, mem, load, net, fw, tl);
    v.append(grid);
  },
  render() {
    const d = S.snap, n = d.node, r = d.radio || {}, h = d.hist || {}, st = d.stats || {};
    const kv = (box, rows) => { box.replaceChildren(); for (const [k, v] of rows) if (v !== undefined && v !== null && v !== "") box.append(el("span", "", k), el("span", "", v)); };
    const up = d.up || 0;
    kv(this.id, [["Name", n.name], ["Board", n.board], ["Firmware", n.fw], ["Key", (n.key || "").toUpperCase()], ["Up for", up > 86400 ? Math.floor(up / 86400) + " d " + Math.floor((up % 86400) / 3600) + " h" : Math.floor(up / 3600) + " h " + Math.floor((up % 3600) / 60) + " min"],
      ["Position", myPos() ? myPos().lat.toFixed(4) + ", " + myPos().lon.toFixed(4) + (n.posSrc ? " (" + n.posSrc + ")" : "") : "not set"],
      ["GPS", n.gps ? (n.gps.on ? (n.gps.fix ? "fix, " : "searching, ") + n.gps.sats + " satellites" : "off") : "none"], ["Wi-Fi", n.rssi ? n.rssi + " dBm" : ""]]);
    kv(this.radio, [["Frequency", r.freq ? r.freq.toFixed(3) + " MHz" : ""], ["Bandwidth", r.bw ? r.bw + " kHz" : ""], ["Spreading factor", r.sf ? "SF" + r.sf : ""], ["Coding rate", r.cr ? "4/" + r.cr : ""], ["Transmit power", r.tx ? r.tx + " dBm" : ""],
      ["Heard", (st.rx || 0) + " (" + (st.flood || 0) + " flood, " + (st.direct || 0) + " direct)"], ["Sent", String(st.tx || 0)], ["Duplicates dropped", String(st.dups || 0)], ["Last signal", st.lastRssi ? st.lastRssi + " dBm / " + st.lastSnr + " dB" : "-"]]);
    this.sys.replaceChildren();
    const meter = (label, used, total, col, txt) => { const w = el("div"); w.style.marginBottom = "12px"; const t = el("div"); t.style.cssText = "display:flex;font-size:13px"; t.append(el("span", "muted", label), el("span", "grow"), el("span", "", txt)); const m = el("div", "meter"), i = el("i"); i.style.width = clamp((used / total) * 100, 0, 100) + "%"; i.style.setProperty("--c", css(col)); m.append(i); w.append(t, m); this.sys.append(w); };
    if (n.heap) meter("Working memory free", n.heap, n.heapTotal || 327680, "--green", Math.round(n.heap / 1024) + " KB");
    if (n.psram) meter("Extra memory (PSRAM) free", n.psram, n.psramTotal || 8388608, "--cyan", (n.psram / 1048576).toFixed(1) + " MB");
    const ld = d.load || {};
    if (ld.loop !== undefined) meter("Main loop speed", Math.min(ld.loop, 10000), 10000, ld.longest > 40 ? "--amber" : "--green", ld.loop + "/s, longest " + ld.longest + " ms");
    meter("Processor", n.cpu || 240, 240, "--violet", (n.cpu || "-") + " MHz" + (n.cpu && n.cpu < 240 ? " (screen off)" : ""));
    const step = h.step || 300, end = h.end || nowS(), xy = (a, f) => (a || []).map((v, i, arr) => [end - (arr.length - 1 - i) * step, v ? f(v) : null]);
    this.bat.set([{ name: "Battery", color: "--amber", data: xy(h.batt, (v) => v / 1000) }]);
    this.mem.set([{ name: "Working memory", color: "--green", data: xy(h.heap, (v) => v) }]);
    this.load.set([{ name: "Screen", color: "--violet", data: xy(h.lvgl, (v) => v) }]);
    this.net.replaceChildren();
    const box = el("div", "kvlist");
    for (const [k, v] of [["Server", (d.feed && d.feed.url) || "-"], ["In this browser", !Feed.want() ? "off" : S.feedState === "refused" ? "refused: the server doesn't accept browsers from other sites (the node's own screen can still show it)" : S.feedState], ["Packets this visit", String(S.feedCount)], ["On the node's own screen", d.feed && d.feed.device ? "on" : "off (saves the node's processor)"]]) box.append(el("span", "", k), el("span", "", v));
    this.net.append(el("p", "muted", "The page connects your browser to the CoreScope server itself, so the internet feed costs the node nothing. Channel messages are decoded here with the channels the node shares with this page."), box);
    this.net.firstChild.style.cssText = "margin:0 0 10px;font-size:12.5px";
    this.feedBtn.textContent = Feed.want() ? "Turn the internet feed off here" : "Turn the internet feed on here";
    this.net.append(this.feedBtn.parentNode);
    kv(this.fwInfo, [["Running", (n.fw || "-") + (n.built ? ", built " + n.built : "")], ["App slot", n.slot],
      ["State", n.trial === undefined ? "" : n.trial ? "on trial: kept after a healthy minute" : "kept"]]);
    kv(this.tlInfo, [["Saved on the node", n.tiles === undefined ? "" : n.tiles + " tiles, " + n.tilesKB + " KB of " + n.tilesCapKB + " KB"]]);
  },
};

// ------------------------------------------------------------------ sign in, and the loop that keeps the page fresh
const Poll = {
  t: 0, fails: 0,
  hold: 0, // until then: an update is going up, or the node is restarting into it
  async tick() {
    clearTimeout(this.t);
    if (Date.now() < this.hold) { this.t = setTimeout(() => this.tick(), 1000); return; }
    try {
      const d = await API.state();
      if (!d) { showLogin(); return; }
      this.fails = 0;
      const first = !S.snap;
      takeSnapshot(d);
      if (first) showApp();
      if (d.feed && d.feed.url) Feed.start(d.feed.url);
      UI.render();
    } catch (e) {
      this.fails++;
      if (this.fails > 2) { const c = $("chips"); if (c) { c.replaceChildren(); const x = el("span", "chip bad"); x.append(el("i"), "Node not reachable, retrying"); c.append(x); } }
    }
    this.t = setTimeout(() => this.tick(), document.hidden ? 10000 : 2000);
  },
};
function showApp() {
  $("login").classList.add("hidden"); $("app").classList.remove("hidden"); document.body.classList.add("app");
  Mesh.stop();
  UI.go((location.hash || "#overview").slice(1));
}
function showLogin() {
  Feed.stop(); S.snap = null;
  $("app").classList.add("hidden"); $("login").classList.remove("hidden"); document.body.classList.remove("app");
  Mesh.start();
  const first = document.querySelector(".pin input"); if (first) setTimeout(() => first.focus(), 50);
}
// the sign-in background: a slow mesh of nodes passing packets
const Mesh = {
  raf: 0,
  start() {
    const c = $("mesh"); if (!c || this.raf) return;
    const N = 34, pts = Array.from({ length: N }, () => ({ x: Math.random(), y: Math.random(), vx: (Math.random() - 0.5) * 0.00012, vy: (Math.random() - 0.5) * 0.00012 })), pulses = [];
    const step = (t) => {
      const [g, w, h] = fitCanvas(c, c.getBoundingClientRect().height || innerHeight);
      g.clearRect(0, 0, w, h);
      for (const p of pts) { p.x += p.vx * 16; p.y += p.vy * 16; if (p.x < 0 || p.x > 1) p.vx *= -1; if (p.y < 0 || p.y > 1) p.vy *= -1; }
      const lim = Math.min(w, h) * 0.22, col = css("--cyan");
      g.lineWidth = 1;
      for (let i = 0; i < N; i++) for (let j = i + 1; j < N; j++) {
        const a = pts[i], b = pts[j], d = Math.hypot((a.x - b.x) * w, (a.y - b.y) * h);
        if (d < lim) { g.strokeStyle = col; g.globalAlpha = (1 - d / lim) * 0.22; g.beginPath(); g.moveTo(a.x * w, a.y * h); g.lineTo(b.x * w, b.y * h); g.stroke(); }
      }
      if (Math.random() < 0.03) { const a = Math.floor(Math.random() * N); let b = -1, bd = lim; pts.forEach((p, j) => { const d = Math.hypot((p.x - pts[a].x) * w, (p.y - pts[a].y) * h); if (j !== a && d < bd) { bd = d; b = j; } }); if (b >= 0) pulses.push({ a, b, t0: t }); }
      for (let k = pulses.length - 1; k >= 0; k--) { const p = pulses[k], f = (t - p.t0) / 900; if (f > 1) { pulses.splice(k, 1); continue; } const A = pts[p.a], B = pts[p.b]; g.globalAlpha = 1 - f * 0.5; g.fillStyle = col; g.beginPath(); g.arc((A.x + (B.x - A.x) * f) * w, (A.y + (B.y - A.y) * f) * h, 2.2, 0, 7); g.fill(); }
      g.globalAlpha = 0.8; g.fillStyle = col; for (const p of pts) { g.beginPath(); g.arc(p.x * w, p.y * h, 1.8, 0, 7); g.fill(); }
      g.globalAlpha = 1;
      this.raf = requestAnimationFrame(step);
    };
    this.raf = requestAnimationFrame(step);
  },
  stop() { cancelAnimationFrame(this.raf); this.raf = 0; },
};
function buildLogin() {
  const boxes = [...document.querySelectorAll(".pin input")], err = $("loginErr"), btn = $("loginBtn");
  const pin = () => boxes.map((b) => b.value).join("");
  boxes.forEach((b, i) => {
    b.addEventListener("input", () => { b.value = b.value.replace(/\D/g, "").slice(-1); if (b.value && i < boxes.length - 1) boxes[i + 1].focus(); if (pin().length === boxes.length) submit(); });
    b.addEventListener("keydown", (e) => { if (e.key === "Backspace" && !b.value && i) boxes[i - 1].focus(); if (e.key === "Enter") submit(); });
    b.addEventListener("paste", (e) => { const t = (e.clipboardData.getData("text") || "").replace(/\D/g, ""); if (t) { e.preventDefault(); boxes.forEach((x, k) => (x.value = t[k] || "")); if (t.length >= boxes.length) submit(); } });
  });
  async function submit() {
    if (pin().length !== boxes.length || btn.disabled) return;
    btn.disabled = true; btn.textContent = "Signing in..."; err.textContent = "";
    try {
      const r = await API.login(pin());
      if (r.ok) { boxes.forEach((b) => (b.value = "")); Poll.tick(); }
      else { err.textContent = r.error || "That PIN didn't work"; if (r.wait) err.textContent += " Try again in " + r.wait + " s."; boxes.forEach((b) => (b.value = "")); boxes[0].focus(); }
    } catch (e) { err.textContent = "Couldn't reach the node."; }
    btn.disabled = false; btn.textContent = "Sign in";
  }
  btn.onclick = submit;
}
function buildShell() {
  const nav = $("nav");
  for (const [v, t] of VIEWS) {
    const b = el("button"); b.dataset.v = v; b.append(svgIcon(ICON[v]), el("span", "", t));
    if (v === "messages") { const bd = el("span", "badge hidden", "0"); bd.id = "msgBadge"; b.append(bd); }
    b.onclick = () => UI.go(v); nav.append(b);
  }
  const theme = () => { const day = document.body.classList.contains("day"); $("themeBtn").replaceChildren(svgIcon(day ? "M12 3a9 9 0 1 0 9 9 7 7 0 0 1-9-9z" : "M12 4V2m0 20v-2m8-8h2M2 12h2m13.7-5.7 1.4-1.4M4.9 19.1l1.4-1.4m0-11.4L4.9 4.9m14.2 14.2-1.4-1.4M12 7a5 5 0 1 1 0 10 5 5 0 0 1 0-10z")); };
  if (store("day") === "1") document.body.classList.add("day");
  theme();
  $("themeBtn").onclick = () => { document.body.classList.toggle("day"); store("day", document.body.classList.contains("day") ? "1" : "0"); theme(); charts.forEach((c) => c.draw()); UI.render(); };
  $("logoutBtn").onclick = async () => { await API.logout(); showLogin(); };
  window.addEventListener("hashchange", () => { if (S.snap) UI.go(location.hash.slice(1)); });
  document.addEventListener("visibilitychange", () => { if (!document.hidden && S.snap) Poll.tick(); });
}
buildShell(); buildLogin();
if (DEMO && DEMO.autologin) Poll.tick(); else { showLogin(); Poll.tick(); }
