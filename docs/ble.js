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
//                          op 0x20=set the power switch's tunings, args
//                          hold_ms(2) boot_timeout_ms(2) sense_low_mv(2)
//                          sense_high_mv(2), LE — the receiver's own
//                          op 0x21=set the power switch's wake input pin,
//                          args pin(1), 0xFF = none — the receiver's own
//   status  a5f20003-... : read/notify  1 byte  0=off 1=booting 2=on
//   fans    a5f20004-... : read/notify  the receiver's own fan view (parseFans)
//   fancfg  a5f20005-... : read/notify  the daemon's fan config, JSON text;
//                          write  token(16) + a partial edit, same shape
//   info    a5f20006-... : read  firmware version + heap + the free input
//                          pins (a gpio:N fan source, the wake input)
//   telem   a5f20007-... : read/notify  the daemon's readings, JSON text
//   stripcfg a5f20008-...: read/notify  the daemon's strip view, JSON text;
//                          write  token(16) + a partial edit, same shape
//   fansa   a5f20009-... : read/notify  the receiver's standalone fan settings
//   sensors a5f2000a-... : read/notify  the daemon's sensor catalogue, JSON text
//                          {"chip":{"label":61.0,"pwm1-8":48}} (fans.hpp sensorsJson)
//   pwr     a5f2000b-... : read/notify  the receiver's own power switch view
//                          (parsePwr): wiring, tunings in force, the sense
//                          wire's live reading — notified 1 Hz while subscribed
//   pwrcfg  a5f2000c-... : read/notify  the daemon's power switch view, JSON text
//                          (power_remote.hpp); write token(16) + a partial edit
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
const PWR    = 'a5f2000b-8f11-4e0e-9b3a-0bc250e0c001';
const PWRCFG = 'a5f2000c-8f11-4e0e-9b3a-0bc250e0c001';
export const OP_ON = 0x01, OP_SHUTDOWN = 0x02, OP_HARD_OFF = 0x03;
export const OP_FAN_HEADER = 0x10;   // + slot(1) + the header's record (SA_HEADER_LEN, encodeRecord)
export const OP_PWR_TUNING = 0x20;   // + hold_ms(2) boot_ms(2) low_mv(2) high_mv(2)
export const OP_PWR_WAKE = 0x21;     // + pin(1), 0xFF = no wake input
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
  pwrChr: null, pcfgChr: null, infoChr: null,
  psu: -1,         // last status byte seen, -1 = unknown
  busy: false,     // a user-initiated connect or write in flight
  attempt: null,   // the device an auto-reconnect (watch or connect) is live for
  editToken: false,// the token box is open on request (change / rejected)
  msg: { text: '', hint: false, html: false }, // one line under the header; errors, or capability hints
  fans: null, sa: null, cfg: null, telem: null, info: null, scfg: null,
  pwr: null,       // the receiver's power switch view (parsePwr)
  pcfg: null,      // the daemon's power switch view (parsePwrCfg)
  sens: null,      // the sensor catalogue: [{ spec, chip, label, pwm, value }] (parseSensors)
  sensMore: 0,     // sensors the catalogue left out for size (its "_more")
  saving: null,    // 'p' or a slot: a write waiting for its answer
  note: null,      // the one status line: { text, cls } — a save's progress, or a refusal
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
  host:     { label: 'Host curve', hint: 'a source only the host reads' },
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
// the standalone value: protocol.hpp CMD_FAN_STANDALONE's layout — one record
// per header (common/fanwire.hpp): fallback boost boost_secs ramp kind gpio npts pts[8][2]
export const SA_HEADER_LEN = 7 + 2 * MAX_POINTS;
export const SA_LEN = CHANNELS * SA_HEADER_LEN;
// what a header runs when its config says nothing (protocol.hpp FAN_DEFAULT_*,
// fans.hpp DEFAULT_HYSTERESIS): the daemon's JSON leaves a tuning out at its default
export const DEFAULTS = { hyst: 3, ramp: 5, boostSecs: 5 };
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

// one header's record → { fb, boost, boostSecs, ramp, kind, gpio, pts }
function decodeRecord(dv, q) {
  const k = dv.getUint8(q + 4), n = Math.min(MAX_POINTS, dv.getUint8(q + 6));
  const pts = [];
  if (k === KIND_BYTE.gpio) for (let j = 0; j < n; j++) pts.push({ x: dv.getUint8(q + 7 + 2 * j), y: dv.getUint8(q + 8 + 2 * j) });
  return { fb: dv.getUint8(q), boost: dv.getUint8(q + 1), boostSecs: dv.getUint8(q + 2), ramp: dv.getUint8(q + 3),
           kind: k === KIND_BYTE.fallback ? 'fallback' : k === KIND_BYTE.gpio ? 'gpio' : 'host', gpio: dv.getUint8(q + 5), pts };
}
// ...and a card's header → the record the receiver stores (the control op's argument)
export function encodeRecord(h) {
  const r = new Uint8Array(SA_HEADER_LEN);
  const gpio = h.kind === 'gpio';
  r.set([h.fallback, h.boost === NONE || h.boost === null || h.boost === undefined ? NONE : h.boost,
         Math.round(h.boostSecs ?? DEFAULTS.boostSecs), Math.round(h.ramp ?? DEFAULTS.ramp),
         KIND_BYTE[h.kind] ?? KIND_BYTE.host, gpio ? h.gpio : NONE, gpio ? h.pts.length : 0]);
  if (gpio) h.pts.forEach((p, j) => r.set([p.x, p.y], 7 + 2 * j));
  return r;
}

// the receiver's standalone settings → { h[slot]: decodeRecord }
export function parseSa(dv) {
  if (dv.byteLength < SA_LEN) return null;
  const out = { h: [] };
  for (let i = 0; i < CHANNELS; i++) out.h.push(decodeRecord(dv, i * SA_HEADER_LEN));
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
           gpio: s.gpio, spec: '', pts: s.pts.map(p => ({ ...p })), boost: s.boost, boostSecs: s.boostSecs,
           ramp: s.ramp, hyst: DEFAULTS.hyst,
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

// the daemon's config JSON -> { editable, h[slot] }; a header's tunings
// (h/r/t) are absent at their defaults or where they don't apply
export function parseCfg(text) {
  let j;
  try { j = JSON.parse(text); } catch { return null; }
  if (!j || typeof j !== 'object') return null;
  const c = { editable: !!j.editable, h: [] };
  for (let i = 0; i < CHANNELS; i++) {
    const h = j['header' + (i + 1)];
    if (!h) { c.h.push(null); continue; }
    const s = srcInfo(h.s);
    const pts = s.kind === 'fallback' ? [] : parseCurve(h.c);
    c.h.push({ name: String(h.n || ''), ...s, pts,
               boost: h.b === null || h.b === undefined ? NONE : +h.b,
               fallback: h.f === undefined ? null : +h.f,
               hyst: h.h === undefined ? DEFAULTS.hyst : +h.h, ramp: h.r === undefined ? DEFAULTS.ramp : +h.r,
               boostSecs: h.t === undefined ? DEFAULTS.boostSecs : +h.t });
  }
  return c;
}
// which tunings a header has (fans.hpp TUNINGS): the editor shows these, the
// daemon refuses the others
export const hasHyst = h => isTempX(h);                 // a temperature source
export const hasRamp = h => !isFixed(h);                // a source with a curve
export const hasBoostSecs = h => h.boost !== NONE;     // a header with a boost

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

// ---- the power switch ----
// the receiver's view (ble.cpp buildPwr): ver(1) flags(1) psu(1) senseMv(2)
// then the four tunings as CMD_PWR_TUNING lays them out, then six pins
export const PWR_LEN = 3 + 2 + 8 + 6;
export const PIN_KEYS = ['ps_on', 'button', 'button_gnd', 'sense', 'led', 'wake']; // the config's names, wire order
export function parsePwr(dv) {
  if (dv.byteLength < PWR_LEN || dv.getUint8(0) !== 1) return null;
  const f = dv.getUint8(1), mv = dv.getUint16(3, true);
  const pins = {};
  PIN_KEYS.forEach((k, i) => { const g = dv.getUint8(13 + i); pins[k] = g === 0xFF ? null : g; });
  return { active: !!(f & 1), sense: !!(f & 2), psu: dv.getUint8(2), mv: mv === 0xFFFF ? null : mv,
           hold: dv.getUint16(5, true), boot: dv.getUint16(7, true),
           low: dv.getUint16(9, true), high: dv.getUint16(11, true), pins };
}

// the daemon's view (daemon/power_remote.hpp), keys as the config spells
// them. `wake` is the config's pins.wake (a GPIO or null); undefined when the
// daemon doesn't say — one from before the key, or a config whose value it
// couldn't use — and the pin is then the receiver's business alone
export function parsePwrCfg(text) {
  let j;
  try { j = JSON.parse(text); } catch { return null; }
  if (!j || typeof j !== 'object' || j.present === false) return null; // no block: the daemon has no say
  return { editable: !!j.editable, hold: +j.hold_seconds || 2, boot: +j.boot_timeout_seconds || 10,
           low: +j.sense_low_mv || 0, high: +j.sense_high_mv || 0,
           wake: !('wake' in j) ? undefined : j.wake === null ? null : Number.isInteger(j.wake) ? j.wake : undefined,
           shortPress: typeof j.short_press === 'string' && j.short_press ? j.short_press : null };
}

// where a power edit goes. 'daemon' while the host is up with a daemon
// that has the block (the edit lands in the config and the daemon pushes the
// receiver); 'readonly' when that daemon can't write its config; 'receiver'
// otherwise — the host off, or no daemon at all: the control op stores the
// tunings on the receiver until a daemon next connects and the config wins
// again (short_press is the daemon's alone, so it can't be edited that way).
export function powerRoute() {
  const hostUp = S.fans ? S.fans.host : S.psu === 2;
  if (S.pcfg && hostUp) return S.pcfg.editable ? 'daemon' : 'readonly';
  return 'receiver';
}
// where the wake pin's edit goes: the daemon's route when the daemon speaks
// the key (it writes pins.wake into the config and pushes the receiver),
// the receiver alone otherwise — a daemon from before the key never pushes
// a wake pin, so a pick stored on the receiver is safe from it
export function wakeRoute() {
  const r = powerRoute();
  if (r !== 'receiver' && S.pcfg && S.pcfg.wake !== undefined) return r;
  return 'receiver';
}
// the settings an editor starts from: the daemon's view when it decides,
// else what the receiver runs — in seconds and millivolts either way; the
// wake pin by its own route (wakeRoute), a GPIO or null
export function powerTuning() {
  const r = powerRoute();
  const wake = wakeRoute() !== 'receiver' ? S.pcfg.wake : S.pwr ? S.pwr.pins.wake : null;
  if (r !== 'receiver' && S.pcfg) return { hold: S.pcfg.hold, boot: S.pcfg.boot, low: S.pcfg.low, high: S.pcfg.high, wake, shortPress: S.pcfg.shortPress };
  if (S.pwr) return { hold: S.pwr.hold / 1000, boot: S.pwr.boot / 1000, low: S.pwr.low, high: S.pwr.high, wake, shortPress: S.pcfg ? S.pcfg.shortPress : null };
  return null;
}
// the pins the wake input may be moved to: the receiver's free input pins
// (parseInfo) with the pin in force kept in the list, less the pins fan
// headers read their PWM on (the receiver refuses those). null when the
// receiver doesn't list its pins (firmware before the info's ver 2)
export function wakePinOptions(current) {
  const pins = S.info && S.info.pins;
  if (!pins) return null;
  const taken = new Set(S.sa ? S.sa.h.filter(h => h.kind === 'gpio' && h.gpio !== NONE).map(h => h.gpio) : []);
  const opts = pins.filter(g => !taken.has(g));
  if (current !== null && current !== undefined && !opts.includes(current)) opts.push(current);
  return opts.sort((a, b) => a - b);
}

function dashReset() {
  S.fansChr = S.cfgChr = S.telemChr = S.stripChr = S.saChr = S.sensChr = S.pwrChr = S.pcfgChr = S.infoChr = null;
  S.fans = S.sa = S.cfg = S.telem = S.info = S.sens = S.pwr = S.pcfg = null; S.sensMore = 0;
  S.saving = null; S.note = null;
  clearTimeout(saveTimer);
  stripReset();
}

// the status line the page shows under its header — one for the whole page,
// whatever is saving: 'ok' ones clear themselves, an error stays until the
// next one, a tap on it, or the user moves on (dismiss)
export function note(text, cls) {
  S.note = text ? { text, cls: cls || '' } : null;
  emit();
  if (cls === 'ok') setTimeout(() => { if (S.note?.text === text) { S.note = null; emit(); } }, 2500);
}
export function dismiss() { if (S.note && S.note.cls === 'err') { S.note = null; emit(); } }

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
const onPwrEvent = notifier(() => S.pwrChr, dv => dv.byteLength >= PWR_LEN, dv => {
  const v = parsePwr(dv);
  if (!v) return;
  const moved = S.pwr && S.pwr.pins.wake !== v.pins.wake; // the one thing that changes the free-pin list
  S.pwr = v; emit();
  if (moved) refreshInfo();
});
// re-read the receiver's build facts — its free input pins, after the wake
// pin moved (the info value is read-only and never notifies)
async function refreshInfo() {
  const chr = S.infoChr;
  if (!chr) return;
  try { const v = parseInfo(await gattRead(chr)); if (chr === S.infoChr && v) { S.info = v; emit(); } } catch {}
}
const onPcfgEvent = jsonNotifier(() => S.pcfgChr, onPcfg);

// an answer landed for the save in flight: that edit is done
function settled(key) {
  S.saving = null;
  clearTimeout(saveTimer);
  note('saved', 'ok');
  onSaved?.(key);
}
const fanSaving = () => S.saving !== null && S.saving !== 'p'; // a fan save ('g' or a slot) is waiting

export function onCfg(dv) {
  S.cfg = dv.byteLength ? parseCfg(utf8.decode(dv)) : null;
  if (fanSaving()) settled(S.saving); // the daemon answered with its new config
  emit();
}

// the receiver's standalone settings (also the echo of a standalone save)
export function onSa(dv) {
  const s = parseSa(dv);
  if (!s) return;
  S.sa = s;
  if (fanSaving() && !S.cfg) settled(S.saving);
  emit();
}

// the daemon's power switch view (also the answer to a routed power save)
export function onPcfg(dv) {
  S.pcfg = dv.byteLength ? parsePwrCfg(utf8.decode(dv)) : null;
  if (S.saving === 'p' && S.pcfg) settled('p');
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
  let sc = null, s = null, se = null, pw = null, pc = null;
  try { sc = await svc.getCharacteristic(STRIPCFG); } catch { sc = null; } // firmware before the strip card
  try { s = await svc.getCharacteristic(FANSA); } catch { s = null; }      // firmware before gpio sources
  try { se = await svc.getCharacteristic(SENSORS); } catch { se = null; }  // firmware before the catalogue
  try { pw = await svc.getCharacteristic(PWR); pc = await svc.getCharacteristic(PWRCFG); } catch { pw = pc = null; } // firmware before the power settings
  if (!connected()) return;
  S.fansChr = f; S.cfgChr = c; S.telemChr = t; S.saChr = s; S.sensChr = se; S.pwrChr = pw; S.pcfgChr = pc; S.infoChr = i;
  f.addEventListener('characteristicvaluechanged', onFansEvent);
  c.addEventListener('characteristicvaluechanged', onCfgEvent);
  t.addEventListener('characteristicvaluechanged', onTelemEvent);
  if (s) s.addEventListener('characteristicvaluechanged', onSaEvent);
  if (se) se.addEventListener('characteristicvaluechanged', onSensEvent);
  if (pw) pw.addEventListener('characteristicvaluechanged', onPwrEvent);
  if (pc) pc.addEventListener('characteristicvaluechanged', onPcfgEvent);
  await gattSubscribe(f);
  await gattSubscribe(c);
  await gattSubscribe(t); // this one tells the daemon to start reporting
  if (s) await gattSubscribe(s);
  if (se) await gattSubscribe(se);
  if (pw) await gattSubscribe(pw);
  if (pc) await gattSubscribe(pc);
  const fv = parseFans(await gattRead(f));
  if (fv) S.fans = fv;
  onCfg(await gattRead(c));
  const tv = await gattRead(t);
  S.telem = tv.byteLength ? parseTelem(utf8.decode(tv)) : null;
  if (s) { try { onSa(await gattRead(s)); } catch {} }
  if (se) { try { const v = await gattRead(se); S.sens = v.byteLength ? parseSensors(utf8.decode(v)) : null; } catch { S.sens = null; } }
  if (pw) { try { S.pwr = parsePwr(await gattRead(pw)); } catch { S.pwr = null; } }
  if (pc) { try { onPcfg(await gattRead(pc)); } catch { S.pcfg = null; } }
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
  if (h.route !== 'daemon' && h.kind !== 'fallback' && h.kind !== 'gpio') return 'the receiver alone runs a fixed speed or a PWM input — pick one';
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
  if (hasHyst(h) && !(h.hyst >= 0)) return 'hysteresis is 0 °C or more';
  if (hasRamp(h) && !(h.ramp >= 0 && h.ramp <= 255)) return 'the ramp is 0–255 % per second';
  if (hasBoostSecs(h) && !(Number.isInteger(h.boostSecs) && h.boostSecs >= 0 && h.boostSecs <= 255)) return 'the boost runs 0–255 whole seconds';
  if (h.fallback !== null && h.fallback !== undefined && !(h.fallback >= 0 && h.fallback <= 100)) return 'the fallback speed is 0–100 %';
  return '';
}

// write a partial edit (one header, or the globals — the daemon's JSON
// shape), then wait for the daemon's config to come back (onCfg) — that is
// what "saved" means here. Resolves true once the write itself went through.
// `chr` is the daemon view's characteristic: the fan config by default, the
// power switch's for a 'p' save (answered by onPcfg).
export async function writeCfg(key, edit, chr = S.cfgChr) {
  if (!chr || S.saving !== null) return false;
  const text = new TextEncoder().encode(JSON.stringify(edit));
  const buf = new Uint8Array(TOKEN_LEN + text.length);
  buf.set(tokenBytes()); buf.set(text, TOKEN_LEN);
  S.saving = key;
  note('saving…');
  try {
    await gattWrite(chr, buf);
    clearTimeout(saveTimer);
    saveTimer = setTimeout(() => {
      if (S.saving !== key) return;
      S.saving = null;
      note('no answer from the host', 'err');
    }, SAVE_TIMEOUT_MS);
    return true;
  } catch (e) {
    S.saving = null;
    if (tokenRejected(e)) { note('token rejected', 'err'); S.editToken = true; emit(); }
    else note(`write failed: ${e.message}${writeHint(e)}`, 'err');
    return false;
  }
}

// a standalone header's edit: its whole record straight to the receiver,
// which validates the pin and the curve itself (a refusal comes back as a
// write error) and echoes the stored value on the standalone characteristic
async function writeRecord(slot, h) {
  if (!S.ctrl || S.saving !== null) return;
  S.saving = slot;
  note('saving…');
  try {
    await writeOp(OP_FAN_HEADER, slot, ...encodeRecord(h));
    if (!S.saChr) { S.saving = null; note('saved', 'ok'); onSaved?.(slot); return; }
    clearTimeout(saveTimer);
    saveTimer = setTimeout(() => {
      if (S.saving !== slot) return;
      S.saving = null;
      note('the receiver did not confirm the change', 'err');
    }, SAVE_TIMEOUT_MS);
  } catch {
    S.saving = null;
    note('refused by the receiver — a pin it can’t read on, a bad curve, or a value out of range', 'err'); // writeOp said more
  }
}

// the editor's Save: the working copy `h` against what the card had
export function saveHeader(slot, h) {
  h.pts.sort((a, b) => a.x - b.x);
  const bad = checkEdit(h);
  if (bad) { note(bad, 'err'); return; }
  if (h.route !== 'daemon') { writeRecord(slot, h); return; }
  if (!S.cfg) return;
  const before = cardHeader(slot) || {};
  const e = { s: srcText(h), b: h.boost === NONE ? null : h.boost, n: h.name.trim() };
  if (!isFixed(h)) e.c = curveText(h.pts);
  if (h.fallback !== null && h.fallback !== undefined) e.f = h.fallback;
  // a tuning travels when it applies to the header as saved and moved (the
  // daemon refuses one that doesn't apply, so an old value stays home)
  if (hasHyst(h) && h.hyst !== before.hyst) e.h = h.hyst;
  if (hasRamp(h) && h.ramp !== before.ramp) e.r = h.ramp;
  if (hasBoostSecs(h) && h.boostSecs !== before.boostSecs) e.t = h.boostSecs;
  writeCfg(slot, { ['header' + (slot + 1)]: e });
}

// ---- power switch edits ----
// what the daemon and the receiver refuse (tools/pwrcfg.py's ranges), said here first
export function checkPower(t) {
  if (!(t.hold >= 0.1 && t.hold <= 65.535)) return 'hold is 0.1–65 seconds';
  if (!(t.boot >= 1 && t.boot <= 65.535)) return 'the boot timeout is 1–65 seconds';
  for (const v of [t.low, t.high]) if (!(Number.isInteger(v) && v >= 0 && v <= 65535)) return 'thresholds are whole millivolts';
  if (t.low >= t.high) return 'the down threshold must be below the up threshold';
  return '';
}

// the editor's Save: `t` = { hold, boot (seconds), low, high (mV), wake (a
// GPIO, null, or undefined = unchanged), shortPress (a command, null, or
// undefined = unchanged) }, routed by powerRoute() — except the wake pin,
// which goes by wakeRoute(): straight to the receiver first when the daemon
// can't carry it, then the rest as usual
export async function savePower(t) {
  const bad = checkPower(t);
  if (bad) { note(bad, 'err'); return; }
  const route = powerRoute();
  if (route === 'readonly') { note('settings are read-only right now', 'err'); return; }
  const wakeHere = t.wake !== undefined && wakeRoute() === 'receiver';
  if (wakeHere && !(await writeWake(t.wake))) return; // it said why
  if (route === 'daemon') {
    const e = { hold_seconds: t.hold, boot_timeout_seconds: t.boot, sense_low_mv: t.low, sense_high_mv: t.high };
    if (t.wake !== undefined && !wakeHere) { e.wake = t.wake; setTimeout(checkWakeLanded, WAKE_LANDED_MS); }
    if (t.shortPress !== undefined) e.short_press = t.shortPress;
    writeCfg('p', e, S.pcfgChr);
    return;
  }
  writeTuning(t);
}

// a wake pin routed through the daemon is written into the config and pushed
// before the receiver has had its say; a pin the receiver refuses stays in
// the config while the receiver keeps its old one. The receiver's view
// (notified within a second) is the truth — compare once the dust settles
const WAKE_LANDED_MS = 3000;
function checkWakeLanded() {
  if (!connected() || !S.pwr || !S.pcfg || S.pcfg.wake === undefined || S.pwr.pins.wake === S.pcfg.wake) return;
  const had = S.pwr.pins.wake === null ? 'no wake input' : `GPIO${S.pwr.pins.wake}`;
  note(`the receiver kept ${had} — it can’t use GPIO${S.pcfg.wake} for the wake input; the config now says ${S.pcfg.wake}`, 'err');
}

// the wake pin straight to the receiver: it checks the pin itself (a
// refusal comes back as a write error) and applies it within a poll; the
// pwr value's next notification shows it, and the free-pin list follows.
// True when the write went through.
async function writeWake(pin) {
  if (!S.ctrl || S.saving !== null) return false;
  try {
    await writeOp(OP_PWR_WAKE, pin === null ? NONE : pin);
    return true;
  } catch {
    note('refused by the receiver — a pin it can’t use for the wake input', 'err'); // writeOp said more
    return false;
  }
}

// straight to the receiver: it validates the set itself (a refusal comes back
// as a write error) and applies it within a poll, so the write's success is
// the answer; the pwr value's next notification shows the stored values
async function writeTuning(t) {
  if (!S.ctrl || S.saving !== null) return;
  const u16 = v => [v & 0xFF, (v >> 8) & 0xFF];
  S.saving = 'p';
  note('saving…');
  try {
    await writeOp(OP_PWR_TUNING, ...u16(Math.round(t.hold * 1000)), ...u16(Math.round(t.boot * 1000)), ...u16(t.low), ...u16(t.high));
    settled('p');
  } catch {
    S.saving = null;
    note('refused by the receiver — a value out of its range', 'err'); // writeOp said more
  }
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
    note('saved', 'ok');
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
  note('saving…');
  try {
    await gattWrite(S.stripChr, buf);
    clearTimeout(sTimer);
    sTimer = setTimeout(() => {
      if (!sSaving) return;
      sSaving = false; sInflight = null; sPending = null;
      note('no answer from the host', 'err');
    }, SAVE_TIMEOUT_MS);
  } catch (e) {
    sSaving = false; sInflight = null; sPending = null;
    if (tokenRejected(e)) { note('token rejected', 'err'); S.editToken = true; emit(); }
    else note(`write failed: ${e.message}${writeHint(e)}`, 'err');
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
// is the same board with the host off.
export function demo() {
  S.demo = true;
  S.cfg = parseCfg(JSON.stringify({ editable: true,
    header1: { n: 'pump', s: 'fallback', b: 100, f: 65 },
    header2: { n: 'radiator', s: 'temp', c: '45:35 60:55 75:100', b: null, f: 100 },
    header3: { n: 'exhaust', s: 'gpio:0', c: '0:25 100:80', b: null, f: 100, r: 0 },
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
  [[65, 100, 'fallback', NONE, [], 5], [100, NONE, 'host', NONE, [], 5], [100, NONE, 'gpio', 0, [[0, 25], [100, 80]], 0],
   [60, NONE, 'host', NONE, [], 5], [NONE, NONE, 'host', NONE, [], 5], [40, NONE, 'gpio', 20, [[0, 30], [100, 100]], 5]]
    .forEach(([fb, b, k, g, pts, ramp], i) =>
      saBytes.set(encodeRecord({ fallback: fb, boost: b, boostSecs: 5, ramp, kind: k, gpio: g, pts: pts.map(([x, y]) => ({ x, y })) }), i * SA_HEADER_LEN));
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
  S.info = { version: 'v1.28.0-demo', heap: 143 * 1024, minHeap: 121 * 1024, pins: [0, 20, 21] };
  S.psu = S.fans.psu;
  // the receiver's power switch: the shipped wiring, the config's tunings, a
  // sense wire reading the board's rail (and the daemon's view of the block)
  const pwrBytes = new Uint8Array(PWR_LEN), pdv = new DataView(pwrBytes.buffer);
  const setPwr = (hold, boot, low, high) => { pdv.setUint16(5, hold, true); pdv.setUint16(7, boot, true); pdv.setUint16(9, low, true); pdv.setUint16(11, high, true); };
  pwrBytes[0] = 1; pwrBytes[1] = 0x03; pwrBytes[2] = S.psu; pdv.setUint16(3, S.psu === 2 ? 2910 : 12, true);
  setPwr(2000, 10000, 800, 2000);
  pwrBytes.set([3, 1, 0xFF, 2, 8, 0xFF], 13);
  S.pwr = parsePwr(pdv);
  S.pcfg = parsePwrCfg(JSON.stringify({ editable: true, hold_seconds: 2, boot_timeout_seconds: 10, sense_low_mv: 800, sense_high_mv: 2000, wake: null, short_press: 'systemctl poweroff' }));
  // the wake pin moving is what the free-pin list follows: the pin taken leaves it, the old one returns
  const setWake = pin => { pwrBytes[18] = pin === null ? 0xFF : pin; S.pwr = parsePwr(pdv); S.info = { ...S.info, pins: [0, 20, 21].filter(g => g !== pin) }; };
  if (location.search.includes('nodaemon')) S.pcfg = null;
  // the wire drifts a little, as a real reading does
  setInterval(() => { if (!S.pwr || !S.pwr.sense) return; pwrBytes[2] = S.psu; pdv.setUint16(3, S.psu === 2 ? 2890 + Math.round(Math.random() * 40) : 5 + Math.round(Math.random() * 12), true); S.pwr = parsePwr(pdv); emit(); }, 1000);
  // stand-ins for the GATT objects so the page believes it is connected
  S.device = { id: 'demo', gatt: { connected: true, disconnect() {} }, addEventListener() {} };
  receivers.demo = { name: 'BC250 (demo)', token: 'demo-token' };
  known.set('demo', S.device);
  S.fansChr = S.telemChr = S.saChr = S.pwrChr = {};
  S.stat = {};
  S.pcfgChr = { writeValueWithResponse: async buf => {
    // merge the partial edit the way the daemon would, then push the receiver
    const edit = JSON.parse(utf8.decode(buf.subarray(TOKEN_LEN))), c = S.pcfg;
    const out = { editable: true, hold_seconds: edit.hold_seconds ?? c.hold, boot_timeout_seconds: edit.boot_timeout_seconds ?? c.boot,
                  sense_low_mv: edit.sense_low_mv ?? c.low, sense_high_mv: edit.sense_high_mv ?? c.high,
                  wake: 'wake' in edit ? edit.wake : c.wake,
                  short_press: 'short_press' in edit ? edit.short_press : c.shortPress };
    setPwr(Math.round(out.hold_seconds * 1000), Math.round(out.boot_timeout_seconds * 1000), out.sense_low_mv, out.sense_high_mv);
    S.pwr = parsePwr(pdv);
    if ('wake' in edit) setWake(edit.wake); // the receiver, pushed
    setTimeout(() => onPcfg(new DataView(new TextEncoder().encode(JSON.stringify(out)).buffer)), 600); } };
  S.ctrl = { writeValueWithResponse: async buf => {
    const op = buf[TOKEN_LEN], slot = buf[TOKEN_LEN + 1], h = S.fans.h[slot], st = S.sa.h[slot];
    if (op === OP_ON) { S.psu = S.fans.psu = 1; emit(); setTimeout(() => { S.psu = S.fans.psu = 2; emit(); }, 3000); return; }
    if (op === OP_SHUTDOWN || op === OP_HARD_OFF) { S.psu = S.fans.psu = 0; emit(); return; }
    if (op === OP_PWR_TUNING) {
      const a = new DataView(buf.buffer, buf.byteOffset + TOKEN_LEN + 1), v = [0, 2, 4, 6].map(o => a.getUint16(o, true));
      if (v[0] < 100 || v[1] < 1000 || v[2] >= v[3]) throw new Error('GATT operation failed'); // what the receiver refuses
      setPwr(...v); S.pwr = parsePwr(pdv); return;
    }
    if (op === OP_PWR_WAKE) {
      const pin = buf[TOKEN_LEN + 1];
      if (pin !== NONE && ![0, 20, 21].includes(pin)) throw new Error('GATT operation failed'); // a pin the receiver refuses
      setWake(pin === NONE ? null : pin); emit(); return;
    }
    if (op === OP_FAN_HEADER) {
      const rec = buf.subarray(TOKEN_LEN + 2, TOKEN_LEN + 2 + SA_HEADER_LEN), r = decodeRecord(new DataView(rec.buffer, rec.byteOffset), 0);
      if (r.kind === 'gpio' && (r.gpio === 4 || r.gpio === 9 || r.gpio > 21)) throw new Error('GATT operation failed'); // a pin the receiver refuses
      Object.assign(st, r);
      h.fb = r.fb; h.kind = KIND_BYTE[r.kind];
      if (r.kind === 'gpio') { h.in = 50; h.src = 4; h.duty = Math.round(r.pts.length ? r.pts[0].y : 50); }
      else { h.in = NONE; h.src = 1; h.duty = h.fb; }
      saBytes.set(rec, slot * SA_HEADER_LEN); // the stored value, as the receiver would echo it
      setTimeout(() => onSa(new DataView(saBytes.buffer)), 400);
    } } };
  S.cfgChr = { writeValueWithResponse: async buf => {
    // merge the partial edit the way the daemon would
    const edit = JSON.parse(utf8.decode(buf.subarray(TOKEN_LEN)));
    const cur = S.cfg, out = { editable: true };
    cur.h.forEach((h, i) => { if (!h) return; const k = 'header' + (i + 1), e = edit[k] || {};
      const s = srcInfo(e.s ?? h.src);
      out[k] = { n: e.n ?? h.name, s: s.src, b: 'b' in e ? e.b : (h.boost === NONE ? null : h.boost), f: e.f ?? h.fallback };
      if (s.kind !== 'fallback') out[k].c = e.c ?? curveText(h.pts);
      // the tunings, as the daemon sends them: where they apply and off their default
      const t = { hyst: e.h ?? h.hyst, ramp: e.r ?? h.ramp, boostSecs: e.t ?? h.boostSecs }, n = { ...h, ...s, boost: out[k].b === null ? NONE : out[k].b };
      if (hasHyst(n) && t.hyst !== DEFAULTS.hyst) out[k].h = t.hyst;
      if (hasRamp(n) && t.ramp !== DEFAULTS.ramp) out[k].r = t.ramp;
      if (hasBoostSecs(n) && t.boostSecs !== DEFAULTS.boostSecs) out[k].t = t.boostSecs; });
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
