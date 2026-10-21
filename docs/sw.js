// Offline cache for the BLE power remote: the page must open with no
// internet at all (a LAN party, a hotel), so everything is pre-cached on
// install and served cache-first forever. Bump VERSION when any cached file
// changes — the new worker drops the old cache on activation. Game
// thumbnails live in their own cache that outlasts versions.
const VERSION = 'v49';
const CACHE = `bc250-power-${VERSION}`;
const THUMBS = 'bc250-thumbs';
const FILES = ['./', 'index.html', 'app.js', 'ble.js', 'demo-ble.js', 'vendor/preact-htm.mjs', 'manifest.webmanifest', 'icon.svg'];

self.addEventListener('install', e => {
  // cache: 'reload' skips the HTTP cache — Pages serves max-age=600, so a plain
  // addAll right after a deploy can bake the previous files into the new cache.
  e.waitUntil(caches.open(CACHE)
    .then(c => c.addAll(FILES.map(f => new Request(f, { cache: 'reload' }))))
    .then(() => self.skipWaiting()));
});

self.addEventListener('activate', e => {
  e.waitUntil(caches.keys()
    .then(keys => Promise.all(keys.filter(k => k !== CACHE && k !== THUMBS).map(k => caches.delete(k))))
    .then(() => self.clients.claim()));
});

// CORS when the CDN allows it: an opaque response is charged megabytes of quota
const fetchThumb = req => fetch(req.url, { mode: 'cors' }).catch(() => fetch(req))
  .then(res => {
    if (res.ok || res.type === 'opaque') { const copy = res.clone(); caches.open(THUMBS).then(c => c.put(req.url, copy)); }
    return res;
  });

self.addEventListener('fetch', e => {
  if (new URL(e.request.url).hostname.endsWith('steamstatic.com')) {
    e.respondWith(caches.open(THUMBS).then(c => c.match(e.request.url)).then(hit => hit || fetchThumb(e.request)));
    return;
  }
  e.respondWith(caches.match(e.request, { ignoreSearch: true })
    .then(hit => hit || fetch(e.request)));
});
