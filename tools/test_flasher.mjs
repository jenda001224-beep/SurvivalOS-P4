// Runs docs/index.html's own script against a simulated ESP32-P4 behind a
// simulated Web Serial port. Nothing here replaces flashing a real board; what
// it does prove is that the page drives esptool-js correctly end to end:
// the right stub is uploaded, the bytes that reach "flash" are the image, the
// MD5 read-back is compared, the settings partitions are left alone when asked,
// a rev >=3 chip is refused before anything is written, and the board's
// direct RTS->EN wiring (which resets the chip whenever the port is reopened)
// ends in a successful flash instead of a dead end.
//
//   node tools/test_flasher.mjs        (Node 20 or newer)
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { createHash } from "node:crypto";
import { inflateSync } from "node:zlib";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
const FLASHER = join(ROOT, "docs");
const manifest = JSON.parse(readFileSync(join(FLASHER, "manifest.json"), "utf8"));
const image = new Uint8Array(readFileSync(join(FLASHER, manifest.image)));

const FLASH_BYTES = 16 * 1024 * 1024;
const OLD_FIRMWARE_FILL = 0x5A;       // stands in for whatever was there before
const MAGIC_REG = 0x40001000;
const EFUSE_REV_REG = 0x5012D000 + 68 + 8;
const SPI_BASE = 0x5008D000;
const REV1_STUB_TEXT_START = 0x4FF10000;
const REV1_STUB_ENTRY = 0x4FF10962;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const u32 = (bytes, at) => (bytes[at] | bytes[at + 1] << 8 | bytes[at + 2] << 16 | bytes[at + 3] << 24) >>> 0;

// ─── Simulated board ─────────────────────────────────────────────────────────
class FakeBoard {
  constructor({ revision = 100, rtsAssertedOnOpen = true, corruptWrites = false } = {}) {
    this.revision = revision;
    this.rtsAssertedOnOpen = rtsAssertedOnOpen;
    this.corruptWrites = corruptWrites;
    this.flash = new Uint8Array(FLASH_BYTES).fill(OLD_FIRMWARE_FILL);
    this.dtr = false; this.rts = false;         // true = asserted = line low
    this.mode = "flash";                        // what the chip is running
    this.stub = false;
    this.chipBaud = 115200;
    this.portBaud = 0;
    this.isOpen = false;
    this.rx = [];                               // partial SLIP frame from host
    this.inFrame = false; this.escape = false;
    this.controller = null;
    this.mem = null; this.defl = null;
    this.events = [];                           // notable things, for assertions
    this.spiW0 = 0;
    const board = this;
    this.port = {
      getInfo: () => ({ usbVendorId: 0x1A86, usbProductId: 0x55D3 }),
      async open({ baudRate }) {
        if (board.isOpen) throw new Error("port already open");
        board.isOpen = true; board.portBaud = baudRate;
        board.port.readable = new ReadableStream({
          start(controller) { board.controller = controller; },
          cancel() { board.controller = null; },
        });
        board.port.writable = new WritableStream({
          write(chunk) { board.fromHost(chunk); },
        });
        // POSIX serial drivers raise DTR and RTS on open.
        if (board.rtsAssertedOnOpen) board.setLines(true, true);
      },
      async close() {
        if (!board.isOpen) throw new Error("port not open");
        board.isOpen = false;
        try { board.controller?.close(); } catch (error) {}
        board.controller = null;
        board.port.readable = null; board.port.writable = null;
        board.setLines(false, false);           // lines drop when the port closes
      },
      async setSignals(signals) {
        board.setLines(signals.dataTerminalReady ?? board.dtr, signals.requestToSend ?? board.rts);
      },
      readable: null, writable: null,
    };
  }

  // RTS -> EN and DTR -> GPIO35, wired directly as on the T-Display-P4.
  setLines(dtr, rts) {
    const wasInReset = this.rts;
    // The strap is sampled when EN rises. EN rises ~10 ms after RTS is released
    // (10k/1uF), so a DTR change in the same call has already taken effect.
    this.dtr = dtr; this.rts = rts;
    if (rts && !wasInReset) {
      this.mode = "reset"; this.stub = false;
      if (this.defl) this.events.push("reset-during-write");
      this.mem = null; this.defl = null;
      this.events.push("reset");
    } else if (!rts && wasInReset) {
      this.chipBaud = 115200; this.stub = false; this.rx = []; this.inFrame = false;
      if (dtr) {
        this.mode = "download"; this.events.push("boot:download");
        this.toHostRaw(new TextEncoder().encode(
          "ESP-ROM:esp32p4-eco2-20240710\r\nBuild:Jul 10 2024\r\nrst:0x1 (POWERON),boot:0x30f (DOWNLOAD(USB/UART0/SDIO_REI_FEO))\r\nwaiting for download\r\n"));
      } else {
        this.mode = "flash"; this.events.push("boot:flash");
      }
    }
  }

  toHostRaw(bytes) {
    if (this.controller && this.portBaud === this.chipBaud) this.controller.enqueue(bytes);
  }
  toHostSlip(payload) {
    const out = [0xC0];
    for (const b of payload) {
      if (b === 0xC0) out.push(0xDB, 0xDC); else if (b === 0xDB) out.push(0xDB, 0xDD); else out.push(b);
    }
    out.push(0xC0);
    this.toHostRaw(new Uint8Array(out));
  }
  respond(op, value, data = []) {
    const status = this.stub ? [0, 0] : [0, 0, 0, 0];
    const body = [...data, ...status];
    this.toHostSlip([1, op, body.length & 0xFF, body.length >> 8,
      value & 0xFF, (value >>> 8) & 0xFF, (value >>> 16) & 0xFF, (value >>> 24) & 0xFF, ...body]);
  }

  fromHost(chunk) {
    // A chip held in reset, running the app, or listening at another baud rate
    // does not answer loader commands.
    if (this.mode !== "download" || this.portBaud !== this.chipBaud) return;
    for (const byte of chunk) {
      if (byte === 0xC0) {
        if (this.inFrame && this.rx.length) { const frame = this.rx; this.rx = []; this.handle(Uint8Array.from(frame)); }
        this.inFrame = true; this.rx = []; this.escape = false;
      } else if (this.inFrame) {
        if (this.escape) { this.rx.push(byte === 0xDC ? 0xC0 : 0xDB); this.escape = false; }
        else if (byte === 0xDB) this.escape = true;
        else this.rx.push(byte);
      }
    }
  }

  readReg(address) {
    if (address === MAGIC_REG) return 0x0ADDBAD0;
    if (address === EFUSE_REV_REG) {
      const major = Math.floor(this.revision / 100), minor = this.revision % 100;
      return ((major >> 2) & 1) << 23 | (major & 3) << 4 | (minor & 15);
    }
    if (address === SPI_BASE + 88) return this.spiW0;       // W0: last SPI result
    return 0;
  }

  handle(frame) {
    if (frame.length < 8 || frame[0] !== 0) return;
    const op = frame[1], checksumField = u32(frame, 4), data = frame.subarray(8);
    switch (op) {
      case 0x08:                                  // SYNC: the ROM answers eight times
        for (let i = 0; i < 8; i++) this.respond(op, this.stub ? 0 : 0x20120707);
        break;
      case 0x0A: this.respond(op, this.readReg(u32(data, 0))); break;
      case 0x09: {                                // WRITE_REG
        const address = u32(data, 0), value = u32(data, 4);
        // Writing the "user command" bit runs the SPI transaction; 0x9F is RDID.
        if (address === SPI_BASE + 32 && (value & 0xFF) === 0x9F) this.spiW0 = 0x1840C8;  // 16 MB part
        this.respond(op, 0); break;
      }
      case 0x05:                                  // MEM_BEGIN
        this.mem = this.mem || [];
        this.mem.push({ offset: u32(data, 12), size: u32(data, 0), bytes: [] });
        this.respond(op, 0); break;
      case 0x07: {                                // MEM_DATA
        const length = u32(data, 0), payload = data.subarray(16, 16 + length);
        let sum = 0xEF; for (const b of payload) sum ^= b;
        if (sum !== checksumField) this.events.push("bad-mem-checksum");
        this.mem[this.mem.length - 1].bytes.push(...payload);
        this.respond(op, 0); break;
      }
      case 0x06: {                                // MEM_END -> jump into the stub
        const entry = u32(data, 4);
        this.respond(op, 0);
        this.events.push(`stub:text=0x${this.mem[0].offset.toString(16)},entry=0x${entry.toString(16)}`);
        this.stub = true; this.mem = null;
        this.toHostSlip([0x4F, 0x48, 0x41, 0x49]);  // "OHAI"
        break;
      }
      case 0x0F:                                  // CHANGE_BAUDRATE
        this.respond(op, 0);
        this.chipBaud = u32(data, 0);
        this.events.push(`baud:${this.chipBaud}`);
        break;
      case 0x10:                                  // FLASH_DEFL_BEGIN
        this.defl = { size: u32(data, 0), blocks: u32(data, 4), blockSize: u32(data, 8),
                      offset: u32(data, 12), chunks: [], seq: 0 };
        this.respond(op, 0); break;
      case 0x11: {                                // FLASH_DEFL_DATA
        const length = u32(data, 0), seq = u32(data, 4), payload = data.subarray(16, 16 + length);
        let sum = 0xEF; for (const b of payload) sum ^= b;
        if (sum !== checksumField) this.events.push("bad-data-checksum");
        if (seq !== this.defl.seq++) this.events.push("bad-sequence");
        if (length > this.defl.blockSize) this.events.push("oversized-block");
        this.defl.chunks.push(Uint8Array.from(payload));
        this.respond(op, 0); break;
      }
      case 0x12: {                                // FLASH_DEFL_END -> commit to flash
        const total = this.defl.chunks.reduce((n, c) => n + c.length, 0);
        const packed = new Uint8Array(total);
        let at = 0; for (const c of this.defl.chunks) { packed.set(c, at); at += c.length; }
        const raw = inflateSync(packed);
        if (raw.length !== this.defl.size) this.events.push(`size-mismatch:${raw.length}!=${this.defl.size}`);
        this.flash.set(raw, this.defl.offset);
        if (this.corruptWrites) this.flash[this.defl.offset + 100] ^= 0x01;
        this.events.push(`write:0x${this.defl.offset.toString(16)}+${raw.length}`);
        this.defl = null;
        this.respond(op, 0); break;
      }
      case 0x13: {                                // SPI_FLASH_MD5
        const address = u32(data, 0), size = u32(data, 4);
        const digest = createHash("md5").update(this.flash.subarray(address, address + size)).digest();
        this.respond(op, 0, [...digest]); break;
      }
      default: this.respond(op, 0);
    }
  }
}

// ─── Minimal page environment ────────────────────────────────────────────────
const elements = new Map();
function element(id) {
  if (!elements.has(id)) {
    elements.set(id, {
      id, style: {}, children: [], handlers: {}, files: [], disabled: false, checked: false,
      scrollTop: 0, scrollHeight: 0, clientHeight: 0, className: "",
      _text: "",
      get textContent() { return this._text + this.children.map((c) => c.textContent).join("\n"); },
      set textContent(value) { this._text = value; this.children = []; },
      classList: { add() {}, remove() {} },
      appendChild(child) { this.children.push(child); },
      addEventListener(type, handler) { this.handlers[type] = handler; },
    });
  }
  return elements.get(id);
}
let nextBoard = null;
globalThis.document = { getElementById: element, createElement: () => element(Symbol("node")) };
Object.defineProperty(globalThis, "navigator", { configurable: true, value: {
  serial: { requestPort: async (options) => { lastPortOptions = options; return nextBoard.port; } },
  clipboard: { writeText: async () => {} },
} });
let lastPortOptions = null;
globalThis.window = globalThis;
globalThis.fetch = async (url) => {
  const name = String(url).split("?")[0];
  const body = readFileSync(join(FLASHER, name));
  return { ok: true, status: 200, json: async () => JSON.parse(body.toString("utf8")),
           arrayBuffer: async () => body.buffer.slice(body.byteOffset, body.byteOffset + body.byteLength) };
};

// Mirror the attributes the HTML gives these buttons before any script runs.
element("flash-btn").disabled = true;
element("custom-btn").disabled = true;

// Load the page's own script, unmodified apart from the import path.
const html = readFileSync(join(FLASHER, "index.html"), "utf8");
const script = html.slice(html.indexOf('<script type="module">') + 22, html.lastIndexOf("</script>"));
const bundleUrl = pathToFileURL(join(FLASHER, "vendor", "esptool-js-0.6.0.bundle.js")).href;
if (!script.includes('"./vendor/esptool-js-0.6.0.bundle.js"')) throw new Error("import line not found");
// build/ is git-ignored, so the extracted script never ends up in a commit.
mkdirSync(join(ROOT, "build"), { recursive: true });
const underTest = join(ROOT, "build", "flasher_under_test.mjs");
writeFileSync(underTest, script.replace('"./vendor/esptool-js-0.6.0.bundle.js"', JSON.stringify(bundleUrl)));
await import(pathToFileURL(underTest).href + "?t=" + Date.now());

// ─── Assertions ──────────────────────────────────────────────────────────────
let failures = 0;
function check(condition, what) {
  console.log(`  ${condition ? "ok  " : "FAIL"}  ${what}`);
  if (!condition) failures++;
}
const logText = () => element("log").children.map((c) => c.textContent).join("\n");
async function waitUntil(predicate, timeoutMs, what) {
  const start = Date.now();
  while (!predicate()) {
    if (Date.now() - start > timeoutMs) throw new Error(`timed out waiting for ${what}\n--- page log ---\n${logText()}`);
    await sleep(20);
  }
}
const same = (a, b) => a.length === b.length && Buffer.compare(Buffer.from(a.buffer, a.byteOffset, a.length), Buffer.from(b.buffer, b.byteOffset, b.length)) === 0;
const allEqual = (bytes, value) => bytes.every((b) => b === value);

async function flash(board, { clean }) {
  nextBoard = board;
  element("opt-clean").checked = clean;
  element("opt-allports").checked = false;
  const run = element("flash-btn").handlers.click();
  await sleep(50);
  await waitUntil(() => !element("flash-btn").disabled, 180000, "the flash to finish");
  await run;
  return logText();
}

console.log("start-up");
await waitUntil(() => !element("flash-btn").disabled, 20000, "firmware to load");
check(/SHA-256 and image checks passed/.test(logText()), "page downloaded and verified the image");
check(window.__flasher.md5(image) === manifest.md5, "page MD5 implementation matches hashlib");
check(createHash("sha256").update(image).digest("hex") === manifest.sha256, "image matches manifest SHA-256");

console.log("image rejection");
const rejects = (mutate, what) => {
  const copy = image.slice(); mutate(copy);
  let rejected = false;
  try { window.__flasher.checkMergedImage(copy, "test.bin"); } catch (error) { rejected = true; }
  check(rejected, `refuses ${what}`);
};
const setU16 = (bytes, at, value) => { bytes[at] = value & 0xFF; bytes[at + 1] = value >> 8; };
rejects((b) => { setU16(b, 0x20000 + 15, 301); setU16(b, 0x20000 + 17, 399); }, "an app built for chip rev >= 3");
rejects((b) => { setU16(b, 0x2000 + 15, 301); setU16(b, 0x2000 + 17, 399); }, "a bootloader built for chip rev >= 3");
rejects((b) => { setU16(b, 0x20000 + 12, 9); }, "an image for another chip (ESP32-S3)");
rejects((b) => { b[0x2000] = 0xFF; }, "a file with no bootloader at 0x2000");
rejects((b) => { b[0x8000] = 0xFF; }, "a file with no partition table");
rejects((b) => { b[0] = 0xE9; }, "an app-only image passed off as a merged one");
{
  let rejected = false;
  try { window.__flasher.checkMergedImage(image.slice(0, 0x21000), "short.bin"); } catch (error) { rejected = true; }
  check(rejected, "refuses a truncated file");
}

console.log("scenario 1: Mac-like host (RTS asserted on open), clean install");
{
  const board = new FakeBoard({ revision: 100, rtsAssertedOnOpen: true });
  const log = await flash(board, { clean: true });
  check(/Done\. SurvivalOS is starting/.test(log), "page reports success");
  check(lastPortOptions?.filters?.[0]?.usbVendorId === 0x1A86, "port picker is filtered to the CH343 vendor id");
  check(/fast mode did not work/.test(log) && /Retrying in safe mode/.test(log), "fast attempt failed and fell back to safe mode");
  check(board.events.includes(`stub:text=0x${REV1_STUB_TEXT_START.toString(16)},entry=0x${REV1_STUB_ENTRY.toString(16)}`), "rev <3 stub was uploaded");
  check(!board.events.includes("reset-during-write"), "no reset happened in the middle of a write");
  check(same(board.flash.subarray(0, image.length), image), "flash 0x0.. equals the image byte for byte");
  check(allEqual(board.flash.subarray(image.length), OLD_FIRMWARE_FILL), "nothing beyond the image was touched (ota_1, storage)");
  check(board.events.filter((e) => e.startsWith("write:")).length === 1, "exactly one write, at 0x0");
  check(board.events.at(-1) === "boot:flash" && board.mode === "flash", "device was left booting from flash, not in download mode");
  check(!board.isOpen && !board.rts && !board.dtr, "port closed with both control lines released");
  check(!board.events.some((e) => e.startsWith("bad-") || e.startsWith("size-") || e === "oversized-block"), "no protocol errors");
}

console.log("scenario 2: host that leaves RTS alone, keep settings");
{
  const board = new FakeBoard({ revision: 103, rtsAssertedOnOpen: false });
  const log = await flash(board, { clean: false });
  check(/Done\. SurvivalOS is starting/.test(log), "page reports success");
  check(board.events.includes("baud:460800") && !/did not work/.test(log), "fast mode worked first time");
  check(same(board.flash.subarray(0x2000, 0x9000), image.subarray(0x2000, 0x9000)), "bootloader + partition table written");
  check(same(board.flash.subarray(0xF000, 0x11000), image.subarray(0xF000, 0x11000)) && allEqual(board.flash.subarray(0xF000, 0x11000), 0xFF), "otadata rewritten blank -> boots ota_0");
  check(same(board.flash.subarray(0x20000, image.length), image.subarray(0x20000)), "app written");
  check(allEqual(board.flash.subarray(0x9000, 0xF000), OLD_FIRMWARE_FILL), "nvs (settings) untouched");
  check(allEqual(board.flash.subarray(0x12000, 0x20000), OLD_FIRMWARE_FILL), "app_nvs (settings) untouched");
  check(allEqual(board.flash.subarray(0, 0x2000), OLD_FIRMWARE_FILL), "nothing written below the bootloader");
  check(board.events.at(-1) === "boot:flash", "device was left booting from flash");
}

console.log("scenario 3: chip revision 3.1 (not this board)");
{
  const board = new FakeBoard({ revision: 301, rtsAssertedOnOpen: false });
  const log = await flash(board, { clean: true });
  check(/built for revision 1\.x/.test(log) && /Nothing was written/.test(log), "page refuses and says nothing was written");
  check(allEqual(board.flash, OLD_FIRMWARE_FILL), "flash is untouched");
  check(!/Retrying/.test(log), "did not retry after a definitive refusal");
}

console.log("scenario 4: device stores a corrupted byte");
{
  const board = new FakeBoard({ revision: 100, rtsAssertedOnOpen: false, corruptWrites: true });
  const log = await flash(board, { clean: true });
  check(/MD5 of file does not match/.test(log), "MD5 read-back catches the corruption");
  check(!/Done\. SurvivalOS is starting/.test(log), "page does not claim success");
  check(/flashed again/.test(log), "page explains that re-flashing fixes it");
}

console.log("scenario 5: serial monitor");
{
  const board = new FakeBoard({ rtsAssertedOnOpen: true });
  nextBoard = board;
  const running = element("mon-btn").handlers.click();
  await waitUntil(() => board.events.includes("boot:flash"), 5000, "monitor to release the reset lines");
  check(board.mode === "flash" && !board.rts && !board.dtr, "monitor leaves the device running normally (not held in reset, not in download mode)");
  board.toHostRaw(new TextEncoder().encode("I (123) boot: hello from the device\r\n"));
  await waitUntil(() => /hello from the device/.test(element("serlog").textContent), 3000, "monitor to show device output");
  check(true, "monitor shows what the device prints");
  await element("mon-btn").handlers.click();
  await running;
  check(!board.isOpen, "monitor closes the port on disconnect");
}

console.log(failures ? `\n${failures} CHECK(S) FAILED` : "\nall checks passed");
process.exit(failures ? 1 : 0);
