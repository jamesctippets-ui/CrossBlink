// CrossBlink browser flasher: installs firmware on a reTerminal Sticky over Web Serial and
// copies Cert Study Hub tracks to its SD card with the reader's USB file-transfer protocol.
import { ESPLoader, Transport } from "./vendor/esptool-js-0.5.7.js";
import {
  PARTITION_TABLE_OFFSET, PARTITION_TABLE_SIZE, SECTOR, SerialLink, TransferError, activeOtaEntry, checkImage,
  md5Hex, parsePartitions, planOtaUpdate, planStudyUpload, readZipEntry, readerStatus, sendFile, u8ToBstr,
} from "./sticky-core.js";

// Both branches are published by CI as plain files, which raw.githubusercontent.com lets any
// page download. ?builds= and ?study= point the page somewhere else (for testing).
const params = new URLSearchParams(location.search);
const BUILDS = (params.get("builds") || "https://raw.githubusercontent.com/jamesctippets-ui/CrossBlink/sticky-builds").replace(/\/$/, "");
const STUDY = (params.get("study") || "https://raw.githubusercontent.com/jamesctippets-ui/Study-App/eink-study-pack").replace(/\/$/, "");
const USB_FILTERS = [{ usbVendorId: 0x303a }]; // Espressif (the Sticky's built-in USB serial)

const $ = (id) => document.getElementById(id);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const store = {
  get(key) { try { return localStorage.getItem("crossblink-flasher:" + key); } catch { return null; } },
  set(key, value) { try { localStorage.setItem("crossblink-flasher:" + key, value); } catch { /* private window */ } },
};

let builds = [];
let pack = null; // { manifest, files }
let busy = false;

function kb(n) {
  return n >= 1048576 ? (n / 1048576).toFixed(1) + " MB" : Math.max(1, Math.round(n / 1024)) + " KB";
}

function logger(id) {
  const el = $(id);
  return {
    clear() { el.textContent = ""; },
    line(text) { el.textContent += text + "\n"; el.scrollTop = el.scrollHeight; },
  };
}
const flashLog = logger("flash-log");
const sendLog = logger("send-log");

function setStatus(id, text, kind = "") {
  const el = $(id);
  el.textContent = text;
  el.className = "status" + (kind ? " " + kind : "");
}

function setProgress(id, done, total) {
  const bar = $(id);
  bar.hidden = total === null;
  if (total) { bar.max = total; bar.value = done; }
}

function setBusy(value) {
  busy = value;
  $("flash").disabled = value || !("serial" in navigator);
  $("send").disabled = value || !pack || !("serial" in navigator);
}

async function fetchBytes(url, label) {
  const res = await fetch(url, { cache: "no-store" });
  if (!res.ok) throw new Error(`Could not download ${label} (HTTP ${res.status}).`);
  return new Uint8Array(await res.arrayBuffer());
}

async function fetchJson(url, label) {
  const res = await fetch(url + (url.includes("?") ? "&" : "?") + "t=" + Date.now(), { cache: "no-store" });
  if (!res.ok) throw new Error(`Could not download ${label} (HTTP ${res.status}).`);
  return res.json();
}

async function sha256Hex(bytes) {
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  return [...digest].map((b) => b.toString(16).padStart(2, "0")).join("");
}

async function inflateRaw(bytes) {
  const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("deflate-raw"));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

// Reuses a Sticky this site was already allowed to use, else asks Chrome to pick one.
// After a reset the Sticky comes back as a new port, so this runs before every job.
async function pickPort() {
  const known = (await navigator.serial.getPorts()).filter((p) => p.getInfo().usbVendorId === 0x303a);
  if (known.length === 1) return known[0];
  return navigator.serial.requestPort({ filters: USB_FILTERS });
}

function friendly(err) {
  const msg = String((err && err.message) || err);
  if (err && err.name === "NotFoundError") return "No device was picked.";
  if (/Failed to open serial port|already open/i.test(msg)) {
    return "The USB port is busy. Close other apps using the Sticky (Sticky Hub, a serial monitor, another tab) and try again.";
  }
  if (msg === "ERR:not_on_home") return "The Sticky is not on its Home screen. Go back to Home and press Send again.";
  return msg;
}

// ---------------------------------------------------------------------------
// Firmware

async function loadBuilds() {
  const select = $("build");
  try {
    const index = await fetchJson(BUILDS + "/index.json", "the build list");
    builds = Array.isArray(index.builds) ? index.builds : [];
  } catch (err) {
    builds = [];
    select.innerHTML = "<option>No builds available yet</option>";
    $("build-info").textContent = " " + friendly(err) + " You can still install a file from this computer.";
    return;
  }
  select.innerHTML = "";
  for (const b of builds) {
    const opt = document.createElement("option");
    opt.value = b.id;
    opt.textContent = b.label || b.id;
    select.append(opt);
  }
  if (!builds.length) select.innerHTML = "<option>No builds available yet</option>";
  showBuildInfo();
}

function showBuildInfo() {
  const b = builds.find((x) => x.id === $("build").value);
  $("build-info").textContent = b
    ? ` Built ${new Date(b.built).toLocaleString()} from ${String(b.sha).slice(0, 7)}, ${kb(b.size)}.`
    : "";
}

async function chosenImage() {
  if ($("src-file").checked) {
    const file = $("file").files[0];
    if (!file) throw new Error("Choose a firmware file first.");
    let bytes = new Uint8Array(await file.arrayBuffer());
    if (bytes[0] === 0x50 && bytes[1] === 0x4b) {
      bytes = await readZipEntry(bytes, "firmware.bin", inflateRaw);
      if (!bytes) throw new Error("That zip does not contain firmware.bin.");
    }
    return { bytes, name: file.name };
  }
  const b = builds.find((x) => x.id === $("build").value);
  if (!b) throw new Error("No build is available to download. Choose a file from this computer instead.");
  flashLog.line(`Downloading ${b.label} …`);
  const bytes = await fetchBytes(`${BUILDS}/${b.path}?sha=${b.sha}`, "the firmware");
  if (b.sha256 && (await sha256Hex(bytes)) !== b.sha256) {
    throw new Error("The downloaded firmware did not match its checksum. Try again in a minute.");
  }
  return { bytes, name: b.label };
}

const terminal = {
  clean() {},
  writeLine(text) { flashLog.line(text); },
  write(text) { flashLog.line(text); },
};

const keepFlash = { flashSize: "keep", flashMode: "keep", flashFreq: "keep", eraseAll: false, compress: true };

async function flash() {
  if (busy) return;
  setBusy(true);
  flashLog.clear();
  setProgress("flash-progress", 0, null);
  let transport = null;
  try {
    setStatus("flash-status", "Getting the firmware…");
    const image = await chosenImage();
    const problem = checkImage(image.bytes);
    if (problem) throw new Error(problem);
    flashLog.line(`Firmware: ${image.name}, ${image.bytes.length} bytes.`);

    setStatus("flash-status", "Pick the Sticky in Chrome's list…");
    const port = await pickPort();
    setStatus("flash-status", "Connecting to the Sticky…");
    transport = new Transport(port, false);
    // The Sticky's USB serial runs at USB speed whatever the baud rate, so 115200 throughout.
    const loader = new ESPLoader({ transport, baudrate: 115200, romBaudrate: 115200, terminal });
    await loader.main();
    if (!String(loader.chip.CHIP_NAME).includes("ESP32-S3")) {
      throw new Error(`This device is an ${loader.chip.CHIP_NAME}, not a Sticky (ESP32-S3).`);
    }

    setStatus("flash-status", "Reading the Sticky's partition layout…");
    const partitions = parsePartitions(await loader.readFlash(PARTITION_TABLE_OFFSET, PARTITION_TABLE_SIZE));
    const otaPart = partitions.find((p) => p.type === 1 && p.subtype === 0);
    if (!otaPart) throw new Error("The Sticky's partition table has no otadata partition.");
    const plan = planOtaUpdate(partitions, await loader.readFlash(otaPart.offset, 2 * SECTOR));
    const tooBig = checkImage(image.bytes, plan.target.size);
    if (tooBig) throw new Error(tooBig);
    flashLog.line(`Running from ${plan.running.label}; writing ${plan.target.label} at 0x${plan.target.offset.toString(16)}.`);

    setStatus("flash-status", `Writing the firmware into the spare slot (${plan.target.label})…`);
    await loader.writeFlash({
      ...keepFlash,
      fileArray: [{ data: u8ToBstr(image.bytes), address: plan.target.offset }],
      reportProgress: (_i, written, total) => setProgress("flash-progress", written, total),
      calculateMD5Hash: md5Hex, // esptool-js compares this with the flash's own MD5 after writing
    });

    setStatus("flash-status", "Switching the Sticky to the new firmware…");
    await loader.writeFlash({
      ...keepFlash,
      fileArray: [{ data: u8ToBstr(plan.otadataSector), address: plan.otadataWriteAddress }],
      calculateMD5Hash: md5Hex,
    });
    const after = activeOtaEntry(await loader.readFlash(otaPart.offset, 2 * SECTOR));
    if (after.seq !== plan.newSeq) throw new Error("The switch to the new firmware did not stick. The old firmware is still in place.");
    flashLog.line(`otadata now selects ${plan.target.label} (sequence ${plan.newSeq}).`);

    setStatus("flash-status", "Restarting the Sticky…");
    await restart(transport);
    setProgress("flash-progress", 1, 1);
    setStatus("flash-status", "Done. The Sticky is restarting into the new firmware.", "ok");
  } catch (err) {
    flashLog.line("Error: " + ((err && err.message) || err));
    setStatus("flash-status", friendly(err), "bad");
  } finally {
    if (transport) {
      try { await transport.disconnect(); } catch { /* the port goes away when the Sticky restarts */ }
    }
    setBusy(false);
  }
}

// A normal restart: GPIO0 high (DTR off) so it boots the firmware, then pulse reset (RTS).
async function restart(transport) {
  await transport.setDTR(false);
  await transport.setRTS(true);
  await sleep(200);
  await transport.setRTS(false);
  await sleep(200);
}

// ---------------------------------------------------------------------------
// Study tracks

async function loadPack() {
  try {
    const [manifest, list] = await Promise.all([
      fetchJson(STUDY + "/study/manifest.json", "the study pack"),
      fetchJson(STUDY + "/study/files.json", "the study pack's file list"),
    ]);
    pack = { manifest, files: list.files || [], built: list.built || manifest.built };
  } catch (err) {
    $("pack-info").textContent = friendly(err);
    return;
  }
  const sizes = {};
  for (const f of pack.files) {
    const top = f.path.split("/")[0];
    sizes[top] = (sizes[top] || 0) + (f.size || 0);
  }
  let saved = null;
  try { saved = JSON.parse(store.get("tracks") || "null"); } catch { saved = null; }
  const box = $("tracks");
  box.innerHTML = "";
  for (const t of pack.manifest.tracks || []) {
    const label = document.createElement("label");
    const input = document.createElement("input");
    input.type = "checkbox";
    input.value = t.key;
    input.checked = Array.isArray(saved) ? saved.includes(t.key) : true;
    input.addEventListener("change", rememberTracks);
    const name = document.createElement("span");
    name.textContent = t.label || t.key;
    const small = document.createElement("small");
    small.textContent = t.subtitle ? `${t.subtitle} · ${kb(sizes[t.key] || 0)}` : kb(sizes[t.key] || 0);
    const text = document.createElement("span");
    text.append(name, document.createElement("br"), small);
    label.append(input, text);
    box.append(label);
  }
  const when = pack.built ? new Date(pack.built).toLocaleString() : "an unknown date";
  $("pack-info").textContent = `${(pack.manifest.tracks || []).length} tracks, built ${when}.`;
  setBusy(busy);
}

function chosenTracks() {
  return [...document.querySelectorAll("#tracks input:checked")].map((i) => i.value);
}

function rememberTracks() {
  store.set("tracks", JSON.stringify(chosenTracks()));
}

async function sendTracks() {
  if (busy || !pack) return;
  setBusy(true);
  sendLog.clear();
  setProgress("send-progress", 0, null);
  let port = null;
  let link = null;
  try {
    const plan = planStudyUpload(pack.files, pack.manifest, chosenTracks());
    const manifestBytes = new TextEncoder().encode(JSON.stringify(plan.manifest, null, 1) + "\n");
    const total = plan.files.reduce((n, f) => n + (f.size || 0), 0) + manifestBytes.length;

    setStatus("send-status", "Pick the Sticky in Chrome's list…");
    port = await pickPort();
    await port.open({ baudRate: 115200, bufferSize: 8192 });
    link = new SerialLink(port.readable.getReader(), port.writable.getWriter());
    setStatus("send-status", "Talking to the Sticky…");
    await link.drain(300);
    let status;
    try {
      status = await readerStatus(link);
    } catch (err) {
      if (!(err instanceof TransferError) || err.message.startsWith("ERR:")) throw err;
      await link.drain(500); // it may have been busy printing logs; ask once more
      status = await readerStatus(link);
    }
    sendLog.line(`Reader: ${status.device || "?"}, firmware ${status.firmware || "?"}.`);

    let done = 0;
    const send = async (path, bytes) => {
      const base = done;
      await sendFile(link, path, bytes, (n) => setProgress("send-progress", base + n, total));
      done = base + bytes.length;
      setProgress("send-progress", done, total);
    };
    for (const [i, f] of plan.files.entries()) {
      setStatus("send-status", `Sending ${i + 1} of ${plan.files.length}: ${f.path}`);
      const bytes = await fetchBytes(`${STUDY}/study/${f.path}`, f.path);
      await send(`/study/${f.path}`, bytes);
      sendLog.line(`Sent /study/${f.path} (${kb(bytes.length)})`);
    }
    // Last, so the reader never sees a manifest listing files that are not there yet.
    setStatus("send-status", "Sending the track list…");
    await send("/study/manifest.json", manifestBytes);
    sendLog.line("Sent /study/manifest.json");
    const names = plan.manifest.tracks.map((t) => t.label || t.key).join(", ");
    setStatus("send-status", `Done. Sent ${names}. Open Study on the Sticky.`, "ok");
  } catch (err) {
    sendLog.line("Error: " + ((err && err.message) || err));
    setStatus("send-status", friendly(err), "bad");
  } finally {
    if (link) await link.close();
    if (port) { try { await port.close(); } catch { /* already closed */ } }
    setBusy(false);
  }
}

// ---------------------------------------------------------------------------

function init() {
  if (!("serial" in navigator)) $("no-serial").hidden = false;
  $("build").addEventListener("change", showBuildInfo);
  $("file").addEventListener("change", () => { $("src-file").checked = true; });
  $("build").addEventListener("focus", () => { $("src-online").checked = true; });
  $("flash").addEventListener("click", flash);
  $("send").addEventListener("click", sendTracks);
  $("all").addEventListener("click", () => { document.querySelectorAll("#tracks input").forEach((i) => { i.checked = true; }); rememberTracks(); });
  $("none").addEventListener("click", () => { document.querySelectorAll("#tracks input").forEach((i) => { i.checked = false; }); rememberTracks(); });
  setBusy(false);
  loadBuilds();
  loadPack();
}

init();
