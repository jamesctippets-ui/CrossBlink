// Tests for site/public/flash/sticky-core.js. Run: node --test site/test/flash-core.test.mjs
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import test from "node:test";
import zlib from "node:zlib";

import {
  ACK, SECTOR, SerialLink, activeOtaEntry, checkImage, commandBytes, crc32, md5Hex, otaSeqCrc, parsePartitions,
  planOtaUpdate, planStudyUpload, readZipEntry, readerStatus, sendFile, u32le,
} from "../public/flash/sticky-core.js";

// The Sticky's partitions.csv: nvs, otadata, app0, app1 (plus the trailing MD5 entry).
function partitionTable() {
  const rows = [
    [1, 2, 0x9000, 0x5000, "nvs"],
    [1, 0, 0xe000, 0x2000, "otadata"],
    [0, 0x10, 0x10000, 0x640000, "app0"],
    [0, 0x11, 0x650000, 0x640000, "app1"],
  ];
  const table = new Uint8Array(0xc00).fill(0xff);
  const view = new DataView(table.buffer);
  rows.forEach(([type, sub, off, size, label], i) => {
    const at = i * 32;
    table[at] = 0xaa; table[at + 1] = 0x50; table[at + 2] = type; table[at + 3] = sub;
    view.setUint32(at + 4, off, true);
    view.setUint32(at + 8, size, true);
    table.fill(0, at + 12, at + 28);
    table.set(new TextEncoder().encode(label), at + 12);
    view.setUint32(at + 28, 0, true);
  });
  table[128] = 0xeb; table[129] = 0xeb; // MD5 entry
  return table;
}

function otadata(entries) {
  const data = new Uint8Array(2 * SECTOR).fill(0xff);
  const view = new DataView(data.buffer);
  entries.forEach((e, i) => {
    if (!e) return;
    view.setUint32(i * SECTOR, e.seq, true);
    view.setUint32(i * SECTOR + 24, e.state ?? 0, true);
    view.setUint32(i * SECTOR + 28, e.crc ?? otaSeqCrc(e.seq), true);
  });
  return data;
}

test("checksums match zlib, ESP-IDF and node's MD5", () => {
  const data = new TextEncoder().encode("CrossBlink ".repeat(500));
  assert.equal(crc32(data), zlib.crc32(data));
  assert.equal(crc32(data.subarray(100), crc32(data.subarray(0, 100))), zlib.crc32(data));
  assert.equal(otaSeqCrc(1), 0x4743989a); // the value ESP-IDF writes for sequence 1
  for (const n of [0, 1, 55, 56, 64, 65, 4097]) {
    const buf = new Uint8Array(n).map((_, i) => (i * 31) & 0xff);
    assert.equal(md5Hex(buf), createHash("md5").update(buf).digest("hex"));
  }
  assert.equal(md5Hex("abc"), "900150983cd24fb0d6963f7d28e17f72");
});

test("checkImage accepts an ESP32-S3 app and rejects the rest", () => {
  const img = new Uint8Array(64);
  img[0] = 0xe9; img[12] = 9;
  assert.equal(checkImage(img), null);
  assert.match(checkImage(img, 32), /more than/);
  img[12] = 5;
  assert.match(checkImage(img), /another chip/);
  img[0] = 0x50;
  assert.match(checkImage(img), /not an ESP32/);
});

test("parsePartitions reads the Sticky layout", () => {
  const parts = parsePartitions(partitionTable());
  assert.deepEqual(parts.map((p) => p.label), ["nvs", "otadata", "app0", "app1"]);
  assert.equal(parts[3].offset, 0x650000);
});

test("blank otadata (USB-flashed app0): writes app1 and sequence 2", () => {
  const plan = planOtaUpdate(parsePartitions(partitionTable()), otadata([]));
  assert.equal(plan.running.label, "app0");
  assert.equal(plan.target.label, "app1");
  assert.equal(plan.newSeq, 2);
  assert.equal(plan.otadataWriteAddress, 0xe000);
  const after = otadata([]);
  after.set(plan.otadataSector, 0);
  assert.deepEqual(activeOtaEntry(after), { index: 0, seq: 2 });
});

test("running app1 (seq 2 in sector 0): writes app0, seq 3, into sector 1", () => {
  const plan = planOtaUpdate(parsePartitions(partitionTable()), otadata([{ seq: 2 }]));
  assert.equal(plan.running.label, "app1");
  assert.equal(plan.target.label, "app0");
  assert.equal(plan.newSeq, 3);
  assert.equal(plan.otadataWriteAddress, 0xf000);
  const after = otadata([{ seq: 2 }]);
  after.set(plan.otadataSector, SECTOR);
  assert.deepEqual(activeOtaEntry(after), { index: 1, seq: 3 });
});

test("invalid, aborted and bad-CRC entries are ignored", () => {
  assert.deepEqual(activeOtaEntry(otadata([{ seq: 5 }, { seq: 6, state: 3 }])), { index: 0, seq: 5 });
  assert.deepEqual(activeOtaEntry(otadata([{ seq: 5 }, { seq: 6, state: 4 }])), { index: 0, seq: 5 });
  assert.deepEqual(activeOtaEntry(otadata([{ seq: 5, crc: 1 }, { seq: 4 }])), { index: 1, seq: 4 });
  // seq 5 runs app0 ((5-1)%2 = 0), so the next one is 6 for app1, written over sector 1.
  const plan = planOtaUpdate(parsePartitions(partitionTable()), otadata([{ seq: 5 }, { seq: 6, state: 3 }]));
  assert.equal(plan.target.label, "app1");
  assert.equal(plan.newSeq, 6);
  assert.equal(plan.otadataWriteAddress, 0xf000);
});

test("readZipEntry finds firmware.bin in a deflated zip", async () => {
  const payload = new Uint8Array(5000).map((_, i) => i & 0xff);
  const deflated = zlib.deflateRawSync(payload);
  const name = Buffer.from("firmware.bin");
  const local = Buffer.alloc(30);
  local.writeUInt32LE(0x04034b50, 0); local.writeUInt16LE(8, 8);
  local.writeUInt32LE(zlib.crc32(payload), 14); local.writeUInt32LE(deflated.length, 18);
  local.writeUInt32LE(payload.length, 22); local.writeUInt16LE(name.length, 26);
  const central = Buffer.alloc(46);
  central.writeUInt32LE(0x02014b50, 0); central.writeUInt16LE(8, 10);
  central.writeUInt32LE(deflated.length, 20); central.writeUInt32LE(payload.length, 24);
  central.writeUInt16LE(name.length, 28); central.writeUInt32LE(0, 42);
  const cdOffset = 30 + name.length + deflated.length;
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(1, 8); end.writeUInt16LE(1, 10);
  end.writeUInt32LE(46 + name.length, 12); end.writeUInt32LE(cdOffset, 16);
  const zip = new Uint8Array(Buffer.concat([local, name, deflated, central, name, end]));
  const got = await readZipEntry(zip, "firmware.bin", async (raw) => new Uint8Array(zlib.inflateRawSync(raw)));
  assert.deepEqual(got, payload);
  assert.equal(await readZipEntry(zip, "other.bin", async () => null), null);
});

test("planStudyUpload keeps chosen tracks, never the progress log, manifest separate", () => {
  const manifest = { pack: "cert-study-eink", tracks: [{ key: "az900" }, { key: "itil" }] };
  const files = [
    { path: "manifest.json" }, { path: "files.json" }, { path: "progress.jsonl" },
    { path: "progress.sample.jsonl" }, { path: ".DS_Store" }, { path: "README.txt" },
    { path: "itil/questions.jsonl" }, { path: "itil/cards.jsonl" }, { path: "az900/cards.jsonl" },
  ];
  const plan = planStudyUpload(files, manifest, ["itil", "nope"]);
  assert.deepEqual(plan.files.map((f) => f.path), ["README.txt", "itil/cards.jsonl", "itil/questions.jsonl"]);
  assert.deepEqual(plan.manifest.tracks, [{ key: "itil" }]);
  assert.throws(() => planStudyUpload(files, manifest, []), /at least one/);
});

// A fake reader speaking the firmware's protocol (src/network/UsbSerialFileTransfer.cpp),
// including the log and BUSY noise the real one prints on the same port.
function fakeReader({ onHome = true } = {}) {
  const files = new Map();
  let push;
  const readable = new ReadableStream({ start(c) { push = (bytes) => c.enqueue(new Uint8Array(bytes)); } });
  const reader = readable.getReader();
  const text = (s) => push(new TextEncoder().encode(s));
  let buf = new Uint8Array(0);
  let waiting = null;
  const take = async (n) => {
    while (buf.length < n) await new Promise((r) => { waiting = r; });
    const out = buf.slice(0, n);
    buf = buf.slice(n);
    return out;
  };
  const le32 = (b) => b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] * 0x1000000);
  (async () => {
    for (;;) {
      const magic = await take(4);
      if (new TextDecoder().decode(magic) !== "CMND") continue;
      const cmd = String.fromCharCode((await take(1))[0]);
      if (cmd === "S") {
        text("[123] [INF] log line\n");
        text(onHome ? "STATUS:protocol=1,device=sticky,firmware=1.2.3,free=1,largest=1\n" : "ERR:not_on_home\n");
        continue;
      }
      if (cmd !== "W") { text("ERR:unknown_cmd\n"); continue; }
      const len = (await take(2)).reduce((a, b, i) => a | (b << (8 * i)), 0);
      const path = new TextDecoder().decode(await take(len));
      const size = le32(await take(4));
      if (!onHome) { text("ERR:not_on_home\n"); continue; }
      text("READY\n");
      const data = new Uint8Array(size);
      let got = 0;
      let crc = 0;
      while (got < size) {
        const chunk = await take(Math.min(256, size - got));
        data.set(chunk, got);
        got += chunk.length;
        crc = crc32(chunk, crc);
        if (got % 4096 === 0) text(`BUSY:write:${got}\n`);
        if (got < size) push([ACK]);
      }
      if (size > 0) push([ACK]);
      const expected = le32(await take(4)) >>> 0;
      if (expected !== crc) { text(`ERR:crc:expected=${expected}\n`); continue; }
      files.set(path, data);
      text("OK\n");
    }
  })();
  const writer = {
    async write(bytes) {
      const merged = new Uint8Array(buf.length + bytes.length);
      merged.set(buf); merged.set(bytes, buf.length);
      buf = merged;
      if (waiting) { const w = waiting; waiting = null; w(); }
    },
    releaseLock() {},
  };
  return { files, link: new SerialLink(reader, writer) };
}

test("readerStatus and sendFile talk the reader's protocol", async () => {
  const { files, link } = fakeReader();
  const status = await readerStatus(link);
  assert.equal(status.device, "sticky");
  for (const size of [0, 1, 256, 257, 10000]) {
    const data = new Uint8Array(size).map((_, i) => (i * 7) & 0xff);
    let last = 0;
    await sendFile(link, `/study/t${size}.jsonl`, data, (n) => { last = n; });
    assert.deepEqual(files.get(`/study/t${size}.jsonl`), data);
    assert.equal(last, size);
  }
  await link.close();
});

test("off the Home screen the reader's error comes through", async () => {
  const { link } = fakeReader({ onHome: false });
  await assert.rejects(readerStatus(link), /ERR:not_on_home/);
  await link.close();
});

test("commandBytes frames a path with its u16 length", () => {
  assert.deepEqual([...commandBytes("W", "/a")], [67, 77, 78, 68, 87, 2, 0, 47, 97]);
  assert.deepEqual([...u32le(0x01020304)], [4, 3, 2, 1]);
});
