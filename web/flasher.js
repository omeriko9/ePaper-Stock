// Flash logic for the web installer. The page (index.html) and the Node test both use it.
// It writes only the parts in the manifest and never erases the chip, so the NVS settings
// (WiFi, stocks, Finnhub token) stay on the device.

import { ESPLoader, Transport } from "./vendor/esptool-js/esptool-0.7.0.js";

// The firmware is for the V1 board: ESP32-S3 with 4 MB flash and 2 MB quad PSRAM.
export const EXPECTED_CHIP = "ESP32-S3";
export const EXPECTED_FLASH_SIZE = "4MB";
export const ESPRESSIF_USB_VENDOR_ID = 0x303a;

const PARTITION_TABLE_OFFSET = 0x8000;

export async function loadManifest(baseUrl) {
  const res = await fetch(new URL("manifest.json", baseUrl), { cache: "no-store" });
  if (!res.ok) throw new Error(`manifest.json: HTTP ${res.status}`);
  return res.json();
}

// Downloads every part and checks its SHA-256 against the manifest.
export async function downloadParts(baseUrl, manifest, onProgress = () => {}) {
  const parts = [];
  for (const [i, part] of manifest.parts.entries()) {
    onProgress(i, manifest.parts.length, part.name);
    const res = await fetch(new URL(part.file, baseUrl), { cache: "no-store" });
    if (!res.ok) throw new Error(`${part.file}: HTTP ${res.status}`);
    const data = new Uint8Array(await res.arrayBuffer());
    const sha = await sha256Hex(data);
    if (data.length !== part.size || sha !== part.sha256) {
      throw new Error(`${part.file}: the download is damaged (size or SHA-256 is not correct)`);
    }
    parts.push({ ...part, offset: Number(part.offset), data });
  }
  onProgress(manifest.parts.length, manifest.parts.length, "");
  return parts;
}

export class Device {
  constructor(port, log = () => {}) {
    this.port = port;
    this.log = log;
    this.transport = null;
    this.loader = null;
  }

  // Puts the chip in download mode, loads the flasher stub and reads the board data.
  async connect() {
    this.transport = new Transport(this.port, false);
    const terminal = {
      clean() {},
      writeLine: (line) => this.log(line),
      write: (text) => this.log(text),
    };
    this.loader = new ESPLoader({ transport: this.transport, baudrate: 921600, terminal });
    const chip = await this.loader.main();
    const features = await this.loader.chip.getChipFeatures(this.loader);
    const flashSize = await this.loader.detectFlashSize();
    const mac = await this.loader.chip.readMac(this.loader);
    return { chip, features, flashSize, mac };
  }

  // Returns null if the board is correct, else a reason for the user.
  static checkBoard(info) {
    if (!info.chip.startsWith(EXPECTED_CHIP)) {
      return `This device has an ${info.chip} chip. This update is for the ESP32-S3 ePaper Stock Ticker only.`;
    }
    if (info.flashSize !== EXPECTED_FLASH_SIZE) {
      return `This device has ${info.flashSize || "an unknown size of"} flash memory. ` +
        `This update is for the board version with 4MB flash only.`;
    }
    if (info.features.some((f) => /PSRAM 8MB/.test(f))) {
      return "This device has 8MB PSRAM. This update is for the board version with 2MB PSRAM only.";
    }
    return null;
  }

  // True if the device has the same flash layout (partition table) as the new firmware.
  async hasSameLayout(partitionTablePart) {
    const len = partitionTablePart.data.length;
    const current = await this.loader.readFlash(PARTITION_TABLE_OFFSET, len);
    if (current.length !== len) return false;
    for (let i = 0; i < len; i++) {
      if (current[i] !== partitionTablePart.data[i]) return false;
    }
    return true;
  }

  // onProgress(writtenBytes, totalBytes)
  async flash(parts, onProgress = () => {}) {
    const total = parts.reduce((sum, p) => sum + p.data.length, 0);
    const before = [0];
    for (const p of parts) before.push(before[before.length - 1] + p.data.length);
    await this.loader.writeFlash({
      fileArray: parts.map((p) => ({ data: p.data, address: p.offset })),
      flashMode: "keep",
      flashFreq: "keep",
      flashSize: "keep",
      eraseAll: false,
      compress: true,
      reportProgress: (fileIndex, written, size) => {
        const done = before[fileIndex] + (written / size) * parts[fileIndex].data.length;
        onProgress(Math.min(done, total), total);
      },
      calculateMD5Hash: md5Hex,
    });
    onProgress(total, total);
  }

  // Starts the new firmware. The "hard_reset" of esptool-js only releases RTS and does not
  // pulse it, so the chip can stay in the stub. This is the pulse of esptool.py: keep DTR
  // (IO0) released, then hold RTS (EN) for 200 ms.
  async reset() {
    if (!this.transport) return;
    await this.transport.setDTR(false);
    await sleep(100);
    await this.transport.setRTS(true);
    await sleep(200);
    await this.transport.setRTS(false);
    await sleep(200);
  }

  async close() {
    if (this.transport) {
      try { await this.transport.disconnect(); } catch (e) { /* the port is already closed */ }
    }
    this.transport = null;
    this.loader = null;
  }
}

export function partitionTablePart(parts) {
  return parts.find((p) => p.offset === PARTITION_TABLE_OFFSET);
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function sha256Hex(data) {
  const digest = await crypto.subtle.digest("SHA-256", data);
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

// MD5 (RFC 1321). esptool-js uses it to check each image after the write.
export function md5Hex(bytes) {
  const s = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
  const k = new Uint32Array(64);
  for (let i = 0; i < 64; i++) k[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 2 ** 32);

  const bitLen = bytes.length * 8;
  const paddedLen = (((bytes.length + 8) >> 6) + 1) << 6;
  const buf = new Uint8Array(paddedLen);
  buf.set(bytes);
  buf[bytes.length] = 0x80;
  const view = new DataView(buf.buffer);
  view.setUint32(paddedLen - 8, bitLen >>> 0, true);
  view.setUint32(paddedLen - 4, Math.floor(bitLen / 2 ** 32), true);

  let a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
  const m = new Uint32Array(16);
  for (let off = 0; off < paddedLen; off += 64) {
    for (let j = 0; j < 16; j++) m[j] = view.getUint32(off + j * 4, true);
    let a = a0, b = b0, c = c0, d = d0;
    for (let i = 0; i < 64; i++) {
      let f, g;
      if (i < 16) { f = (b & c) | (~b & d); g = i; }
      else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
      else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
      else { f = c ^ (b | ~d); g = (7 * i) & 15; }
      const tmp = d;
      d = c;
      c = b;
      const x = (a + f + k[i] + m[g]) >>> 0;
      const r = s[(i >> 4) * 4 + (i & 3)];
      b = (b + ((x << r) | (x >>> (32 - r)))) >>> 0;
      a = tmp;
    }
    a0 = (a0 + a) >>> 0; b0 = (b0 + b) >>> 0; c0 = (c0 + c) >>> 0; d0 = (d0 + d) >>> 0;
  }
  const out = new DataView(new ArrayBuffer(16));
  [a0, b0, c0, d0].forEach((w, i) => out.setUint32(i * 4, w, true));
  return [...new Uint8Array(out.buffer)].map((b) => b.toString(16).padStart(2, "0")).join("");
}
