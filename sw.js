// Service worker sederhana: network-first (selalu ambil versi terbaru, cadangan cache saat offline)
const CACHE = "manggot-v2";
const SHELL = ["./", "index.html", "style.css", "app.js", "firebase-config.js", "manifest.json", "icons/icon.svg", "icons/icon-192.png", "icons/teknobro.webp"];
self.addEventListener("install", (e) => { e.waitUntil(caches.open(CACHE).then((c) => c.addAll(SHELL)).catch(() => {})); self.skipWaiting(); });
self.addEventListener("activate", (e) => { e.waitUntil(caches.keys().then((k) => Promise.all(k.filter((x) => x !== CACHE).map((x) => caches.delete(x))))); self.clients.claim(); });
self.addEventListener("fetch", (e) => {
  const u = new URL(e.request.url);
  if (e.request.method !== "GET" || u.origin !== location.origin || u.pathname.includes("/firmware/")) return;
  e.respondWith(
    fetch(e.request).then((r) => { const cp = r.clone(); caches.open(CACHE).then((c) => c.put(e.request, cp)); return r; })
      .catch(() => caches.match(e.request))
  );
});
