// c5flash.js — browser flashing for ESP32-C5 boards via esptool-js 0.7.0 in
// ROM bootloader (--no-stub) mode.
//
// Why this exists:
// 1. esp-web-tools 10.4.0 bundles esptool-js 0.6, which doesn't recognise newer
//    ESP32-C5 silicon revisions.
// 2. More importantly, on the ESP32-C5's native USB-Serial/JTAG interface
//    (VID 0x303A / PID 0x1001), uploading and starting the esptool RAM stub
//    flasher drops the USB-Serial/JTAG link ("Failed to start stub flasher" /
//    "Serial data stream stopped") during "Connecting to the chip…". That is why
//    platformio.ini uses `upload_flags = --no-stub`.
// 3. esptool-js 0.7.0 assumes the stub is always used and has four bugs when
//    IS_STUB === false on ESP32-C5:
//      a) USBJTAGSerialReset returns immediately after releasing RTS with no
//         settle delay, so the ROM boot banner collides with the first SLIP
//         sync packet ("Invalid head of packet (0x45)").
//      b) Skipping runStub() leaves SPI flash unattached (ROM requires an
//         8-byte ESP_SPI_ATTACH command + 24-byte ESP_SPI_SET_PARAMS command)
//         and leaves FLASH_WRITE_SIZE at 16384 instead of the ROM's 1024.
//      c) flashDeflBegin() only appends the 5th uint32 (encrypted_write = 0)
//         for S2/S3/C3/C2 when IS_STUB === false, omitting ESP32-C5/C6.
//      d) HardReset.reset() never asserts RTS (true) before deasserting RTS
//         (false), leaving the C5 stuck in download mode after flashing.
//
// patchEspLoaderForC5() fixes all four on an ESPLoader instance so both the
// single-board flasher (index.html) and batch flasher (batch.html) work
// reliably over native USB without holding BOOT/RESET.

const ESPTOOL_JS = "https://unpkg.com/esptool-js@0.7.0/bundle.js";
const SECTOR_SIZE = 0x1000; // 4 KB flash sector

// Strip leading 4 KB sectors that are all 0xFF (e.g. 0x0000..0x1fff before the
// C5 bootloader at 0x2000) and trailing 0xFF padding (e.g. 8 MB-padded release
// images), returning the trimmed Uint8Array and adjusted flash start address.
export function trimFlashImage(data, baseAddress = 0) {
  let start = 0;
  while (start + SECTOR_SIZE <= data.length) {
    let allFF = true;
    for (let i = start; i < start + SECTOR_SIZE; i++) {
      if (data[i] !== 0xff) {
        allFF = false;
        break;
      }
    }
    if (!allFF) break;
    start += SECTOR_SIZE;
  }

  let end = data.length;
  while (end > start + SECTOR_SIZE && data[end - 1] === 0xff) {
    end--;
  }
  end = Math.min(data.length, (end + 3) & ~3);

  return {
    data: data.subarray(start, end),
    address: baseAddress + start,
  };
}

// Patch an esptool-js 0.7.0 ESPLoader instance so ESP32-C5 boards connect,
// flash in ROM (--no-stub) mode, and hard-reset cleanly.
export function patchEspLoaderForC5(esploader, { onEraseStart } = {}) {
  // 1. Add a 150 ms settle delay after USB-JTAG reset releases RTS so the C5's
  //    ROM boot banner ("ESP-ROM:esp32c5... waiting for download") arrives in
  //    the serial buffer before flushInput() + sync() run.
  const origUsbReset = esploader.resetConstructors?.usbJTAGSerialReset;
  if (origUsbReset) {
    esploader.resetConstructors.usbJTAGSerialReset = (transport) => {
      const resetter = origUsbReset(transport);
      const origReset = resetter.reset.bind(resetter);
      resetter.reset = async () => {
        await origReset();
        await new Promise((r) => setTimeout(r, 150));
      };
      return resetter;
    };
  }

  // 2. Fix HardReset (esptool-js 0.7.0 omits setRTS(true) before setRTS(false)).
  if (esploader.resetConstructors) {
    esploader.resetConstructors.hardReset = (transport) => ({
      async reset() {
        await transport.setDTR(false);
        await transport.setRTS(true);
        await new Promise((r) => setTimeout(r, 200));
        await transport.setRTS(false);
        await new Promise((r) => setTimeout(r, 200));
      },
    });
  }

  // 3. On ESP32-C5, bypass the RAM stub flasher (which drops USB-Serial/JTAG)
  //    and initialize the ROM bootloader's SPI flash interface directly.
  const origRunStub = esploader.runStub.bind(esploader);
  esploader.runStub = async function () {
    const chipName = this.chip?.CHIP_NAME || "";
    if (!chipName.startsWith("ESP32-C5")) {
      return origRunStub();
    }
    this.IS_STUB = false;
    if (this.chip && this.chip.FLASH_WRITE_SIZE) {
      this.FLASH_WRITE_SIZE = this.chip.FLASH_WRITE_SIZE; // 1024 bytes in ROM mode
    }
    this.info("Using ROM bootloader (--no-stub mode for ESP32-C5)...");
    // Attach default SPI flash pads (ROM expects 8 zero bytes: hspi_arg + is_legacy)
    await this.checkCommand(
      "configure SPI flash pins",
      this.ESP_SPI_ATTACH,
      new Uint8Array(8)
    );
    // Configure ROM flashchip parameters for 8 MB flash so erase/write works
    const params = new Uint8Array(24);
    const dv = new DataView(params.buffer);
    dv.setUint32(0, 0, true);               // fl_id
    dv.setUint32(4, 8 * 1024 * 1024, true); // total_size (8 MB)
    dv.setUint32(8, 64 * 1024, true);       // block_size (64 KB)
    dv.setUint32(12, 4 * 1024, true);       // sector_size (4 KB)
    dv.setUint32(16, 256, true);            // page_size (256 B)
    dv.setUint32(20, 0xffff, true);         // status_mask
    await this.checkCommand("set SPI params", 0x0b, params);
    return this.chip;
  };

  // 4. Fix flashDeflBegin in ROM mode (IS_STUB === false): esptool-js 0.7.0
  //    forgot to append the 5th uint32 (encrypted_write = 0) for ESP32-C5/C6.
  const origFlashDeflBegin = esploader.flashDeflBegin.bind(esploader);
  esploader.flashDeflBegin = async function (size, compsize, offset) {
    if (this.IS_STUB) {
      return origFlashDeflBegin(size, compsize, offset);
    }
    const numBlocks = Math.floor((compsize + this.FLASH_WRITE_SIZE - 1) / this.FLASH_WRITE_SIZE);
    const eraseBlocks = Math.floor((size + this.FLASH_WRITE_SIZE - 1) / this.FLASH_WRITE_SIZE);
    const writeSize = eraseBlocks * this.FLASH_WRITE_SIZE;
    const timeout = this.timeoutPerMb(this.ERASE_REGION_TIMEOUT_PER_MB, writeSize);
    this.info("Compressed " + size + " bytes to " + compsize + "...");
    this.info("Erasing flash (this takes a few seconds in ROM mode)...");
    if (onEraseStart) onEraseStart();
    let pkt = this._appendArray(this._intToByteArray(writeSize), this._intToByteArray(numBlocks));
    pkt = this._appendArray(pkt, this._intToByteArray(this.FLASH_WRITE_SIZE));
    pkt = this._appendArray(pkt, this._intToByteArray(offset));
    pkt = this._appendArray(pkt, this._intToByteArray(0)); // 5th uint32: encrypted_write = 0
    await this.checkCommand(
      "enter compressed flash mode",
      this.ESP_FLASH_DEFL_BEGIN,
      pkt,
      undefined,
      undefined,
      timeout
    );
    return numBlocks;
  };
}

function statusEl(card) {
  let el = card.querySelector("[data-c5status]");
  if (!el) {
    el = document.createElement("p");
    el.setAttribute("data-c5status", "");
    el.style.cssText = "font-size:12px;margin:8px 0 0;word-break:break-word";
    const btn = card.querySelector("[data-c5flash]");
    (btn ? btn.parentNode : card).insertBefore(el, btn ? btn.nextSibling : null);
  }
  return el;
}

function setStatus(card, msg, kind) {
  const el = statusEl(card);
  el.hidden = false;
  el.textContent = msg;
  el.style.color = kind === "err" ? "var(--danger)"
                 : kind === "ok"  ? "var(--ok, var(--accent))"
                 : "var(--muted)";
}

function binUrlFor(card, btn) {
  if (card && card.dataset.fwActive) {
    return card.dataset.fwActive === "ham" ? card.dataset.binHam : card.dataset.binFork;
  }
  return btn.getAttribute("data-c5bin") || (card && card.dataset.binFork);
}

async function flashC5(btn) {
  const card = btn.closest(".board") || btn.closest(".card") || btn.parentNode;
  if (!("serial" in navigator)) {
    setStatus(card, "This browser has no Web Serial — use desktop Chrome, Edge or Brave.", "err");
    return;
  }
  const url = binUrlFor(card, btn);
  if (!url) { setStatus(card, "No firmware image is set for this card.", "err"); return; }

  btn.disabled = true;
  let transport = null;
  try {
    // Request the serial port IMMEDIATELY on the click gesture before awaiting
    // network imports/downloads (which can expire Chrome's user activation).
    setStatus(card, "Select the C5's serial port in the prompt…");
    const esptoolPromise = import(ESPTOOL_JS);
    const fwPromise = fetch(url, { cache: "no-store" }).then(async (resp) => {
      if (!resp.ok) throw new Error("couldn't fetch image (" + resp.status + ")");
      return new Uint8Array(await resp.arrayBuffer());
    });

    let port;
    try {
      port = await navigator.serial.requestPort();
    } catch (pickErr) {
      // User cancelled the port chooser — clear status quietly.
      if (pickErr && pickErr.name === "NotFoundError") {
        setStatus(card, "No port selected.", "muted");
        return;
      }
      throw pickErr;
    }

    setStatus(card, "Loading flasher & firmware…");
    const [{ ESPLoader, Transport }, rawData] = await Promise.all([esptoolPromise, fwPromise]);
    const { data, address } = trimFlashImage(rawData, 0);

    transport = new Transport(port, false);
    const esploader = new ESPLoader({
      transport,
      baudrate: 115200,
      terminal: { clean() {}, writeLine(s) { console.log(s); }, write(s) { console.log(s); } },
      debugLogging: false,
    });

    patchEspLoaderForC5(esploader, {
      onEraseStart: () => setStatus(card, "Erasing flash (takes ~5–10s)…"),
    });

    setStatus(card, "Connecting to the C5…");
    const chip = await esploader.main();

    setStatus(card, "Connected: " + chip + ". Preparing flash…");
    await esploader.writeFlash({
      fileArray: [{ data, address }],
      flashSize: "keep",
      flashMode: "keep",
      flashFreq: "keep",
      eraseAll: false,
      compress: true,
      reportProgress: (_i, written, total) => {
        setStatus(card, "Flashing " + Math.round((written / total) * 100) + "%…");
      },
    });

    setStatus(card, "Flashed — resetting the board…", "ok");
    try { await esploader.after("hard_reset"); } catch (_) {}
    setStatus(card, "Done. The C5 is running the new firmware.", "ok");
  } catch (err) {
    const m = (err && err.message) ? err.message : String(err);
    const portBusy = /failed to open/i.test(m) || /already open/i.test(m) || /in use/i.test(m);
    const hint = portBusy
      ? "the port is in use. Close other serial apps / flasher tabs, unplug & replug the board, reload this page, then retry."
      : "if the board didn't enter download mode, hold BOOT, tap RESET, release BOOT, and click Install again.";
    setStatus(card, "Failed: " + m + " — " + hint, "err");
  } finally {
    try { if (transport) await transport.disconnect(); } catch (_) {}
    btn.disabled = false;
  }
}

export function mountC5Flash() {
  document.querySelectorAll("[data-c5flash]").forEach((btn) => {
    btn.addEventListener("click", () => flashC5(btn));
  });
}
