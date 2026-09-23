// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

"use strict";

const cards = [];
let online = false;
let recordingState = { active: false, directory: "recordings" };
let recordingDirectoryDirty = false;
let browsedFolder = { path: "", parent: "" };

const $ = (id) => document.getElementById(id);
const text = (card, selector, value) => { card.querySelector(selector).textContent = value; };
const number = (value, digits = 1) => Number(value || 0).toFixed(digits);

function createCards() {
  const root = $("channels");
  const template = $("channel-template");
  for (let i = 0; i < 4; ++i) {
    const card = template.content.firstElementChild.cloneNode(true);
    card.dataset.index = i;
    text(card, ".channel-title", `Channel ${i + 1}`);
    card.querySelectorAll("input,select").forEach((control) => {
      control.addEventListener("input", () => {
        card.dataset.dirty = "1";
        text(card, ".message", "unapplied changes");
        card.querySelector(".message").classList.remove("bad");
      });
    });
    card.querySelector(".omt-enabled").addEventListener("change", () => outputToggles(card));
    card.querySelector(".srt-output-enabled").addEventListener("change", () => outputToggles(card));
    card.querySelector(".apply").addEventListener("click", () => apply(card));
    root.appendChild(card);
    cards.push(card);
  }
}

function outputToggles(card) {
  card.classList.toggle("omt-off", !card.querySelector(".omt-enabled").checked);
  card.classList.toggle("srt-output-off", !card.querySelector(".srt-output-enabled").checked);
}

function populate(card, channel) {
  if (card.dataset.dirty === "1") return;
  card.querySelector(".enabled").checked = channel.enabled;
  card.querySelector(".port").value = channel.port;
  card.querySelector(".latency").value = channel.latency_ms;
  card.querySelector(".stream-id").value = channel.stream_id || "";
  card.querySelector(".decoder").value = channel.decoder;
  card.querySelector(".omt-name").value = channel.omt_name;
  card.querySelector(".omt-quality").value = channel.omt_quality;
  card.querySelector(".pbkeylen").value = channel.pbkeylen;
  card.querySelector(".passphrase").placeholder = channel.encrypted ? "set · leave blank to keep" : "disabled";
  card.querySelector(".clear-passphrase").checked = false;
  card.querySelector(".omt-enabled").checked = channel.omt_enabled;
  card.querySelector(".srt-output-enabled").checked = channel.srt_output_enabled;
  card.querySelector(".srt-output-port").value = channel.srt_output_port;
  card.querySelector(".srt-output-latency").value = channel.srt_output_latency_ms;
  card.querySelector(".srt-output-pbkeylen").value = channel.srt_output_pbkeylen;
  card.querySelector(".srt-output-passphrase").placeholder = channel.srt_output_encrypted ? "set · leave blank to keep" : "disabled";
  card.querySelector(".clear-srt-output-passphrase").checked = false;
  outputToggles(card);
}

function render(card, channel) {
  populate(card, channel);
  card.classList.remove("live", "waiting", "error");
  if (channel.connected && channel.state === "streaming") card.classList.add("live");
  else if (["waiting", "probing", "rejected"].includes(channel.state)) card.classList.add("waiting");
  else if (channel.state === "error") card.classList.add("error");
  text(card, ".state", channel.state);
  text(card, ".detail", channel.detail || "—");
  const rate = channel.fps_den ? `${channel.fps_num}/${channel.fps_den}` : "—";
  const format = channel.width
    ? `${channel.width}×${channel.height} · ${channel.codec.toUpperCase()} · ${number(channel.fps)} fps`
    : "No signal";
  text(card, ".format", format);
  text(card, ".peer", channel.peer || "—");
  text(card, ".omt-address", channel.omt_enabled ? channel.omt_address || channel.omt_name || "—" : "off");
  text(card, ".input-rate", number(channel.input_mbps));
  text(card, ".rtt", number(channel.srt_rtt_ms));
  text(card, ".omt-clients", channel.omt_connections);
  text(card, ".loss", `${channel.srt_lost} / ${channel.srt_retransmitted}`);
  text(card, ".frames", `${channel.frames_decoded} / ${channel.frames_sent}`);
  text(card, ".errors", channel.decode_errors);
  const recording = card.querySelector(".recording-file");
  recording.textContent = channel.recording_error
    ? "error"
    : channel.recording_active
      ? `● ${channel.recording_path || "active"}`
      : channel.recording_path || "—";
  recording.title = channel.recording_error || channel.recording_path || "";
  text(card, ".recording-packets", `${channel.packets_recorded} / ${channel.recording_errors}`);
  const srtOut = card.querySelector(".srt-output-clients");
  srtOut.textContent = channel.srt_output_error
    ? "error"
    : channel.srt_output_listening
      ? `${channel.srt_output_clients} on :${channel.srt_output_port}`
      : channel.srt_output_enabled ? "starting" : "off";
  srtOut.title = channel.srt_output_error || channel.srt_output_peers || "";
  text(card, ".srt-output-rate", number(channel.srt_output_mbps));
  text(card, ".srt-output-packets", `${channel.srt_output_packets_sent} / ${channel.srt_output_packets_dropped}`);
  card.querySelector(".format").title = channel.width ? `${rate} source cadence · ${channel.active_decoder} decode` : "";
}

function renderRecording(state) {
  recordingState = state.recording || recordingState;
  if (!recordingDirectoryDirty) {
    $("global-recording-directory").value = recordingState.directory || "recordings";
  }
  const openFiles = (state.channels || []).filter((channel) => channel.recording_active).length;
  $("recording-toggle").textContent = recordingState.active ? "Stop recording" : "Start recording";
  $("recording-toggle").classList.toggle("active", recordingState.active);
  $("recording-dot").classList.toggle("active", recordingState.active);
  $("recording-summary").textContent = recordingState.active
    ? `${openFiles} file${openFiles === 1 ? "" : "s"} open`
    : "stopped";
}

async function refresh() {
  try {
    const response = await fetch("/api/status", { cache: "no-store" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const state = await response.json();
    renderRecording(state);
    online = true;
    $("conn").textContent = "online";
    $("conn").className = "chip on";
    let live = 0, enabled = 0, fps = 0, input = 0, clients = 0, srtClients = 0, errors = 0;
    state.channels.forEach((channel, index) => {
      render(cards[index], channel);
      if (channel.enabled) ++enabled;
      if (channel.connected && channel.state === "streaming") ++live;
      fps += Number(channel.fps || 0);
      input += Number(channel.input_mbps || 0);
      clients += Number(channel.omt_connections || 0);
      srtClients += Number(channel.srt_output_clients || 0);
      errors += Number(channel.decode_errors || 0);
    });
    $("live-chip").textContent = `${live} live / ${enabled} enabled`;
    $("live-chip").className = `chip ${live === enabled && enabled ? "on" : enabled ? "" : "off"}`;
    $("total-fps").textContent = number(fps);
    $("total-in").textContent = number(input);
    $("total-clients").textContent = clients;
    $("total-srt-out").textContent = srtClients;
    $("total-errors").textContent = errors;
    $("cuda").textContent = state.capabilities?.cuda ? "ready" : "fallback";
  } catch (error) {
    if (online) console.warn("status polling failed", error);
    online = false;
    $("conn").textContent = "offline";
    $("conn").className = "chip off";
  }
}

async function apply(card) {
  const index = Number(card.dataset.index);
  const get = (selector) => card.querySelector(selector);
  const patch = {
    enabled: get(".enabled").checked,
    port: Number(get(".port").value),
    latency_ms: Number(get(".latency").value),
    stream_id: get(".stream-id").value,
    decoder: get(".decoder").value,
    omt_name: get(".omt-name").value,
    omt_quality: get(".omt-quality").value,
    pbkeylen: Number(get(".pbkeylen").value),
    clear_passphrase: get(".clear-passphrase").checked,
    omt_enabled: get(".omt-enabled").checked,
    srt_output_enabled: get(".srt-output-enabled").checked,
    srt_output_port: Number(get(".srt-output-port").value),
    srt_output_latency_ms: Number(get(".srt-output-latency").value),
    srt_output_pbkeylen: Number(get(".srt-output-pbkeylen").value),
    clear_srt_output_passphrase: get(".clear-srt-output-passphrase").checked,
  };
  if (get(".passphrase").value) patch.passphrase = get(".passphrase").value;
  if (get(".srt-output-passphrase").value) patch.srt_output_passphrase = get(".srt-output-passphrase").value;
  const button = get(".apply");
  button.disabled = true;
  text(card, ".message", "applying…");
  try {
    const response = await fetch(`/api/channels/${index}`, {
      method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify(patch),
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || `HTTP ${response.status}`);
    card.dataset.dirty = "0";
    get(".passphrase").value = "";
    get(".clear-passphrase").checked = false;
    get(".srt-output-passphrase").value = "";
    get(".clear-srt-output-passphrase").checked = false;
    get(".message").classList.remove("bad");
    text(card, ".message", "applied");
    if (result.channels?.[index]) render(card, result.channels[index]);
  } catch (error) {
    get(".message").classList.add("bad");
    text(card, ".message", error.message);
  } finally {
    button.disabled = false;
  }
}

async function setRecording(active, requestedDirectory) {
  const directory = requestedDirectory ?? $("global-recording-directory").value.trim();
  const controls = [$("recording-toggle"), $("apply-recording-folder")];
  controls.forEach((control) => { control.disabled = true; });
  $("recording-message").textContent = active ? "starting…" : "applying…";
  try {
    const response = await fetch("/api/recording", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ active, directory }),
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || `HTTP ${response.status}`);
    recordingDirectoryDirty = false;
    $("recording-message").classList.remove("bad");
    $("recording-message").textContent = active ? "recording command applied" : "recording stopped";
    renderRecording(result);
  } catch (error) {
    $("recording-message").classList.add("bad");
    $("recording-message").textContent = error.message;
  } finally {
    controls.forEach((control) => { control.disabled = false; });
  }
}

async function browseFolder(path) {
  $("folder-error").textContent = "";
  try {
    const response = await fetch("/api/recording/directories", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ path }),
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || `HTTP ${response.status}`);
    browsedFolder = result;
    $("folder-path").textContent = result.path;
    $("folder-up").disabled = !result.parent || result.parent === result.path;
    const list = $("folder-list");
    list.replaceChildren();
    result.directories.forEach((directory) => {
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = `📁 ${directory.name}`;
      button.addEventListener("click", () => browseFolder(directory.path));
      list.appendChild(button);
    });
    if (!result.directories.length) {
      const empty = document.createElement("span");
      empty.className = "dim";
      empty.textContent = "No subfolders";
      list.appendChild(empty);
    }
  } catch (error) {
    $("folder-error").textContent = error.message;
  }
}

$("global-recording-directory").addEventListener("input", () => {
  recordingDirectoryDirty = true;
  $("recording-message").textContent = "folder not applied";
});
$("recording-toggle").addEventListener("click", () => {
  const directory = recordingState.active
    ? recordingState.directory
    : $("global-recording-directory").value.trim();
  setRecording(!recordingState.active, directory);
});
$("apply-recording-folder").addEventListener("click", () => setRecording(recordingState.active));
$("browse-recording-folder").addEventListener("click", () => {
  $("folder-dialog").showModal();
  browseFolder($("global-recording-directory").value.trim());
});
$("folder-up").addEventListener("click", () => browseFolder(browsedFolder.parent));
$("folder-select").addEventListener("click", () => {
  $("global-recording-directory").value = browsedFolder.path;
  recordingDirectoryDirty = true;
  $("folder-dialog").close();
  setRecording(recordingState.active, browsedFolder.path);
});
$("folder-close").addEventListener("click", () => $("folder-dialog").close());

createCards();
refresh();
setInterval(refresh, 1000);
