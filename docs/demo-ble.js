// A provider for ble.js (useProvider) that simulates receivers instead of
// talking to them: ?demo runs the real page against it, so every screen,
// save and refusal goes through the same code paths a board would drive.
//
// It is the far side of the link, written from the protocol at the top of
// ble.js: a Web Bluetooth-shaped `bluetooth` (getDevices, requestDevice, the
// devices, their GATT servers and characteristics) whose characteristics are
// served by a simulated receiver (firmware/main/ble.cpp) with, while its host
// is up, a simulated daemon behind it (daemon/fans.hpp, strip_remote.hpp,
// power_remote.hpp). Plus an in-memory `storage`, so the simulated boards and
// their tokens never touch the receivers this browser really knows.
//
// Three boards: a BC-250 with its host up, a desk board with its host off
// (the receiver alone runs its fans until the power button brings the host
// up), and one out of range. Readings drift slowly, as real ones do.
import { SVC, TOKEN_LEN, OP_ON, OP_SHUTDOWN, OP_HARD_OFF, OP_FAN_HEADER, OP_PWR_TUNING, OP_PWR_WAKE,
         NONE, CHANNELS, MAX_POINTS, FANS_LEN, SA_HEADER_LEN, SA_LEN, PWR_LEN,
         encodeRecord, outInfo, srcInfo, parseCurve, ownCurve } from './ble.js';

const uuid = n => `a5f200${n}-8f11-4e0e-9b3a-0bc250e0c001`;
const U = { ctrl: uuid('02'), stat: uuid('03'), fans: uuid('04'), fancfg: uuid('05'), info: uuid('06'), telem: uuid('07'),
            stripcfg: uuid('08'), fansa: uuid('09'), sensors: uuid('0a'), pwr: uuid('0b'), pwrcfg: uuid('0c'), page: uuid('0d') };
const JSON_SLOT = { [U.fancfg]: 0, [U.telem]: 1, [U.stripcfg]: 2, [U.sensors]: 3, [U.pwrcfg]: 4 }; // dash::Slot
const SLOT_UUID = Object.fromEntries(Object.entries(JSON_SLOT).map(([u, s]) => [s, u]));
const KIND_BYTE = { fallback: 0, gpio: 1, host: 2, esp32_temp: 3 };                           // protocol.hpp FAN_KIND_*
const SRC = { fallback: 1, live: 2, boost: 3, curve: 4 };                                      // fan::SRC_*
const OUT_HEADER = 1, OUT_GPIO = 2;
const PAGE_CHUNK = 500;
const ADV_MFG_ID = 0xFFFF;

const sleep = ms => new Promise(r => setTimeout(r, ms));
const enc = new TextEncoder(), dec = new TextDecoder();
const dv = bytes => new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
const r1 = v => Math.round(v * 10) / 10;
const refused = () => new DOMException('GATT operation failed for unknown reason.', 'NotSupportedError');
const notPermitted = () => new DOMException('GATT operation not permitted.', 'NotAllowedError');

// a curve's duty at x, flat beyond the ends (fancurve.hpp)
function curveAt(pts, x) {
  if (!pts.length) return null;
  if (x <= pts[0].x) return pts[0].y;
  for (let k = 1; k < pts.length; k++)
    if (x <= pts[k].x) { const a = pts[k - 1], b = pts[k]; return Math.round(a.y + (x - a.x) / (b.x - a.x) * (b.y - a.y)); }
  return pts[pts.length - 1].y;
}

// one fan record off the wire (common/fanwire.hpp): fallback boost boost_secs
// ramp kind gpio npts pts[8][2] out_kind out
function decodeRecord(b) {
  const kindOf = Object.fromEntries(Object.entries(KIND_BYTE).map(([k, v]) => [v, k]));
  const ok = b[7 + 2 * MAX_POINTS], on = b[8 + 2 * MAX_POINTS], n = Math.min(MAX_POINTS, b[6]);
  if (ok !== OUT_HEADER && ok !== OUT_GPIO) return null;
  const kind = kindOf[b[4]] ?? 'host', pts = [];
  if (kind === 'gpio' || kind === 'esp32_temp') for (let j = 0; j < n; j++) pts.push({ x: b[7 + 2 * j], y: b[8 + 2 * j] });
  return { out: ok === OUT_HEADER ? { kind: 'header', num: on } : { kind: 'gpio', num: on },
           fallback: b[0], boost: b[1], boostSecs: b[2], ramp: b[3], kind, gpio: b[5], pts };
}

// ---- the simulated host: a daemon and the machine's sensors ----
class Host {
  constructor({ fans, strip, power, chips, outs, vrm = false }) {
    this.fans = fans; this.strip = strip; this.power = power;
    this.hasVrm = vrm;  // a BC-250 VRM controller answers (fans.hpp vrmJson)
    this.chips = chips; // { chip: { label: base reading } }, drifting
    this.outs = outs;   // [spec, base duty, rpm at that duty (-1 none), writable]
    this.rev = 1;
    this.t0 = Date.now();
  }
  wave(base, period, amp) { return base + amp * Math.sin((Date.now() - this.t0) / period); }
  drift(base, period, amp) { return r1(this.wave(base, period, amp)); }
  get temp() { return this.drift(58.3, 23000, 2.5); }
  get cpu() { return Math.round(this.drift(37, 9000, 12)); }
  get gpu() { return Math.round(this.drift(62, 14000, 15)); }
  reading(kind, spec) {
    if (kind === 'temp') return this.temp;
    if (kind === 'cpu_load') return this.cpu;
    if (kind === 'gpu_load') return this.gpu;
    if (kind === 'hwmon') {
      const [chip, lbl] = [spec.slice(0, spec.indexOf(':')), spec.slice(spec.indexOf(':') + 1)];
      const base = this.chips[chip]?.[lbl];
      return base === undefined ? undefined : this.drift(base, 17000 + base * 90, base > 45 ? 2 : 0.6);
    }
    if (kind === 'pwm') { const o = this.outs.find(x => x[0] === spec); return o ? o[1] : undefined; }
    return undefined;
  }
  // what the daemon reports per fan (fans.hpp telemetryJson)
  telemOf(f) {
    const out = outInfo(f.o), s = srcInfo(f.i), x = this.reading(s.kind, s.spec);
    const pts = parseCurve(f.c), fb = f.f ?? 100;
    if (out.kind === 'host') {
      if (x === undefined) return { duty: fb, st: 'host' };
      return { in: x, duty: curveAt(pts, x) ?? fb, st: 'host' };
    }
    if (out.kind === 'parked') return x === undefined ? {} : { in: x };
    return x === undefined ? null : { in: x, duty: curveAt(pts, x) };
  }
  // the VRM rails, their current following the loads
  get vrm() {
    const rail = (v, idle, perLoad, load, t) => ({ v, a: r1(idle + perLoad * load / 100), t: Math.round(t) });
    const r3 = v => Math.round(v * 1000) / 1000;
    return { vin: Math.round(this.wave(12.08, 31000, 0.03) * 100) / 100,
             cpu: rail(r3(this.wave(1.05, 7000, 0.04)), 6, 22, this.cpu, this.drift(52, 19000, 3)),
             gpu: rail(r3(this.wave(0.95, 8000, 0.03)), 12, 70, this.gpu, this.drift(61, 21000, 4)) };
  }
  telemJson() {
    return JSON.stringify({ temp: this.temp, cpu: this.cpu, gpu: this.gpu, ...(this.hasVrm ? { vrm: this.vrm } : {}),
                            fans: this.fans.map(f => this.telemOf(f)) });
  }
  // the catalogue, with each host output at the duty its fan runs (fans.hpp sensorsJson)
  sensorsJson() {
    const j = {};
    for (const [chip, g] of Object.entries(this.chips)) {
      j[chip] = {};
      for (const k of Object.keys(g)) j[chip][k] = /^pwm/.test(k) ? g[k] : this.reading('hwmon', `${chip}:${k}`);
    }
    j._outs = this.outs.map(([spec, duty, rpm, w]) => {
      const i = this.fans.findIndex(f => f.o === spec), t = i >= 0 ? this.telemOf(this.fans[i]) : null;
      const d = t && t.duty !== undefined ? t.duty : duty;
      return [spec, d, rpm < 0 ? -1 : Math.round(rpm * d / Math.max(1, duty)), w];
    });
    return JSON.stringify(j);
  }
  cfgJson(err) { return JSON.stringify({ editable: true, rev: String(this.rev), ...(err ? { err } : {}), fans: this.fans }); }
  // one edit, the way fans.hpp applyJson takes it: null = taken, else why not
  applyFanEdit(edit) {
    if (edit.rev !== String(this.rev)) return 'the fan list changed on the host since the phone read it — look again and retry';
    const next = this.fans.map(x => ({ ...x }));
    if ('del' in edit) next.splice(edit.del, 1);
    else {
      const f = 'add' in edit ? edit.add : { ...next[edit.fan], ...edit.edit };
      for (const k of Object.keys(f)) if (f[k] === undefined) delete f[k];
      if (f.b === null) delete f.b;
      if (next.some((x, i) => x.o && x.o === f.o && ('add' in edit || i !== edit.fan))) return `"${f.o}" is another fan's output already — one fan per output`;
      if (f.o && outInfo(f.o).kind === 'host' && !this.outs.some(o => o[0] === f.o && o[3])) return `"${f.o}" is not a pwm output on this machine`;
      if (f.o && f.i === f.o) return 'a host output can\'t follow itself';
      if ('add' in edit) next.push(f); else next[edit.fan] = f;
    }
    this.fans = next;
    this.rev++;
    return null;
  }
  stripJson() {
    const s = this.strip;
    return JSON.stringify({ editable: true, leds: s.leds, pin: s.pin, reverse: s.reverse, brightness: s.brightness,
                            gamma: s.gamma, white_balance: s.white_balance, scenes: s.scenes });
  }
  applyStripEdit(e) {
    const s = this.strip;
    for (const k of ['reverse', 'brightness', 'gamma', 'white_balance']) if (k in e) s[k] = e[k];
    for (const x of e.scenes || []) {
      const sc = s.scenes.find(q => q.p === x.p);
      if (!sc) continue;
      if ('on' in x) sc.on = x.on;
      if ('color' in x && 'color' in sc) sc.color = x.color;
      if ('l' in x && 'l' in sc) sc.l = x.l;
    }
  }
  pcfgJson() { return JSON.stringify({ editable: true, ...this.power }); }
}

// ---- the simulated receiver ----
class Board {
  constructor({ id, name, token, near, psu, host, sense, headerPins, inPins, outPins, fans }) {
    Object.assign(this, { id, name, token, near, psu, host, sense, headerPins, inPins, outPins });
    this.hostUp = psu === 2 && !!host;
    this.bootAt = Date.now() - (5 * 3600 + 17 * 60) * 1000;
    // the power switch: tunings in force, the wiring (ble.cpp buildPwr)
    const p = host ? host.power : {};
    this.tune = { hold: Math.round((p.hold_seconds ?? 2) * 1000), boot: Math.round((p.boot_timeout_seconds ?? 10) * 1000),
                  low: p.sense_low_mv ?? 800, high: p.sense_high_mv ?? 2000 };
    this.pins = { ps_on: 3, button: 1, button_gnd: NONE, sense: sense ? 2 : NONE, led: 8, wake: p.wake ?? NONE };
    // the standalone records, as the daemon last pushed them
    this.slots = new Array(CHANNELS).fill(null);
    this.push(fans || (host && host.fans) || []);
    this.chrs = null;   // uuid -> characteristic, while connected
    this.pageReq = null;
    this.snapshot = new Uint8Array(0);
    setInterval(() => this.tick(), 2000);
    setInterval(() => this.notify(U.pwr), 1000); // the sense wire, 1 Hz while subscribed
  }

  // the daemon pushes each receiver-output fan to a slot, in list order
  push(fans) {
    this.slots.fill(null);
    let slot = 0;
    for (const f of fans) {
      const out = outInfo(f.o);
      if ((out.kind !== 'header' && out.kind !== 'gpio') || slot >= CHANNELS) continue;
      const s = srcInfo(f.i);
      const kind = s.kind === 'fallback' || s.kind === 'gpio' || s.kind === 'esp32_temp' ? s.kind : 'host';
      this.slots[slot++] = { out, fallback: f.f ?? 100, boost: f.b ?? NONE, boostSecs: f.t ?? 5, ramp: f.r ?? 5, kind,
                             gpio: s.kind === 'gpio' ? s.gpio : NONE, pts: ownCurve(s) ? parseCurve(f.c) : [] };
    }
  }
  get temp() { return 34.2 + Math.sin(Date.now() / 31000) * 0.8; } // the chip's own sensor
  pwmIn() { return Math.round(62 + Math.sin(Date.now() / 11000) * 6); } // a fan wire on a receiver pin
  // one slot's live state: [state, duty, stored fallback, kind, input]
  live(i) {
    const r = this.slots[i];
    if (!r) return [0, NONE, NONE, NONE, NONE];
    let src, duty, inp = NONE;
    if (r.kind === 'gpio') { inp = this.pwmIn(); src = SRC.curve; duty = curveAt(r.pts, inp) ?? r.fallback; }
    else if (r.kind === 'esp32_temp') { src = SRC.curve; duty = curveAt(r.pts, this.temp) ?? r.fallback; }
    else if (r.kind === 'host' && this.hostUp) {
      src = SRC.live;
      const fi = this.fanIndexOf(i), t = fi >= 0 ? this.host.telemOf(this.host.fans[fi]) : null;
      duty = t && t.duty !== undefined ? t.duty : r.fallback;
    } else { src = SRC.fallback; duty = r.fallback; }
    return [1 | src << 1, duty, r.fallback, KIND_BYTE[r.kind], inp];
  }
  fanIndexOf(slot) {
    let n = -1;
    return this.host.fans.findIndex(f => { const o = outInfo(f.o); if (o.kind === 'header' || o.kind === 'gpio') n++; return n === slot; });
  }

  // ---- the values ----
  fansValue() {
    const b = new Uint8Array(FANS_LEN(3) + 2), v = dv(b);
    b[0] = 3; b[1] = 0x01 | (this.hostUp ? 0x08 | 0x10 | 0x20 : 0); b[2] = this.psu; b[3] = this.hostUp ? 1 : 255;
    for (let i = 0; i < CHANNELS; i++) b.set(this.live(i), 4 + i * 5);
    v.setUint32(4 + CHANNELS * 5, Math.floor((Date.now() - this.bootAt) / 1000), true);
    v.setInt16(FANS_LEN(3), Math.round(this.temp * 10), true);
    return b;
  }
  saValue() {
    const b = new Uint8Array(SA_LEN);
    this.slots.forEach((r, i) => b.set(encodeRecord(r), i * SA_HEADER_LEN));
    return b;
  }
  infoValue() {
    const b = new Uint8Array(49 + CHANNELS + 8), v = dv(b);
    const mask = (pins, at) => { for (const g of pins) b[at + (g >> 3)] |= 1 << (g & 7); };
    b[0] = 3;
    b.set(enc.encode('v1.33.0').subarray(0, 31), 1);
    v.setUint32(33, 143 * 1024, true); v.setUint32(37, 121 * 1024, true);
    mask(this.inPins.filter(g => g !== this.pins.wake), 41);
    for (let i = 0; i < CHANNELS; i++) b[49 + i] = this.headerPins[i] ?? NONE;
    mask(this.outPins, 49 + CHANNELS);
    return b;
  }
  pwrValue() {
    const b = new Uint8Array(PWR_LEN), v = dv(b);
    const mv = !this.sense ? 0xFFFF : this.psu === 2 ? 2890 + Math.round(Math.random() * 40) : 5 + Math.round(Math.random() * 12);
    b[0] = 1; b[1] = 0x01 | (this.sense ? 0x02 : 0); b[2] = this.psu; v.setUint16(3, mv, true);
    [this.tune.hold, this.tune.boot, this.tune.low, this.tune.high].forEach((x, i) => v.setUint16(5 + 2 * i, x, true));
    b.set([this.pins.ps_on, this.pins.button, this.pins.button_gnd, this.pins.sense, this.pins.led, this.pins.wake], 13);
    return b;
  }
  // a daemon value: empty while there's no daemon to say anything
  jsonValue(slot) {
    if (!this.hostUp) return new Uint8Array(0);
    const h = this.host;
    return enc.encode([() => h.cfgJson(), () => h.telemJson(), () => h.stripJson(), () => h.sensorsJson(), () => h.pcfgJson()][slot]());
  }
  read(u) {
    if (u === U.stat) return Uint8Array.of(this.psu);
    if (u === U.fans) return this.fansValue();
    if (u === U.fansa) return this.saValue();
    if (u === U.info) return this.infoValue();
    if (u === U.pwr) return this.pwrValue();
    if (u in JSON_SLOT) return this.jsonValue(JSON_SLOT[u]).subarray(0, 512);
    if (u === U.page) {
      const { slot, page } = this.pageReq || { slot: 0, page: 0 };
      const pages = Math.max(1, Math.ceil(this.snapshot.length / PAGE_CHUNK));
      const part = this.snapshot.subarray(page * PAGE_CHUNK, (page + 1) * PAGE_CHUNK);
      const b = new Uint8Array(6 + part.length);
      b.set([1, slot, page, pages]); dv(b).setUint16(4, this.snapshot.length, true); b.set(part, 6);
      return b;
    }
    return new Uint8Array(0);
  }

  // ---- the writes ----
  checkToken(b) {
    const want = new Uint8Array(TOKEN_LEN);
    want.set(enc.encode(this.token).subarray(0, TOKEN_LEN));
    if (b.length < TOKEN_LEN || want.some((x, i) => x !== b[i])) throw notPermitted();
    return b.subarray(TOKEN_LEN);
  }
  later(ms, ...uuids) { setTimeout(() => uuids.forEach(u => this.notify(u)), ms); }
  async write(u, b) {
    if (u === U.page) {
      this.pageReq = { slot: b[0], page: b[1] };
      if (b[1] === 0) this.snapshot = this.jsonValue(b[0]);
      return;
    }
    const a = this.checkToken(b);
    if (u === U.ctrl) return this.control(a);
    if (!this.hostUp) return; // relayed to a daemon that isn't there: no answer comes
    const edit = JSON.parse(dec.decode(a)), h = this.host;
    if (u === U.fancfg) {
      const err = h.applyFanEdit(edit);
      if (!err) { this.push(h.fans); this.later(300, U.fansa, U.fans, U.telem); }
      setTimeout(() => this.notifyWith(U.fancfg, enc.encode(h.cfgJson(err))), 600);
    } else if (u === U.stripcfg) {
      h.applyStripEdit(edit);
      this.later(400, U.stripcfg);
    } else if (u === U.pwrcfg) {
      for (const [k, x] of Object.entries(edit)) h.power[k] = x;
      Object.assign(this.tune, { hold: Math.round(h.power.hold_seconds * 1000), boot: Math.round(h.power.boot_timeout_seconds * 1000),
                                 low: h.power.sense_low_mv, high: h.power.sense_high_mv });
      if ('wake' in edit && (edit.wake === null || this.inPins.includes(edit.wake))) this.pins.wake = edit.wake ?? NONE;
      this.later(600, U.pwrcfg, U.pwr);
    }
  }
  control(a) {
    const op = a[0];
    if (op === OP_ON) { if (this.psu === 0) this.power(1); return; }
    if (op === OP_SHUTDOWN) { this.setHost(false); setTimeout(() => this.power(0), 2500); return; }
    if (op === OP_HARD_OFF) { this.setHost(false); this.power(0); return; }
    if (op === OP_PWR_TUNING) {
      const v = dv(a.slice(1)), t = [0, 2, 4, 6].map(o => v.getUint16(o, true));
      if (t[0] < 100 || t[1] < 1000 || t[2] >= t[3]) throw refused();
      [this.tune.hold, this.tune.boot, this.tune.low, this.tune.high] = t;
      this.notify(U.pwr);
      return;
    }
    if (op === OP_PWR_WAKE) {
      const pin = a[1];
      if (pin !== NONE && !this.inPins.includes(pin)) throw refused();
      this.pins.wake = pin;
      this.notify(U.pwr);
      return;
    }
    if (op === OP_FAN_HEADER) {
      const slot = a[1], r = decodeRecord(a.subarray(2, 2 + SA_HEADER_LEN));
      if (slot >= CHANNELS) throw refused();
      if (r && r.kind === 'gpio' && (r.gpio === 4 || r.gpio === 9 || r.gpio > 21)) throw refused(); // a pin it can't read
      this.slots[slot] = r;
      this.later(400, U.fansa, U.fans);
      return;
    }
    throw refused();
  }
  // the PSU: on boots for a while, then the host's daemon connects and pushes its config
  power(psu) {
    this.psu = psu;
    this.notify(U.stat); this.notify(U.fans); this.notify(U.pwr);
    if (psu === 1) setTimeout(() => { if (this.psu !== 1) return; this.power(2); setTimeout(() => { if (this.psu === 2) this.setHost(true); }, 1500); }, 3000);
  }
  setHost(up) {
    if (!this.host || this.hostUp === up) return;
    this.hostUp = up;
    if (up) {
      this.push(this.host.fans);
      const p = this.host.power;
      Object.assign(this.tune, { hold: Math.round(p.hold_seconds * 1000), boot: Math.round(p.boot_timeout_seconds * 1000), low: p.sense_low_mv, high: p.sense_high_mv });
      if (p.wake !== undefined) this.pins.wake = p.wake ?? NONE;
    }
    for (const u of [U.fans, U.fansa, U.pwr, U.fancfg, U.telem, U.stripcfg, U.sensors, U.pwrcfg]) this.notify(u);
  }
  tick() {
    this.notify(U.fans);
    if (this.hostUp) { this.notify(U.telem); this.notify(U.sensors); }
  }

  // ---- the link ----
  notify(u) { this.notifyWith(u, null); }
  notifyWith(u, bytes) {
    const c = this.chrs && this.chrs[u];
    if (!c || !c.notifying) return;
    c.value = dv(bytes ?? (u in JSON_SLOT ? this.jsonValue(JSON_SLOT[u]) : this.read(u)));
    c.dispatchEvent(new Event('characteristicvaluechanged'));
  }
}

class Characteristic extends EventTarget {
  constructor(board, service, u) { super(); this.board = board; this.service = service; this.uuid = u; this.value = null; this.notifying = false; }
  live() { if (this.board.chrs?.[this.uuid] !== this) throw new DOMException('GATT Server is disconnected.', 'NetworkError'); }
  async readValue() { await sleep(15); this.live(); this.value = dv(this.board.read(this.uuid).slice()); return this.value; }
  async writeValueWithResponse(v) {
    await sleep(20); this.live();
    await this.board.write(this.uuid, new Uint8Array(v.buffer ? v.buffer.slice(v.byteOffset, v.byteOffset + v.byteLength) : v));
  }
  async startNotifications() { await sleep(10); this.live(); this.notifying = true; return this; }
}

class Device extends EventTarget {
  constructor(board) {
    super();
    this.board = board; this.id = board.id; this.name = board.name;
    const device = this;
    const service = { uuid: SVC, device, isPrimary: true,
      async getCharacteristic(u) {
        const c = board.chrs && board.chrs[u];
        if (!c) throw new DOMException(`No Characteristics matching UUID ${u} found in Service.`, 'NotFoundError');
        return c;
      } };
    this.gatt = {
      device, connected: false,
      async connect() {
        if (!board.near) { await sleep(3000); throw new DOMException('Connection failed for unknown reason.', 'NetworkError'); }
        await sleep(250);
        if (!this.connected) {
          board.chrs = Object.fromEntries(Object.values(U).map(u => [u, new Characteristic(board, service, u)]));
          this.connected = true;
        }
        return this;
      },
      disconnect() {
        if (!this.connected) return;
        this.connected = false; board.chrs = null;
        setTimeout(() => device.dispatchEvent(new Event('gattserverdisconnected')));
      },
      async getPrimaryService(u) {
        if (!this.connected) throw new DOMException('GATT Server is disconnected.', 'NetworkError');
        if (u !== SVC) throw new DOMException(`No Services matching UUID ${u} found in Device.`, 'NotFoundError');
        return service;
      },
    };
  }
  // advertisements, every 1.3 s while in range and not connected (ble.cpp:
  // manufacturer data 0xFFFF = ver 1, the host state)
  async watchAdvertisements({ signal } = {}) {
    const advertise = () => {
      if (!this.board.near || this.gatt.connected) return;
      this.dispatchEvent(Object.assign(new Event('advertisementreceived'),
        { device: this, manufacturerData: new Map([[ADV_MFG_ID, dv(Uint8Array.of(1, this.board.psu))]]) }));
    };
    const first = setTimeout(advertise, 300), every = setInterval(advertise, 1300);
    signal?.addEventListener('abort', () => { clearTimeout(first); clearInterval(every); });
  }
  async forget() { granted.delete(this.id); }
}

// ---- the boards ----
const bc250 = new Board({
  id: 'sim-bc250', name: 'BC250', token: 'bc250-token', near: true, psu: 2, sense: true,
  headerPins: [5, 6, 7, 10, null, null], inPins: [0, 20, 21], outPins: [0, 1, 2, 3, 20, 21],
  host: new Host({
    fans: [
      { n: 'pump', o: 'header1', i: 'fallback', b: 100, f: 65 },
      { n: 'radiator', o: 'header2', i: 'temp', c: '45:35 60:55 75:100', f: 100 },
      { n: 'exhaust', o: 'header3', i: 'gpio:0', c: '0:25 100:80', f: 100, r: 0 },
      { n: 'intake', o: 'header4', i: 'gpu_load', c: '0:20 40:20 100:60', f: 60 },
      { n: 'board fan', o: 'nct6686:pwm2', i: 'k10temp:Tctl', c: '50:30 80:100', f: 60 },
      { n: 'chipset', o: '', i: 'nct6686:pwm1' },
      { n: 'spare', o: '', i: 'fallback', f: 40 } ],
    strip: { leds: 33, pin: 4, reverse: false, brightness: 1, gamma: '2.2', white_balance: 'ffb0f0', scenes: [
      { p: '/tmp/led-static-color', e: 'solid', on: false, color: 'ffffff', l: 0.6 },
      { p: '/tmp/led-night', e: 'drift', on: false } ] },
    power: { hold_seconds: 2, boot_timeout_seconds: 10, sense_low_mv: 800, sense_high_mv: 2000, wake: null, short_press: 'systemctl poweroff' },
    chips: { amdgpu: { edge: 61.0, junction: 64.5, mem: 58.0 }, k10temp: { Tctl: 58.3 },
             nct6686: { CPU: 52.0, System: 38.5, 'VRM MOS': 41.0 } },
    outs: [['nct6686:pwm1', 48, 1100, 1], ['nct6686:pwm2', 57, 1420, 1], ['amdgpu:pwm1', 30, 900, 0]],
    vrm: true,
  }),
});
const desk = new Board({
  id: 'sim-desk', name: 'Desk ESP32', token: 'desk-token', near: true, psu: 0, sense: false,
  headerPins: [16, 17, null, null, null, null], inPins: [25, 26], outPins: [18, 19, 25, 26],
  host: new Host({
    fans: [
      { n: 'cpu', o: 'header1', i: 'temp', c: '40:30 70:100', f: 70, b: 100 },
      { n: 'case', o: 'header2', i: 'esp32_temp', c: '30:30 50:100', f: 50 } ],
    strip: { leds: 60, pin: 13, reverse: true, brightness: 0.7, gamma: '2.2', white_balance: 'ffffff', scenes: [] },
    power: { hold_seconds: 4, boot_timeout_seconds: 20, sense_low_mv: 800, sense_high_mv: 2000, short_press: 'systemctl suspend' },
    chips: { k10temp: { Tctl: 46.0 } },
    outs: [],
  }),
});
const media = new Board({
  id: 'sim-media', name: 'Media PC', token: 'media-token', near: false, psu: 0, sense: true,
  headerPins: [5, 6, 7, 10, null, null], inPins: [0], outPins: [0, 1], host: null, fans: [],
});
const BOARDS = [bc250, desk, media];
const devices = new Map(BOARDS.map(b => [b.id, new Device(b)]));
const granted = new Set(devices.keys());

const bluetooth = {
  async getDevices() { return [...granted].map(id => devices.get(id)); },
  // the chooser: the first board in range, granted from then on
  async requestDevice() {
    await sleep(400);
    const d = [...devices.values()].find(x => x.board.near);
    if (!d) throw new DOMException('User cancelled the requestDevice() chooser.', 'NotFoundError');
    granted.add(d.id);
    return d;
  },
};

// this page's memory of the boards, as if each had been added before
function memoryStorage() {
  const m = new Map([
    ['ble-receivers', JSON.stringify(Object.fromEntries(BOARDS.map(b => [b.id, { name: b.name, token: b.token }])))],
    ['ble-current', bc250.id],
    ['ble-token', bc250.token] ]);
  return { getItem: k => m.has(k) ? m.get(k) : null, setItem: (k, v) => m.set(k, String(v)) };
}

export const provider = () => ({ bluetooth, storage: memoryStorage() });
