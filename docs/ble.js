// The link: everything between the page and the receiver's GATT service,
// with no DOM in it. app.js renders whatever is in `S` and calls the
// actions below; every change to `S` ends in emit(), which re-renders.
//
// Everything protocol-shaped must match firmware/main/ble.cpp (JSON shapes:
// daemon/fans.hpp and daemon/strip_remote.hpp):
//   service a5f20001-8f11-4e0e-9b3a-0bc250e0c001
//   control a5f20002-... : write  token(16, NUL-padded) + op(1) [+ args]
//                          op 0x01=on 0x02=graceful shutdown 0x03=hard off
//                          op 0x10=set a fan header's fallback duty,
//                          args slot(1) percent(1) — the receiver's own
//                          op 0x11=set a header's standalone source,
//                          args slot(1) kind(1) gpio(1) npts(1) pts(2*npts)
//   status  a5f20003-... : read/notify  1 byte  0=off 1=booting 2=on
//   fans    a5f20004-... : read/notify  the receiver's own fan view (parseFans)
//   fancfg  a5f20005-... : read/notify  the daemon's fan config, JSON text;
//                          write  token(16) + a partial edit, same shape
//   info    a5f20006-... : read  firmware version + heap + usable input pins
//   telem   a5f20007-... : read/notify  the daemon's readings, JSON text
//   stripcfg a5f20008-...: read/notify  the daemon's strip view, JSON text;
//                          write  token(16) + a partial edit, same shape
//   fansa   a5f20009-... : read/notify  the receiver's standalone fan settings
//   sensors a5f2000a-... : read/notify  the daemon's sensor catalogue, JSON text
//                          {"chip":{"label":61.0,"pwm1-8":48}} (fans.hpp sensorsJson)
// A receiver on older firmware has only the first two; the dashboard then
// stays hidden and the power remote works as before.
//
// Reconnects on its own: the chooser grants a persistent permission, so
// navigator.bluetooth.getDevices() can hand the devices back on the next
// visit. Chrome refuses to gatt.connect() a device it hasn't seen this
// session, so the page first calls watchAdvertisements() and waits for one
// advertisement (the receiver advertises in both PSU states) before
// connecting — the sequence Chrome's own sample uses.
//
// CAVEAT: as of 2026, auto-reconnect needs TWO chrome://flags (desktop AND
// Android): #enable-experimental-web-platform-features unlocks the
// getDevices()/watchAdvertisements() APIs, and — separately —
// #enable-web-bluetooth-new-permissions-backend makes the chooser grant
// persist across sessions. Without either, tap-to-connect always works.
//
// iOS: Safari has no Web Bluetooth. Bluefy (free) and WebBLE (paid) are
// WKWebView shells that inject a spec-shaped navigator.bluetooth bridged to
// CoreBluetooth; this module uses nothing beyond the spec, so it runs in
// them as-is, skipping the advertisement dance (CoreBluetooth connects a
// known peripheral without a scan) and falling back to writeValue() where
// writeValueWithResponse() is missing.

export const SVC    = 'a5f20001-8f11-4e0e-9b3a-0bc250e0c001';
const CTRL   = 'a5f20002-8f11-4e0e-9b3a-0bc250e0c001';
const STAT   = 'a5f20003-8f11-4e0e-9b3a-0bc250e0c001';
const FANS   = 'a5f20004-8f11-4e0e-9b3a-0bc250e0c001';
const FANCFG = 'a5f20005-8f11-4e0e-9b3a-0bc250e0c001';
const INFO   = 'a5f20006-8f11-4e0e-9b3a-0bc250e0c001';
const TELEM  = 'a5f20007-8f11-4e0e-9b3a-0bc250e0c001';
const STRIPCFG = 'a5f20008-8f11-4e0e-9b3a-0bc250e0c001';
const FANSA  = 'a5f20009-8f11-4e0e-9b3a-0bc250e0c001';
const SENSORS = 'a5f2000a-8f11-4e0e-9b3a-0bc250e0c001';
export const OP_ON = 0x01, OP_SHUTDOWN = 0x02, OP_HARD_OFF = 0x03;
export const OP_FAN_FALLBACK = 0x10; // + slot(1) percent(1)
export const OP_FAN_SOURCE = 0x11;   // + slot(1) kind(1) gpio(1) npts(1) pts(2*npts)
export const TOKEN_LEN = 16;
export const DEFAULT_NAME = 'BC250'; // what the firmware advertises without a ble_remote.name

// iPhone / iPad (iPadOS 13+ claims to be a Mac; touch points tell). Bluefy
// and WebBLE run in a WKWebView, so their UA is an iOS Safari UA as well.
export const IOS = /iP(hone|ad|od)/.test(navigator.userAgent) ||
                   (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
export const BLUEFY_STORE = 'https://apps.apple.com/app/bluefy-web-ble-browser/id1492822055';
// Bluefy registers a URL scheme that opens a page inside it (its vendor's
// documented way to hand a page over from Safari)
export const bluefyLink = () => 'bluefy://open?url=' + encodeURIComponent(location.href.split('#')[0]);
export const HAS_BT = 'bluetooth' in navigator;

// ---- the store ----
export const S = {
  device: null, ctrl: null, stat: null,   // the selected receiver + its live GATT
  fansChr: null, cfgChr: null, telemChr: null, stripChr: null, saChr: null, sensChr: null,
  psu: -1,         // last status byte seen, -1 = unknown
  busy: false,     // a user-initiated connect or write in flight
  attempt: null,   // the device an auto-reconnect (watch or connect) is live for
  editToken: false,// the token box is open on request (change / rejected)
  msg: { text: '', hint: false, html: false }, // one line under the header; errors, or capability hints
  fans: null, sa: null, cfg: null, telem: null, info: null, scfg: null,
  sens: null,      // the sensor catalogue: [{ spec, chip, label, pwm, value }] (parseSensors)
  sensMore: 0,     // sensors the catalogue left out for size (its "_more")
  saving: null,    // 'g' or a slot: a write waiting for the daemon's config to come back
  notes: {},       // key ('g', a slot, 'strip') -> { text, cls } shown by that card
  demo: false,
};
const subs = new Set();
export const subscribe = fn => { subs.add(fn); return () => subs.delete(fn); };
export const emit = () => { for (const fn of subs) fn(); };

let retryTimer = 0, watchAbort = null, saveTimer = 0;

// ---- storage (localStorage; wrapped — storage can throw in private windows) ----
const store = {
  get(k) { try { return localStorage.getItem(k) || ''; } catch { return ''; } },
  set(k, v) { try { localStorage.setItem(k, v); } catch {} },
};
export const prefs = store;

// Every receiver the chooser has ever granted, by BluetoothDevice.id:
// { name, token }. The name caches what the board advertised so the list
// renders before (or without) getDevices(); the token is per board because
// each is flashed with its own ble_remote.token — a board without one of
// its own uses the shared 'ble-token', the first token ever entered here.
export const receivers = (() => {
  try { return JSON.parse(store.get('ble-receivers')) || {}; } catch { return {}; }
})();
const saveReceivers = () => store.set('ble-receivers', JSON.stringify(receivers));
// devices Chrome has handed out this session (getDevices / chooser), by id —
// only these can be connected without going through the chooser again
export const known = new Map();

// earlier versions of this page knew exactly one receiver
if (store.get('ble-device')) {
  receivers[store.get('ble-device')] ||= { name: '', token: '' };
  store.set('ble-current', store.get('ble-device'));
  store.set('ble-device', '');
  saveReceivers();
}

export const token = () => (S.device && receivers[S.device.id]?.token) || store.get('ble-token');
export const anyKnown = () => Object.keys(receivers).length > 0;
export const currentId = () => S.device?.id || store.get('ble-current');
export const connected = () => !!(S.device && S.device.gatt.connected);

function tokenBytes() {
  const bytes = new Uint8Array(TOKEN_LEN); // NUL padding, as the firmware compares
  const t = token();
  for (let i = 0; i < Math.min(t.length, TOKEN_LEN); i++) bytes[i] = t.charCodeAt(i);
  return bytes;
}

function remember(d) {
  const r = receivers[d.id] ||= { name: '', token: '' };
  if (d.name) r.name = d.name;
  known.set(d.id, d);
  saveReceivers();
}

export function say(text, hint = false, html = false) { S.msg = { text, hint, html }; emit(); }

// display names, with twins (two boards both flashed as "BC250") numbered
export function labels() {
  const ids = Object.keys(receivers).sort((a, b) =>
    (receivers[a].name || DEFAULT_NAME).localeCompare(receivers[b].name || DEFAULT_NAME) || a.localeCompare(b));
  const seen = {}, out = {};
  for (const id of ids) {
    const base = receivers[id].name || DEFAULT_NAME;
    const n = seen[base] = (seen[base] || 0) + 1;
    out[id] = n > 1 ? `${base} (${n})` : base;
  }
  return { ids, labels: out };
}
export const label = id => labels().labels[id] || receivers[id]?.name || DEFAULT_NAME;

// ---- GATT ----
// Chrome does not queue GATT operations: on Android a read or write started
// while another is still in flight on the same device fails outright,
// whatever the characteristic. Every operation on the link goes through this
// one chain, so a tap on the power button can't land in the middle of a
// dashboard read. An operation that never settles would park everything
// behind it, so each gets a deadline; a disconnect or a receiver switch
// starts a fresh chain. A deadline that fires is taken at its word: the link
// is dropped, and the usual auto-reconnect brings the board back.
const GATT_OP_TIMEOUT_MS = 10000;
let gattChain = Promise.resolve();
function gattQueue(op) {
  const p = gattChain.then(() => new Promise((resolve, reject) => {
    const t = setTimeout(() => {
      reject(new Error('Bluetooth operation timed out — the link may be gone'));
      try { S.device?.gatt.disconnect(); } catch {}
    }, GATT_OP_TIMEOUT_MS);
    Promise.resolve().then(op).then(resolve, reject).finally(() => clearTimeout(t));
  }));
  gattChain = p.then(() => {}, () => {});
  return p;
}
const gattReset = () => { gattChain = Promise.resolve(); };
const gattRead = c => gattQueue(() => c.readValue());
// writeValueWithResponse is newer than the iOS polyfills' first releases —
// the deprecated writeValue is the same thing where the new name is missing
const gattWrite = (c, v) => gattQueue(() =>
  c.writeValueWithResponse ? c.writeValueWithResponse(v) : c.writeValue(v));
const gattSubscribe = c => gattQueue(() => c.startNotifications());

// top-level named handlers: addEventListener dedupes identical refs, so
// re-attaching across reconnects can't stack them
const onStatusEvent = e => { if (e.target === S.stat) onStatus(e.target.value.getUint8(0)); };

// make d the selected receiver: the live connection (if any) moves with it
function attach(d) {
  watchAbort?.abort(); // a pick/switch supersedes any pending auto-attempt
  if (S.device && S.device !== d && S.device.gatt.connected) S.device.gatt.disconnect();
  S.device = d;
  S.ctrl = S.stat = null;
  S.psu = -1;
  S.editToken = false;
  gattReset();
  dashReset();
  store.set('ble-current', d.id);
  d.addEventListener('gattserverdisconnected', onDisconnected);
}

async function open() {
  const d = S.device;
  const gatt = await d.gatt.connect();
  const svc = await gatt.getPrimaryService(SVC);
  const c = await svc.getCharacteristic(CTRL);
  const s = await svc.getCharacteristic(STAT);
  await gattSubscribe(s);
  const first = (await gattRead(s)).getUint8(0);
  if (S.device !== d) { gatt.disconnect(); return; } // superseded while we were at it
  S.ctrl = c; S.stat = s;
  s.addEventListener('characteristicvaluechanged', onStatusEvent);
  onStatus(first);
  remember(d);
  say('');
  dashOpen(svc).catch(e => { say(`Dashboard: ${e.message}`); });
}

function onStatus(v) { S.psu = v; emit(); }

function onDisconnected(e) {
  if (e.target !== S.device) return; // a stale device we already replaced
  S.ctrl = S.stat = null;
  S.psu = -1;
  gattReset();
  dashReset();
  emit();
  scheduleReconnect(1000);
}

// Chrome won't gatt.connect() a getDevices() device it hasn't discovered
// this session: watch its advertisements and connect on the first one seen.
// Resolves 'seen' (in range, connect now), 'fallback' (watch API missing or
// refused — attempt a blind connect), or 'aborted' (superseded). On iOS the
// blind connect IS the right path.
function waitForAdvertisement(d) {
  if (IOS || !d.watchAdvertisements) return Promise.resolve('fallback');
  return new Promise(resolve => {
    watchAbort?.abort();
    const ac = watchAbort = new AbortController();
    ac.signal.addEventListener('abort', () => resolve('aborted'));
    d.addEventListener('advertisementreceived', () => { resolve('seen'); ac.abort(); },
                       { once: true, signal: ac.signal });
    d.watchAdvertisements({ signal: ac.signal }).catch(() => { resolve('fallback'); ac.abort(); });
  });
}

// auto-reconnect: the advertisement watch pends until the receiver is in
// range, so one live attempt is the whole loop; the retry timer only guards
// against failures after that. The attempt is pinned to a device: switching
// boards while one pends lets the stale attempt fall through.
async function reconnect() {
  if (!S.device || S.device.gatt.connected || S.attempt === S.device || S.busy) return;
  const d = S.attempt = S.device;
  emit();
  try {
    const how = await waitForAdvertisement(d);
    if (how === 'aborted' || S.device !== d || d.gatt.connected) return;
    await open();
  } catch (e) {
    if (S.device !== d) return; // died because we switched away — not news
    say(`Auto-reconnect: ${e.message} — retrying.`, true);
    scheduleReconnect(10000);
  } finally { if (S.attempt === d) S.attempt = null; emit(); }
}

function scheduleReconnect(ms) {
  clearTimeout(retryTimer);
  retryTimer = setTimeout(reconnect, ms);
}

// on load (and when the tab returns — Chrome drops pending connects in the
// background), pick up the boards the chooser permitted before and connect
// to the one used last
export async function resume() {
  if (S.demo) return;
  if (S.device) { reconnect(); return; }
  if (!token() || !HAS_BT) return;
  if (!navigator.bluetooth.getDevices) {
    // stock Chrome (2026): persistent-device APIs still flag-gated; on iOS
    // it is the browser app — there is no flag to name
    say(IOS
      ? 'This browser can’t remember receivers between visits — tap to connect (Bluefy can).'
      : 'Auto-reconnect needs “Experimental Web Platform features” turned on in ' +
        '<code>chrome://flags</code> — until then, tap to connect.', true, true);
    return;
  }
  try {
    const list = await navigator.bluetooth.getDevices();
    for (const d of list) remember(d);
    const d = list.find(x => x.id === store.get('ble-current')) || list[0];
    if (!d) {
      // we HAVE connected before (receivers are stored) yet Chrome
      // remembers nothing. One tap re-grants it; whether THAT grant then
      // survives the session is the second flag's doing
      if (anyKnown())
        say(IOS
          ? 'The browser holds no permission for the receiver — tap to connect once.'
          : 'Chrome holds no permission for the receiver — tap to connect once. If this keeps ' +
            'coming back, enable <code>chrome://flags/#enable-web-bluetooth-new-permissions-backend</code> ' +
            'and relaunch Chrome.', true, true);
      emit();
      return;
    }
    attach(d);
    reconnect();
  } catch (e) { say(`Auto-reconnect failed: ${e.message}`); }
}

// the chooser: grants (and thereby adds) a receiver, then connects to it
async function pick() {
  const d = await navigator.bluetooth.requestDevice({ filters: [{ services: [SVC] }] });
  remember(d);
  if (S.device && S.device.id === d.id && S.device.gatt.connected) {
    store.set('ble-current', d.id);
    return; // picked the board we're already on
  }
  attach(d);
  emit();
  await open();
}

// a user-driven connect: the chooser (new receiver, permission revoked —
// the manual escape hatch while an auto-reconnect pends)
export async function connect() {
  if (S.busy) return;
  S.busy = true; say('');
  try { await pick(); }
  catch (e) {
    if (e.name !== 'NotFoundError') say(`Couldn’t connect: ${e.message}`); // chooser dismissed = not an error
  } finally { S.busy = false; emit(); }
}

// the receiver list: a board Chrome handed us this session connects directly
// (through the advertisement dance); anything else needs the chooser
export async function select(id) {
  if (S.busy) return; // never yank a board mid-write
  if (id && known.has(id)) {
    store.set('ble-current', id);
    if (S.device?.id === id) { reconnect(); return; }
    attach(known.get(id));
    say('');
    reconnect();
    return;
  }
  await connect();
}

async function writeOp(op, ...args) {
  const buf = new Uint8Array(TOKEN_LEN + 1 + args.length);
  buf.set(tokenBytes());
  buf[TOKEN_LEN] = op;
  buf.set(args, TOKEN_LEN + 1);
  try {
    await gattWrite(S.ctrl, buf);
    say('');
  } catch (e) {
    if (tokenRejected(e)) {
      say(`Token rejected by ${label(S.device.id)} — enter the one it was flashed with.`);
      S.editToken = true;
    } else say(`Write failed: ${e.message}${writeHint(e)}`);
    throw e;
  }
}

// the power ops, from the ring and its sheet
export async function power(op) {
  if (S.busy || !connected()) return;
  S.busy = true; say('');
  try { await writeOp(op); } catch {} // writeOp said why
  S.busy = false; emit();
}

// What a failed write means. The firmware answers a wrong token with "write
// not permitted" — the ATT code for an application-level refusal. On
// Android, Chrome folds almost everything else into "GATT Error Unknown."
// (a full message queue, a bad length, link loss, and the "insufficient
// authentication" firmware before Sep 2026 answered a wrong token with).
const tokenRejected = e => /not permitted|not authori[sz]ed|not paired|authentication/i.test(e.message);
const writeHint = e => /GATT Error Unknown/i.test(e.message)
  ? ' — Android reports several things this way: the link dropping, the receiver’s queue being full, or a wrong token on older firmware'
  : '';

// ---- the token ----
export function saveToken(v) {
  if (v.length < 8) { say('The token is 8–16 characters.'); return false; }
  // the selected board gets it; it also becomes the shared default when
  // there is none yet (first run) — the token boards without their own try
  if (S.device) { receivers[S.device.id].token = v; saveReceivers(); }
  if (!S.device || !store.get('ble-token')) store.set('ble-token', v);
  S.editToken = false;
  say('');
  resume();
  return true;
}
export function editToken(on) { S.editToken = on; say(''); }

export async function forget() {
  if (S.busy || !S.device) return;
  const d = S.device;
  clearTimeout(retryTimer);
  watchAbort?.abort();
  if (d.gatt.connected) d.gatt.disconnect();
  S.device = null; S.ctrl = S.stat = null; S.psu = -1; S.editToken = false;
  dashReset();
  delete receivers[d.id];
  known.delete(d.id);
  saveReceivers();
  if (store.get('ble-current') === d.id) store.set('ble-current', '');
  try { await d.forget?.(); } catch {} // Chrome ≥110 revokes the grant too
  say('');
  resume(); // falls over to the next known board, if any
}

// ---- the dashboard's data ----
// a header's source: the config's string ("fallback", "gpio:0", "temp",
// "amdgpu:edge", "nct6686:pwm1", "cpu_load", "gpu_load") → its kind and parts
export const SRC_KINDS = {
  fallback: { label: 'Fixed speed', hint: 'runs at the fallback speed, always' },
  gpio:     { label: 'PWM input', hint: 'a fan wire on a receiver pin' },
  temp:     { label: 'CPU temperature' },
  hwmon:    { label: 'Sensor', hint: 'chip:label, e.g. amdgpu:edge or pmbus:GPU VRM' },
  pwm:      { label: 'Board fan header', hint: 'chip:pwmN, e.g. nct6686:pwm1' },
  cpu_load: { label: 'CPU load' },
  gpu_load: { label: 'GPU load' },
  host:     { label: 'Machine’s curve', hint: 'a source only the machine reads' },
};
export const HOST_KINDS = ['temp', 'hwmon', 'pwm', 'cpu_load', 'gpu_load']; // the daemon runs these
export const FILE_PREFIX = 'file:'; // hwmon.hpp: a spec naming a file holding one temperature
export function srcInfo(s) {
  s = String(s || '');
  if (s === 'fallback' || s === 'constant') return { kind: 'fallback', src: 'fallback' };
  if (s === 'temp' || s === 'cpu_load' || s === 'gpu_load') return { kind: s, src: s };
  if (s === 'pwm') return { kind: 'pwm', src: s, spec: '' }; // an older daemon sent kind names
  const i = s.indexOf(':'), chip = i < 0 ? s : s.slice(0, i), lbl = i < 0 ? '' : s.slice(i + 1);
  if (chip === 'gpio') return { kind: 'gpio', src: s, gpio: parseInt(lbl, 10) };
  if (/^pwm\d+$/.test(lbl)) return { kind: 'pwm', src: s, spec: s };
  return { kind: 'hwmon', src: s, spec: s };
}
// the config string a working copy stands for
export const srcText = h => h.kind === 'gpio' ? `gpio:${h.gpio}` : (h.kind === 'hwmon' || h.kind === 'pwm') ? (h.spec || '') : h.kind;
export const isFixed = h => h.kind === 'fallback';                 // no curve: the fallback is the duty
export const isTempX = h => h.kind === 'temp' || h.kind === 'hwmon'; // the curve's x is °C
export const NONE = 0xFF;
export const CHANNELS = 6, MAX_POINTS = 8; // common/fancurve.hpp: the daemon and the receiver cap at this too
export const SRC_NAMES = ['', 'fallback', 'live', 'boost', 'curve']; // fan::SRC_* (curve = the receiver's own gpio curve)
const KIND_BYTE = { fallback: 0, gpio: 1, host: 2 }; // protocol.hpp FAN_KIND_*
// the fans view's length by layout version: 1 = state(1) duty(1) per header,
// 2 adds the receiver's stored fallback(1), 3 the source kind(1) and input(1)
const FANS_STRIDE = { 1: 2, 2: 3, 3: 5 };
export const FANS_LEN = ver => 4 + CHANNELS * FANS_STRIDE[ver] + 4;
// the standalone value: protocol.hpp CMD_FAN_STANDALONE's layout
export const SA_V1_LEN = 1 + 2 * CHANNELS, SA_PER_HEADER = 3 + 2 * MAX_POINTS;
export const SA_LEN = SA_V1_LEN + 1 + CHANNELS * SA_PER_HEADER;
const SAVE_TIMEOUT_MS = 6000;
const utf8 = new TextDecoder();
const u32 = (dv, o) => dv.getUint32(o, true);

export function parseFans(dv) {
  if (dv.byteLength < 1) return null;
  const ver = dv.getUint8(0), stride = FANS_STRIDE[ver] || 0;
  if (!stride || dv.byteLength < FANS_LEN(ver)) return null;
  const f = dv.getUint8(1);
  const out = {
    active: !!(f & 1), boosting: !!(f & 2), hold: !!(f & 4), live: !!(f & 8),
    host: !!(f & 16), telem: !!(f & 32),
    psu: dv.getUint8(2), age: dv.getUint8(3), h: [], uptime: u32(dv, 4 + CHANNELS * stride),
  };
  for (let i = 0; i < CHANNELS; i++) {
    const o = 4 + i * stride, st = dv.getUint8(o);
    out.h.push({ wired: !!(st & 1), src: (st >> 1) & 7, duty: dv.getUint8(o + 1),
                 fb: stride >= 3 ? dv.getUint8(o + 2) : null,   // null: firmware before the slider
                 kind: stride >= 5 ? dv.getUint8(o + 3) : null, // null: firmware before gpio sources
                 in: stride >= 5 ? dv.getUint8(o + 4) : NONE });
  }
  return out;
}

// the receiver's standalone settings → { boostSecs, ramp, h[slot]: { fb, boost, kind, gpio, pts } }
export function parseSa(dv) {
  if (dv.byteLength < SA_LEN) return null;
  const out = { boostSecs: dv.getUint8(0), ramp: dv.getUint8(SA_V1_LEN), h: [] };
  for (let i = 0; i < CHANNELS; i++) {
    const q = SA_V1_LEN + 1 + i * SA_PER_HEADER, k = dv.getUint8(q), n = Math.min(MAX_POINTS, dv.getUint8(q + 2));
    const pts = [];
    if (k === KIND_BYTE.gpio) for (let j = 0; j < n; j++) pts.push({ x: dv.getUint8(q + 3 + 2 * j), y: dv.getUint8(q + 4 + 2 * j) });
    out.h.push({ fb: dv.getUint8(1 + 2 * i), boost: dv.getUint8(2 + 2 * i),
                 kind: k === KIND_BYTE.fallback ? 'fallback' : k === KIND_BYTE.gpio ? 'gpio' : 'host',
                 gpio: dv.getUint8(q + 1), pts });
  }
  return out;
}

// the header a card shows: the daemon's, when it has one (route 'daemon'),
// else the receiver's own standalone view of a wired header (route
// 'receiver'), else null. Route 'daemon' edits go into the config file and
// the daemon pushes the receiver; route 'receiver' edits are stored on the
// receiver until a daemon next connects and the config wins again.
export function cardHeader(slot) {
  const c = S.cfg && S.cfg.h[slot];
  if (c) return { ...c, route: 'daemon' };
  const f = S.fans && S.fans.h[slot], s = S.sa && S.sa.h[slot];
  if (!f || !f.wired || !s) return null;
  return { name: `header${slot + 1}`, kind: s.kind, src: s.kind === 'gpio' ? `gpio:${s.gpio}` : s.kind,
           gpio: s.gpio, spec: '', pts: s.pts.map(p => ({ ...p })), boost: s.boost,
           fallback: f.fb !== null && f.fb !== NONE ? f.fb : (s.fb !== NONE ? s.fb : null), route: 'receiver' };
}

// which headers get a card: every wired one plus every one the daemon's
// config has, so a header in the config but not flashed still shows
export function cardSlots() {
  const slots = new Set();
  if (S.fans) S.fans.h.forEach((h, i) => { if (h.wired) slots.add(i); });
  if (S.cfg) S.cfg.h.forEach((h, i) => { if (h) slots.add(i); });
  return [...slots].sort((a, b) => a - b);
}

// the reading a header's curve sees right now: a gpio header's from the
// receiver (it samples the pin), a host source's from the daemon's telemetry
export function inputOf(slot, h) {
  if (!h) return null;
  if (h.kind === 'gpio') { const f = S.fans && S.fans.h[slot]; return f && f.in !== NONE ? f.in : null; }
  const tm = S.fans && S.fans.telem && S.telem && S.telem.h[slot];
  return tm && tm.in !== undefined ? tm.in : null;
}

// what a kind reads right now, for the source list: null when nothing does
export function readingOf(kind, slot) {
  const t = S.fans && S.fans.telem && S.telem;
  if (kind === 'temp') return t && t.temp !== undefined ? t.temp : null;
  if (kind === 'cpu_load') return t && t.cpu !== undefined ? t.cpu : null;
  if (kind === 'gpu_load') return t && t.gpu !== undefined ? t.gpu : null;
  if (kind === 'gpio') { const f = S.fans && S.fans.h[slot]; return f && f.kind === KIND_BYTE.gpio && f.in !== NONE ? f.in : null; }
  return null;
}
// a catalogue entry's reading by its spec ("amdgpu:edge", "nct6686:pwm1"), null when not listed
export function sensorReading(spec) {
  const e = S.sens && S.sens.find(x => x.spec === spec);
  return e ? e.value : null;
}

// the daemon's sensor catalogue (fans.hpp sensorsJson): chips → labelled
// readings; a "pwm1-8" key is one entry standing for a run of pwm outputs
// that read alike — its spec is the first of them. "_more" counts sensors
// that did not fit the wire (S.sensMore); they can still be typed.
export function parseSensors(text) {
  let j;
  try { j = JSON.parse(text); } catch { return null; }
  if (!j || typeof j !== 'object') return null;
  const out = [];
  S.sensMore = typeof j._more === 'number' ? j._more : 0;
  for (const chip of Object.keys(j)) {
    const g = j[chip];
    if (!g || typeof g !== 'object') continue;
    for (const k of Object.keys(g)) {
      const m = /^pwm(\d+)(?:-(\d+))?$/.exec(k);
      if (m) out.push({ spec: `${chip}:pwm${m[1]}`, chip, label: m[2] ? `pwm ${m[1]}–${m[2]}` : `pwm ${m[1]}`, pwm: true, value: +g[k] });
      else out.push({ spec: `${chip}:${k}`, chip, label: chip === 'file' ? k.split('/').pop() : k, pwm: false, value: +g[k] }); // a file entry's key is its path
    }
  }
  return out;
}

// "45:35 60:55" -> [{x, y}], sorted; the daemon's notation
export function parseCurve(text) {
  const toks = String(text || '').trim().split(/[\s,]+/).filter(Boolean);
  return toks.map(t => { const [x, y] = t.split(':').map(parseFloat); return { x, y }; })
             .filter(p => !isNaN(p.x) && !isNaN(p.y)).sort((a, b) => a.x - b.x);
}
export const curveText = pts => pts.map(p => `${p.x}:${p.y}`).join(' ');

// the daemon's config JSON -> { editable, hyst, ramp, boostSecs, h[slot] }
export function parseCfg(text) {
  let j;
  try { j = JSON.parse(text); } catch { return null; }
  if (!j || typeof j !== 'object') return null;
  const c = { editable: !!j.editable, hyst: +j.hysteresis || 0, ramp: +j.ramp || 0,
              boostSecs: +j.boost_seconds || 0, h: [] };
  for (let i = 0; i < CHANNELS; i++) {
    const h = j['header' + (i + 1)];
    if (!h) { c.h.push(null); continue; }
    const s = srcInfo(h.s);
    const pts = s.kind === 'fallback' ? [] : parseCurve(h.c); // an older daemon wrote a constant's value as the curve
    c.h.push({ name: String(h.n || ''), ...s, pts,
               boost: h.b === null || h.b === undefined ? NONE : +h.b,
               fallback: h.f === undefined ? null : +h.f });
  }
  return c;
}

// the daemon's telemetry JSON -> { temp, cpu, gpu, h[slot]: { in, duty } }
export function parseTelem(text) {
  let j;
  try { j = JSON.parse(text); } catch { return null; }
  if (!j || typeof j !== 'object') return null;
  const t = { temp: j.temp, cpu: j.cpu, gpu: j.gpu, h: [] };
  for (let i = 0; i < CHANNELS; i++) t.h.push(j['header' + (i + 1)] || null);
  return t;
}

// ver 1: version, heap. ver 2 adds the GPIOs a gpio:N source may read as a
// 64-bit mask (bit N = GPIO N) — `pins` is that as a sorted list, or null on
// firmware that doesn't say (the editor then asks for a number)
export function parseInfo(dv) {
  const ver = dv.byteLength ? dv.getUint8(0) : 0;
  if (dv.byteLength < 41 || ver < 1 || ver > 2) return null;
  let v = '';
  for (let k = 1; k < 33; k++) { const ch = dv.getUint8(k); if (!ch) break; v += String.fromCharCode(ch); }
  let pins = null;
  if (ver >= 2 && dv.byteLength >= 49) {
    pins = [];
    for (let g = 0; g < 64; g++) if (dv.getUint8(41 + (g >> 3)) >> (g & 7) & 1) pins.push(g);
  }
  return { version: v, heap: u32(dv, 33), minHeap: u32(dv, 37), pins };
}

function dashReset() {
  S.fansChr = S.cfgChr = S.telemChr = S.stripChr = S.saChr = S.sensChr = null;
  S.fans = S.sa = S.cfg = S.telem = S.info = S.sens = null; S.sensMore = 0;
  S.saving = null; S.notes = {};
  clearTimeout(saveTimer);
  stripReset();
}

// a message a card shows: 'ok' ones clear themselves
export function note(key, text, cls) {
  S.notes[key] = text ? { text, cls: cls || '' } : null;
  emit();
  if (cls === 'ok') setTimeout(() => { if (S.notes[key]?.text === text) { S.notes[key] = null; emit(); } }, 2500);
}

// a notification is truncated to the ATT MTU; a read isn't. The fans value
// fits any MTU; the JSON values and the standalone value may not. A value
// that arrived whole is used as it is; one cut short is re-read — one read at
// a time, notifications landing meanwhile mark it stale so it runs once more.
const whole = text => { try { JSON.parse(text); return true; } catch { return false; } };
function notifier(current, ok, apply) {
  let reading = false, stale = false;
  return async e => {
    const chr = e.target;
    if (chr !== current()) return;
    const dv = chr.value;
    if (ok(dv)) { apply(dv); return; }
    if (reading) { stale = true; return; }
    reading = true;
    try {
      do {
        stale = false;
        const v = await gattRead(chr);
        if (chr !== current()) return;
        apply(v);
      } while (stale);
    } catch {} finally { reading = false; }
  };
}
const jsonNotifier = (current, apply) => notifier(current, dv => !dv.byteLength || whole(utf8.decode(dv)), apply);
const onFansEvent = e => {
  if (e.target !== S.fansChr) return;
  const f = parseFans(e.target.value);
  if (f) { S.fans = f; emit(); }
};
const onCfgEvent = jsonNotifier(() => S.cfgChr, onCfg);
const onTelemEvent = jsonNotifier(() => S.telemChr, dv => {
  S.telem = dv.byteLength ? parseTelem(utf8.decode(dv)) : null;
  emit();
});
const onSaEvent = notifier(() => S.saChr, dv => dv.byteLength >= SA_LEN, onSa);
const onSensEvent = jsonNotifier(() => S.sensChr, dv => {
  S.sens = dv.byteLength ? parseSensors(utf8.decode(dv)) : null;
  emit();
});

export function onCfg(dv) {
  S.cfg = dv.byteLength ? parseCfg(utf8.decode(dv)) : null;
  if (S.saving !== null) {
    // the daemon answered a save with its new config: that edit is done
    const key = S.saving;
    S.saving = null;
    clearTimeout(saveTimer);
    note(key, 'saved', 'ok');
    onSaved?.(key);
  }
  emit();
}

// the receiver's standalone settings (also the echo of a standalone save)
export function onSa(dv) {
  const s = parseSa(dv);
  if (!s) return;
  S.sa = s;
  if (S.saving !== null && !S.cfg) {
    const key = S.saving;
    S.saving = null;
    clearTimeout(saveTimer);
    note(key, 'saved', 'ok');
    onSaved?.(key);
  }
  emit();
}
// the UI's hook: a save landed, close that editor
let onSaved = null;
export const whenSaved = fn => { onSaved = fn; };

async function dashOpen(svc) {
  let f, c, t, i;
  try {
    f = await svc.getCharacteristic(FANS);
    c = await svc.getCharacteristic(FANCFG);
    t = await svc.getCharacteristic(TELEM);
  } catch { return; } // older firmware: no dashboard, and nothing to say
  try { i = await svc.getCharacteristic(INFO); } catch { i = null; }
  let sc = null, s = null, se = null;
  try { sc = await svc.getCharacteristic(STRIPCFG); } catch { sc = null; } // firmware before the strip card
  try { s = await svc.getCharacteristic(FANSA); } catch { s = null; }      // firmware before gpio sources
  try { se = await svc.getCharacteristic(SENSORS); } catch { se = null; }  // firmware before the catalogue
  if (!connected()) return;
  S.fansChr = f; S.cfgChr = c; S.telemChr = t; S.saChr = s; S.sensChr = se;
  f.addEventListener('characteristicvaluechanged', onFansEvent);
  c.addEventListener('characteristicvaluechanged', onCfgEvent);
  t.addEventListener('characteristicvaluechanged', onTelemEvent);
  if (s) s.addEventListener('characteristicvaluechanged', onSaEvent);
  if (se) se.addEventListener('characteristicvaluechanged', onSensEvent);
  await gattSubscribe(f);
  await gattSubscribe(c);
  await gattSubscribe(t); // this one tells the daemon to start reporting
  if (s) await gattSubscribe(s);
  if (se) await gattSubscribe(se);
  const fv = parseFans(await gattRead(f));
  if (fv) S.fans = fv;
  onCfg(await gattRead(c));
  const tv = await gattRead(t);
  S.telem = tv.byteLength ? parseTelem(utf8.decode(tv)) : null;
  if (s) { try { onSa(await gattRead(s)); } catch {} }
  if (se) { try { const v = await gattRead(se); S.sens = v.byteLength ? parseSensors(utf8.decode(v)) : null; } catch { S.sens = null; } }
  if (i) { try { S.info = parseInfo(await gattRead(i)); } catch { S.info = null; } }
  emit();
  if (sc) await stripOpen(sc);
}

// ---- fan edits ----
// what the daemon (or the receiver) will refuse, said here first (they also validate)
export function checkEdit(h) {
  if (h.route === 'daemon') {
    const nm = (h.name || '').trim();
    if (!nm || [...nm].length > 16) return 'a name is 1–16 characters'; // characters, as the daemon counts
  }
  if (h.route !== 'daemon' && h.kind !== 'fallback' && h.kind !== 'gpio') return 'with the machine off a header runs a fixed speed or a PWM input — pick one';
  if (h.kind === 'gpio' && !(Number.isInteger(h.gpio) && h.gpio >= 0 && h.gpio <= 48)) return 'the pin is a number 0–48';
  if (h.kind === 'hwmon' && (h.spec || '').startsWith(FILE_PREFIX) && !/^\/\S/.test(h.spec.slice(FILE_PREFIX.length))) // the daemon insists on an absolute path
    return 'a temperature file is its full path, e.g. /tmp/some_custom_temp_reading';
  if ((h.kind === 'hwmon' || h.kind === 'pwm') && !/^[^:\s]+:\S[^:]*$/.test(h.spec || '')) // a label may hold spaces ("AMD TSI Addr 98h", "CPU VRM")
    return h.kind === 'pwm' ? 'a board fan header is chip:pwmN, e.g. nct6686:pwm1' : 'a sensor is chip:label, e.g. amdgpu:edge';
  if (h.kind === 'pwm' && !/:pwm\d+$/.test(h.spec)) return 'a board fan header is chip:pwmN, e.g. nct6686:pwm1';
  if (!isFixed(h)) {
    if (!h.pts.length) return 'a curve needs at least one point';
    if (h.pts.length > MAX_POINTS) return `at most ${MAX_POINTS} points`;
    const xs = new Set();
    for (const p of h.pts) {
      if (!(p.y >= 0 && p.y <= 100)) return 'speed must be 0–100 %';
      if (h.kind === 'gpio' && !(Number.isInteger(p.x) && p.x >= 0 && p.x <= 100)) return 'a PWM input curve reads whole percents 0–100';
      if (xs.has(p.x)) return `two points at ${p.x}`;
      xs.add(p.x);
    }
  }
  if (h.boost !== NONE && !(h.boost >= 0 && h.boost <= 100)) return 'boost must be 0–100 % or blank';
  if (h.fallback !== null && h.fallback !== undefined && !(h.fallback >= 0 && h.fallback <= 100)) return 'the fallback speed is 0–100 %';
  return '';
}

// write a partial edit (one header, or the globals — the daemon's JSON
// shape), then wait for the daemon's config to come back (onCfg) — that is
// what "saved" means here. Resolves true once the write itself went through.
export async function writeCfg(key, edit) {
  if (!S.cfgChr || S.saving !== null) return false;
  const text = new TextEncoder().encode(JSON.stringify(edit));
  const buf = new Uint8Array(TOKEN_LEN + text.length);
  buf.set(tokenBytes()); buf.set(text, TOKEN_LEN);
  S.saving = key;
  note(key, 'saving…');
  try {
    await gattWrite(S.cfgChr, buf);
    clearTimeout(saveTimer);
    saveTimer = setTimeout(() => {
      if (S.saving !== key) return;
      S.saving = null;
      note(key, 'no answer from the machine — is it on?', 'err');
    }, SAVE_TIMEOUT_MS);
    return true;
  } catch (e) {
    S.saving = null;
    if (tokenRejected(e)) { note(key, 'token rejected', 'err'); S.editToken = true; emit(); }
    else note(key, `write failed: ${e.message}${writeHint(e)}`, 'err');
    return false;
  }
}

// a standalone header's edit: the control ops straight to the receiver,
// which validates the pin and the curve itself (a refusal comes back as a
// write error) and echoes the stored value on the standalone characteristic
async function writeSource(slot, h, before) {
  if (!S.ctrl || S.saving !== null) return;
  const kind = KIND_BYTE[h.kind] ?? KIND_BYTE.host;
  const pts = h.kind === 'gpio' ? h.pts.flatMap(p => [p.x, p.y]) : [];
  S.saving = slot;
  note(slot, 'saving…');
  try {
    if (h.fallback !== null && h.fallback !== undefined && h.fallback !== before.fallback)
      await writeOp(OP_FAN_FALLBACK, slot, h.fallback);
    await writeOp(OP_FAN_SOURCE, slot, kind, h.kind === 'gpio' ? h.gpio : NONE, pts.length / 2, ...pts);
    if (!S.saChr) { S.saving = null; note(slot, 'saved', 'ok'); onSaved?.(slot); return; }
    clearTimeout(saveTimer);
    saveTimer = setTimeout(() => {
      if (S.saving !== slot) return;
      S.saving = null;
      note(slot, 'the receiver did not confirm the change', 'err');
    }, SAVE_TIMEOUT_MS);
  } catch {
    S.saving = null;
    note(slot, 'refused by the receiver — a pin it can’t read on, or a bad curve', 'err'); // writeOp said more
  }
}

// the editor's Save: the working copy `h` against what the card had
export function saveHeader(slot, h) {
  h.pts.sort((a, b) => a.x - b.x);
  const bad = checkEdit(h);
  if (bad) { note(slot, bad, 'err'); return; }
  const before = cardHeader(slot) || {};
  if (h.route !== 'daemon') { writeSource(slot, h, before); return; }
  if (!S.cfg) return;
  const e = { s: srcText(h), b: h.boost === NONE ? null : h.boost, n: h.name.trim() };
  if (!isFixed(h)) e.c = curveText(h.pts);
  if (h.fallback !== null && h.fallback !== undefined) e.f = h.fallback;
  writeCfg(slot, { ['header' + (slot + 1)]: e });
}

export function saveGlobals(g) {
  if (!S.cfg) return;
  writeCfg('g', { hysteresis: g.hyst, ramp: g.ramp, boost_seconds: g.boostSecs });
}

// ---- the strip ----
// The daemon's strip view (daemon/strip_remote.hpp): the strip block's knobs
// and the scenes. There is no Save: a slider applies on release, a switch on
// the tap — the daemon corrects the live strip at once and writes the value
// into its config, and the answering view is what says "saved". Edits that
// land while one is in flight are merged and sent when the answer comes.
let sSaving = false, sInflight = null, sPending = null, sTimer = 0;
export const hex2 = v => Math.max(0, Math.min(255, Math.round(v))).toString(16).padStart(2, '0');

export function parseStripCfg(text) {
  let j;
  try { j = JSON.parse(text); } catch { return null; }
  if (!j || typeof j !== 'object') return null;
  const g = String(j.gamma ?? '2.2').trim().split(/\s+/).map(parseFloat).filter(x => !isNaN(x));
  const gamma = g.length === 3 ? g : [g[0] ?? 2.2, g[0] ?? 2.2, g[0] ?? 2.2];
  const w = String(j.white_balance ?? 'ffffff').replace('#', '').padStart(6, 'f');
  const wb = [0, 2, 4].map(i => parseInt(w.substr(i, 2), 16) || 0);
  return { editable: !!j.editable, leds: +j.leds || 0, pin: +j.pin || 0, reverse: !!j.reverse,
           brightness: Math.max(0, Math.min(1, +j.brightness || 0)), gamma, wb,
           scenes: Array.isArray(j.scenes) ? j.scenes.filter(s => s && typeof s.p === 'string').map(s => ({
             p: s.p, e: String(s.e || ''), on: !!s.on,
             color: typeof s.color === 'string' ? s.color.replace('#', '') : null,
             l: typeof s.l === 'number' ? s.l : null })) : [] };
}

function stripReset() {
  S.scfg = null; sSaving = false; sInflight = null; sPending = null;
  clearTimeout(sTimer);
}

const onStripEvent = jsonNotifier(() => S.stripChr, onStripCfg);

// does the daemon's view carry the edit? The view is also pushed unsolicited
// (a scene's file appearing or disappearing), so an answer is recognised by
// its content, not its arrival; a refused edit sends nothing and times out.
const near = (a, b) => Math.abs(a - b) < 0.006;
function reflects(v, edit) {
  if ('brightness' in edit && !near(v.brightness, edit.brightness)) return false;
  if ('reverse' in edit && v.reverse !== edit.reverse) return false;
  if ('white_balance' in edit && v.wb.map(hex2).join('') !== edit.white_balance.toLowerCase()) return false;
  if ('gamma' in edit) {
    const g = String(edit.gamma).split(/\s+/).map(parseFloat);
    const want = g.length === 3 ? g : [g[0], g[0], g[0]];
    if (!want.every((x, i) => near(x, v.gamma[i]))) return false;
  }
  for (const e of edit.scenes || []) {
    const s = v.scenes.find(x => x.p === e.p);
    if (!s) return false;
    if ('on' in e && s.on !== e.on) return false;
    if ('color' in e && s.color !== e.color.toLowerCase()) return false;
    if ('l' in e && !near(s.l, e.l)) return false;
  }
  return true;
}

export function onStripCfg(dv) {
  S.scfg = dv.byteLength ? parseStripCfg(utf8.decode(dv)) : null;
  if (sSaving && S.scfg && reflects(S.scfg, sInflight)) {
    sSaving = false; sInflight = null;
    clearTimeout(sTimer);
    note('strip', 'saved', 'ok');
    if (sPending) { const e = sPending; sPending = null; writeStrip(e); }
  }
  emit();
}

async function stripOpen(sc) {
  S.stripChr = sc;
  sc.addEventListener('characteristicvaluechanged', onStripEvent);
  await gattSubscribe(sc);
  const v = await gattRead(sc);
  if (S.stripChr !== sc) return;
  onStripCfg(v);
}

// merge a partial edit into another (scenes by path)
function mergeStrip(into, edit) {
  for (const k in edit) {
    if (k !== 'scenes') { into[k] = edit[k]; continue; }
    into.scenes ||= [];
    for (const s of edit.scenes) {
      const have = into.scenes.find(x => x.p === s.p);
      if (have) Object.assign(have, s); else into.scenes.push({ ...s });
    }
  }
  return into;
}
// the edits the daemon hasn't answered yet, merged — their fields hold
export const pendingStrip = () => mergeStrip(mergeStrip({}, sInflight || {}), sPending || {});

export async function writeStrip(edit) {
  if (!S.stripChr || !S.scfg || !S.scfg.editable) return;
  if (sSaving) { sPending = mergeStrip(sPending || {}, edit); return; }
  const text = new TextEncoder().encode(JSON.stringify(edit));
  const buf = new Uint8Array(TOKEN_LEN + text.length);
  buf.set(tokenBytes()); buf.set(text, TOKEN_LEN);
  sSaving = true; sInflight = edit;
  note('strip', 'saving…');
  try {
    await gattWrite(S.stripChr, buf);
    clearTimeout(sTimer);
    sTimer = setTimeout(() => {
      if (!sSaving) return;
      sSaving = false; sInflight = null; sPending = null;
      note('strip', 'no answer from the machine — is it on?', 'err');
    }, SAVE_TIMEOUT_MS);
  } catch (e) {
    sSaving = false; sInflight = null; sPending = null;
    if (tokenRejected(e)) { note('strip', 'token rejected', 'err'); S.editToken = true; emit(); }
    else note('strip', `write failed: ${e.message}${writeHint(e)}`, 'err');
  }
}

// ---- lifecycle ----
document.addEventListener('visibilitychange', () => {
  // Chrome silently stops background scans: abort ours so the attempt
  // settles cleanly, and start a fresh one when the tab returns
  if (document.visibilityState === 'visible') resume();
  else watchAbort?.abort();
});

// ?demo: the dashboard on sample data, no receiver needed — to see the
// layout on a desktop, or to try the editor before wiring anything. Saves
// land locally after a moment, the way a round trip would. ?demo&nodaemon
// is the same board with the machine off.
export function demo() {
  S.demo = true;
  S.cfg = parseCfg(JSON.stringify({ editable: true, hysteresis: 3, ramp: 5, boost_seconds: 5,
    header1: { n: 'pump', s: 'fallback', b: 100, f: 65 },
    header2: { n: 'radiator', s: 'temp', c: '45:35 60:55 75:100', b: null, f: 100 },
    header3: { n: 'exhaust', s: 'gpio:0', c: '0:25 100:80', b: null, f: 100 },
    header4: { n: 'intake', s: 'gpu_load', c: '0:20 40:20 100:60', b: null, f: 60 } }));
  S.scfg = parseStripCfg(JSON.stringify({ editable: true, leds: 33, pin: 4, reverse: false, brightness: 1,
    gamma: '2.2', white_balance: 'ffb0f0', scenes: [
      { p: '/tmp/led-static-color', e: 'solid', on: false, color: 'ffffff', l: 0.6 },
      { p: '/tmp/led-night', e: 'drift', on: false } ] }));
  S.telem = parseTelem(JSON.stringify({ temp: 58.3, cpu: 37, gpu: 62,
    header2: { in: 58.3, duty: 52 }, header4: { in: 62, duty: 45 } }));
  S.sens = parseSensors(JSON.stringify({ amdgpu: { edge: 61.0, junction: 64.5, mem: 58.0 },
    k10temp: { Tctl: 58.3 }, nct6686: { CPU: 52.0, System: 38.5, 'VRM MOS': 41.0, 'pwm1-8': 48 } }));
  // the receiver's stored standalone settings: the config's, as the daemon
  // pushes them (header6 is wired but not in the daemon's config — dropped
  // from the file after flashing — and was dialled from the phone)
  const saBytes = new Uint8Array(SA_LEN);
  saBytes[0] = 5; saBytes[SA_V1_LEN] = 5;
  [[65, 100, 'fallback', NONE, []], [100, NONE, 'host', NONE, []], [100, NONE, 'gpio', 0, [[0, 25], [100, 80]]],
   [60, NONE, 'host', NONE, []], [NONE, NONE, 'host', NONE, []], [40, NONE, 'gpio', 20, [[0, 30], [100, 100]]]]
    .forEach(([fb, b, k, g, pts], i) => {
      saBytes[1 + 2 * i] = fb; saBytes[2 + 2 * i] = b;
      const q = SA_V1_LEN + 1 + i * SA_PER_HEADER;
      saBytes[q] = KIND_BYTE[k]; saBytes[q + 1] = g; saBytes[q + 2] = pts.length;
      pts.forEach(([x, y], j) => { saBytes[q + 3 + 2 * j] = x; saBytes[q + 4 + 2 * j] = y; });
    });
  S.sa = parseSa(new DataView(saBytes.buffer));
  const f = new Uint8Array(FANS_LEN(3)), dv = new DataView(f.buffer);
  f[0] = 3; f[1] = 0x01 | 0x08 | 0x10 | 0x20; f[2] = 2; f[3] = 1;
  // state: wired | src<<1 (1 = fallback, 2 = live, 3 = boost, 4 = the
  // receiver's own curve); duty; the stored fallback; the source kind; a gpio
  // header's reading
  [[0x03, 65, 100, 0, NONE], [0x05, 52, 100, 2, NONE], [0x07, 61, 100, 1, 65], [0x05, 45, 60, 2, NONE],
   [0, NONE, NONE, NONE, NONE], [0x09, 58, 40, 1, 40]]
    .forEach(([st, d, fb, k, inp], i) => { f.set([st, d, fb, k, inp], 4 + i * 5); });
  dv.setUint32(4 + CHANNELS * 5, 5 * 3600 + 17 * 60, true);
  S.fans = parseFans(dv);
  if (location.search.includes('nodaemon')) {
    S.cfg = S.telem = S.scfg = S.sens = null; // nothing from the daemon
    S.fans.live = S.fans.telem = S.fans.host = false; S.fans.age = 255; S.fans.psu = 0;
    // the receiver alone: a gpio header keeps its own curve, everything else runs its fallback
    S.fans.h.forEach(h => { if (!h.wired) return; if (h.kind === KIND_BYTE.gpio) h.src = 4; else { h.src = 1; h.duty = h.fb; } });
  }
  S.info = { version: 'v1.26.0-demo', heap: 143 * 1024, minHeap: 121 * 1024, pins: [0, 20, 21] };
  S.psu = S.fans.psu;
  // stand-ins for the GATT objects so the page believes it is connected
  S.device = { id: 'demo', gatt: { connected: true, disconnect() {} }, addEventListener() {} };
  receivers.demo = { name: 'BC250 (demo)', token: 'demo-token' };
  known.set('demo', S.device);
  S.fansChr = S.telemChr = S.saChr = {};
  S.stat = {};
  S.ctrl = { writeValueWithResponse: async buf => {
    const op = buf[TOKEN_LEN], slot = buf[TOKEN_LEN + 1], h = S.fans.h[slot], st = S.sa.h[slot];
    if (op === OP_ON) { S.psu = S.fans.psu = 1; emit(); setTimeout(() => { S.psu = S.fans.psu = 2; emit(); }, 3000); return; }
    if (op === OP_SHUTDOWN || op === OP_HARD_OFF) { S.psu = S.fans.psu = 0; emit(); return; }
    if (op === OP_FAN_FALLBACK) {
      h.fb = st.fb = buf[TOKEN_LEN + 2];
      if (h.src === 1) h.duty = h.fb;
    } else if (op === OP_FAN_SOURCE) {
      const kind = buf[TOKEN_LEN + 2], gpio = buf[TOKEN_LEN + 3], n = buf[TOKEN_LEN + 4];
      if (kind === KIND_BYTE.gpio && (gpio === 4 || gpio === 9 || gpio > 21)) throw new Error('GATT operation failed'); // a pin the receiver refuses
      st.kind = kind === KIND_BYTE.gpio ? 'gpio' : 'fallback'; st.gpio = gpio; st.pts = [];
      for (let j = 0; j < n; j++) st.pts.push({ x: buf[TOKEN_LEN + 5 + 2 * j], y: buf[TOKEN_LEN + 6 + 2 * j] });
      h.kind = kind;
      if (st.kind === 'gpio') { h.in = 50; h.src = 4; h.duty = Math.round(st.pts.length ? st.pts[0].y : 50); }
      else { h.in = NONE; h.src = 1; h.duty = h.fb; }
      const q = SA_V1_LEN + 1 + slot * SA_PER_HEADER; // the stored value, as the receiver would echo it
      saBytes.fill(0, q, q + SA_PER_HEADER);
      saBytes[q] = kind; saBytes[q + 1] = gpio; saBytes[q + 2] = n;
      saBytes.set(buf.subarray(TOKEN_LEN + 5, TOKEN_LEN + 5 + 2 * n), q + 3);
      saBytes[1 + 2 * slot] = st.fb;
      setTimeout(() => onSa(new DataView(saBytes.buffer)), 400);
    } } };
  S.cfgChr = { writeValueWithResponse: async buf => {
    // merge the partial edit the way the daemon would
    const edit = JSON.parse(utf8.decode(buf.subarray(TOKEN_LEN)));
    const cur = S.cfg, out = { editable: true, hysteresis: edit.hysteresis ?? cur.hyst, ramp: edit.ramp ?? cur.ramp,
                               boost_seconds: edit.boost_seconds ?? cur.boostSecs };
    cur.h.forEach((h, i) => { if (!h) return; const k = 'header' + (i + 1), e = edit[k] || {};
      const s = srcInfo(e.s ?? h.src);
      out[k] = { n: e.n ?? h.name, s: s.src, b: 'b' in e ? e.b : (h.boost === NONE ? null : h.boost), f: e.f ?? h.fallback };
      if (s.kind !== 'fallback') out[k].c = e.c ?? curveText(h.pts); });
    // ...and push the standalone values to the receiver, as the daemon does
    cur.h.forEach((h, i) => { const e = edit['header' + (i + 1)]; const f = S.fans.h[i];
      if (e && 'f' in e && f.wired) { f.fb = e.f; if (f.src === 1) f.duty = e.f; } });
    setTimeout(() => onCfg(new DataView(new TextEncoder().encode(JSON.stringify(out)).buffer)), 600); } };
  S.stripChr = { writeValueWithResponse: async buf => {
    // apply the partial edit the way the daemon would, scenes by path
    const edit = JSON.parse(utf8.decode(buf.subarray(TOKEN_LEN)));
    const sc = S.scfg;
    const out = { editable: true, leds: sc.leds, pin: sc.pin, reverse: edit.reverse ?? sc.reverse,
                  brightness: edit.brightness ?? sc.brightness,
                  gamma: edit.gamma ?? (sc.gamma.every(x => x === sc.gamma[0]) ? String(sc.gamma[0]) : sc.gamma.join(' ')),
                  white_balance: edit.white_balance ?? sc.wb.map(hex2).join(''),
                  scenes: sc.scenes.map(s => { const e = (edit.scenes || []).find(x => x.p === s.p) || {};
                    const o = { p: s.p, e: s.e, on: e.on ?? s.on };
                    if (s.color !== null) o.color = e.color ?? s.color;
                    if (s.l !== null) o.l = e.l ?? s.l;
                    return o; }) };
    setTimeout(() => onStripCfg(new DataView(new TextEncoder().encode(JSON.stringify(out)).buffer)), 600); } };
  say('Demo data — not connected to a receiver.', true);
}

export function boot() {
  if (location.search.includes('demo')) demo();
  else if (!HAS_BT)
    say(IOS
      ? 'Safari has no Web Bluetooth. Open this page in ' +
        `<a href="${bluefyLink()}">Bluefy</a>, a free browser that has it ` +
        `(<a href="${BLUEFY_STORE}">App Store</a>), and bookmark it there.`
      : 'No Web Bluetooth here — use Chrome or Edge.', false, true);
  else resume();
  if ('serviceWorker' in navigator) navigator.serviceWorker.register('sw.js').catch(() => {});
}
