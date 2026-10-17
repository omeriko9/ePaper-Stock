// Node test of web/flasher.js with a small Web Serial shim on top of the serialport package.
// Usage: node test.mjs <port> [check|reset|flash] [firmware base URL]
//   check: read the board and the flash layout, change nothing
//   reset: check, then reset the chip
//   flash: check, then write all parts of the manifest and reset
import { SerialPort } from "serialport";
const [, , comPath, mode = "check", baseUrl = "http://127.0.0.1:8765/firmware/"] = process.argv;
const { loadManifest, downloadParts, partitionTablePart, Device } =
  await import(new URL("../../web/flasher.js", import.meta.url));

class WebSerialShim {
  constructor(path) { this.path = path; this.p = null; this.readable = null; this.writable = null; }
  getInfo() { return { usbVendorId: 0x303a, usbProductId: 0x1001 }; }
  async open({ baudRate }) {
    this.p = new SerialPort({ path: this.path, baudRate, autoOpen: false });
    await new Promise((res, rej) => this.p.open((e) => (e ? rej(e) : res())));
    const p = this.p;
    this.readable = new ReadableStream({
      start(c) { p.on("data", (d) => { try { c.enqueue(new Uint8Array(d)); } catch {} }); p.on("close", () => { try { c.close(); } catch {} }); },
    });
    this.writable = new WritableStream({
      write(chunk) { return new Promise((res, rej) => p.write(Buffer.from(chunk), (e) => (e ? rej(e) : p.drain(res)))); },
    });
  }
  // Web Serial changes only the given signals; serialport.set() resets the others, so keep both.
  async setSignals({ dataTerminalReady, requestToSend, break: brk }) {
    this.sig = this.sig || { dtr: false, rts: false, brk: false };
    if (dataTerminalReady !== undefined) this.sig.dtr = dataTerminalReady;
    if (requestToSend !== undefined) this.sig.rts = requestToSend;
    if (brk !== undefined) this.sig.brk = !!brk;
    await new Promise((res, rej) => this.p.set({ ...this.sig }, (e) => (e ? rej(e) : res())));
  }
  async close() {
    if (this.p && this.p.isOpen) await new Promise((res) => this.p.close(() => res()));
    this.readable = null; this.writable = null;
  }
}

const log = (l) => process.stdout.write("  | " + l.replace(/\n$/, "") + "\n");
const t0 = Date.now();
const manifest = await loadManifest(new URL(baseUrl));
console.log("manifest", manifest.version, manifest.parts.map((p) => p.file).join(", "));
const parts = await downloadParts(new URL(baseUrl), manifest);
console.log("downloaded + sha256 ok");
const dev = new Device(new WebSerialShim(comPath), log);
try {
  const info = await dev.connect();
  console.log("info", JSON.stringify(info));
  console.log("checkBoard ->", Device.checkBoard(info) ?? "OK");
  console.log("same layout ->", await dev.hasSameLayout(partitionTablePart(parts)));
  if (mode === "reset") {
    await dev.reset();
    console.log("reset only");
  }
  if (mode === "flash") {
    let last = -1;
    await dev.flash(parts, (d, t) => { const p = Math.floor((d / t) * 10); if (p !== last) { last = p; console.log("progress", p * 10 + "%"); } });
    await dev.reset();
    console.log("flashed and reset");
  }
} finally {
  await dev.close();
  console.log("time", (Date.now() - t0) / 1000, "s");
}
process.exit(0);
