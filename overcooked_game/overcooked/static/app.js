"use strict";

const ICONS = { cutting_board: "🔪", pan: "🍳", pot: "🍲", plate: "🍽️", delivery: "🛎️" };
const PHASE_LABEL = {
  cal_master: "Calibration 1/3", cal_stations: "Calibration 2/3", cal_food: "Calibration 3/3",
  ready: "Ready", countdown: "Get ready", playing: "Playing", ended: "Round over",
};

const $ = (id) => document.getElementById(id);
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const pretty = (s) => String(s).replace(/_/g, " ");
const mmss = (s) => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, "0")}`;

// Replace an element's HTML only when it changed, so buttons keep their
// state (a click needs the same element on mouse-down and mouse-up).
function setHTML(el, html) {
  if (el._html !== html) { el._html = html; el.innerHTML = html; }
}

let socket = null;
let state = null;
let muted = localStorage.getItem("muted") === "1";
let lastLogId = null;
let lastCountdown = null;

// ---- connection ---------------------------------------------------------------

function connect() {
  socket = new WebSocket(`ws://${location.host}/ws`);
  socket.onmessage = (event) => {
    const msg = JSON.parse(event.data);
    if (msg.type === "state") { state = msg; render(); }
    else if (msg.type === "error") showBanner(msg.text);
  };
  socket.onclose = () => { showBanner("Lost connection to the game server, retrying..."); setTimeout(connect, 1000); };
  socket.onopen = () => showBanner(null);
}

function send(message) {
  if (socket && socket.readyState === WebSocket.OPEN) socket.send(JSON.stringify(message));
}

let bannerTimer = null;
function showBanner(text) {
  const el = $("banner");
  el.hidden = !text;
  el.textContent = text || "";
  clearTimeout(bannerTimer);
  if (text && !text.startsWith("Lost")) bannerTimer = setTimeout(() => (el.hidden = true), 4000);
}

document.addEventListener("click", (event) => {
  const btn = event.target.closest("[data-action]");
  if (!btn || btn.disabled) return;
  const { action, mac, tagFrom, n } = btn.dataset;
  const message = { action };
  if (mac) message.mac = mac;
  if (n) message.n = Number(n);
  if (tagFrom) message.tag = document.getElementById(tagFrom).value;
  if (action === "start_calibration" && !confirm("Start calibration over? Enrolled tags are forgotten.")) return;
  send(message);
});

$("mute").addEventListener("click", () => {
  muted = !muted;
  localStorage.setItem("muted", muted ? "1" : "0");
  $("mute").textContent = muted ? "Sound off" : "Sound on";
});
$("mute").textContent = muted ? "Sound off" : "Sound on";

// ---- rendering ------------------------------------------------------------------

function render() {
  const s = state;
  const cal = s.phase.startsWith("cal_");
  const playing = s.phase === "playing";

  $("phase").textContent = PHASE_LABEL[s.phase] || s.phase;
  const bridge = $("bridge");
  bridge.textContent = s.bridge.connected ? "Bridge connected" : "Bridge not connected";
  bridge.className = "chip " + (s.bridge.connected ? "ok" : "bad");

  renderHud(s, cal);
  renderCalibration(s, cal);
  renderOrders(s);
  renderStations(s);
  renderControls(s);
  renderLog(s);
  renderItems(s);
  renderOverlay(s);
  renderSim(s);
  playSounds(s);
}

function renderHud(s, cal) {
  $("hud").hidden = cal;
  if (cal) return;
  const low = s.phase === "playing" && s.time_left <= 10;
  const pct = s.duration ? (100 * s.time_left) / s.duration : 0;
  setHTML($("hud"), `
    <div class="stat time ${low ? "low" : ""}">
      <div class="label">Time left</div>
      <div class="value">${mmss(Math.ceil(s.time_left))}</div>
      <div class="bar ${low ? "low" : ""}"><i style="width:${pct}%"></i></div>
    </div>
    <div class="stat"><div class="label">Score</div><div class="value">${s.score}</div></div>
    <div class="stat"><div class="label">Delivered</div><div class="value">${s.delivered}</div></div>`);
}

function renderCalibration(s, cal) {
  const el = $("calibration");
  el.hidden = !cal;
  if (!cal) return;
  const c = s.calibration;
  const idx = { cal_master: 0, cal_stations: 1, cal_food: 2 }[s.phase];
  const steps = ["Calibration tag", "Stations", "Food and plates"]
    .map((t, i) => `<span class="step ${i === idx ? "now" : i < idx ? "done" : ""}">${i + 1}. ${t}</span>`).join("");

  let body = "";
  if (s.phase === "cal_master") {
    body = `<div class="prompt">Touch any tag to the server reader</div>
      <p class="empty">That tag becomes the calibration tag. You will touch it to every station next.</p>`;
  } else if (s.phase === "cal_stations") {
    const checks = c.slots.map((slot) => `
      <div class="check ${slot.mac ? "done" : ""}">${slot.mac ? "✓" : "○"} ${ICONS[slot.kind] || ""} ${esc(pretty(slot.kind))} ${slot.index + 1}
        <span class="mac">${esc(slot.mac || "")}</span></div>`).join("");
    body = `<div class="prompt">Touch the calibration tag to each station</div><div class="checks">${checks}</div>
      <div class="row"><button class="ghost" data-action="next">Skip remaining stations</button></div>`;
  } else {
    const current = c.steps.find((x) => x.current);
    const checks = c.steps.map((st) => `
      <div class="check ${st.count >= st.total ? "done" : ""} ${st.current ? "now" : ""}">
        ${st.count >= st.total ? "✓" : "○"} ${esc(st.label)} <span class="mac">${st.count}/${st.total}</span></div>`).join("");
    body = `<div class="prompt">Touch ${current ? `<b>${esc(current.label)}</b>${current.total > 1 ? ` ${current.count + 1}/${current.total}` : ""}` : "a tag"} to the server reader</div>
      <div class="checks">${checks}</div>
      <div class="row">
        <button class="ghost" data-action="next">Skip ${current ? esc(current.label) : ""}</button>
        <button data-action="finish_calibration">Done enrolling</button>
      </div>`;
  }
  const load = s.has_saved_calibration ? `<button class="ghost small" data-action="load_calibration">Load last calibration</button>` : "";
  setHTML(el, `<div class="steps">${steps}</div>${body}<div class="row" style="margin-top:12px">${load}</div>`);
}

function renderOrders(s) {
  const show = ["playing", "countdown"].includes(s.phase);
  $("orders-section").hidden = !show;
  if (!show) return;
  const html = s.orders.length ? s.orders.map((o) => {
    const pct = (100 * o.time_left) / o.total;
    const low = pct < 25;
    return `<div class="order">
      <div class="top"><span class="name">${esc(pretty(o.recipe))}</span><span class="pts">+${o.points}</span></div>
      <div class="needs">${o.needs.map((n) => `<span class="need">${esc(n.replace(":", " "))}</span>`).join("")}</div>
      <div class="bar ${low ? "low" : ""}"><i style="width:${pct}%"></i></div></div>`;
  }).join("") : `<span class="empty">No orders yet</span>`;
  setHTML($("orders"), html);
}

function renderStations(s) {
  const html = s.stations.map((st) => {
    const what = st.item
      ? `${esc(st.item)} <span class="tag-state">${esc(st.item_state || "")}</span>`
      : `<span class="empty">empty</span>`;
    const pct = st.progress != null ? `<div class="bar ${st.note === "burnt" ? "low" : ""}"><i style="width:${Math.round(100 * st.progress)}%"></i></div>` : "";
    return `<div class="station ${st.online ? "" : "offline"}">
      <div class="head"><span class="icon">${ICONS[st.kind] || "?"}</span>
        <span class="name">${esc(st.label)}</span><span class="dot ${st.online ? "on" : ""}" title="${st.online ? "online" : "offline"}"></span></div>
      <div class="what">${what}</div>${pct}
      <div class="note">${esc(st.note || (st.calibrated ? "" : "not calibrated"))}</div></div>`;
  }).join("") || `<span class="empty">No stations have connected yet. Power them on near the bridge.</span>`;
  setHTML($("stations"), html);
}

function renderControls(s) {
  const p = s.phase;
  const buttons = [];
  if (p === "ready" || p === "ended") buttons.push(`<button data-action="start_game">${p === "ended" ? "Play again" : "Start round"}</button>`);
  if (p === "playing") buttons.push(`<button class="danger" data-action="end_game">End round</button>`);
  if (["ready", "countdown", "playing", "ended"].includes(p)) buttons.push(`<button class="ghost" data-action="reset">Reset food and score</button>`);
  if (!p.startsWith("cal_") || s.calibration) buttons.push(`<button class="ghost" data-action="start_calibration">Recalibrate</button>`);
  setHTML($("controls"), buttons.join(""));
}

function renderLog(s) {
  setHTML($("log"), s.log.map((e) => `<li class="k-${esc(e.kind)}">${esc(e.text)}</li>`).reverse().join(""));
}

function renderItems(s) {
  setHTML($("items"), s.items.map((it) =>
    `<span class="item ${esc(it.state)}" title="${esc(it.uid)}">${esc(it.label)} · ${it.dirty ? "dirty" : esc(it.state)}${it.contents.length ? " [" + esc(it.contents.join(", ")) + "]" : ""}</span>`
  ).join("") || `<span class="empty">none enrolled</span>`);
}

function renderOverlay(s) {
  const el = $("overlay");
  if (s.phase === "countdown") {
    el.hidden = false;
    setHTML(el, `<div class="big">${Math.max(1, Math.ceil(s.countdown))}</div>`);
  } else if (s.phase === "ended") {
    el.hidden = false;
    setHTML(el, `<div class="title">Time's up!</div><div class="final">${s.score}</div>
      <div class="title" style="font-size:28px">${s.delivered} dishes delivered</div>
      <div class="row"><button data-action="start_game">Play again</button><button class="ghost" data-action="reset">Back</button></div>`);
  } else {
    el.hidden = true;
  }
}

// ---- simulator panel ---------------------------------------------------------------

let simSignature = "";
function renderSim(s) {
  const el = $("sim");
  el.hidden = !s.sim;
  if (!s.sim) return;

  const signature = s.sim.stations.map((x) => x.mac).join() + s.sim.tags.join();
  if (signature !== simSignature) {
    simSignature = signature;
    const options = s.sim.tags.map((t) => `<option>${esc(t)}</option>`).join("");
    const rows = s.sim.stations.map((st) => `
      <tr><td>${ICONS[st.kind]} ${esc(pretty(st.kind))}<br><small>${esc(st.mac)}</small></td>
        <td id="sim-state-${st.mac}"></td>
        <td><select id="sim-tag-${st.mac}">${options}</select></td>
        <td class="row">
          <button class="small" data-action="sim_place" data-mac="${st.mac}" data-tag-from="sim-tag-${st.mac}">Place</button>
          <button class="small ghost" data-action="sim_remove" data-mac="${st.mac}">Remove</button>
          <button class="small ghost" data-action="sim_work" data-mac="${st.mac}" data-n="1">+1</button>
          <button class="small ghost" data-action="sim_work" data-mac="${st.mac}" data-n="10">+10</button>
          <button class="small ghost" data-action="sim_power" data-mac="${st.mac}">Power</button>
        </td></tr>`).join("");
    el.innerHTML = `<h2>Simulator (no hardware)</h2>
      <div class="row" style="margin-bottom:12px">
        <select id="sim-reader">${options}</select>
        <button class="small" data-action="sim_tap" data-tag-from="sim-reader">Touch to server reader</button>
        <button class="small ghost" data-action="sim_autocal">Auto-calibrate</button>
      </div>
      <table><tr><th>Station</th><th>Status</th><th>Tag</th><th>Actions</th></tr>${rows}</table>`;
  }
  for (const st of s.sim.stations) {
    const cell = document.getElementById(`sim-state-${st.mac}`);
    if (!cell) continue;
    const parts = [st.powered ? (st.connected ? "connected" : "connecting") : "power off"];
    if (st.tag) parts.push(`${st.tag}: ${st.state}`);
    if (st.state === "active") parts.push(`${st.progress}/${st.goal}`);
    cell.textContent = parts.join(" · ");
  }
}

// ---- sound ---------------------------------------------------------------------------

let audio = null;
function beep(freq, duration = 0.15, type = "sine", delay = 0) {
  if (muted) return;
  try {
    audio = audio || new (window.AudioContext || window.webkitAudioContext)();
    const t0 = audio.currentTime + delay;
    const osc = audio.createOscillator();
    const gain = audio.createGain();
    osc.type = type; osc.frequency.value = freq;
    gain.gain.setValueAtTime(0.15, t0);
    gain.gain.exponentialRampToValueAtTime(0.0001, t0 + duration);
    osc.connect(gain); gain.connect(audio.destination);
    osc.start(t0); osc.stop(t0 + duration);
  } catch (e) { /* audio not available */ }
}

function playSounds(s) {
  const newest = s.log.length ? s.log[s.log.length - 1].id : 0;
  if (lastLogId === null) { lastLogId = newest; return; } // don't replay history on load
  for (const e of s.log.filter((x) => x.id > lastLogId)) {
    if (e.kind === "score") { beep(660, 0.12); beep(880, 0.12, "sine", 0.12); beep(1320, 0.25, "sine", 0.24); }
    else if (e.kind === "burn") beep(140, 0.6, "sawtooth");
    else if (e.kind === "order") beep(520, 0.12, "triangle");
    else if (e.kind === "warn") beep(220, 0.12, "square");
    else if (e.kind === "ok") beep(740, 0.08, "triangle");
  }
  lastLogId = newest;

  const cd = s.countdown == null ? null : Math.ceil(s.countdown);
  if (cd !== null && cd !== lastCountdown) beep(cd > 0 ? 440 : 880, 0.2);
  lastCountdown = cd;
}

connect();
