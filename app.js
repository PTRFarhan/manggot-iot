// =====================================================================
//  Manggot IoT - aplikasi monitoring inkubator larva BSF
//  Vanilla JS + Firebase (Auth + Realtime Database). Tanpa framework.
// =====================================================================
import { firebaseConfig, DEVICE_ID, SITE_URL } from "./firebase-config.js";

// true jika berjalan di dalam aplikasi Android (Capacitor)
const CAP = !!(window.Capacitor && window.Capacitor.isNativePlatform && window.Capacitor.isNativePlatform());
const PLUG = (n) => (CAP && window.Capacitor.Plugins ? window.Capacitor.Plugins[n] : null);
const SITE = SITE_URL || location.href;

const APP_VERSION = "1.2.0";
const FB_VER = "10.12.2";
const DEV = `devices/${DEVICE_ID}`;
const $ = (id) => document.getElementById(id);
const qsa = (s, el = document) => [...el.querySelectorAll(s)];
const ALERTS = [
  [1, "Suhu terlalu rendah (kedinginan)", "err"],
  [2, "Suhu terlalu tinggi (kepanasan)", "err"],
  [4, "Kelembapan terlalu rendah", "warn"],
  [8, "Kelembapan terlalu tinggi", "warn"],
  [16, "Sensor SHT30 tidak terbaca, semua relay dimatikan", "err"],
];

// ---------------- util ----------------
const pad2 = (n) => String(n).padStart(2, "0");
const fmtDT = (ms, sec = false) => {
  const d = new Date(ms);
  return `${pad2(d.getDate())}/${pad2(d.getMonth() + 1)} ${pad2(d.getHours())}:${pad2(d.getMinutes())}${sec ? ":" + pad2(d.getSeconds()) : ""}`;
};
const fmtFull = (ms) => {
  const d = new Date(ms);
  return `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())} ${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
};
const fmtDur = (s) => {
  s = Math.max(0, Math.floor(s));
  const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
  return d ? `${d}h ${h}j` : h ? `${h}j ${m}m` : `${m}m ${s % 60}d`;
};
const ago = (ms) => {
  const s = Math.round(ms / 1000);
  if (s < 5) return "baru saja";
  if (s < 60) return `${s} dtk lalu`;
  if (s < 3600) return `${Math.floor(s / 60)} mnt lalu`;
  if (s < 86400) return `${Math.floor(s / 3600)} jam lalu`;
  return `${Math.floor(s / 86400)} hari lalu`;
};
const num = (v, d = 1) => (typeof v === "number" && isFinite(v) ? v.toFixed(d) : "--");
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const asList = (v) => (Array.isArray(v) ? v.filter(Boolean) : v && typeof v === "object" ? Object.values(v).filter(Boolean) : []);

function toast(msg, type = "") {
  const t = document.createElement("div");
  t.className = "toast " + type;
  t.textContent = msg;
  $("toasts").appendChild(t);
  setTimeout(() => t.remove(), 3200);
}
function errMsg(e) {
  const c = (e && e.code) || "";
  const map = {
    "auth/invalid-credential": "Email atau password salah",
    "auth/wrong-password": "Password salah",
    "auth/user-not-found": "Akun tidak ditemukan",
    "auth/too-many-requests": "Terlalu banyak percobaan, coba lagi nanti",
    "auth/email-already-in-use": "Email sudah dipakai",
    "auth/weak-password": "Password minimal 6 karakter",
    "auth/invalid-email": "Format email salah",
    "auth/network-request-failed": "Tidak ada koneksi internet",
    PERMISSION_DENIED: "Akses ditolak (cek peran / database rules)",
  };
  return map[c] || (e && e.message) || String(e);
}

// =====================================================================
//  LAPISAN DATA: Firebase asli atau Demo (simulasi)
// =====================================================================
async function makeFirebase() {
  const base = `https://www.gstatic.com/firebasejs/${FB_VER}/`;
  const [A, Au, D] = await Promise.all([
    import(base + "firebase-app.js"),
    import(base + "firebase-auth.js"),
    import(base + "firebase-database.js"),
  ]);
  const app = A.initializeApp(firebaseConfig);
  const auth = Au.getAuth(app);
  const db = D.getDatabase(app);
  let offset = 0;
  D.onValue(D.ref(db, ".info/serverTimeOffset"), (s) => (offset = s.val() || 0));
  const r = (p) => D.ref(db, p);
  return {
    demo: false,
    TS: D.serverTimestamp(),
    now: () => Date.now() + offset,
    onAuth: (cb) => Au.onAuthStateChanged(auth, (u) => cb(u ? { uid: u.uid, email: u.email } : null)),
    login: (e, p) => Au.signInWithEmailAndPassword(auth, e, p),
    logout: () => Au.signOut(auth),
    reset: (e) => Au.sendPasswordResetEmail(auth, e),
    async changePass(oldP, newP) {
      const u = auth.currentUser;
      await Au.reauthenticateWithCredential(u, Au.EmailAuthProvider.credential(u.email, oldP));
      await Au.updatePassword(u, newP);
    },
    sub: (p, cb) => D.onValue(r(p), (s) => cb(s.val()), (e) => console.warn(p, e)),
    subLast: (p, child, n, cb) => D.onValue(D.query(r(p), D.orderByChild(child), D.limitToLast(n)), (s) => cb(s.val()), (e) => console.warn(p, e)),
    get: async (p) => (await D.get(r(p))).val(),
    set: (p, v) => D.set(r(p), v),
    update: (p, v) => D.update(r(p), v),
    push: (p, v) => D.push(r(p), v),
    remove: (p) => D.remove(r(p)),
    async range(p, a, b) {
      const s = await D.get(D.query(r(p), D.orderByKey(), D.startAt(a), D.endAt(b)));
      return s.val() || {};
    },
    async keysBefore(p, b) {
      const s = await D.get(D.query(r(p), D.orderByKey(), D.endAt(b)));
      return Object.keys(s.val() || {});
    },
    async createUser(email, pass) {
      // akun dibuat lewat instance kedua supaya admin tidak ikut logout
      const sec = A.initializeApp(firebaseConfig, "sec" + Date.now());
      const sa = Au.getAuth(sec);
      try {
        const c = await Au.createUserWithEmailAndPassword(sa, email, pass);
        return c.user.uid;
      } finally {
        await Au.signOut(sa).catch(() => {});
        await A.deleteApp(sec).catch(() => {});
      }
    },
  };
}

function makeDemo() {
  const store = {};
  const subs = [];
  const TS = { __ts: true };
  const parts = (p) => p.split("/").filter(Boolean);
  const getP = (p) => parts(p).reduce((o, k) => (o == null ? undefined : o[k]), store);
  const setP = (p, v) => {
    const ks = parts(p);
    let o = store;
    ks.slice(0, -1).forEach((k) => {
      if (typeof o[k] !== "object" || o[k] === null) o[k] = {};
      o = o[k];
    });
    const last = ks[ks.length - 1];
    if (v === null || v === undefined) delete o[last];
    else o[last] = v;
  };
  const fix = (v) => {
    if (v === TS) return Date.now();
    if (Array.isArray(v)) return v.map(fix);
    if (v && typeof v === "object") {
      const o = {};
      for (const k in v) o[k] = fix(v[k]);
      return o;
    }
    return v;
  };
  const clone = (v) => (v === undefined ? null : JSON.parse(JSON.stringify(v)));
  const emit = () =>
    subs.forEach((s) => {
      let v = clone(getP(s.p));
      if (s.n && v) {
        const arr = Object.entries(v).sort((a, b) => (a[1][s.child] || 0) - (b[1][s.child] || 0)).slice(-s.n);
        v = Object.fromEntries(arr);
      }
      s.cb(v);
    });
  let user = null;
  const authCbs = [];

  // ---- data awal ----
  const now = Date.now();
  setP("users/demo", { name: "Admin Demo", email: "demo@manggot.app", role: "admin", created: now });
  setP("users/op1", { name: "Operator Kandang", email: "operator@manggot.app", role: "operator", created: now });
  setP(`${DEV}/config`, { mode: 0, sp: 30, hy: 0.5, wtl: 27, wth: 33, whl: 60, whh: 85, smax: 38, smin: 20, mc: false, mh: false, hs: 60, hsp: 70 });
  setP(`${DEV}/system`, { wifi: [{ s: "Redmi Note 14 5G", p: "********" }, { s: "Kandang-BSF", p: "********" }], rbEn: true, rbMode: 1, rbHours: 24, rbAt: "03:00", rlSwap: false, rlHigh: false });
  const hist = {};
  let t = 30, sim = { heat: false, cool: false };
  for (let s = Math.floor(now / 1000) - 3 * 86400; s < now / 1000; s += 120) {
    const amb = 30.4 + 2.8 * Math.sin((s / 86400) * 2 * Math.PI - 1.2);
    t += (amb - t) * 0.08 + (sim.heat ? 0.35 : 0) - (sim.cool ? 0.38 : 0) + (Math.random() - 0.5) * 0.08;
    if (t >= 30.5) { sim.cool = true; sim.heat = false; } else if (t <= 29.5) { sim.heat = true; sim.cool = false; }
    else { if (sim.cool && t <= 30) sim.cool = false; if (sim.heat && t >= 30) sim.heat = false; }
    const h = 70 + 6 * Math.sin(s / 20000) + (Math.random() - 0.5) * 2;
    hist["t" + s] = { t: +t.toFixed(2), h: +h.toFixed(1), f: 16 | (sim.cool ? 1 : 0) | (sim.heat ? 2 : 0), a: 0 };
  }
  setP(`${DEV}/history`, hist);
  [["ESP32", "Perangkat menyala (boot ke-12, v2.0.0)", 5400], ["Admin Demo", "Target suhu diubah ke 30 °C", 3600], ["ESP32", "Reboot otomatis harian", 86400]]
    .forEach(([u, m, ago], i) => setP(`${DEV}/events/e${i}`, { u, m, ts: now - ago * 1000 }));

  // ---- simulasi perangkat ----
  const dev = { t: 31.2, h: 71, cool: false, heat: false, lastSw: 0, boot: Date.now(), lastHist: Date.now() };
  function tick() {
    const c = getP(`${DEV}/config`) || {};
    const amb = 31;
    dev.t += (amb - dev.t) * 0.015 + (dev.heat ? 0.16 : 0) - (dev.cool ? 0.17 : 0) + (Math.random() - 0.5) * 0.04;
    dev.h += (70 - dev.h) * 0.05 + (Math.random() - 0.5) * 0.6 + (dev.heat ? -0.15 : 0);
    const nowMs = Date.now();
    const can = nowMs - dev.lastSw > 10000;
    let sf = 0;
    const sw = (k, v) => { if (dev[k] !== v) { if (v && !can) return; dev[k] = v; dev.lastSw = nowMs; } };
    if (c.mode === 1) {
      let wc = !!c.mc, wh = !!c.mh;
      if (wh && dev.t >= c.smax) { wh = false; sf |= 1; }
      if (wc && dev.t <= c.smin) { wc = false; sf |= 2; }
      if (!wc) dev.cool = false;
      if (!wh) dev.heat = false;
      if (wc && !dev.heat) sw("cool", true);
      if (wh && !dev.cool) sw("heat", true);
    } else {
      if (dev.t >= c.sp + c.hy) { dev.heat = false; sw("cool", true); }
      else if (dev.t <= c.sp - c.hy) { dev.cool = false; sw("heat", true); }
      else { if (dev.cool && dev.t <= c.sp) sw("cool", false); if (dev.heat && dev.t >= c.sp) sw("heat", false); }
    }
    let al = 0;
    if (dev.t < c.wtl) al |= 1; if (dev.t > c.wth) al |= 2; if (dev.h < c.whl) al |= 4; if (dev.h > c.whh) al |= 8;
    setP(`${DEV}/status`, {
      t: +dev.t.toFixed(2), h: +dev.h.toFixed(1), ok: true, cool: dev.cool, heat: dev.heat, mode: c.mode, al, sf,
      rssi: -58 - Math.round(Math.random() * 6), ssid: "Redmi Note 14 5G", ip: "192.168.43.120",
      up: Math.floor((nowMs - dev.boot) / 1000) + 51234, fw: "2.0.0", q: 0, ap: false, boot: 12, heap: 182000,
      seen: nowMs, ota: { st: "idle", msg: "", p: 0 },
    });
    if (nowMs - dev.lastHist > (c.hs || 60) * 1000) {
      dev.lastHist = nowMs;
      setP(`${DEV}/history/t${Math.floor(nowMs / 1000)}`, { t: +dev.t.toFixed(2), h: +dev.h.toFixed(1), f: 16 | (dev.cool ? 1 : 0) | (dev.heat ? 2 : 0) | (c.mode ? 4 : 0), a: al });
    }
    const sys = getP(`${DEV}/system`) || {};
    if (sys.rebootReq && sys.rebootReq !== dev.rbAck) {
      dev.rbAck = sys.rebootReq; dev.boot = Date.now() + 4000;
      setP(`${DEV}/events/e${nowMs}`, { u: "ESP32", m: "Reboot dari aplikasi", ts: nowMs });
    }
    emit();
  }
  tick();
  setInterval(tick, 2000);

  return {
    demo: true,
    TS,
    now: () => Date.now(),
    onAuth(cb) { authCbs.push(cb); setTimeout(() => cb(user), 50); },
    async login() { user = { uid: "demo", email: "demo@manggot.app" }; authCbs.forEach((f) => f(user)); },
    async logout() { user = null; authCbs.forEach((f) => f(null)); },
    async reset() {},
    async changePass() {},
    sub(p, cb) { const s = { p, cb }; subs.push(s); cb(clone(getP(p))); return () => subs.splice(subs.indexOf(s), 1); },
    subLast(p, child, n, cb) { const s = { p, cb, child, n }; subs.push(s); emit(); return () => subs.splice(subs.indexOf(s), 1); },
    async get(p) { return clone(getP(p)); },
    async set(p, v) { setP(p, fix(v)); emit(); },
    async update(p, v) { for (const k in v) setP(p + "/" + k, fix(v[k])); emit(); },
    async push(p, v) { setP(p + "/d" + Date.now() + Math.random().toString(36).slice(2, 6), fix(v)); emit(); },
    async remove(p) { setP(p, null); emit(); },
    async range(p, a, b) { const o = getP(p) || {}; const r = {}; for (const k in o) if (k >= a && k <= b) r[k] = o[k]; return clone(r); },
    async keysBefore(p, b) { return Object.keys(getP(p) || {}).filter((k) => k <= b); },
    async createUser() { return "u" + Date.now(); },
  };
}

// =====================================================================
//  STATE
// =====================================================================
const S = {
  api: null, user: null, me: null, users: {}, status: null, config: null, system: null,
  online: false, draft: null, dirty: false, hist: { rows: [], from: 0, to: 0, hours: 24 }, tableN: 100,
  prevAlerts: 0, unsubs: [], fwAvail: null,
};
const isAdmin = () => S.me && S.me.role === "admin";
const myName = () => (S.me && S.me.name) || (S.user && S.user.email) || "?";
function log(m) { S.api.push(`${DEV}/events`, { ts: S.api.TS, u: myName(), m }).catch(() => {}); }

// =====================================================================
//  ANIMASI LARVA (dibuat sekali)
// =====================================================================
function larvaSVG(x, y, s, flip, d) {
  let segs = "";
  for (let i = 7; i >= 0; i--) {
    const rx = 5.6 - Math.max(0, i - 4) * 0.55, ry = 5 - Math.abs(i - 2) * 0.28;
    segs += `<ellipse class="seg" cx="${i * 6.4}" cy="0" rx="${rx}" ry="${ry}"/>`;
  }
  return `<g transform="translate(${x} ${y}) scale(${flip ? -s : s} ${s})"><g class="larva" style="--d:${d}s">
    ${segs}
    <ellipse class="seg head" cx="-5" cy="0.3" rx="5" ry="4.4"/>
    <circle class="eye" cx="-7.2" cy="-1.4" r=".95"/><circle class="eye" cx="-3.6" cy="-1.8" r=".95"/>
    <circle class="blush" cx="-3.2" cy="1.3" r="1.3" fill="#f9a8a8" opacity=".7"/>
    <path class="mouth m-ok" d="M-8 1.3 q1.6 1.8 3.2 0"/>
    <ellipse class="mouth m-hot" cx="-6.4" cy="2" rx="1.2" ry="1.5" fill="#7a2e1a" stroke="none"/>
    <path class="mouth m-cold" d="M-9 2 l.9 -.8 l.9 .8 l.9 -.8 l.9 .8"/>
    <path class="sweat" d="M-1 -9 q-2 3 0 4.2 q2 -1.2 0 -4.2z" fill="#7dd3fc"/>
    <path class="ice" d="M20 -6 l0 -5 M17.8 -8.5 l4.4 0 M18.3 -10.2 l3.4 3.4 M21.7 -10.2 l-3.4 3.4" stroke="#e0f6ff" stroke-width=".9"/>
  </g></g>`;
}
function buildLarvae() {
  $("larvae").innerHTML = [
    [62, 187, 1.1, false, 0], [176, 188, 1.15, false, 1.1], [160, 204, 1.05, true, 0.5], [302, 203, 1.0, true, 0.3], [250, 186, 0.9, false, 1.6],
  ].map((a) => larvaSVG(...a)).join("");
}

// =====================================================================
//  RENDER STATUS
// =====================================================================
function computeOnline() {
  const st = S.status;
  S.online = !!(st && st.seen && S.api.now() - st.seen < 20000);
  return S.online;
}

function renderStatus() {
  const st = S.status || {}, c = S.config || {};
  const online = computeOnline();
  const pill = $("pillOnline");
  pill.className = "pill " + (online ? "on" : st.seen ? "off" : "");
  pill.querySelector("span").textContent = online ? "Online" : "Offline";
  $("hdrSub").textContent = st.seen ? (online ? "Data langsung" : "Terakhir " + ago(S.api.now() - st.seen)) : "Belum ada data";

  const ok = online && st.ok && typeof st.t === "number";
  const t = st.t, h = st.h, sp = c.sp ?? 30, hy = c.hy ?? 0.5;
  // mood
  let mood = "off";
  if (online && !st.ok) mood = "err";
  else if (ok) mood = t >= sp + hy ? "hot" : t <= sp - hy ? "cold" : "ok";
  const scene = $("scene");
  scene.setAttribute("class", `inc mood-${mood}${online && st.heat ? " heat-on" : ""}${online && st.cool ? " cool-on" : ""}`);
  $("sceneLcd").textContent = ok ? `${num(t)} °C` : "--.- °C";

  const MOODS = {
    off: ["💤", "Perangkat offline", st.seen ? "Terakhir terlihat " + ago(S.api.now() - st.seen) : "Belum pernah terhubung"],
    err: ["⚠️", "Sensor error", "Sensor tidak terbaca, relay dimatikan demi keamanan"],
    hot: ["🥵", "Manggot kepanasan!", ""],
    cold: ["🥶", "Manggot kedinginan", ""],
    ok: ["😊", "Manggot nyaman", ""],
  };
  let [emo, title, text] = MOODS[mood];
  if (mood === "hot" || mood === "cold" || mood === "ok") {
    const diff = t - sp;
    text = `${diff >= 0 ? "+" : ""}${diff.toFixed(1)} °C dari target. `;
    text += st.cool ? "Peltier + kipas sedang mendinginkan." : st.heat ? "Lampu pemanas sedang menghangatkan." : mood === "ok" ? "Suhu stabil di zona ideal." : c.mode === 1 ? "Mode manual, aktuator mati." : "Menunggu jeda relay.";
  }
  $("moodEmoji").textContent = emo;
  $("moodTitle").textContent = title;
  $("moodText").textContent = text;

  // kartu
  $("vTemp").textContent = ok ? num(t) : "--.-";
  $("vHum").textContent = ok ? num(h, 0) : "--";
  $("vTarget").textContent = num(sp);
  $("vHyst").textContent = num(hy);
  const hsp = c.hsp ?? 70;
  $("vHumRange").textContent = `target ${hsp}% · aman ${c.whl ?? "-"}–${c.whh ?? "-"}%`;
  const pct = (v, a, b) => Math.max(0, Math.min(100, ((v - a) / (b - a)) * 100));
  $("mTemp").style.width = ok ? pct(t, 20, 40) + "%" : "0";
  $("mTarget").style.left = pct(sp, 20, 40) + "%";
  $("mHum").style.width = ok ? pct(h, 0, 100) + "%" : "0";
  $("mHumT").style.left = pct(hsp, 0, 100) + "%";
  const tS = $("tState"), hS = $("hState");
  tS.className = ""; hS.className = "";
  if (ok) {
    if (mood === "hot") { tS.textContent = "Panas"; tS.className = "em-hot"; }
    else if (mood === "cold") { tS.textContent = "Dingin"; tS.className = "em-cold"; }
    else { tS.textContent = "Ideal"; tS.className = "em-ok"; }
    if (h < c.whl) { hS.textContent = "Terlalu kering"; hS.className = "em-hot"; }
    else if (h > c.whh) { hS.textContent = "Terlalu lembap"; hS.className = "em-cold"; }
    else if (Math.abs(h - hsp) <= 5) { hS.textContent = "Ideal"; hS.className = "em-ok"; }
    else { hS.textContent = h < hsp ? "Agak kering" : "Agak lembap"; hS.className = ""; }
  } else { tS.textContent = "-"; hS.textContent = "-"; }

  const setAct = (id, on) => { const e = $(id); e.classList.toggle("on", !!on); e.querySelector(".act-st").textContent = on ? "MENYALA" : "OFF"; };
  setAct("actHeat", online && st.heat);
  setAct("actCool", online && st.cool);

  // peringatan
  const al = online ? st.al || 0 : 0;
  let html = ALERTS.filter(([b]) => al & b).map(([, m, k]) => `<div class="alert ${k}">⚠️ ${m}</div>`).join("");
  if (online && st.sf) html += `<div class="alert warn">🛡️ Batas aman mode manual aktif: ${st.sf & 1 ? "pemanas" : "pendingin"} diputus</div>`;
  if (online && st.ap) html += `<div class="alert warn">📶 Hotspot setup MANGGOT-SETUP sedang aktif</div>`;
  $("alertBox").innerHTML = html;
  notifyAlerts(al);

  $("iMode").textContent = c.mode === 1 ? "MANUAL" : "AUTO";
  $("iWifi").textContent = online ? `${st.rssi} dBm` : "-";
  $("iUp").textContent = online ? fmtDur(st.up) : "-";
  $("iQ").textContent = st.q ?? "-";

  renderManualButtons();
  renderDevInfo();
  renderOta();
}

function notifyAlerts(al) {
  const added = al & ~S.prevAlerts;
  S.prevAlerts = al;
  if (!added) return;
  ALERTS.filter(([b]) => added & b).forEach(([, m]) => {
    toast("⚠️ " + m, "err");
    const LN = PLUG("LocalNotifications");
    if (LN) LN.schedule({ notifications: [{ id: Math.floor(Math.random() * 1e6), title: "Inkubator Manggot", body: m }] }).catch(() => {});
    else if (document.hidden && "Notification" in window && Notification.permission === "granted") {
      try { new Notification("Inkubator Manggot", { body: m, icon: "icons/icon-192.png", tag: "manggot" + m }); } catch (e) {}
    }
    if (navigator.vibrate) navigator.vibrate([120, 60, 120]);
  });
}

function renderManualButtons() {
  const st = S.status || {}, c = S.config || {};
  const manual = c.mode === 1;
  const seg = $("segMode");
  seg.classList.toggle("m1", manual);
  qsa("button", seg).forEach((b) => b.classList.toggle("on", +b.dataset.m === (manual ? 1 : 0)));
  $("modeDesc").textContent = manual
    ? "MANUAL: kamu yang menyalakan pemanas atau pendingin. Batas aman tetap aktif."
    : `AUTO: pemanas menyala di bawah ${num((c.sp ?? 30) - (c.hy ?? 0.5))} °C, pendingin menyala di atas ${num((c.sp ?? 30) + (c.hy ?? 0.5))} °C.`;
  $("manualLock").classList.toggle("hidden", manual);
  const set = (id, req, act, sfBit) => {
    const b = $(id);
    b.classList.toggle("req", !!req);
    b.classList.toggle("on", !!act && S.online);
    b.querySelector("b").textContent = act && S.online ? "MENYALA" : "OFF";
    let note = "";
    if (!S.online) note = "perangkat offline";
    else if (req && !act) note = st.sf & sfBit ? "diputus batas aman" : "menunggu jeda relay...";
    else if (!req && act) note = "mematikan...";
    else if (req && act) note = "ketuk untuk mematikan";
    b.querySelector("small").textContent = manual ? note : "";
  };
  set("bHeat", c.mh, st.heat, 1);
  set("bCool", c.mc, st.cool, 2);
}

function renderDevInfo() {
  const st = S.status || {};
  const on = S.online;
  const items = [
    ["Status", on ? "🟢 Online" : "🔴 Offline"],
    ["Terakhir terlihat", st.seen ? ago(S.api.now() - st.seen) : "-"],
    ["Firmware", st.fw ? "v" + st.fw : "-"],
    ["WiFi", st.ssid ? `${st.ssid} (${st.rssi} dBm)` : "-"],
    ["Alamat IP", st.ip || "-"],
    ["Aktif sejak boot", st.up != null ? fmtDur(st.up) : "-"],
    ["Boot ke-", st.boot ?? "-"],
    ["Memori bebas", st.heap ? Math.round(st.heap / 1024) + " KB" : "-"],
    ["Riwayat antri", st.q ?? "-"],
    ["Hotspot setup", st.ap ? "Aktif" : "Mati"],
  ];
  $("devInfo").innerHTML = items.map(([k, v]) => `<div><small>${k}</small><b>${esc(v)}</b></div>`).join("");
}

// =====================================================================
//  KONTROL & SETELAN
// =====================================================================
const PARAMS = [["pHy", "hy"], ["pWtl", "wtl"], ["pWth", "wth"], ["pWhl", "whl"], ["pWhh", "whh"], ["pSmax", "smax"], ["pSmin", "smin"], ["pHs", "hs"]];

function loadDraft() {
  const c = S.config || {};
  S.draft = { sp: c.sp ?? 30, hy: c.hy ?? 0.5, wtl: c.wtl ?? 27, wth: c.wth ?? 33, whl: c.whl ?? 60, whh: c.whh ?? 85, smax: c.smax ?? 38, smin: c.smin ?? 20, hs: c.hs ?? 60, hsp: c.hsp ?? 70 };
  S.dirty = false;
  fillDraft();
}
function fillDraft() {
  const d = S.draft;
  $("spVal").textContent = num(d.sp);
  $("spRange").value = d.sp;
  $("hspVal").textContent = Math.round(d.hsp);
  $("hspRange").value = d.hsp;
  PARAMS.forEach(([id, k]) => { if (document.activeElement !== $(id)) $(id).value = d[k]; });
  $("saveBar").classList.toggle("hidden", !S.dirty);
}
function markDirty() { S.dirty = true; $("saveBar").classList.remove("hidden"); }
function setSp(v) {
  S.draft.sp = Math.round(Math.max(20, Math.min(40, v)) * 2) / 2;
  markDirty();
  fillDraft();
}
function setHsp(v) {
  S.draft.hsp = Math.round(Math.max(30, Math.min(95, v)));
  markDirty();
  fillDraft();
}
async function saveConfig() {
  const d = S.draft;
  if (d.wtl >= d.wth) return toast("Peringatan suhu min harus lebih kecil dari maks", "err");
  if (d.whl >= d.whh) return toast("Peringatan RH min harus lebih kecil dari maks", "err");
  const old = S.config || {};
  try {
    await S.api.update(`${DEV}/config`, { ...d });
    const ch = [];
    if (old.sp !== d.sp) ch.push(`target ${num(old.sp)} → ${num(d.sp)} °C`);
    if (old.hsp !== d.hsp) ch.push(`target RH ${old.hsp ?? "-"} → ${d.hsp} %`);
    ["hy", "wtl", "wth", "whl", "whh", "smax", "smin", "hs"].forEach((k) => { if (old[k] !== d[k]) ch.push(`${k} ${old[k]} → ${d[k]}`); });
    if (ch.length) log("Setelan diubah: " + ch.join(", "));
    S.dirty = false;
    $("saveBar").classList.add("hidden");
    toast("Setelan tersimpan, perangkat akan menyesuaikan", "ok");
  } catch (e) { toast(errMsg(e), "err"); }
}
async function setMode(m) {
  if ((S.config || {}).mode === m) return;
  try {
    await S.api.update(`${DEV}/config`, { mode: m, mc: false, mh: false });
    log(`Mode diubah ke ${m ? "MANUAL" : "AUTO"}`);
  } catch (e) { toast(errMsg(e), "err"); }
}
async function toggleManual(which) {
  const c = S.config || {};
  if (c.mode !== 1) return toast("Ubah ke mode MANUAL dulu");
  const key = which === "heat" ? "mh" : "mc";
  const on = !c[key];
  const upd = { [key]: on };
  if (on) upd[which === "heat" ? "mc" : "mh"] = false;   // interlock
  try {
    await S.api.update(`${DEV}/config`, upd);
    log(`${which === "heat" ? "Lampu pemanas" : "Peltier + kipas"} ${on ? "dinyalakan" : "dimatikan"} (manual)`);
  } catch (e) { toast(errMsg(e), "err"); }
}

// =====================================================================
//  GRAFIK (canvas ringan, tanpa library)
// =====================================================================
class LineChart {
  constructor(canvas, tip, empty) {
    this.c = canvas; this.tip = tip; this.empty = empty; this.rows = []; this.hover = -1;
    this.show = { t: true, h: true, s: true };
    new ResizeObserver(() => this.draw()).observe(canvas);
    canvas.addEventListener("pointermove", (e) => this.onMove(e));
    canvas.addEventListener("pointerleave", () => { this.hover = -1; this.tip.classList.add("hidden"); this.draw(); });
  }
  set(rows, opt) { this.opt = opt; this.rows = this.downsample(rows); this.hover = -1; this.draw(); }
  downsample(rows) {
    const W = Math.max(300, this.c.clientWidth || 600), max = Math.round(W * 1.2);
    if (rows.length <= max) return rows;
    const per = rows.length / max, out = [];
    for (let i = 0; i < max; i++) {
      const sl = rows.slice(Math.floor(i * per), Math.floor((i + 1) * per));
      const vt = sl.filter((r) => r.t != null), vh = sl.filter((r) => r.h != null);
      out.push({
        ts: sl[Math.floor(sl.length / 2)].ts,
        t: vt.length ? vt.reduce((a, r) => a + r.t, 0) / vt.length : null,
        h: vh.length ? vh.reduce((a, r) => a + r.h, 0) / vh.length : null,
        heat: sl.some((r) => r.heat), cool: sl.some((r) => r.cool), man: sl.some((r) => r.man),
      });
    }
    return out;
  }
  geom() {
    const W = this.c.clientWidth, H = this.c.clientHeight;
    return { W, H, l: 40, r: 40, t: 12, b: H - 52, band: H - 44 };
  }
  draw() {
    const cv = this.c, dpr = window.devicePixelRatio || 1;
    const { W, H, l, r, t: top, b, band } = this.geom();
    if (!W) return;
    cv.width = W * dpr; cv.height = H * dpr;
    const x = cv.getContext("2d");
    x.setTransform(dpr, 0, 0, dpr, 0, 0);
    x.clearRect(0, 0, W, H);
    const rows = this.rows, o = this.opt || {};
    this.empty.classList.toggle("hidden", rows.length > 0);
    if (!rows.length) return;
    const from = o.from, to = o.to, sp = o.sp;
    const tv = rows.filter((q) => q.t != null).map((q) => q.t);
    let tmin = Math.min(...tv, sp), tmax = Math.max(...tv, sp);
    tmin = Math.floor(tmin - 0.5); tmax = Math.ceil(tmax + 0.5);
    if (tmax - tmin < 4) { tmin -= 1; tmax += 1; }
    const X = (ts) => l + ((ts - from) / (to - from)) * (W - l - r);
    const YT = (v) => b - ((v - tmin) / (tmax - tmin)) * (b - top);
    const YH = (v) => b - (v / 100) * (b - top);
    this.X = X;
    x.font = "11px system-ui,sans-serif";
    const cs = getComputedStyle(document.documentElement);
    const cGrid = cs.getPropertyValue("--line").trim() || "#22302a";
    const cTxt = cs.getPropertyValue("--mut").trim() || "#8ea198";
    const cTx = cs.getPropertyValue("--tx").trim() || "#fff";
    const cCard = cs.getPropertyValue("--card").trim() || "#000";
    // grid
    x.strokeStyle = cGrid; x.lineWidth = 1; x.fillStyle = cTxt;
    for (let i = 0; i <= 5; i++) {
      const y = top + ((b - top) * i) / 5;
      x.beginPath(); x.moveTo(l, y); x.lineTo(W - r, y); x.stroke();
      x.textAlign = "right"; x.fillText((tmax - ((tmax - tmin) * i) / 5).toFixed(1), l - 6, y + 4);
      x.textAlign = "left"; x.fillText(Math.round(100 - 20 * i) + "%", W - r + 6, y + 4);
    }
    // sumbu waktu
    const span = (to - from) / 1000;
    const steps = [300, 600, 900, 1800, 3600, 7200, 10800, 21600, 43200, 86400, 172800];
    const step = steps.find((s) => span / s <= 7) || 172800;
    const tz = new Date().getTimezoneOffset() * 60;
    let tick = Math.ceil((from / 1000 - tz) / step) * step + tz;
    x.textAlign = "center";
    for (; tick * 1000 <= to; tick += step) {
      const px = X(tick * 1000);
      x.strokeStyle = cGrid; x.globalAlpha = 0.55; x.beginPath(); x.moveTo(px, top); x.lineTo(px, b); x.stroke(); x.globalAlpha = 1;
      const d = new Date(tick * 1000);
      const lab = step >= 86400 || (d.getHours() === 0 && d.getMinutes() === 0 && span > 43200) ? `${pad2(d.getDate())}/${pad2(d.getMonth() + 1)}` : `${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
      x.fillStyle = cTxt; x.fillText(lab, px, H - 8);
    }
    // pita aktuator
    const gap = this.gapLimit(rows);
    for (let i = 0; i < rows.length; i++) {
      const q = rows[i], nx = rows[i + 1];
      const w = nx && nx.ts - q.ts < gap ? X(nx.ts) - X(q.ts) : 2;
      if (q.heat) { x.fillStyle = "#f59e0b"; x.fillRect(X(q.ts), band, Math.max(1, w), 7); }
      if (q.cool) { x.fillStyle = "#38bdf8"; x.fillRect(X(q.ts), band + 10, Math.max(1, w), 7); }
    }
    x.fillStyle = cTxt; x.textAlign = "right"; x.font = "9px system-ui"; x.fillText("PMS", l - 6, band + 7); x.fillText("DGN", l - 6, band + 17);
    // target
    if (this.show.s) {
      x.setLineDash([6, 5]); x.strokeStyle = cTx; x.globalAlpha = 0.55; x.lineWidth = 1.2;
      x.beginPath(); x.moveTo(l, YT(sp)); x.lineTo(W - r, YT(sp)); x.stroke(); x.setLineDash([]); x.globalAlpha = 1;
    }
    const line = (key, Y, color, width, fill) => {
      x.strokeStyle = color; x.lineWidth = width; x.lineJoin = "round";
      x.beginPath();
      let pen = false, prev = null;
      for (const q of rows) {
        if (q[key] == null) { pen = false; continue; }
        if (prev && q.ts - prev.ts > gap) pen = false;
        const px = X(q.ts), py = Y(q[key]);
        pen ? x.lineTo(px, py) : x.moveTo(px, py);
        pen = true; prev = q;
      }
      x.stroke();
      if (fill) {
        const g = x.createLinearGradient(0, top, 0, b);
        g.addColorStop(0, fill); g.addColorStop(1, "rgba(0,0,0,0)");
        x.lineTo(X(prev ? prev.ts : to), b); x.lineTo(X(rows[0].ts), b); x.closePath();
        x.fillStyle = g; x.globalAlpha = 0.5; x.fill(); x.globalAlpha = 1;
      }
    };
    if (this.show.h) line("h", YH, "#60a5fa", 1.6);
    if (this.show.t) line("t", YT, "#f59e0b", 2.2, "rgba(245,158,11,.35)");
    // hover
    if (this.hover >= 0) {
      const q = rows[this.hover], px = X(q.ts);
      x.strokeStyle = cTx; x.globalAlpha = 0.35; x.lineWidth = 1;
      x.beginPath(); x.moveTo(px, top); x.lineTo(px, b + 24); x.stroke(); x.globalAlpha = 1;
      const dot = (y, c) => { x.fillStyle = c; x.beginPath(); x.arc(px, y, 4, 0, 7); x.fill(); x.strokeStyle = cCard; x.lineWidth = 2; x.stroke(); };
      if (this.show.h && q.h != null) dot(YH(q.h), "#60a5fa");
      if (this.show.t && q.t != null) dot(YT(q.t), "#f59e0b");
    }
  }
  gapLimit(rows) {
    if (rows.length < 3) return Infinity;
    const d = [];
    for (let i = 1; i < Math.min(rows.length, 200); i++) d.push(rows[i].ts - rows[i - 1].ts);
    d.sort((a, b) => a - b);
    return Math.max(d[Math.floor(d.length / 2)] * 4, 5 * 60000);
  }
  onMove(e) {
    if (!this.rows.length || !this.X) return;
    const rect = this.c.getBoundingClientRect(), mx = e.clientX - rect.left;
    let best = 0, bd = Infinity;
    let lo = 0, hi = this.rows.length - 1;
    while (hi - lo > 1) { const m = (lo + hi) >> 1; this.X(this.rows[m].ts) < mx ? (lo = m) : (hi = m); }
    [lo, hi].forEach((i) => { const d = Math.abs(this.X(this.rows[i].ts) - mx); if (d < bd) { bd = d; best = i; } });
    this.hover = best;
    const q = this.rows[best];
    this.tip.innerHTML = `<b>${fmtDT(q.ts)}</b><br>🌡️ ${num(q.t)} °C &nbsp; 💧 ${num(q.h, 0)} %<br>${q.heat ? "🔥 Pemanas ON" : q.cool ? "❄️ Pendingin ON" : "Aktuator OFF"}${q.man ? " · manual" : ""}`;
    this.tip.classList.remove("hidden");
    const px = this.X(q.ts), tw = this.tip.offsetWidth;
    this.tip.style.left = Math.min(Math.max(4, px + 12), rect.width - tw - 4) + "px";
    this.tip.style.top = "8px";
    this.draw();
  }
}
let chart;

// =====================================================================
//  RIWAYAT
// =====================================================================
function parseRows(obj) {
  return Object.entries(obj || {})
    .map(([k, v]) => {
      const f = v.f || 0;
      return { ts: parseInt(k.slice(1), 10) * 1000, t: typeof v.t === "number" ? v.t : null, h: typeof v.h === "number" ? v.h : null, cool: !!(f & 1), heat: !!(f & 2), man: !!(f & 4), a: v.a || 0 };
    })
    .filter((r) => r.ts > 0)
    .sort((a, b) => a.ts - b.ts);
}

async function loadHistory(fromMs, toMs) {
  S.hist.from = fromMs; S.hist.to = toMs;
  $("chartEmpty").textContent = "Memuat data...";
  $("chartEmpty").classList.remove("hidden");
  try {
    const obj = await S.api.range(`${DEV}/history`, "t" + Math.floor(fromMs / 1000), "t" + Math.ceil(toMs / 1000));
    S.hist.rows = parseRows(obj);
  } catch (e) {
    S.hist.rows = [];
    toast(errMsg(e), "err");
  }
  $("chartEmpty").textContent = "Belum ada data di rentang ini";
  S.tableN = 100;
  renderHistory();
}

function renderHistory() {
  const rows = S.hist.rows;
  chart.set(rows, { from: S.hist.from, to: S.hist.to, sp: (S.config && S.config.sp) || 30 });
  const tv = rows.filter((r) => r.t != null);
  const hv = rows.filter((r) => r.h != null);
  const avg = (a, k) => (a.length ? a.reduce((s, r) => s + r[k], 0) / a.length : NaN);
  const pctOn = (k) => (rows.length ? Math.round((rows.filter((r) => r[k]).length / rows.length) * 100) : 0);
  const sp = (S.config && S.config.sp) || 30, hy = (S.config && S.config.hy) || 0.5;
  const inZone = tv.length ? Math.round((tv.filter((r) => Math.abs(r.t - sp) <= hy).length / tv.length) * 100) : 0;
  const stats = [
    ["Suhu rata-rata", tv.length ? num(avg(tv, "t")) + " °C" : "-"],
    ["Min / Maks", tv.length ? `${num(Math.min(...tv.map((r) => r.t)))} / ${num(Math.max(...tv.map((r) => r.t)))}` : "-"],
    ["RH rata-rata", hv.length ? num(avg(hv, "h"), 0) + " %" : "-"],
    ["Waktu di zona ideal", tv.length ? inZone + " %" : "-"],
    ["Pemanas menyala", pctOn("heat") + " %"],
    ["Pendingin menyala", pctOn("cool") + " %"],
    ["Jumlah data", rows.length.toLocaleString("id-ID")],
    ["Rentang", fmtDT(S.hist.from) + " – " + fmtDT(S.hist.to)],
  ];
  $("histStats").innerHTML = stats.map(([k, v]) => `<div class="card stat4"><small>${k}</small><b>${v}</b></div>`).join("");
  renderTable();
}

function renderTable() {
  const rows = S.hist.rows;
  const show = rows.slice(-S.tableN).reverse();
  $("tblBody").innerHTML = show.map((r) =>
    `<tr><td>${fmtDT(r.ts, true)}</td><td>${num(r.t)}</td><td>${num(r.h, 0)}</td><td class="${r.heat ? "on-h" : ""}">${r.heat ? "ON" : "-"}</td><td class="${r.cool ? "on-c" : ""}">${r.cool ? "ON" : "-"}</td><td>${r.man ? "Manual" : "Auto"}</td></tr>`
  ).join("") || `<tr><td colspan="6" class="muted center">Tidak ada data</td></tr>`;
  $("btnMore").classList.toggle("hidden", rows.length <= S.tableN);
}

function downloadCsv() {
  const rows = S.hist.rows;
  if (!rows.length) return toast("Tidak ada data untuk diunduh", "err");
  const sep = $("csvSep").value;
  const dec = (v, d) => (v == null ? "" : sep === ";" ? v.toFixed(d).replace(".", ",") : v.toFixed(d));
  const alertTxt = (a) => ALERTS.filter(([b]) => a & b).map(([, m]) => m).join(" | ");
  const head = ["Waktu", "Suhu (C)", "Kelembapan (%)", "Pemanas", "Pendingin", "Mode", "Peringatan"];
  const lines = [head.join(sep)];
  rows.forEach((r) => lines.push([fmtFull(r.ts), dec(r.t, 2), dec(r.h, 1), r.heat ? "ON" : "OFF", r.cool ? "ON" : "OFF", r.man ? "MANUAL" : "AUTO", `"${alertTxt(r.a)}"`].join(sep)));
  const csv = "\ufeff" + lines.join("\r\n");
  const d = new Date(S.hist.from), e = new Date(S.hist.to);
  const fname = `riwayat-manggot_${d.getFullYear()}${pad2(d.getMonth() + 1)}${pad2(d.getDate())}-${e.getFullYear()}${pad2(e.getMonth() + 1)}${pad2(e.getDate())}.csv`;
  if (CAP) return shareCsvNative(fname, csv, rows.length);
  const blob = new Blob([csv], { type: "text/csv;charset=utf-8" });
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = fname;
  document.body.appendChild(a);
  a.click();
  setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 500);
  toast(`${rows.length} baris diunduh`, "ok");
}

// Di aplikasi Android: simpan CSV lalu buka menu bagikan (simpan ke Drive, WhatsApp, File, dll)
async function shareCsvNative(fname, csv, n) {
  const FS = PLUG("Filesystem"), SH = PLUG("Share");
  if (!FS || !SH) return toast("Fitur unduh belum tersedia di aplikasi ini", "err");
  try {
    const w = await FS.writeFile({ path: fname, data: csv, directory: "CACHE", encoding: "utf8" });
    await SH.share({ title: fname, text: `Riwayat inkubator manggot (${n} baris)`, files: [w.uri], dialogTitle: "Simpan / kirim CSV" });
  } catch (err) {
    if (!String(err).toLowerCase().includes("cancel")) toast("Gagal membagikan CSV: " + errMsg(err), "err");
  }
}

async function cleanHistory() {
  const days = +$("cleanDays").value;
  if (!confirm(`Hapus semua riwayat lebih lama dari ${days} hari? Tidak bisa dibatalkan.`)) return;
  try {
    const cutoff = "t" + Math.floor((S.api.now() - days * 86400000) / 1000);
    const keys = await S.api.keysBefore(`${DEV}/history`, cutoff);
    for (let i = 0; i < keys.length; i += 500) {
      const upd = {};
      keys.slice(i, i + 500).forEach((k) => (upd[k] = null));
      await S.api.update(`${DEV}/history`, upd);
    }
    log(`Riwayat lebih lama dari ${days} hari dihapus (${keys.length} data)`);
    toast(`${keys.length} data dihapus`, "ok");
    loadHistory(S.hist.from, S.hist.to);
  } catch (e) { toast(errMsg(e), "err"); }
}

function renderEvents(obj) {
  const arr = Object.values(obj || {}).filter((e) => e && e.ts).sort((a, b) => b.ts - a.ts);
  $("eventList").innerHTML = arr.map((e) => `<li><time>${fmtDT(e.ts)}</time><span><em>${esc(e.u)}</em>${esc(e.m)}</span></li>`).join("") || `<li class="muted">Belum ada aktivitas</li>`;
}

// =====================================================================
//  SISTEM: WiFi, reboot, firmware, pengguna
// =====================================================================
function renderSystem() {
  const sys = S.system || {};
  const list = asList(sys.wifi);
  const cur = S.online && S.status ? S.status.ssid : "";
  $("wifiCount").textContent = `${list.length}/5`;
  $("wifiList").innerHTML = list.map((w, i) =>
    `<li><span>${i + 1}.</span><div class="grow"><b>${esc(w.s)}</b><small>${w.p ? "••••••••" : "tanpa password"}</small></div>${i === 0 ? '<span class="tag main">utama</span>' : `<button class="icon-btn" data-wtop="${i}">Jadikan utama</button>`}${w.s === cur ? '<span class="tag live">terhubung</span>' : ""}<button class="icon-btn del" data-wdel="${i}">Hapus</button></li>`
  ).join("") || `<li class="muted">Belum ada WiFi tersimpan</li>`;
  if (!S.relayDirty) {
    $("rlSwap").checked = !!sys.rlSwap;
    $("rlHigh").value = sys.rlHigh ? "1" : "0";
  }
  renderRelayMap();

  if (document.activeElement && document.activeElement.closest && document.activeElement.closest("#rbOpts")) return;
  $("rbEn").checked = !!sys.rbEn;
  $("rbMode").value = String(sys.rbMode ?? 0);
  $("rbHours").value = sys.rbHours ?? 24;
  $("rbAt").value = sys.rbAt || "03:00";
  rbVisibility();
}
function rbVisibility() {
  const daily = $("rbMode").value === "1";
  $("rbHoursL").classList.toggle("hidden", daily);
  $("rbAtL").classList.toggle("hidden", !daily);
  $("rbOpts").style.opacity = $("rbEn").checked ? 1 : 0.5;
}
async function addWifi(e) {
  e.preventDefault();
  const s = $("wSsid").value.trim(), p = $("wPass").value;
  if (!s) return;
  if (p && p.length < 8) return toast("Password WiFi minimal 8 karakter", "err");
  const list = asList((S.system || {}).wifi).map((w) => ({ s: w.s, p: w.p || "" }));
  const i = list.findIndex((w) => w.s === s);
  if (i >= 0) list[i].p = p;
  else if (list.length >= 5) return toast("Maksimal 5 WiFi. Hapus salah satu dulu", "err");
  else list.push({ s, p });
  try {
    await S.api.set(`${DEV}/system/wifi`, list);
    log(`WiFi "${s}" ${i >= 0 ? "diperbarui" : "ditambahkan"}`);
    $("wifiForm").reset();
    toast("WiFi tersimpan, dikirim ke perangkat", "ok");
  } catch (err) { toast(errMsg(err), "err"); }
}
async function delWifi(i) {
  const list = asList((S.system || {}).wifi);
  if (list.length <= 1) return toast("Minimal harus ada 1 WiFi", "err");
  const w = list[i];
  if (S.online && S.status.ssid === w.s && !confirm(`Perangkat sedang memakai "${w.s}". Tetap hapus?`)) return;
  if (!confirm(`Hapus WiFi "${w.s}"?`)) return;
  list.splice(i, 1);
  try { await S.api.set(`${DEV}/system/wifi`, list.map((x) => ({ s: x.s, p: x.p || "" }))); log(`WiFi "${w.s}" dihapus`); }
  catch (e) { toast(errMsg(e), "err"); }
}
function renderRelayMap() {
  const sw = $("rlSwap").checked, hi = $("rlHigh").value === "1";
  $("relayMap").innerHTML = `<div><small>GPIO 26 (Relay 1)</small><b>${sw ? "🔥 Lampu pemanas" : "❄️ Peltier + kipas"}</b></div>
    <div><small>GPIO 27 (Relay 2)</small><b>${sw ? "❄️ Peltier + kipas" : "🔥 Lampu pemanas"}</b></div>
    <div style="grid-column:span 2"><small>Relay menyala saat pin</small><b>${hi ? "HIGH (3,3 V)" : "LOW (0 V)"}</b></div>`;
}
async function saveRelay() {
  const v = { rlSwap: $("rlSwap").checked, rlHigh: $("rlHigh").value === "1" };
  if (!confirm("Simpan pengaturan relay? Semua relay akan mati sebentar.")) return;
  try {
    await S.api.update(`${DEV}/system`, v);
    S.relayDirty = false;
    log(`Pengaturan relay: ${v.rlSwap ? "ditukar" : "normal"}, aktif ${v.rlHigh ? "HIGH" : "LOW"}`);
    toast("Pengaturan relay tersimpan, dikirim ke perangkat", "ok");
  } catch (e) { toast(errMsg(e), "err"); }
}
async function wifiMakeMain(i) {
  const list = asList((S.system || {}).wifi).map((x) => ({ s: x.s, p: x.p || "" }));
  const [w] = list.splice(i, 1);
  list.unshift(w);
  try { await S.api.set(`${DEV}/system/wifi`, list); log(`WiFi "${w.s}" dijadikan utama`); toast(`"${w.s}" sekarang WiFi utama`, "ok"); }
  catch (e) { toast(errMsg(e), "err"); }
}

// ---------- OTA dari file .bin yang dipilih ----------
const OTA_CHUNK = 12288;
function bufToB64(u8) {
  let bin = "";
  for (let i = 0; i < u8.length; i += 0x8000) bin += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000));
  return btoa(bin);
}
function onOtaFile() {
  const f = $("otaFile").files[0];
  $("btnOtaFile").disabled = !f;
  if (!f) return;
  $("otaFileName").innerHTML = `📦 <b>${esc(f.name)}</b> (${Math.round(f.size / 1024)} KB)`;
  const m = f.name.match(/(\d+\.\d+\.\d+)/);
  if (m && !$("otaFileVer").value) $("otaFileVer").value = m[1];
}
async function startOtaFile() {
  const f = $("otaFile").files[0];
  if (!f) return;
  if (!S.online) return toast("Perangkat harus online untuk update", "err");
  const ver = $("otaFileVer").value.trim() || "?";
  const buf = new Uint8Array(await f.arrayBuffer());
  if (buf[0] !== 0xe9) return toast("Ini bukan file firmware ESP32 (.ino.bin). Jangan pakai bootloader/merged/partitions.", "err");
  if (buf.length > 1900000) return toast("File terlalu besar untuk partisi OTA", "err");
  if (!confirm(`Kirim ${f.name} (${Math.round(buf.length / 1024)} KB) dan update ke v${ver}?\nJangan cabut daya ESP32 selama proses.`)) return;
  const n = Math.ceil(buf.length / OTA_CHUNK);
  const bar = $("upBar"), txt = $("upText"), btn = $("btnOtaFile");
  btn.disabled = true;
  bar.classList.remove("hidden", "indet");
  try {
    await S.api.remove(`${DEV}/otabin`);
    for (let i = 0; i < n; i++) {
      await S.api.set(`${DEV}/otabin/c${i}`, bufToB64(buf.subarray(i * OTA_CHUNK, (i + 1) * OTA_CHUNK)));
      const p = Math.round(((i + 1) / n) * 100);
      bar.querySelector("i").style.width = p + "%";
      txt.textContent = `Mengirim file ke cloud ${p}% (${i + 1}/${n})`;
    }
    await S.api.update(`${DEV}/system`, { ota: { src: "file", size: buf.length, n, ver, name: f.name, req: S.api.TS } });
    log(`Update firmware dari file ${f.name} (v${ver}) diminta`);
    txt.textContent = "File terkirim. ESP32 sedang mengunduh dan memasang firmware, pantau status di atas.";
    toast("File terkirim, ESP32 mulai update", "ok");
  } catch (e) {
    txt.textContent = "";
    toast("Gagal mengirim file: " + errMsg(e), "err");
  }
  bar.classList.add("hidden");
  btn.disabled = false;
}

async function saveReboot() {
  const v = { rbEn: $("rbEn").checked, rbMode: +$("rbMode").value, rbHours: Math.max(1, Math.min(720, +$("rbHours").value || 24)), rbAt: $("rbAt").value || "03:00" };
  try {
    await S.api.update(`${DEV}/system`, v);
    log(v.rbEn ? `Reboot otomatis aktif: ${v.rbMode ? "setiap hari jam " + v.rbAt : "setiap " + v.rbHours + " jam"}` : "Reboot otomatis dimatikan");
    toast("Pengaturan reboot tersimpan", "ok");
  } catch (e) { toast(errMsg(e), "err"); }
}
async function rebootNow() {
  if (!S.online) return toast("Perangkat sedang offline", "err");
  if (!confirm("Reboot perangkat sekarang? Relay mati sebentar (±15 detik).")) return;
  try { await S.api.update(`${DEV}/system`, { rebootReq: S.api.TS }); toast("Perintah reboot dikirim", "ok"); }
  catch (e) { toast(errMsg(e), "err"); }
}

// --- firmware ---
const binUrl = () => new URL("firmware/manggot.bin", SITE).href;
async function loadFwAvail() {
  try {
    const r = await fetch(new URL("firmware/version.json", SITE).href + "?ts=" + Date.now(), { cache: "no-store" });
    const j = r.ok ? await r.json() : null;
    S.fwAvail = j && j.version && j.size > 0 ? j : null;
  } catch (e) { S.fwAvail = null; }
  renderOta();
}
function cmpVer(a, b) {
  const pa = String(a || "0").split(".").map(Number), pb = String(b || "0").split(".").map(Number);
  for (let i = 0; i < 3; i++) if ((pa[i] || 0) !== (pb[i] || 0)) return (pa[i] || 0) - (pb[i] || 0);
  return 0;
}
function renderOta() {
  const st = S.status || {}, ota = st.ota || {}, av = S.fwAvail;
  const items = [
    ["Versi perangkat", st.fw ? "v" + st.fw : "-"],
    ["Versi tersedia", av && av.version ? `v${av.version}${av.size ? " (" + Math.round(av.size / 1024) + " KB)" : ""}` : "belum ada file"],
    ["Status update", ota.st === "unduh" ? `Mengunduh ${ota.p || 0}%` : ota.st === "gagal" ? "Gagal: " + (ota.msg || "") : ota.st === "ok" ? "Berhasil, restart" : "Siap"],
  ];
  $("fwInfo").innerHTML = items.map(([k, v]) => `<div><small>${k}</small><b>${esc(v)}</b></div>`).join("");
  const bar = $("otaBar");
  bar.classList.toggle("hidden", !(S.online && (ota.st === "unduh" || ota.st === "ok")));
  bar.classList.toggle("indet", ota.st === "unduh" && !ota.p);
  bar.querySelector("i").style.width = (ota.st === "ok" ? 100 : ota.p || 0) + "%";
  const b = $("btnOta");
  const newer = av && st.fw && cmpVer(av.version, st.fw) > 0;
  b.textContent = av && av.version ? (newer ? `Update ke v${av.version}` : `Pasang ulang v${av.version}`) : "Update firmware";
}
async function startOta() {
  if (!S.online) return toast("Perangkat harus online untuk update", "err");
  const adv = !$("otaAdv").classList.contains("hidden") && $("otaUrl").value.trim();
  const url = adv ? $("otaUrl").value.trim() : binUrl();
  const ver = adv ? $("otaVer").value.trim() || "?" : S.fwAvail && S.fwAvail.version;
  if (!url.startsWith("http")) return toast("URL tidak valid", "err");
  if (!adv && !ver) return toast("File firmware belum ada. Jalankan publish.bat dulu", "err");
  const same = S.status && cmpVer(ver, S.status.fw) <= 0;
  if (!confirm(`${same ? "Versi sama atau lebih lama. " : ""}Update firmware ke v${ver}? Jangan cabut daya selama proses (±1 menit).`)) return;
  try {
    await S.api.update(`${DEV}/system`, { ota: { url, ver, req: S.api.TS } });
    log(`Update firmware ke v${ver} diminta`);
    toast("Perintah update dikirim. Pantau status di sini.", "ok");
  } catch (e) { toast(errMsg(e), "err"); }
}

// --- pengguna ---
function renderUsers() {
  const arr = Object.entries(S.users || {});
  $("userList").innerHTML = arr.map(([uid, u]) => {
    const me = S.user && uid === S.user.uid;
    return `<li><span>${u.role === "admin" ? "👑" : "👤"}</span><div class="grow"><b>${esc(u.name || u.email)}${me ? " (kamu)" : ""}</b><small>${esc(u.email || uid)}</small></div>
      <span class="tag ${u.role === "admin" ? "admin" : ""}">${u.role === "admin" ? "Admin" : "Operator"}</span>
      ${me ? "" : `<button class="icon-btn" data-urole="${uid}">${u.role === "admin" ? "Jadikan operator" : "Jadikan admin"}</button><button class="icon-btn del" data-udel="${uid}">Hapus</button>`}</li>`;
  }).join("");
  const me = S.me || {};
  $("meInfo").innerHTML = [["Nama", me.name || "-"], ["Email", (S.user && S.user.email) || "-"], ["Peran", me.role === "admin" ? "Admin" : "Operator"]]
    .map(([k, v]) => `<div><small>${k}</small><b>${esc(v)}</b></div>`).join("");
}
async function addUser(e) {
  e.preventDefault();
  const name = $("uName").value.trim(), email = $("uEmail").value.trim(), pass = $("uPass").value, role = $("uRole").value;
  const btn = e.submitter; if (btn) btn.disabled = true;
  try {
    const uid = await S.api.createUser(email, pass);
    await S.api.set(`users/${uid}`, { name, email, role, created: S.api.TS, by: myName() });
    log(`Pengguna ${name} (${role}) ditambahkan`);
    $("userForm").reset();
    toast(`Akun ${email} dibuat. Beritahu password awalnya.`, "ok");
  } catch (err) { toast(errMsg(err), "err"); }
  if (btn) btn.disabled = false;
}

// =====================================================================
//  NAVIGASI & EVENT
// =====================================================================
function showTab(name) {
  qsa(".tab").forEach((t) => t.classList.toggle("active", t.id === "tab-" + name));
  qsa(".nav button").forEach((b) => b.classList.toggle("on", b.dataset.tab === name));
  if (name === "hist") {
    if (!S.hist.loaded) { S.hist.loaded = true; pickRange(24); }
    else chart.draw();
  }
  if (name === "sys") loadFwAvail();
  history.replaceState(null, "", "#" + name);
  window.scrollTo({ top: 0 });
}
function pickRange(h) {
  qsa("#rangeChips button").forEach((b) => b.classList.toggle("on", +b.dataset.h === h));
  $("customRange").classList.toggle("hidden", h !== 0);
  if (h === 0) {
    const toLocal = (ms) => { const d = new Date(ms - new Date().getTimezoneOffset() * 60000); return d.toISOString().slice(0, 16); };
    if (!$("dFrom").value) { $("dFrom").value = toLocal(Date.now() - 86400000); $("dTo").value = toLocal(Date.now()); }
    return;
  }
  S.hist.hours = h;
  const to = S.api.now();
  loadHistory(to - h * 3600000, to);
}

// =====================================================================
//  TEMA (warna + mode gelap/terang), disimpan di perangkat
// =====================================================================
const THEME_COLORS = { blue: "#06b6d4", red: "#ef4444", green: "#10b981", purple: "#8b5cf6" };
function applyTheme(theme, mode) {
  const d = document.documentElement;
  theme = THEME_COLORS[theme] ? theme : "blue";
  mode = ["dark", "light", "auto"].includes(mode) ? mode : "dark";
  const real = mode === "auto" ? (matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark") : mode;
  d.setAttribute("data-theme", theme);
  d.setAttribute("data-mode-pref", mode);
  d.setAttribute("data-mode", real);
  try { localStorage.setItem("mg-theme", theme); localStorage.setItem("mg-mode", mode); } catch (e) {}
  const meta = document.querySelector('meta[name="theme-color"]');
  if (meta) meta.setAttribute("content", real === "light" ? "#ffffff" : "#08101f");
  qsa("#themeSw button").forEach((b) => b.classList.toggle("on", b.dataset.t === theme));
  qsa("#modeSeg button").forEach((b) => b.classList.toggle("on", b.dataset.m === mode));
  const SB = PLUG("StatusBar");
  if (SB) { SB.setStyle({ style: real === "light" ? "LIGHT" : "DARK" }).catch(() => {}); SB.setBackgroundColor({ color: real === "light" ? "#ffffff" : "#08101f" }).catch(() => {}); }
  if (chart) chart.draw();
}
const curTheme = () => document.documentElement.getAttribute("data-theme") || "blue";
const curMode = () => document.documentElement.getAttribute("data-mode-pref") || "dark";
function bindTheme() {
  qsa("#themeSw button").forEach((b) => b.addEventListener("click", () => applyTheme(b.dataset.t, curMode())));
  qsa("#modeSeg button").forEach((b) => b.addEventListener("click", () => applyTheme(curTheme(), b.dataset.m)));
  qsa("[data-toggle-mode]").forEach((b) => b.addEventListener("click", () => {
    const real = document.documentElement.getAttribute("data-mode");
    applyTheme(curTheme(), real === "light" ? "dark" : "light");
  }));
  matchMedia("(prefers-color-scheme: light)").addEventListener("change", () => { if (curMode() === "auto") applyTheme(curTheme(), "auto"); });
  applyTheme(curTheme(), curMode());
}

// Pratinjau desain dengan data simulasi (tanpa login Firebase)
function startPreview() {
  S.api = makeDemo();
  S.preview = true;
  $("demoBar").classList.remove("hidden");
  $("demoBar").innerHTML = 'Pratinjau desain: data simulasi. <a href="./" style="color:inherit;font-weight:700">Keluar pratinjau</a>';
  S.api.onAuth(onLogin);
  S.api.login();
}

function bindUI() {
  qsa(".nav button").forEach((b) => b.addEventListener("click", () => showTab(b.dataset.tab)));
  qsa("#segMode button").forEach((b) => b.addEventListener("click", () => setMode(+b.dataset.m)));
  $("bHeat").addEventListener("click", () => toggleManual("heat"));
  $("bCool").addEventListener("click", () => toggleManual("cool"));
  $("spMinus").addEventListener("click", () => setSp(S.draft.sp - 0.5));
  $("spPlus").addEventListener("click", () => setSp(S.draft.sp + 0.5));
  $("spRange").addEventListener("input", (e) => setSp(+e.target.value));
  $("hspMinus").addEventListener("click", () => setHsp(S.draft.hsp - 1));
  $("hspPlus").addEventListener("click", () => setHsp(S.draft.hsp + 1));
  $("hspRange").addEventListener("input", (e) => setHsp(+e.target.value));
  PARAMS.forEach(([id, k]) => $(id).addEventListener("input", (e) => { const v = parseFloat(e.target.value); if (!isNaN(v)) { S.draft[k] = v; markDirty(); } }));
  $("btnSave").addEventListener("click", saveConfig);
  $("btnRevert").addEventListener("click", loadDraft);

  qsa("#rangeChips button").forEach((b) => b.addEventListener("click", () => pickRange(+b.dataset.h)));
  $("btnRange").addEventListener("click", () => {
    const a = new Date($("dFrom").value).getTime(), b = new Date($("dTo").value).getTime();
    if (!(a < b)) return toast("Rentang tanggal tidak valid", "err");
    loadHistory(a, b);
  });
  ["lgT", "lgH", "lgS"].forEach((id) => $(id).addEventListener("change", () => {
    chart.show = { t: $("lgT").checked, h: $("lgH").checked, s: $("lgS").checked };
    chart.draw();
  }));
  $("btnCsv").addEventListener("click", downloadCsv);
  $("btnMore").addEventListener("click", () => { S.tableN += 200; renderTable(); });
  $("btnClean").addEventListener("click", cleanHistory);

  $("wifiForm").addEventListener("submit", addWifi);
  $("wifiList").addEventListener("click", (e) => {
    const b = e.target.closest("[data-wdel]"), t = e.target.closest("[data-wtop]");
    if (b) delWifi(+b.dataset.wdel);
    if (t) wifiMakeMain(+t.dataset.wtop);
  });
  qsa(".eye, .eye2").forEach((b) => b.addEventListener("click", () => { const i = $(b.dataset.t); i.type = i.type === "password" ? "text" : "password"; }));
  $("rbEn").addEventListener("change", rbVisibility);
  $("rbMode").addEventListener("change", rbVisibility);
  $("btnRbSave").addEventListener("click", saveReboot);
  ["rlSwap", "rlHigh"].forEach((id) => $(id).addEventListener("change", () => { S.relayDirty = true; renderRelayMap(); }));
  $("btnRelaySave").addEventListener("click", saveRelay);
  $("otaFile").addEventListener("change", onOtaFile);
  $("btnOtaFile").addEventListener("click", startOtaFile);
  $("btnRebootNow").addEventListener("click", rebootNow);
  $("btnOta").addEventListener("click", startOta);
  $("btnOtaAdv").addEventListener("click", () => $("otaAdv").classList.toggle("hidden"));
  $("userForm").addEventListener("submit", addUser);
  $("userList").addEventListener("click", async (e) => {
    const r = e.target.closest("[data-urole]"), d = e.target.closest("[data-udel]");
    try {
      if (r) {
        const uid = r.dataset.urole, u = S.users[uid], nr = u.role === "admin" ? "operator" : "admin";
        await S.api.update(`users/${uid}`, { role: nr });
        log(`Peran ${u.name} diubah ke ${nr}`);
      }
      if (d) {
        const uid = d.dataset.udel, u = S.users[uid];
        if (!confirm(`Cabut akses ${u.name || u.email}? Akun tidak bisa login ke aplikasi lagi.`)) return;
        await S.api.remove(`users/${uid}`);
        log(`Akses ${u.name || u.email} dicabut`);
      }
    } catch (err) { toast(errMsg(err), "err"); }
  });
  $("passForm").addEventListener("submit", async (e) => {
    e.preventDefault();
    try { await S.api.changePass($("pOld").value, $("pNew").value); $("passForm").reset(); toast("Password diganti", "ok"); }
    catch (err) { toast(errMsg(err), "err"); }
  });
  $("btnLogout").addEventListener("click", () => {
    if (!confirm("Keluar dari aplikasi?")) return;
    if (S.preview) location.reload(); else S.api.logout();
  });
  $("btnNaLogout").addEventListener("click", () => S.api.logout());
  $("btnNotif").addEventListener("click", async () => {
    const LN = PLUG("LocalNotifications");
    if (LN) {
      const r = await LN.requestPermissions().catch(() => ({}));
      return toast(r.display === "granted" ? "Notifikasi aktif" : "Izin notifikasi ditolak", r.display === "granted" ? "ok" : "err");
    }
    if (!("Notification" in window)) return toast("Browser tidak mendukung notifikasi", "err");
    const p = await Notification.requestPermission();
    toast(p === "granted" ? "Notifikasi aktif (saat aplikasi terbuka di latar)" : "Izin notifikasi ditolak", p === "granted" ? "ok" : "err");
  });

  $("loginForm").addEventListener("submit", async (e) => {
    e.preventDefault();
    $("loginErr").textContent = "";
    try { await S.api.login($("lEmail").value.trim(), $("lPass").value); }
    catch (err) { $("loginErr").textContent = errMsg(err); }
  });
  $("btnPreview").addEventListener("click", startPreview);
  $("btnForgot").addEventListener("click", async () => {
    const em = $("lEmail").value.trim();
    if (!em) return ($("loginErr").textContent = "Isi email dulu");
    try { await S.api.reset(em); toast("Link reset password dikirim ke email", "ok"); }
    catch (err) { $("loginErr").textContent = errMsg(err); }
  });
}

// =====================================================================
//  SESI
// =====================================================================
const BOOT_T0 = performance.now();
function screen(which) {
  // logo TeknoBro tampil minimal 1,6 detik saat aplikasi dibuka
  const wait = Math.max(0, 1600 - (performance.now() - BOOT_T0));
  setTimeout(() => {
    ["login", "noaccess", "app"].forEach((id) => $(id).classList.toggle("hidden", id !== which));
    const b = $("boot");
    if (!b.classList.contains("hidden")) { b.classList.add("fade"); setTimeout(() => b.classList.add("hidden"), 350); }
  }, wait);
}
function stopSubs() { S.unsubs.forEach((u) => typeof u === "function" && u()); S.unsubs = []; }

async function onLogin(user) {
  stopSubs();
  S.user = user;
  if (!user) { S.me = null; screen("login"); return; }
  // pastikan akun terdaftar; akun pertama otomatis jadi admin
  let me = null;
  try {
    me = await S.api.get(`users/${user.uid}`);
    if (!me) {
      const any = await S.api.get("users");
      if (!any) {
        me = { name: user.email.split("@")[0], email: user.email, role: "admin", created: S.api.TS };
        await S.api.set(`users/${user.uid}`, me);
        toast("Akun pertama: kamu otomatis menjadi admin", "ok");
      }
    }
  } catch (e) { console.warn(e); }
  if (!me) { $("naEmail").textContent = user.email; screen("noaccess"); return; }
  S.me = me;
  document.body.classList.toggle("is-admin", me.role === "admin");
  screen("app");

  S.unsubs.push(S.api.sub(`users/${user.uid}`, (v) => {
    if (!v) { S.api.logout(); return; }
    S.me = v; document.body.classList.toggle("is-admin", v.role === "admin"); renderUsers();
  }));
  S.unsubs.push(S.api.sub("users", (v) => { S.users = v || {}; renderUsers(); }));
  S.unsubs.push(S.api.sub(`${DEV}/status`, (v) => { S.status = v; renderStatus(); if (S.system) renderSystem(); }));
  S.unsubs.push(S.api.sub(`${DEV}/config`, (v) => {
    S.config = v || {};
    if (!S.dirty) loadDraft();
    renderStatus();
  }));
  S.unsubs.push(S.api.sub(`${DEV}/system`, (v) => { S.system = v || {}; renderSystem(); }));
  S.unsubs.push(S.api.subLast(`${DEV}/events`, "ts", 60, renderEvents));
  const tab = location.hash.slice(1);
  if (["ctrl", "hist", "sys"].includes(tab)) showTab(tab);
  if (S.histTimer) return;
  // tambah data riwayat terbaru secara langsung saat melihat rentang "sampai sekarang"
  S.histTimer = setInterval(() => {
    if (S.hist.loaded && S.hist.hours && $("tab-hist").classList.contains("active")) {
      const to = S.api.now();
      loadHistory(to - S.hist.hours * 3600000, to);
    }
  }, 60000);
}

async function main() {
  $("appVer").textContent = APP_VERSION;
  buildLarvae();
  bindTheme();
  chart = new LineChart($("chart"), $("chartTip"), $("chartEmpty"));
  const demo = !firebaseConfig.apiKey || firebaseConfig.apiKey.startsWith("ISI");
  try {
    S.api = demo ? makeDemo() : await makeFirebase();
  } catch (e) {
    $("boot").querySelector("p").textContent = "Gagal memuat Firebase. Cek koneksi internet.";
    return;
  }
  if (demo) {
    $("demoBar").classList.remove("hidden");
    $("lEmail").value = "demo@manggot.app";
    $("lPass").value = "demo123";
  }
  bindUI();
  S.api.onAuth(onLogin);
  setInterval(() => { if (S.status) renderStatus(); }, 1000);
  if (!CAP && "serviceWorker" in navigator && location.protocol === "https:") navigator.serviceWorker.register("sw.js").catch(() => {});
  // link unduh APK (hanya tampil di versi web)
  try {
    const u = new URL(SITE), owner = u.hostname.split(".")[0], repo = u.pathname.split("/").filter(Boolean)[0];
    if (!CAP && u.hostname.endsWith("github.io") && repo) {
      $("apkLink").href = `https://github.com/${owner}/${repo}/releases/latest/download/manggot-iot.apk`;
      $("apkCard").classList.remove("hidden");
    }
  } catch (e) {}
}
main();
