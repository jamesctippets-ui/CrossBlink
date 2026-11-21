// Pure helpers for the CrossBlink browser flasher (no DOM, no Web Serial), so they can be
// tested in Node: firmware image checks, the partition table, the otadata switch, CRC32,
// MD5, a small zip reader, and the reader's USB serial file-transfer protocol.

// ---------------------------------------------------------------------------
// Checksums

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  return table;
})();

// zlib-compatible CRC32: crc32(data) is zlib.crc32(data), and crc32(data, prev) continues it.
// The reader's esp_rom_crc32_le(0, ...) gives the same value.
export function crc32(data, prev = 0) {
  let c = ~prev >>> 0;
  for (let i = 0; i < data.length; i++) c = CRC_TABLE[(c ^ data[i]) & 0xff] ^ (c >>> 8);
  return ~c >>> 0;
}

// MD5 as lowercase hex, over a Uint8Array or a binary string (esptool-js passes the latter).
export function md5Hex(input) {
  const bytes = typeof input === "string" ? bstrToU8(input) : input;
  const len = bytes.length;
  const padded = new Uint8Array(((len + 8) >>> 6) * 64 + 64);
  padded.set(bytes);
  padded[len] = 0x80;
  const view = new DataView(padded.buffer);
  view.setUint32(padded.length - 8, (len * 8) >>> 0, true);
  view.setUint32(padded.length - 4, Math.floor(len / 0x20000000), true);

  const S = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
  const K = new Uint32Array(64);
  for (let i = 0; i < 64; i++) K[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 0x100000000) >>> 0;

  let a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
  const M = new Uint32Array(16);
  for (let off = 0; off < padded.length; off += 64) {
    for (let i = 0; i < 16; i++) M[i] = view.getUint32(off + i * 4, true);
    let A = a0, B = b0, C = c0, D = d0;
    for (let i = 0; i < 64; i++) {
      let F, g;
      if (i < 16) { F = (B & C) | (~B & D); g = i; }
      else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
      else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) % 16; }
      else { F = C ^ (B | ~D); g = (7 * i) % 16; }
      const s = S[(i >>> 4) * 4 + (i % 4)];
      const sum = (A + F + K[i] + M[g]) >>> 0;
      A = D; D = C; C = B;
      B = (B + ((sum << s) | (sum >>> (32 - s)))) >>> 0;
    }
    a0 = (a0 + A) >>> 0; b0 = (b0 + B) >>> 0; c0 = (c0 + C) >>> 0; d0 = (d0 + D) >>> 0;
  }
  let hex = "";
  for (const word of [a0, b0, c0, d0]) {
    for (let i = 0; i < 4; i++) hex += ((word >>> (i * 8)) & 0xff).toString(16).padStart(2, "0");
  }
  return hex;
}

// ---------------------------------------------------------------------------
// Byte helpers

export function bstrToU8(s) {
  const out = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i) & 0xff;
  return out;
}

export function u8ToBstr(u8) {
  let s = "";
  for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000));
  return s;
}

export function u16le(n) {
  return new Uint8Array([n & 0xff, (n >>> 8) & 0xff]);
}

export function u32le(n) {
  return new Uint8Array([n & 0xff, (n >>> 8) & 0xff, (n >>> 16) & 0xff, (n >>> 24) & 0xff]);
}

export function concatBytes(...parts) {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) { out.set(p, at); at += p.length; }
  return out;
}

// ---------------------------------------------------------------------------
// Firmware image

export const ESP32S3_CHIP_ID = 9;

// Checks the ESP app image header: magic 0xE9 and the ESP32-S3 chip id at bytes 12-13.
export function checkImage(image, maxSize) {
  if (!image || image.length < 24) return "That file is too small to be firmware.";
  if (image[0] !== 0xe9) return "That file is not an ESP32 firmware image (it should be firmware.bin).";
  const chip = image[12] | (image[13] << 8);
  if (chip !== ESP32S3_CHIP_ID) return `That firmware is for another chip (id ${chip}), not the Sticky's ESP32-S3.`;
  if (maxSize && image.length > maxSize) {
    return `That firmware is ${image.length} bytes, more than the ${maxSize} byte app slot.`;
  }
  return null;
}

// ---------------------------------------------------------------------------
// Partition table (at 0x8000) and otadata

export const PARTITION_TABLE_OFFSET = 0x8000;
export const PARTITION_TABLE_SIZE = 0xc00;
export const SECTOR = 0x1000;

export function parsePartitions(table) {
  const view = new DataView(table.buffer, table.byteOffset, table.byteLength);
  const out = [];
  for (let off = 0; off + 32 <= table.length; off += 32) {
    if (view.getUint16(off, true) !== 0x50aa) break; // magic AA 50; MD5 entry (EB EB) or 0xFF ends the list
    let label = "";
    for (let i = 0; i < 16; i++) {
      const ch = table[off + 12 + i];
      if (!ch) break;
      label += String.fromCharCode(ch);
    }
    out.push({
      type: table[off + 2],
      subtype: table[off + 3],
      offset: view.getUint32(off + 4, true),
      size: view.getUint32(off + 8, true),
      label,
    });
  }
  return out;
}

const OTA_IMG_INVALID = 3;
const OTA_IMG_ABORTED = 4;

// CRC over the 4-byte ota_seq, initial value 0xFFFFFFFF, the way ESP-IDF stores it.
export function otaSeqCrc(seq) {
  return crc32(u32le(seq), 0xffffffff);
}

function readEntry(bytes, off) {
  const view = new DataView(bytes.buffer, bytes.byteOffset + off, 32);
  return { seq: view.getUint32(0, true), state: view.getUint32(24, true), crc: view.getUint32(28, true) };
}

// Which otadata sector holds the live entry, and its sequence number. seq 0 means none
// (blank otadata), in which case the bootloader starts the first OTA slot.
export function activeOtaEntry(otadata) {
  let index = -1;
  let seq = 0;
  for (let i = 0; i < 2; i++) {
    const e = readEntry(otadata, i * SECTOR);
    if (e.seq === 0xffffffff || e.seq === 0) continue;
    if (e.crc !== otaSeqCrc(e.seq)) continue;
    if (e.state === OTA_IMG_INVALID || e.state === OTA_IMG_ABORTED) continue;
    if (index < 0 || e.seq > seq) { index = i; seq = e.seq; }
  }
  return { index, seq };
}

// Works out where the new firmware goes: the OTA app slot that is not running now, and the
// otadata sector to write so the bootloader starts that slot next. Mirrors the firmware's
// own SD-card update (src/network/OtaBootSwitch.cpp), so the running firmware is never
// overwritten and a failed write leaves the reader booting what it had.
export function planOtaUpdate(partitions, otadata) {
  const otaData = partitions.find((p) => p.type === 1 && p.subtype === 0);
  const slots = [0x10, 0x11].map((sub) => partitions.find((p) => p.type === 0 && p.subtype === sub));
  if (!otaData || otaData.size < 2 * SECTOR) throw new Error("The reader's partition table has no otadata partition.");
  if (!slots[0] || !slots[1]) throw new Error("The reader's partition table does not have two app slots.");

  const active = activeOtaEntry(otadata);
  const runningSlot = active.index < 0 ? 0 : (active.seq - 1) % 2;
  const targetSlot = runningSlot === 0 ? 1 : 0;
  let newSeq = active.seq + 1;
  while ((newSeq - 1) % 2 !== targetSlot) newSeq++;
  const sectorIndex = active.index === 0 ? 1 : 0;

  const sector = new Uint8Array(SECTOR).fill(0xff);
  const view = new DataView(sector.buffer);
  view.setUint32(0, newSeq, true);
  view.setUint32(24, 0, true); // ESP_OTA_IMG_NEW
  view.setUint32(28, otaSeqCrc(newSeq), true);

  return {
    running: slots[runningSlot],
    target: slots[targetSlot],
    otadata: otaData,
    otadataWriteAddress: otaData.offset + sectorIndex * SECTOR,
    otadataSector: sector,
    newSeq,
  };
}

// ---------------------------------------------------------------------------
// Zip (for a downloaded GitHub Actions artifact that wraps firmware.bin)

export async function readZipEntry(zip, wantedName, inflateRaw) {
  const view = new DataView(zip.buffer, zip.byteOffset, zip.byteLength);
  let eocd = -1;
  for (let i = zip.length - 22; i >= Math.max(0, zip.length - 65557); i--) {
    if (view.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
  }
  if (eocd < 0) throw new Error("That zip file looks damaged.");
  const count = view.getUint16(eocd + 10, true);
  let p = view.getUint32(eocd + 16, true);
  for (let n = 0; n < count; n++) {
    if (view.getUint32(p, true) !== 0x02014b50) throw new Error("That zip file looks damaged.");
    const method = view.getUint16(p + 10, true);
    const compSize = view.getUint32(p + 20, true);
    const nameLen = view.getUint16(p + 28, true);
    const extraLen = view.getUint16(p + 30, true);
    const commentLen = view.getUint16(p + 32, true);
    const local = view.getUint32(p + 42, true);
    const name = new TextDecoder().decode(zip.subarray(p + 46, p + 46 + nameLen));
    if (name === wantedName || name.endsWith("/" + wantedName)) {
      const dataAt = local + 30 + view.getUint16(local + 26, true) + view.getUint16(local + 28, true);
      const raw = zip.subarray(dataAt, dataAt + compSize);
      if (method === 0) return raw;
      if (method === 8) return inflateRaw(raw);
      throw new Error("That zip uses a compression method this page cannot read.");
    }
    p += 46 + nameLen + extraLen + commentLen;
  }
  return null;
}

// ---------------------------------------------------------------------------
// Study pack

// Files the computer must never send: the reader's own progress log, the sample log, the
// branch's file list, and dot-files.
export function isSendableStudyPath(path) {
  const name = path.split("/").pop();
  if (!name || name.startsWith(".")) return false;
  if (path.split("/").some((part) => part.startsWith("."))) return false;
  return !["progress.jsonl", "progress.sample.jsonl", "files.json", "manifest.json"].includes(name);
}

// Picks the files for the chosen tracks and a manifest listing only those tracks.
// The manifest is returned separately so it can be sent last.
export function planStudyUpload(fileList, manifest, chosenKeys) {
  const known = new Set((manifest.tracks || []).map((t) => t.key));
  const keys = new Set(chosenKeys.filter((k) => known.has(k)));
  if (!keys.size) throw new Error("Tick at least one track to send.");
  const files = fileList.filter((f) => {
    if (!isSendableStudyPath(f.path)) return false;
    const top = f.path.includes("/") ? f.path.split("/")[0] : null;
    return top === null || keys.has(top);
  });
  files.sort((a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0));
  const filtered = { ...manifest, tracks: manifest.tracks.filter((t) => keys.has(t.key)) };
  return { files, manifest: filtered };
}

// ---------------------------------------------------------------------------
// USB serial file transfer (firmware: src/network/UsbSerialFileTransfer.cpp)
//
// Each command is "CMND" plus one letter. Paths are a u16 LE length and the bytes. A write
// is: path, u32 LE size, wait for "READY", then 256-byte chunks each answered by ACK (0x06)
// (the last one too, once the reader has saved the file), then the CRC32 as u32 LE, then
// "OK" or "ERR:...". BUSY lines and log lines share the port and are skipped.

export const ACK = 0x06;
export const CHUNK = 256;
const MAGIC = new Uint8Array([0x43, 0x4d, 0x4e, 0x44]); // CMND
const encoder = new TextEncoder();

export function commandBytes(letter, path) {
  const head = concatBytes(MAGIC, new Uint8Array([letter.charCodeAt(0)]));
  if (path === undefined) return head;
  const p = encoder.encode(path);
  if (p.length > 255) throw new Error(`Path too long: ${path}`);
  return concatBytes(head, u16le(p.length), p);
}

export class TransferError extends Error {}

// Reads bytes from the port into a queue so the protocol code can wait for a line or a byte.
// `source` is a ReadableStreamDefaultReader (or anything with read() -> {value, done}).
export class SerialLink {
  constructor(reader, writer) {
    this.reader = reader;
    this.writer = writer;
    this.queue = [];
    this.waiters = [];
    this.closed = false;
    this.line = "";
    this.pump = this.#pump();
  }

  async #pump() {
    try {
      for (;;) {
        const { value, done } = await this.reader.read();
        if (done) break;
        if (value && value.length) {
          for (const b of value) this.queue.push(b);
          this.#wake();
        }
      }
    } catch (e) {
      this.error = e;
    }
    this.closed = true;
    this.#wake();
  }

  #wake() {
    const w = this.waiters;
    this.waiters = [];
    for (const fn of w) fn();
  }

  #wait(ms) {
    return new Promise((resolve) => {
      const t = setTimeout(() => { this.waiters = this.waiters.filter((f) => f !== done); resolve(); }, ms);
      const done = () => { clearTimeout(t); resolve(); };
      this.waiters.push(done);
    });
  }

  async write(bytes) {
    await this.writer.write(bytes);
  }

  // Drops anything queued (boot logs and the like) for `ms`.
  async drain(ms) {
    const until = Date.now() + ms;
    while (Date.now() < until) {
      this.queue.length = 0;
      await this.#wait(until - Date.now());
    }
    this.queue.length = 0;
    this.line = "";
  }

  async #nextByte(deadline, what) {
    while (!this.queue.length) {
      if (this.closed) throw new TransferError("The reader disconnected.");
      const left = deadline() - Date.now();
      if (left <= 0) throw new TransferError(`The reader did not answer (${what}).`);
      await this.#wait(left);
    }
    return this.queue.shift();
  }

  // Waits for a line accepted by `wanted` (others are skipped). A line starting "ERR:" throws.
  async readLine(wanted, timeoutMs, what) {
    const end = Date.now() + timeoutMs;
    for (;;) {
      const b = await this.#nextByte(() => end, what);
      if (b === 0x0a) {
        const line = this.line.replace(/\r$/, "");
        this.line = "";
        if (line.startsWith("ERR:")) throw new TransferError(line);
        if (wanted(line)) return line;
      } else if (b !== ACK) {
        this.line += String.fromCharCode(b);
        if (this.line.length > 512) this.line = "";
      }
    }
  }

  // Waits for an ACK byte. Text (BUSY, logs) is skipped; an ERR line throws. A BUSY line
  // means the reader is still working, so it restarts the timer.
  async readAck(timeoutMs, what) {
    let end = Date.now() + timeoutMs;
    for (;;) {
      const b = await this.#nextByte(() => end, what);
      if (b === ACK) return;
      if (b === 0x0a) {
        const line = this.line.replace(/\r$/, "");
        this.line = "";
        if (line.startsWith("ERR:")) throw new TransferError(line);
        if (line.startsWith("BUSY:")) end = Date.now() + timeoutMs;
      } else {
        this.line += String.fromCharCode(b);
        if (this.line.length > 512) this.line = "";
      }
    }
  }

  async close() {
    try { await this.reader.cancel(); } catch { /* already closed */ }
    try { await this.pump; } catch { /* ignore */ }
    try { this.reader.releaseLock(); } catch { /* ignore */ }
    try { this.writer.releaseLock(); } catch { /* ignore */ }
  }
}

const isReply = (line) => line === "OK" || line.startsWith("READY") || line.startsWith("STATUS:");

// Asks the reader for its status. Throws TransferError("ERR:not_on_home") off the Home screen.
export async function readerStatus(link) {
  await link.write(commandBytes("S"));
  const line = await link.readLine((l) => l.startsWith("STATUS:"), 3000, "status");
  const fields = {};
  for (const part of line.slice(7).split(",")) {
    const eq = part.indexOf("=");
    if (eq > 0) fields[part.slice(0, eq)] = part.slice(eq + 1);
  }
  return fields;
}

export async function sendFile(link, path, data, onProgress) {
  await link.write(concatBytes(commandBytes("W", path), u32le(data.length)));
  await link.readLine((l) => l.startsWith("READY"), 10000, `start ${path}`);
  let crc = 0;
  for (let at = 0; at < data.length; at += CHUNK) {
    const chunk = data.subarray(at, Math.min(at + CHUNK, data.length));
    crc = crc32(chunk, crc);
    await link.write(chunk);
    await link.readAck(45000, `write ${path}`);
    if (onProgress) onProgress(at + chunk.length);
  }
  await link.write(u32le(crc));
  await link.readLine(isReply, 45000, `finish ${path}`);
}
