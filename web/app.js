"use strict";

const cards = [];
let online = false;

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
    card.querySelector(".apply").addEventListener("click", () => apply(card));
    root.appendChild(card);
    cards.push(card);
  }
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
  text(card, ".omt-address", channel.omt_address || channel.omt_name || "—");
  text(card, ".input-rate", number(channel.input_mbps));
  text(card, ".rtt", number(channel.srt_rtt_ms));
  text(card, ".omt-clients", channel.omt_connections);
  text(card, ".loss", `${channel.srt_lost} / ${channel.srt_retransmitted}`);
  text(card, ".frames", `${channel.frames_decoded} / ${channel.frames_sent}`);
  text(card, ".errors", channel.decode_errors);
  card.querySelector(".format").title = channel.width ? `${rate} source cadence · ${channel.active_decoder} decode` : "";
}

async function refresh() {
  try {
    const response = await fetch("/api/status", { cache: "no-store" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const state = await response.json();
    online = true;
    $("conn").textContent = "online";
    $("conn").className = "chip on";
    let live = 0, enabled = 0, fps = 0, input = 0, clients = 0, errors = 0;
    state.channels.forEach((channel, index) => {
      render(cards[index], channel);
      if (channel.enabled) ++enabled;
      if (channel.connected && channel.state === "streaming") ++live;
      fps += Number(channel.fps || 0);
      input += Number(channel.input_mbps || 0);
      clients += Number(channel.omt_connections || 0);
      errors += Number(channel.decode_errors || 0);
    });
    $("live-chip").textContent = `${live} live / ${enabled} enabled`;
    $("live-chip").className = `chip ${live === enabled && enabled ? "on" : enabled ? "" : "off"}`;
    $("total-fps").textContent = number(fps);
    $("total-in").textContent = number(input);
    $("total-clients").textContent = clients;
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
  };
  if (get(".passphrase").value) patch.passphrase = get(".passphrase").value;
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

createCards();
refresh();
setInterval(refresh, 1000);

