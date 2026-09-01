// Offline cache for the BLE power remote: the page must open with no
// internet at all (a LAN party, a hotel), so everything is pre-cached on
// install and served cache-first forever. Bump VERSION when any cached file
// changes — the new worker drops the old cache on activation.
const VERSION = 'v6';
const CACHE = `bc250-power-${VERSION}`;
const FILES = ['./', 'index.html', 'manifest.webmanifest', 'icon.svg'];

self.addEventListener('install', e => {
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(FILES)).then(() => self.skipWaiting()));
});

self.addEventListener('activate', e => {
  e.waitUntil(caches.keys()
    .then(keys => Promise.all(keys.filter(k => k !== CACHE).map(k => caches.delete(k))))
    .then(() => self.clients.claim()));
});

self.addEventListener('fetch', e => {
  e.respondWith(caches.match(e.request, { ignoreSearch: true })
    .then(hit => hit || fetch(e.request)));
});
