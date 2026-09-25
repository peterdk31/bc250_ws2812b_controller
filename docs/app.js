// The screens: Preact + htm (vendor/preact-htm.mjs, no build step) rendering
// whatever ble.js holds in `S`. Four tabs — Power, Fans, LEDs, Receiver —
// plus two full-screen editors (a fan header, the power switch's settings)
// and the power sheet. Every one of those is a history entry, so the phone's back
// gesture closes the sheet, leaves the editor, returns to the Power tab, and
// only then leaves the app.
//
// Nothing here talks to the receiver directly: the actions are ble.js's.
// Anything that must survive a redraw (a slider under the finger, an editor's
// working copy) is component state; everything else is a function of `S`.
import { html, render, useState, useEffect, useRef, useMemo } from './vendor/preact-htm.mjs';
import * as B from './ble.js';
const { S } = B;
const hasBt = () => B.HAS_BT || S.demo; // the demo stands in for a receiver

// ---- helpers ----
const fmt1 = v => (Math.round(v * 10) / 10).toString();
const fmtIn = (h, v) => B.isTempX(h) ? `${fmt1(v)} °C` : `${fmt1(v)} %`;
const fmtUptime = s => s < 3600 ? `${Math.floor(s / 60)} min` :
  s < 86400 ? `${Math.floor(s / 3600)} h ${Math.floor(s % 3600 / 60)} min` :
  `${Math.floor(s / 86400)} d ${Math.floor(s % 86400 / 3600)} h`;
const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

// re-render on every emit()
function useStore() {
  const [, tick] = useState(0);
  // subscribe, then redraw once: whatever changed before the effect ran
  // (boot() fires right after the first render) is picked up
  useEffect(() => { const off = B.subscribe(() => tick(n => n + 1)); tick(n => n + 1); return off; }, []);
}

// a <select> built once per distinct `key` (a string naming its options and
// value): the page redraws on every telemetry frame, and Preact's diff blanks
// and rewrites each option's value on every pass (its guard against text
// children clobbering the value) — Android Chrome dismisses an open dropdown
// the moment an option changes under it, so a select under the finger closed
// a second after it opened. Handing the diff the same vnode back skips the
// subtree wholesale; the vnode is rebuilt only when the key changes.
const useSteadySelect = (key, make) => useMemo(make, [key]);

// ---- icons: stroke SVGs on a 24 px grid, colored by currentColor ----
const Icon = ({ d, size = 20, sw = 2 }) => html`<svg class="ic" width=${size} height=${size} viewBox="0 0 24 24" fill="none"
  stroke="currentColor" stroke-width=${sw} stroke-linecap="round" stroke-linejoin="round" dangerouslySetInnerHTML=${{ __html: d }}></svg>`;
const I = {
  power: '<path d="M12 3v9"></path><path d="M6.3 6.3a8 8 0 1 0 11.4 0"></path>',
  fan: '<circle cx="12" cy="12" r="2"></circle><path d="M12 10c0-4 2-7 5-7 1 2 0 5-3 6"></path><path d="M14 12c4 0 7 2 7 5-2 1-5 0-6-3"></path><path d="M12 14c0 4-2 7-5 7-1-2 0-5 3-6"></path><path d="M10 12c-4 0-7-2-7-5 2-1 5 0 6 3"></path>',
  strip: '<rect x="2" y="9" width="20" height="6" rx="3"></rect><circle cx="7" cy="12" r="1"></circle><circle cx="12" cy="12" r="1"></circle><circle cx="17" cy="12" r="1"></circle>',
  board: '<rect x="4" y="4" width="16" height="16" rx="2"></rect><rect x="9" y="9" width="6" height="6"></rect><path d="M9 1v3M15 1v3M9 20v3M15 20v3M1 9h3M1 15h3M20 9h3M20 15h3"></path>',
  down: '<path d="M6 9l6 6 6-6"></path>', up: '<path d="M6 15l6-6 6 6"></path>',
  right: '<path d="M9 6l6 6-6 6"></path>', left: '<path d="M15 6l-6 6 6 6"></path>',
  plus: '<path d="M12 5v14M5 12h14"></path>', check: '<path d="M5 12l5 5L20 7"></path>',
  cog: '<circle cx="12" cy="12" r="3"></circle><path d="M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z"></path>',
};

// ---- routing: the route IS the history entry ----
// { tab, editor: null | a fan card's key ('f2', 's0', 'new') | 'p' (power), sheet }
const TABS = ['power', 'fans', 'leds', 'receiver'];
let route = { tab: 'power', editor: null, sheet: false };
let setRouteState = null;
let leaveGuard = null; // () => true when a dirty editor may be left (asks the user)
let skipGuard = false; // one back() that must not ask: a save just landed
function go(patch, replace = false) {
  route = { ...route, ...patch };
  B.dismiss();
  try { history[replace ? 'replaceState' : 'pushState'](route, ''); } catch {}
  setRouteState?.(route);
}
function back() { history.back(); }
window.addEventListener('popstate', e => {
  const next = e.state && TABS.includes(e.state.tab) ? e.state : { tab: 'power', editor: null, sheet: false };
  if (route.editor !== null && next.editor === null && !skipGuard && leaveGuard && !leaveGuard()) {
    try { history.pushState(route, ''); } catch {} // stay: the user kept the editor
    return;
  }
  skipGuard = false;
  B.dismiss();
  route = next;
  setRouteState?.(route);
});

// ---- small controls ----
// a range slider that follows `value` except while the finger is on it;
// live(v) on every move, done(v) once on release
function Slider({ value, min = 0, max = 100, step = 1, live, done, disabled, label, cls = '' }) {
  const [local, setLocal] = useState(null); // the dragged value, or null when following
  const v = local === null ? value : local;
  return html`<div class="sl ${cls}">
    <input type="range" min=${min} max=${max} step=${step} value=${v} disabled=${disabled}
      onPointerDown=${() => setLocal(value)}
      onInput=${e => { const n = +e.target.value; setLocal(n); live?.(n); }}
      onChange=${e => { const n = +e.target.value; setLocal(null); done?.(n); }}
      onPointerCancel=${() => setLocal(null)} />
    ${label !== undefined && html`<span class="val">${label(v)}</span>`}
  </div>`;
}

// a number box that keeps what is typed while it has focus. A controlled
// <input value> redraws the stored number on every render — and the page
// redraws every second — so a box emptied to type a new value got its old
// number back under the finger. This one shows the typed text until the box
// is left, reports every value that parses (onValue(null) for an emptied box
// where `blank` allows one), and on leaving shows the stored value again —
// so an emptied or half-typed box falls back to it, and a clamped one shows
// what was kept.
function NumField({ value, onValue, min, max, step, placeholder, blank = false, round = false, disabled, aria }) {
  const [text, setText] = useState(null); // null: follow `value`
  const stored = value === null || value === undefined || Number.isNaN(value) ? '' : String(value);
  return html`<input type="number" inputmode="decimal" min=${min} max=${max} step=${step} placeholder=${placeholder}
    disabled=${disabled} aria-label=${aria} value=${text !== null ? text : stored}
    onFocus=${() => setText(stored)}
    onInput=${e => {
      const t = e.target.value;
      setText(t);
      if (t === '') { if (blank) onValue(null); return; }
      const n = parseFloat(t);
      if (!Number.isNaN(n)) onValue(round ? Math.round(n) : n);
    }}
    onBlur=${() => setText(null)} />`;
}

const Switch = ({ on, change, disabled }) => html`<label class="sw">
  <input type="checkbox" checked=${on} disabled=${disabled} onChange=${e => change(e.target.checked)} /><i></i></label>`;

const Card = ({ title, right, children, cls = '' }) => html`<div class="card ${cls}">
  ${title && html`<h2>${title}${right && html`<span class="r">${right}</span>`}</h2>`}
  ${children}</div>`;

// the page's one status line, under the header: a save's progress, or a
// refusal. Positioned over the content, so nothing moves when it shows.
const Status = () => { const n = S.note; return n?.text ? html`<div class="status ${n.cls}" onClick=${B.dismiss}>${n.text}</div>` : null; };

// ---- the header: receiver, PSU state, the host's readings ----
function Header({ back: backLabel, title }) {
  const on = B.connected();
  const state = !on ? (S.busy || S.attempt ? 'Connecting…' : 'Not connected')
              : S.psu === 2 ? 'On' : S.psu === 1 ? 'Booting' : 'Off';
  const color = on && S.psu === 2 ? 'var(--on)' : on && S.psu === 1 ? 'var(--booting)' : 'var(--off)';
  const { ids, labels } = B.labels();
  const cur = B.currentId();
  const live = S.fans && S.fans.telem && S.telem;
  const sel = ids.includes(cur) ? cur : '';
  const pickReceiver = useSteadySelect(JSON.stringify([ids, ids.map(id => labels[id]), sel, !!S.busy]), () => html`
            <select aria-label="Receiver" value=${sel} disabled=${S.busy}
              onChange=${e => { const v = e.target.value; e.target.value = sel; if (v === '+') B.connect(); else if (v) B.select(v); }}>
              ${!sel && html`<option value="" disabled>Receiver…</option>`}
              ${ids.map(id => html`<option key=${id} value=${id}>${labels[id]}</option>`)}
              <option value="+">Add a receiver…</option>
            </select>`);
  return html`<header>
    <div class="top">
      ${backLabel ? html`<button class="back" onClick=${back}><${Icon} d=${I.left} /><span>${backLabel}</span></button>`
        : B.anyKnown() && hasBt() ? html`<div class="pill">${pickReceiver}<${Icon} d=${I.down} size=${16} /></div>`
        : html`<h1>BC-250</h1>`}
      ${title && html`<span class="ttl">${title}</span>`}
      <div class="st"><i style=${`background: ${color}; box-shadow: 0 0 10px ${color}`}></i><span>${state}</span></div>
    </div>
    ${live && html`<div class="readings">
      <div><span class="k">Temp</span><span class="v">${S.telem.temp !== undefined ? fmt1(S.telem.temp) : '—'}<small>°C</small></span></div>
      <div><span class="k">CPU</span><span class="v">${S.telem.cpu ?? '—'}<small>%</small></span></div>
      <div><span class="k">GPU</span><span class="v">${S.telem.gpu ?? '—'}<small>%</small></span></div>
    </div>`}
    <${Status} />
  </header>`;
}

const Msg = () => S.msg.text ? html`<div class="msg ${S.msg.hint ? 'hint' : ''}" ...${S.msg.html ? { dangerouslySetInnerHTML: { __html: S.msg.text } } : {}}>${S.msg.html ? null : S.msg.text}</div>` : null;

// the token: first run, "Change token", or a rejected write
function TokenCard() {
  const [v, setV] = useState('');
  const has = !!B.token();
  const save = () => { if (B.saveToken(v.trim())) setV(''); };
  return html`<${Card} title=${S.device ? `Token for ${B.label(S.device.id)}` : 'Receiver token'}>
    <input class="tok" type="password" autocomplete="off" placeholder="token" maxlength="16" value=${v}
      onInput=${e => setV(e.target.value)} onKeyDown=${e => { if (e.key === 'Enter') save(); }} />
    <div class="note">The token the receiver was flashed with. Kept on this phone only.</div>
    <div class="btns">${has && html`<button class="minor" onClick=${() => B.editToken(false)}>Cancel</button>`}
      <button class="primary" onClick=${save}>Save token</button></div>
  <//>`;
}

// ---- Power ----
// a receiver whose pwr value says it has no power switch: the remote runs for
// the fans and the strip alone, and the receiver refuses the power ops
const noSwitch = () => B.connected() && !!S.pwr && !S.pwr.active;

function PowerScreen() {
  const on = B.connected();
  if (noSwitch()) return html`<div class="empty">No power switch on this receiver. Fans and LEDs are in their tabs.</div>`;
  const pending = !!S.device && S.attempt === S.device;
  let label, disabled = false, act = null;
  if (!hasBt() || !B.token()) { label = 'Connect'; disabled = true; }
  else if (!on) { label = S.busy || pending ? 'Connecting…' : 'Connect'; disabled = S.busy; act = B.connect; }
  else if (S.psu === 0) { label = 'Power on'; disabled = S.busy; act = () => B.power(B.OP_ON); }
  else if (S.psu === 1) { label = 'Booting…'; disabled = true; }
  else { label = 'On'; act = () => go({ sheet: true }); }
  const cls = (['off', 'booting', 'on'][S.psu] || 'off') + (pending || S.busy ? ' wait' : '');
  // the cog: the switch's tunings, on a receiver whose firmware publishes them
  const cog = on && S.pwr && S.pwr.active;
  return html`<div class="center">
    ${cog && html`<button class="pcog" aria-label="Power switch settings" onClick=${() => go({ editor: 'p' })}><${Icon} d=${I.cog} sw=${1.8} /></button>`}
    <button id="power" class=${cls} disabled=${disabled} onClick=${act}><span class="sym"></span><span>${label}</span></button>
    ${on && S.psu === 2 && html`<div class="hint">tap for shutdown options</div>`}
  </div>`;
}

// ---- the power switch editor ----
// The four tunings the receiver runs, plus the daemon's short-press command.
// Where Save goes depends on who can hear it (B.powerRoute): the daemon
// while the host is up (into the config, pushed to the receiver), the
// receiver alone otherwise. Of the pins only the wake input is edited — an
// input that can do nothing but power on, picked from the receiver's free
// pins (B.wakePinOptions) and routed by B.wakeRoute; the rest are shown,
// never edited: they are flash-time, and a pin the host's power hangs on is
// nothing for a phone.
const fmtS = v => (Math.round(v * 10) / 10).toString();
function PowerEditor() {
  const origRef = useRef(null);
  if (!origRef.current) origRef.current = B.powerTuning();
  const orig = origRef.current;
  const [t, setT] = useState(() => orig && { ...orig });
  useEffect(() => { if (!t && orig) setT({ ...orig }); }, [!!orig]);
  const dirty = () => JSON.stringify(t) !== JSON.stringify(orig);
  useEffect(() => { leaveGuard = () => !dirty() || confirm('Leave without saving?'); return () => { leaveGuard = null; }; });
  const set = patch => setT(x => ({ ...x, ...patch }));
  // the wake pin's picker, built once per distinct option list (see useSteadySelect)
  const wakeRoute = B.wakeRoute(), wakeRo = wakeRoute === 'readonly';
  const wake = t ? t.wake : null;
  const wakeOpts = B.wakePinOptions(wake);
  const wakeSelect = useSteadySelect(wakeOpts ? `${wakeOpts.join()}=${wake}${wakeRo ? '!' : ''}` : '', () => wakeOpts && html`
          <select value=${wake === null ? '' : wake} disabled=${wakeRo} onChange=${e => set({ wake: e.target.value === '' ? null : +e.target.value })}>
            <option value="">None</option>${wakeOpts.map(g => html`<option key=${g} value=${g}>GPIO${g}</option>`)}</select>`);
  const p = S.pwr;
  if (!B.connected() || !p || !t) return html`<${Header} back="Power" title="Power switch" /><div class="empty">${B.connected() ? 'No power switch on this receiver.' : 'Not connected.'}</div>`;
  const route = B.powerRoute();
  const ro = route === 'readonly';
  // the two number fields are uncontrolled and land as typed (a controlled
  // value clamped on every keystroke would turn a "0" on its way to "0.5"
  // into the floor and eat the dot); the range is applied on blur, and
  // checkPower refuses anything still outside it at Save
  const num = (k, v) => { const n = parseFloat(v); if (!isNaN(n)) set({ [k]: n }); };
  const fit = (k, lo, hi) => e => { const n = parseFloat(e.target.value), v = clamp(isNaN(n) ? t[k] : n, lo, hi); set({ [k]: v }); e.target.value = fmtS(v); };
  const saving = S.saving !== null;
  // the wire as the current thresholds would read it
  const mv = p.mv, level = mv === null ? null : mv > t.high ? 'up' : mv < t.low ? 'down' : 'between';
  const wires = B.PIN_KEYS.filter(k => k !== 'wake').map(k => [k, p.pins[k]]).filter(([, g]) => g !== null); // the wake pin has its own card
  const NAMES = { ps_on: 'PS_ON#', button: 'Button', button_gnd: 'Button ground', sense: 'Sense', led: 'LED' };
  const cmd = t.shortPress ?? null;
  return html`<${Header} back="Power" title="Power switch" />
    <div class="list">
      ${p.sense ? html`<div class="card">
        <h2>Sense wire<span class="r">${mv === null ? '' : `reads ${level}`}</span></h2>
        <div class="mvrow"><span class="mv">${mv === null ? '—' : mv}<small>mV</small></span>
          <div class="note">The board's 3.3 V rail, as the receiver reads it right now. Above the upper value the board counts as up, below the lower one as down; in between nothing changes. Read it with the board on and off, and put the two values well apart between those readings.</div></div>
        <div class="sub">Board is up above</div>
        <${Slider} value=${t.high} min=${0} max=${3300} step=${10} disabled=${ro} label=${v => `${v} mV`} live=${v => set({ high: v })} done=${v => set({ high: v })} />
        <div class="sub">Board is down below</div>
        <${Slider} value=${t.low} min=${0} max=${3300} step=${10} disabled=${ro} label=${v => `${v} mV`} live=${v => set({ low: v })} done=${v => set({ low: v })} />
        <label class="frow"><span>Boot timeout</span><input type="number" min="1" max="65" step="1" disabled=${ro} defaultValue=${fmtS(t.boot)} onInput=${e => num('boot', e.target.value)} onBlur=${fit('boot', 1, 65.535)} /><small>s for the board to come up, else the power is cut</small></label>
      </div>` : html`<div class="card"><h2>Sense wire</h2><div class="note">None on this receiver: the switch can't follow a shutdown down or give up on a boot that never comes up. Only the hold time applies.</div></div>`}
      <div class="card">
        <h2>Button</h2>
        <label class="frow"><span>Hold to force off</span><input type="number" min="0.1" max="65" step="0.1" disabled=${ro} defaultValue=${fmtS(t.hold)} onInput=${e => num('hold', e.target.value)} onBlur=${fit('hold', 0.1, 65.535)} /><small>s while on</small></label>
        ${S.pcfg && route !== 'receiver' ? html`<div class="swrow"><span>Short press shuts down</span><${Switch} on=${cmd !== null} disabled=${ro} change=${on => set({ shortPress: on ? (orig.shortPress || 'systemctl poweroff') : null })} /></div>
          <div class="note">${cmd !== null ? html`Runs <code>${cmd}</code>. ` : 'A short press is ignored. '}The same setting the phone's Shut down uses.</div>`
        : html`<div class="note">What a short press does is the host's setting — available when on.</div>`}
      </div>
      <div class="card">
        <h2>Wake input${route !== 'receiver' && wakeRoute === 'receiver' && html`<span class="r">receiver only</span>`}</h2>
        ${wakeOpts
          ? html`<label class="frow"><span>Pin</span>${wakeSelect}</label>`
          : html`<label class="frow"><span>Pin</span><${NumField} min=${0} max=${63} step=${1} placeholder="none" disabled=${wakeRo} value=${wake}
              blank=${true} round=${true} onValue=${v => set({ wake: v === null ? null : clamp(v, 0, 63) })} /><small>a free receiver pin, blank = none</small></label>`}
        <div class="note">A 3.3 V pulse on this pin powers the host on and can do nothing else — an OpenPuck's ColdBoot output, for one (GPIO20 on the carrier's J11, with the puck's ground beside it). ${wakeOpts ? 'The list is the receiver\u2019s free pins.' : 'This receiver doesn\u2019t list its free pins; it refuses one it can\u2019t use.'}${route !== 'receiver' && wakeRoute === 'receiver' ? ' The host\u2019s daemon predates this setting, so the pin is kept on the receiver alone.' : ''}</div>
      </div>
      ${wires.length > 0 && html`<div class="card"><h2>Wiring<span class="r">set when flashing</span></h2>
        <div class="facts">${wires.map(([k, g]) => html`<span key=${k} class="k">${NAMES[k]}</span><span key=${k + 'v'}>GPIO${g}</span>`)}</div>
      </div>`}
      <div class="note">${route === 'daemon' ? 'Saved into the config and pushed to the receiver.'
        : route === 'readonly' ? 'Settings are read-only right now: the config can\u2019t be written.'
        : 'Saved on the receiver. At the next boot the config takes over again — put the same values there to keep them.'}</div>
      <div class="btns two"><button class="minor" onClick=${back}>Cancel</button>
        <button class="primary" disabled=${saving || ro} onClick=${() => B.savePower({ ...t, wake: t.wake === orig.wake ? undefined : t.wake, shortPress: t.shortPress === orig.shortPress ? undefined : t.shortPress })}>Save</button></div>
    </div>`;
}

function PowerSheet() {
  const name = S.device ? B.label(S.device.id) : 'The host';
  const off = op => { if (op === B.OP_HARD_OFF && !confirm(`Force ${name} off? Unsaved work on it is lost.`)) return; B.power(op); back(); };
  return html`<div class="scrim" onClick=${e => { if (e.target === e.currentTarget) back(); }}>
    <div class="sheet">
      <div class="grip"></div>
      <div class="sname">${name} is ${S.psu === 1 ? 'booting' : 'on'}</div>
      ${S.fans && html`<div class="ssub">Uptime ${fmtUptime(S.fans.uptime)}</div>`}
      ${S.psu === 2 && html`<button class="danger" disabled=${S.busy} onClick=${() => off(B.OP_SHUTDOWN)}>Shut down</button>
        <div class="note">A clean shutdown. It takes a moment.</div>`}
      ${S.psu >= 1 && html`<button class="hard" disabled=${S.busy} onClick=${() => off(B.OP_HARD_OFF)}>Force off</button>
        <div class="note">Cuts the power like holding the button. Only for a host that is wedged — unsaved work is lost.</div>`}
      <button class="minor" onClick=${back}>Cancel</button>
    </div></div>`;
}

// ---- Fans ----
// what a row says under its name: the output, the input, then what the fan
// runs on right now — the input's reading when the input is in force,
// otherwise the thing that runs it instead (the chip names the same state).
// "no reading" is only for an input that is in force and has none.
const INPUT_NAMES = { fallback: 'Fixed speed', host: 'Host curve', temp: 'CPU temperature', cpu_load: 'CPU load',
                      gpu_load: 'GPU load', board: 'Board curve' };
const inputName = c => c.kind === 'gpio' ? `PWM input on GPIO${c.gpio}` : c.kind === 'hwmon' ? c.spec
  : c.kind === 'pwm' ? `Board fan header ${c.spec}` : INPUT_NAMES[c.kind] ?? c.src;
const HOST_STATES = { gone: 'not found on this machine', ro: 'read-only driver — the board runs it', busy: 'not driven by this host',
                      board: 'the board runs it' };
function describe(c) {
  const out = c.out.kind === 'parked' ? 'No output' : B.outLabel(c.out);
  const what = `${out} · ${inputName(c)}`;
  if (c.out.kind === 'parked') return what;
  const f = B.liveOf(c), t = B.telemOf(c);
  if (B.isReceiverOut(c.out) && f && !f.wired) return `${what} · not driven by the receiver`;
  if (c.out.kind === 'host' && t && t.st && t.st !== 'host' && !(t.st === 'board' && c.kind === 'board')) return `${what} · ${HOST_STATES[t.st] || t.st}`;
  if (c.kind === 'fallback') return what;
  const chip = chipOf(c);
  if (chip) {
    const state = { hold: 'held at the last speed', boost: 'power-on boost', fallback: 'fallback speed' };
    return `${what} · ${state[chip[0]]}`;
  }
  if (c.kind === 'board') return what;
  const now = B.inputOf(c);
  return `${what} · ${now === null ? 'no reading' : fmtIn(c, now)}`;
}

// the exception chip: only when the fan is not doing what it is set up for
function chipOf(c) {
  if (c.out.kind === 'host') {
    const t = B.telemOf(c);
    if (!t || !t.st || t.st === 'host' || (t.st === 'board' && c.kind === 'board')) return null;
    return ['fallback', t.st === 'gone' ? 'no output' : t.st === 'ro' ? 'read-only' : t.st === 'busy' ? 'not driven' : 'board'];
  }
  const f = B.liveOf(c);
  if (!f || !f.wired) return null;
  if (f.hold) return ['hold', 'hold'];
  const src = B.SRC_NAMES[f.src];
  if (src === 'boost') return ['boost', 'boost'];
  if (src === 'fallback' && !B.isFixed(c)) return ['fallback', 'fallback'];
  return null;
}

// the duty a card runs right now: the receiver's for its outputs, the
// daemon's telemetry for a host output (the board's own duty included)
function dutyOf(c) {
  if (B.isReceiverOut(c.out)) { const f = B.liveOf(c); return f && f.wired && f.duty !== B.NONE ? f.duty : null; }
  const t = B.telemOf(c);
  return t && t.duty !== undefined ? t.duty : null;
}

function FanRow({ c, open, toggle }) {
  const duty = dutyOf(c);
  const chip = chipOf(c);
  const editable = c.route === 'daemon' ? !!(S.cfg && S.cfg.editable) : true;
  const hasCurve = c.pts.length > 0 && !B.isFixed(c);
  return html`<div class="card fan ${open && hasCurve ? 'open' : ''}">
    <div class="head" onClick=${hasCurve ? toggle : null}>
      <div class="nm"><span class="name">${c.name || B.outLabel(c.out)}</span>${chip && html`<span class="chip ${chip[0]}">${chip[1]}</span>`}</div>
      <span class="duty">${duty === null ? '—' : duty}<small>%</small></span>
      ${editable ? html`<button class="cog" aria-label="Settings" onClick=${e => { e.stopPropagation(); go({ editor: c.key }); }}><${Icon} d=${I.cog} sw=${1.8} /></button>` : html`<span class="cog"></span>`}
      <div class="what">${describe(c)}</div>
      <div class="bar ${chip ? chip[0] : ''}"><i style=${`width: ${duty === null ? 0 : duty}%`}></i></div>
    </div>
    ${open && hasCurve && html`<${Curve} h=${c} now=${B.inputOf(c)} />`}
  </div>`;
}

function FansScreen() {
  const [open, setOpen] = useState(() => new Set());
  const toggle = key => setOpen(s => { const n = new Set(s); n.has(key) ? n.delete(key) : n.add(key); return n; });
  if (!B.connected() || !S.fansChr)
    return html`<div class="empty">${B.connected() ? 'This receiver has no fan control.' : 'Not connected.'}</div>`;
  if (!B.fansCurrent())
    return html`<div class="empty">This receiver runs older firmware. Update it (<code>make flash</code>) and the host's daemon (<code>make install</code>) to see and edit the fans here.</div>`;
  const list = B.cards();
  const ro = S.cfg && !S.cfg.editable;
  const canAdd = S.cfg ? !ro : !!(S.fans && S.fans.active && B.newFan());
  return html`<div class="list">
    ${!list.length && html`<div class="empty">${S.fans && !S.fans.active && !S.cfg ? 'The fan controller is off on this receiver.' : 'No fans yet.'}</div>`}
    ${list.map(c => html`<${FanRow} key=${c.key} c=${c} open=${open.has(c.key)} toggle=${() => toggle(c.key)} />`)}
    ${canAdd && html`<button class="rowlink add" onClick=${() => go({ editor: 'new' })}><${Icon} d=${I.plus} size=${16} /><span>add a fan</span></button>`}
    ${ro && html`<div class="empty">Settings are read-only right now.</div>`}
  </div>`;
}

// ---- the curve chart ----
// x spans the curve's points with a margin (percent kinds always 0..100), y
// is 0..100 duty; the polyline is flat beyond the end points, as the daemon
// evaluates it. `now` marks the current input on the curve. While editing
// the points drag: x stays between its neighbours, y in 0..100. A temperature
// axis stays within 0..TEMP_MAX, or dragging a point past the edge would
// widen the axis under the finger and run away with it.
const PAD = { l: 26, r: 8, t: 6, b: 16 }, W = 320, H = 120;
function xDomain(h) {
  if (!B.isTempX(h)) return [0, 100];
  const xs = h.pts.map(p => p.x).filter(Number.isFinite);
  let lo = Math.min(20, ...xs) - 5, hi = Math.max(90, ...xs) + 5;
  return [Math.max(0, Math.floor(lo / 10) * 10), Math.min(B.TEMP_MAX, Math.ceil(hi / 10) * 10)];
}
const sx = (x, d) => PAD.l + (x - d[0]) / (d[1] - d[0]) * (W - PAD.l - PAD.r);
const sy = y => PAD.t + (1 - y / 100) * (H - PAD.t - PAD.b);
const ux = (px, d) => d[0] + (px - PAD.l) / (W - PAD.l - PAD.r) * (d[1] - d[0]);
const uy = py => (1 - (py - PAD.t) / (H - PAD.t - PAD.b)) * 100;
function evalCurve(h, x) {
  const p = h.pts;
  if (x <= p[0].x) return p[0].y;
  if (x >= p[p.length - 1].x) return p[p.length - 1].y;
  for (let i = 1; i < p.length; i++)
    if (x <= p[i].x) { const a = p[i - 1], b = p[i]; return a.y + (x - a.x) / (b.x - a.x) * (b.y - a.y); }
  return p[p.length - 1].y;
}

function Curve({ h, now = null, editing = false, onChange }) {
  const svg = useRef(null), drag = useRef(null);
  const d = xDomain(h);
  // a point whose box is being retyped (blank for the moment) isn't drawn
  const shown = h.pts.map((p, i) => ({ ...p, i })).filter(p => Number.isFinite(p.x) && Number.isFinite(p.y));
  if (!shown.length) return html`<svg class="curve" viewBox="0 0 ${W} ${H}" preserveAspectRatio="none"></svg>`;
  const pts = shown.map(p => [sx(p.x, d), sy(p.y)]);
  const line = [[PAD.l, pts[0][1]], ...pts, [W - PAD.r, pts[pts.length - 1][1]]];
  const step = B.isTempX(h) ? 10 : 25;
  const xt = []; for (let x = d[0]; x <= d[1]; x += step) xt.push(x);
  const pos = e => { const r = svg.current.getBoundingClientRect(); return [(e.clientX - r.left) / r.width * W, (e.clientY - r.top) / r.height * H]; };
  const down = e => {
    if (!editing) return;
    const t = e.target.closest('.pt'); if (!t) return;
    drag.current = +t.dataset.idx; svg.current.setPointerCapture(e.pointerId); e.preventDefault();
  };
  const move = e => {
    if (drag.current === null || drag.current === undefined) return;
    const i = drag.current, [px, py] = pos(e);
    const prev = h.pts[i - 1], next = h.pts[i + 1];
    const lo = prev && Number.isFinite(prev.x) ? prev.x + 0.5 : d[0], hi = next && Number.isFinite(next.x) ? next.x - 0.5 : d[1];
    const x = clamp(ux(px, d), lo, hi);
    onChange(i, { x: B.isTempX(h) ? Math.round(x * 2) / 2 : Math.round(x), y: Math.round(clamp(uy(py), 0, 100)) });
  };
  const end = () => { drag.current = null; };
  const nowPt = now !== null && !editing ? [clamp(now, d[0], d[1]), evalCurve({ pts: shown }, now)] : null;
  return html`<svg ref=${svg} class="curve ${editing ? 'editing' : ''}" viewBox="0 0 ${W} ${H}" preserveAspectRatio="none"
      onPointerDown=${down} onPointerMove=${move} onPointerUp=${end} onPointerCancel=${end}>
    ${[0, 25, 50, 75, 100].map(y => html`<line key=${y} class="grid" x1=${PAD.l} x2=${W - PAD.r} y1=${sy(y)} y2=${sy(y)} />`)}
    ${[0, 50, 100].map(y => html`<text key=${'y' + y} class="axis" x=${PAD.l - 4} y=${sy(y) + 3} text-anchor="end">${y}</text>`)}
    ${xt.map(x => html`<text key=${'x' + x} class="axis" x=${sx(x, d)} y=${H - 4} text-anchor="middle">${x}</text>`)}
    <polyline class="line" points=${line.map(p => p.join(',')).join(' ')} />
    ${nowPt && html`<line class="now" x1=${sx(nowPt[0], d)} x2=${sx(nowPt[0], d)} y1=${PAD.t} y2=${H - PAD.b} />
      <circle class="nowpt" cx=${sx(nowPt[0], d)} cy=${sy(nowPt[1])} r="3.5" />`}
    ${pts.map(([x, y], k) => html`<circle key=${shown[k].i} class="pt" data-idx=${shown[k].i} cx=${x} cy=${y} r=${editing ? 7 : 5} />`)}
  </svg>`;
}

// ---- the fan editor ----
// a starting curve for an input the fan didn't have before
const defaultCurve = h => B.isTempX(h) ? [{ x: 40, y: 30 }, { x: 70, y: 100 }] : [{ x: 0, y: 20 }, { x: 100, y: 100 }];
// the input changed kind: keep a curve whose x unit still fits, start a
// fresh one otherwise, none for a fixed speed or the board's own curve
function withKind(h, kind) {
  const wasTemp = B.isTempX(h), hadCurve = !B.isFixed(h);
  const n = { ...h, kind, pts: h.pts.map(p => ({ ...p })) };
  if (kind === 'gpio' && !(n.gpio >= 0)) n.gpio = 0;
  if ((kind === 'hwmon' || kind === 'pwm') && n.spec === undefined) n.spec = '';
  if (B.isFixed(n)) n.pts = [];
  else if (!hadCurve || wasTemp !== B.isTempX(n) || !n.pts.length) n.pts = defaultCurve(n);
  else if (kind === 'gpio') n.pts = n.pts.map(p => ({ x: Math.round(clamp(p.x, 0, 100)), y: p.y }));
  return n;
}
// the output changed: an input the new output can't have becomes a fixed
// speed (a PWM input on a host output, the board's own curve anywhere but its
// own output), and a host output drops the boost (the receiver's alone)
function withOutput(h, output) {
  let n = { ...h, output, out: B.outInfo(output) };
  if (h.kind === 'board' && n.out.kind !== 'host') n = withKind(n, 'fallback');
  if (h.kind === 'gpio' && n.out.kind === 'host') n = withKind(n, 'fallback');
  if (n.out.kind === 'host') n.boost = B.NONE;
  return n;
}
const fmtReading = (kind, v) => v === null ? '—' : kind === 'temp' || kind === 'hwmon' ? `${fmt1(v)} °C` : `${fmt1(v)} %`;
// what a fan reads right now, for the picker: a catalogue spec's reading
// for the hwmon kinds, the telemetry's otherwise
const readingFor = h => (h.kind === 'hwmon' || h.kind === 'pwm') ? B.sensorReading(h.spec) : B.readingOf(h.kind, h);
// the picker's rows: the fixed kinds, then the catalogue (a row per sensor
// and per board pwm output, picking fills the spec), then a row for a
// temperature file (a typed path — the daemon lists /run/bc250's *_temp files
// itself, anything else is named here), then an "other" row (the typed spec)
// only when the catalogue can't stand in for typing: it was cut for size, the
// fan follows a spec it doesn't list, or a chip's pwm outputs are still
// one grouped entry (following one of them by name)
const inCatalogue = spec => !!(spec && S.sens && S.sens.some(e => e.spec === spec));
const isFileSpec = spec => typeof spec === 'string' && spec.startsWith(B.FILE_PREFIX);
const FILE_ROW = { label: 'Temperature file…', hint: 'a file another program keeps a temperature in',
                   placeholder: '/tmp/some_custom_temp_reading',
                   help: 'A plain text file holding one number: a temperature in degrees, or in millidegrees the way sysfs writes them (1000 and up). Give the full path. This is for a reading some other program of yours publishes as a file; files under /run/bc250 named *_temp are already listed above. The reading shows once the fan is saved and the daemon has read the file.' };
function pickerRows(h, kinds) {
  const rows = [];
  const inCat = inCatalogue(h.spec);
  const typedFile = h.kind === 'hwmon' && isFileSpec(h.spec) && !inCat;
  const grouped = !!(S.sens && S.sens.some(e => e.pwm && e.label.includes('–')));
  const needOther = k => S.sensMore > 0 || (h.kind === k && !inCat && !typedFile) || (k === 'pwm' && grouped);
  for (const k of kinds) {
    if (k === 'hwmon' || k === 'pwm') continue;
    rows.push({ id: k, kind: k, label: B.SRC_KINDS[k].label, hint: B.SRC_KINDS[k].hint, disabled: k === 'host',
                on: h.kind === k, value: fmtReading(k, B.readingOf(k, h)) });
  }
  if (kinds.includes('hwmon') && S.sens) {
    for (const e of S.sens) {
      const kind = e.pwm ? 'pwm' : 'hwmon';
      if (kind === 'pwm' && h.out.kind === 'host' && e.spec === h.output) continue; // following its own output is the board's curve, above
      rows.push({ id: e.spec, kind, spec: e.spec, label: e.pwm ? `${e.chip} ${e.label}` : e.label, hint: e.pwm ? 'board fan header' : e.chip,
                  on: h.kind === kind && h.spec === e.spec, value: fmtReading(kind, e.value) });
    }
  }
  if (kinds.includes('hwmon'))
    rows.push({ id: 'file', kind: 'hwmon', file: true, label: FILE_ROW.label, hint: FILE_ROW.hint,
                on: typedFile, value: typedFile ? fmtReading('hwmon', B.sensorReading(h.spec)) : '' });
  for (const k of ['hwmon', 'pwm']) if (kinds.includes(k) && needOther(k))
    rows.push({ id: 'other-' + k, kind: k, other: true, label: `Other ${k === 'pwm' ? 'board fan header' : 'sensor'}…`,
                hint: (k === 'hwmon' && S.sensMore ? `${S.sensMore} more than fit here — ` : '') + B.SRC_KINDS[k].hint,
                on: h.kind === k && !inCat, value: h.kind === k && !inCat ? fmtReading(k, B.sensorReading(h.spec)) : '' });
  return rows;
}

// the output picker's options: the board's headers, the receiver's free
// pins, and (with a daemon) the host's pwm outputs it can drive, plus none —
// each output another fan has is listed but can't be picked. The fan's own
// output is always there, whatever the lists say
function outputOptions(h, fkey) {
  const daemon = h.route === 'daemon';
  const taken = new Map(B.cards().filter(c => c.key !== fkey && c.output).map(c => [c.output, c.name || B.outLabel(c.out)]));
  const opts = [];
  const add = (v, label) => { if (!opts.some(o => o.v === v)) opts.push({ v, label: taken.has(v) ? `${label} (${taken.get(v)})` : label, dis: taken.has(v) }); };
  if (daemon) add('', 'None — parked');
  for (const n of B.headerList()) add(`header${n}`, `Header ${n}`);
  const pins = S.info && S.info.outPins;
  if (pins) for (const g of pins) add(`gpio:${g}`, `GPIO${g}`);
  if (daemon && S.outs) for (const o of S.outs) if (o.writable || o.spec === h.output)
    add(o.spec, `${o.spec.replace(':', ' ')} — ${o.duty} %${o.rpm !== null ? ` · ${o.rpm} rpm` : ''}${o.writable ? '' : ' (read-only)'}`);
  if (!opts.some(o => o.v === h.output))
    opts.push({ v: h.output, label: h.out.kind === 'parked' ? 'None — parked' : B.outLabel(h.out), dis: false });
  return opts;
}

function FanEditor({ fkey }) {
  // the card as it was when the editor opened (or when it first showed up,
  // on a deep link that outran the data); 'new' starts from a blank fan
  const origRef = useRef(null);
  if (!origRef.current) origRef.current = fkey === 'new' ? B.newFan() : B.cardOf(fkey);
  const orig = origRef.current;
  const copy = o => o && { ...o, out: { ...o.out }, pts: o.pts.map(p => ({ ...p })) };
  const [h, setH] = useState(() => copy(orig));
  useEffect(() => { if (!h && orig) setH(copy(orig)); }, [!!orig]);
  const [open, setOpen] = useState(false);
  const [other, setOther] = useState(false); // "Other sensor…" or "Temperature file…" picked: the spec is typed, whatever the catalogue lists
  const dirty = () => JSON.stringify(h) !== JSON.stringify(orig);
  useEffect(() => { leaveGuard = () => !dirty() || confirm('Leave without saving?'); return () => { leaveGuard = null; }; });
  const set = patch => setH(x => ({ ...x, ...patch }));
  // the receiver's free input pins (plus the configured one, should it not be free), or null for a typed pin
  const pins = S.info && S.info.pins;
  const pinOpts = h && pins && (pins.includes(h.gpio) ? pins : [...pins, h.gpio].sort((a, b) => a - b));
  const pinSelect = useSteadySelect(pinOpts ? `${pinOpts.join()}=${h.gpio}` : '', () => pinOpts && html`
          <select value=${h.gpio} onChange=${e => set({ gpio: +e.target.value })}>${pinOpts.map(g => html`<option key=${g} value=${g}>GPIO${g}</option>`)}</select>`);
  const outOpts = h ? outputOptions(h, fkey) : [];
  const outSelect = useSteadySelect(h ? JSON.stringify([outOpts, h.output]) : '', () => h && html`
          <select value=${h.output} onChange=${e => { const v = e.target.value; setH(x => withOutput(x, v)); }}>
            ${outOpts.map(o => html`<option key=${o.v} value=${o.v} disabled=${o.dis}>${o.label}</option>`)}</select>`);
  if (!h) return html`<${Header} back="Fans" /><div class="empty">${fkey === 'new' ? 'No free output for another fan.' : 'This fan is gone.'}</div>`;
  const daemon = h.route === 'daemon';
  const hostOut = h.out.kind === 'host';
  const kinds = !daemon ? (h.kind === 'host' ? ['host', 'fallback', 'gpio'] : ['fallback', 'gpio'])
    : hostOut ? ['board', 'fallback', ...B.HOST_KINDS]
    : ['fallback', 'gpio', ...B.HOST_KINDS];
  const setPt = (i, p) => setH(x => { const pts = x.pts.map(q => ({ ...q })); pts[i] = { ...pts[i], ...p }; return { ...x, pts }; });
  const addPt = () => setH(x => {
    const p = x.pts.map(q => ({ ...q })), d = xDomain(x);
    // between the last two points, or past the last one
    let at = p.length, xv, yv;
    if (p.length >= 2) { const a = p[p.length - 2], b = p[p.length - 1]; xv = (a.x + b.x) / 2; yv = Math.round((a.y + b.y) / 2); at = p.length - 1; }
    else { xv = Math.min(d[1], p[0].x + 10); yv = p[0].y; }
    if (!B.isTempX(x)) xv = Math.round(xv);
    p.splice(at, 0, { x: xv, y: yv });
    return { ...x, pts: p };
  });
  const rmPt = i => setH(x => ({ ...x, pts: x.pts.filter((_, j) => j !== i) }));
  const unit = B.isTempX(h) ? '°C' : '%';
  const saving = S.saving !== null;
  const cur = B.SRC_KINDS[h.kind];
  const curEntry = (h.kind === 'hwmon' || h.kind === 'pwm') && S.sens && S.sens.find(e => e.spec === h.spec);
  const curLabel = curEntry ? (curEntry.pwm ? `${curEntry.chip} ${curEntry.label}` : curEntry.label)
                 : h.kind === 'hwmon' && h.spec === B.FILE_PREFIX ? FILE_ROW.label.replace('…', '') // a file row picked, no path yet
                 : (h.kind === 'hwmon' || h.kind === 'pwm') && h.spec ? h.spec : cur.label;
  const typed = (h.kind === 'hwmon' || h.kind === 'pwm') && (other || !curEntry); // a spec the catalogue doesn't list, or chosen to type
  const typedFile = typed && h.kind === 'hwmon' && isFileSpec(h.spec); // ... and it is a file path
  const title = fkey === 'new' ? 'New fan' : (orig && orig.name) || B.outLabel(h.out);
  const del = () => { if (confirm(`Delete ${title}? ${daemon ? 'It goes from the config.' : 'The receiver stops driving its output.'}`)) B.deleteFan(fkey); };
  return html`<${Header} back="Fans" title=${title} />
    <div class="list">
      ${daemon && html`<div class="card"><input class="text" type="text" maxlength=${B.NAME_CHARS} autocomplete="off" aria-label="Name" placeholder="name" value=${h.name} onInput=${e => set({ name: e.target.value })} /></div>`}
      <div class="card">
        <h2>Output</h2>
        <label class="frow"><span>Drives</span>${outSelect}</label>
        ${h.out.kind === 'gpio' && !(S.info && S.info.outPins) && html`<label class="frow"><span>Pin</span>
          <${NumField} value=${h.out.num} min=${0} max=${48} step=${1} round=${true} onValue=${v => { if (v !== null) setH(x => withOutput(x, `gpio:${clamp(v, 0, 48)}`)); }} /><small>a free receiver pin</small></label>`}
        <div class="note">${hostOut ? 'A fan header on the host itself, driven by its daemon. It goes back to the board’s own curve whenever the daemon stops.'
          : h.out.kind === 'parked' ? 'Nothing: the fan keeps its settings and drives no output.'
          : 'A header on the receiver, or one of its pins.'}</div>
      </div>
      <div class="card">
        <button class="fold" onClick=${() => setOpen(o => !o)}>
          <h2>Follows</h2>
          ${!open && html`<span class="pick">${curLabel}</span><span class="rd on">${fmtReading(h.kind, readingFor(h))}</span>`}
          <${Icon} d=${open ? I.up : I.down} size=${18} />
        </button>
        ${open && html`<div class="srcs">${pickerRows(h, kinds).map(r => html`<button key=${r.id} class="src ${r.on ? 'on' : ''}" disabled=${r.disabled}
            onClick=${() => { setH(x => { const n = withKind(x, r.kind);
                              if (r.spec) n.spec = r.spec;                                                                       // a listed sensor
                              else if (r.file) n.spec = isFileSpec(x.spec) && !inCatalogue(x.spec) ? x.spec : B.FILE_PREFIX;     // keep a path being typed, start an empty one otherwise
                              else if (r.other) n.spec = inCatalogue(x.spec) || isFileSpec(x.spec) ? '' : (x.spec || '');       // keep a spec being typed
                              return n; }); setOther(!!r.other || !!r.file); setOpen(false); }}>
            <span class="mark">${r.on ? html`<${Icon} d=${I.check} size=${18} />` : html`<i></i>`}</span>
            <span class="lbl"><span>${r.label}</span>${r.hint && html`<small>${r.hint}</small>`}</span>
            <span class="rd">${r.value}</span></button>`)}</div>`}
        ${h.kind === 'gpio' && (pinOpts
          ? html`<label class="frow"><span>Pin</span>${pinSelect}</label>`
          : html`<label class="frow"><span>Pin</span><${NumField} value=${h.gpio} min=${0} max=${48} step=${1} round=${true} onValue=${v => { if (v !== null) set({ gpio: clamp(v, 0, 48) }); }} /><small>a free receiver pin</small></label>`)}
        ${typedFile ? html`<label class="frow"><span>file:</span>
          <input class="text" type="text" autocomplete="off" autocapitalize="off" spellcheck="false" placeholder=${FILE_ROW.placeholder}
            value=${h.spec.slice(B.FILE_PREFIX.length)} onInput=${e => set({ spec: B.FILE_PREFIX + e.target.value.trim() })} /></label>
          <div class="note">${FILE_ROW.help}</div>`
        : typed && html`<label class="frow"><span>${h.kind === 'pwm' ? 'Header' : 'Sensor'}</span>
          <input class="text" type="text" autocomplete="off" placeholder=${cur.hint} value=${h.spec || ''} onInput=${e => set({ spec: e.target.value.trim() })} /></label>`}
        ${h.kind === 'board' && html`<div class="note">The board runs this output with its own curve, as it would with no daemon at all. Pick an input to have the host run it instead.</div>`}
      </div>
      ${h.kind === 'host' && html`<div class="card"><div class="note">This fan follows a curve the host runs; with the host off it sits at its fallback speed. Pick Fixed speed or PWM input for something the receiver runs on its own.</div></div>`}
      ${!B.isFixed(h) && h.kind !== 'host' && html`<div class="card">
        <h2>Curve</h2>
        <${Curve} h=${h} editing=${true} onChange=${setPt} />
        <div class="pts">${h.pts.map((p, i) => html`<div key=${i} class="prow">
          <label>${i + 1}</label>
          <${NumField} aria=${`point ${i + 1} ${B.isTempX(h) ? 'temperature' : 'input'}`} step=${B.isTempX(h) ? 0.5 : 1} min=${0} max=${B.isTempX(h) ? B.TEMP_MAX : 100} value=${p.x} onValue=${v => setPt(i, { x: v === null ? v : clamp(v, 0, B.isTempX(h) ? B.TEMP_MAX : 100) })} /><span class="unit">${unit}</span>
          <${NumField} aria=${`point ${i + 1} speed`} min=${0} max=${100} step=${1} value=${p.y} onValue=${v => setPt(i, { y: v })} /><span class="unit">%</span>
          ${h.pts.length > 1 ? html`<button class="x" aria-label="remove point" onClick=${() => rmPt(i)}>×</button>` : html`<span></span>`}
        </div>`)}</div>
        ${h.pts.length < B.MAX_POINTS && html`<button class="rowlink add" onClick=${addPt}><${Icon} d=${I.plus} size=${16} /><span>add a point</span></button>`}
        <label class="frow"><span>Ramp down</span><${NumField} min=${0} max=${255} step=${0.5} value=${h.ramp} onValue=${v => { if (v !== null) set({ ramp: clamp(v, 0, 255) }); }} /><small>%/s, 0 = at once</small></label>
        ${B.hasHyst(h) && html`<label class="frow"><span>Hysteresis</span><${NumField} min=${0} max=${50} step=${0.5} value=${h.hyst} onValue=${v => { if (v !== null) set({ hyst: clamp(v, 0, 50) }); }} /><small>°C before slowing down</small></label>`}
      </div>`}
      ${h.kind !== 'board' && html`<div class="card">
        <h2>Fallback speed</h2>
        <${Slider} value=${h.fallback} live=${v => set({ fallback: v })} done=${v => set({ fallback: v })} label=${v => `${v} %`} />
        <div class="note">${hostOut ? 'What this fan runs when its input can’t be read.'
          : B.isFixed(h) ? 'The speed this fan runs at.'
          : 'What this fan runs when its input can’t be read — and, on the receiver, whenever nothing drives it: off, or before the host connects.'}</div>
      </div>`}
      ${daemon && B.hasBoost(h) && html`<div class="card"><h2>Power-on boost</h2>
        <label class="frow"><span class="grow">Speed after power-on</span>
          <${NumField} min=${0} max=${100} step=${1} placeholder="—" round=${true} blank=${true} value=${h.boost === B.NONE ? null : h.boost}
            onValue=${v => set({ boost: v === null ? B.NONE : clamp(v, 0, 100) })} /><small>%</small></label>
        ${B.hasBoostSecs(h) ? html`<label class="frow"><span class="grow">For the first</span>
          <${NumField} min=${0} max=${255} step=${1} round=${true} value=${h.boostSecs} onValue=${v => { if (v !== null) set({ boostSecs: clamp(v, 0, 255) }); }} /><small>s</small></label>`
        : html`<div class="note">Blank: this fan starts at its fallback speed instead.</div>`}
      </div>`}
      <div class="btns two"><button class="minor" onClick=${back}>Cancel</button>
        <button class="primary" disabled=${saving} onClick=${() => B.saveFan(fkey, h)}>${fkey === 'new' ? 'Add' : 'Save'}</button></div>
      ${fkey !== 'new' && html`<button class="minor danger" disabled=${saving} onClick=${del}>Delete this fan</button>`}
    </div>`;
}

// ---- LEDs ----
// the white swatch: a full-white pixel through the balance alone — the
// daemon's correction (color_lut.hpp) multiplies each channel by its gain in
// linear light, so the gains are encoded back to sRGB to show roughly the
// tint the eye sees. Gamma is unity at full white and brightness only dims,
// so neither is in the swatch: it shows tint, nothing else.
const whiteSwatch = wb => '#' + wb.map(v => B.hex2(Math.round(255 * Math.pow(v / 255, 1 / 2.2)))).join('');

function LedsScreen() {
  const sc = S.scfg;
  if (!B.connected()) return html`<div class="empty">Not connected.</div>`;
  if (!sc) return html`<div class="empty">${S.psu === 2 ? 'No LED settings yet.' : 'Available when on.'}</div>`;
  const ro = !sc.editable;
  const pend = B.pendingStrip();
  const wb = 'white_balance' in pend ? [0, 2, 4].map(i => parseInt(pend.white_balance.substr(i, 2), 16)) : sc.wb;
  const gamma = 'gamma' in pend ? (g => g.length === 3 ? g : [g[0], g[0], g[0]])(String(pend.gamma).split(/\s+/).map(parseFloat)) : sc.gamma;
  const bri = 'brightness' in pend ? pend.brightness : sc.brightness;
  const rev = 'reverse' in pend ? pend.reverse : sc.reverse;
  const sceneOf = s => ({ ...s, ...((pend.scenes || []).find(x => x.p === s.p) || {}) });
  const wbHex = wb.map(B.hex2).join('');
  const writeWb = (i, v) => { const n = [...wb]; n[i] = v; B.writeStrip({ white_balance: n.map(B.hex2).join('') }); };
  const writeGamma = (i, v) => {
    const n = [...gamma]; n[i] = Math.round(v * 100) / 100; // the slider's 0.05 steps, free of float dust
    if (n.some(x => !(x >= 0.5 && x <= 5))) { B.note('gamma is 0.5–5 per channel', 'err'); return; }
    B.writeStrip({ gamma: n.every(x => x === n[0]) ? String(n[0]) : n.join(' ') });
  };
  const fmtGamma = v => String(Math.round(v * 100) / 100);
  const CH = [['R', 'r'], ['G', 'g'], ['B', 'b']];
  return html`<div class="list">
    ${sc.scenes.length > 0 && html`<${Card} title="Scenes">
      ${sc.scenes.map(sceneOf).map(s => html`<div key=${s.p} class="scene">
        <${Switch} on=${s.on} disabled=${ro} change=${on => B.writeStrip({ scenes: [{ p: s.p, on }] })} />
        <span class="sname">${s.e || s.p}</span>
        ${s.color !== null ? html`<input type="color" aria-label="Scene color" disabled=${ro} value=${'#' + s.color}
            onChange=${e => B.writeStrip({ scenes: [{ p: s.p, color: e.target.value.replace('#', '') }] })} />` : html`<span></span>`}
        ${s.l !== null && html`<div class="extra"><span class="unit">Level</span>
          <${Slider} value=${Math.round(s.l * 100)} disabled=${ro} label=${v => `${v} %`} done=${v => B.writeStrip({ scenes: [{ p: s.p, l: v / 100 }] })} /></div>`}
      </div>`)}
    <//>`}
    <${Card} title="Brightness">
      <${Slider} value=${Math.round(bri * 100)} disabled=${ro} label=${v => `${v} %`} done=${v => B.writeStrip({ brightness: v / 100 })} />
    <//>
    <${Card} title="White">
      <div class="prev"><div class="swatch" style=${`background: ${whiteSwatch(wb)}`}></div>
        <div class="note">White through the balance below, roughly as the strip tints it (<span class="hex">${wbHex}</span> in the config). With a solid scene switched on in white, adjust until the light through the front plate looks white.</div></div>
      <div class="sub">Balance</div>
      ${CH.map(([L, c], i) => html`<div key=${c} class="chrow"><i class="ch ${c}">${L}</i>
        <${Slider} value=${wb[i]} max=${255} disabled=${ro} label=${v => String(v)} done=${v => writeWb(i, v)} /></div>`)}
      <div class="sub">Gamma</div>
      ${CH.map(([L, c], i) => html`<div key=${c} class="chrow"><i class="ch ${c}">${L}</i>
        <${Slider} value=${gamma[i]} min=${0.5} max=${5} step=${0.05} disabled=${ro} label=${fmtGamma} done=${v => writeGamma(i, v)} /></div>`)}
    <//>
    <${Card} title="Direction">
      <div class="swrow"><span>Reversed</span><${Switch} on=${rev} disabled=${ro} change=${v => B.writeStrip({ reverse: v })} /></div>
    <//>
    ${ro && html`<div class="empty">Settings are read-only right now.</div>`}
    <div class="foot">${sc.leds} LEDs</div>
  </div>`;
}

// ---- Receiver ----
function ReceiverScreen() {
  const { ids, labels } = B.labels();
  const cur = B.currentId();
  const on = B.connected();
  const d = S.device;
  const rows = [];
  if (S.info) {
    rows.push(['Firmware', S.info.version || '?']);
    rows.push(['Free memory', `${Math.round(S.info.heap / 1024)} KB (min ${Math.round(S.info.minHeap / 1024)} KB)`]);
  }
  if (S.fans) {
    rows.push(['Uptime', fmtUptime(S.fans.uptime)]);
    rows.push(['Fan outputs', S.fans.active ? `${S.fans.h.filter(h => h.wired).length} driven` : 'off']);
    rows.push(['Host link', S.fans.host ? (S.fans.telem ? 'connected' : 'connected, no telemetry') : 'absent']);
  }
  return html`<div class="list">
    ${hasBt() && html`<${Card} title="Receivers">
      ${ids.map(id => html`<button key=${id} class="recv ${id === cur ? 'on' : ''}" disabled=${S.busy} onClick=${() => B.select(id)}>
        <span class="mark">${id === cur ? html`<${Icon} d=${I.check} size=${18} />` : ''}</span>
        <span class="lbl"><span>${labels[id]}</span><small>${id === cur ? (on ? (S.psu === 2 ? 'connected · on' : S.psu === 1 ? 'connected · booting' : 'connected · off') : 'not connected') : ''}</small></span>
      </button>`)}
      <button class="rowlink add" disabled=${S.busy} onClick=${B.connect}><${Icon} d=${I.plus} size=${16} /><span>add a receiver</span></button>
    <//>`}
    ${d && html`<${Card} title=${B.label(d.id)}>
      ${rows.length > 0 && html`<div class="facts">${rows.map(([k, v]) => html`<span key=${k} class="k">${k}</span><span key=${k + 'v'}>${v}</span>`)}</div>`}
      <div class="btns two eq"><button class="minor" onClick=${() => B.editToken(true)}>Change token</button>
        <button class="minor danger" disabled=${S.busy} onClick=${() => { if (confirm(`Forget ${B.label(d.id)}? Its token and permission go; adding it back takes one tap.`)) B.forget(); }}>Forget</button></div>
    <//>`}
  </div>`;
}

// ---- the shell ----
const TabBar = ({ tab }) => html`<nav>
  ${[['power', 'Power', I.power], ['fans', 'Fans', I.fan], ['leds', 'LEDs', I.strip], ['receiver', 'Receiver', I.board]].map(([id, name, d]) =>
    html`<button key=${id} class=${tab === id ? 'on' : ''} onClick=${() => { if (tab !== id) go({ tab: id, editor: null, sheet: false }); }}>
      <${Icon} d=${d} size=${22} sw=${1.8} /><span>${name}</span></button>`)}
</nav>`;

function App() {
  useStore();
  const [r, setR] = useState(route);
  setRouteState = setR;
  // a save that landed closes its editor; nothing is unsaved, so no guard
  useEffect(() => { B.whenSaved(key => { if (route.editor === key) { skipGuard = true; back(); } }); }, []);
  const tokenBox = hasBt() && (S.editToken || !B.token());
  if (r.editor !== null && r.tab === 'fans')
    return html`<div class="screen"><${FanEditor} key=${r.editor} fkey=${String(r.editor)} /></div>`;
  if (r.editor === 'p' && r.tab === 'power')
    return html`<div class="screen"><${PowerEditor} /></div>`;
  const body = r.tab === 'power' ? html`<${PowerScreen} />` : r.tab === 'fans' ? html`<${FansScreen} />`
             : r.tab === 'leds' ? html`<${LedsScreen} />` : html`<${ReceiverScreen} />`;
  return html`<div class="screen">
    <${Header} />
    <${Msg} />
    ${tokenBox && html`<div class="list"><${TokenCard} /></div>`}
    ${body}
    <${TabBar} tab=${r.tab} />
    ${r.sheet && B.connected() && S.psu >= 1 && !noSwitch() && html`<${PowerSheet} />`}
  </div>`;
}

// the root entry: back from here leaves the app. ?tab=fans (&edit=<card key>,
// e.g. f1 or new; &sheet; ?tab=power&edit=p) opens elsewhere — for the demo,
// and for a bookmark straight to a tab
const q = new URLSearchParams(location.search);
const ed = q.get('edit');
go({ tab: TABS.includes(q.get('tab')) ? q.get('tab') : 'power', editor: null, sheet: false }, true);
// an editor or the sheet deep-linked sits on top of its tab, so back has somewhere to go
if (ed !== null || q.has('sheet')) go({ editor: ed, sheet: q.has('sheet') });
B.boot();
render(html`<${App} />`, document.getElementById('app'));
