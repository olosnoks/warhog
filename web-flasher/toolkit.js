// ============================================================================
//  Piglet Toolkit — WebSerial client for the firmware "WT" protocol.
//  SD file explorer + wardriver.cfg editor over the same USB cable as flashing.
//  Vanilla JS (no build step); talks to SerialTool.cpp in the firmware.
// ============================================================================
(function () {
  'use strict';
  if (!('serial' in navigator)) {         // no WebSerial -> show the unsupported note, disable Connect
    const reveal = () => {
      const u = document.getElementById('tkUnsupported'); if (u) u.style.display = '';
      const c = document.getElementById('tkConnect'); if (c) { c.disabled = true; c.textContent = '▶ USB not supported'; }
    };
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', reveal); else reveal();
    return;
  }

  // ---- helpers ----
  const $ = (id) => document.getElementById(id);
  const enc = new TextEncoder();
  const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

  function crc32(bytes) {
    let c = 0xFFFFFFFF;
    for (let i = 0; i < bytes.length; i++) {
      c ^= bytes[i];
      for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xEDB88320 & -(c & 1));
    }
    return (c ^ 0xFFFFFFFF) >>> 0;
  }
  const hex8 = (n) => (n >>> 0).toString(16).toUpperCase().padStart(8, '0');
  function b64encode(bytes) {
    let s = '';
    for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    return btoa(s);
  }
  function b64decode(str) {
    const bin = atob(str);
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  }
  function fmtSize(n) {
    if (n < 1024) return n + ' B';
    if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
    return (n / (1024 * 1024)).toFixed(2) + ' MB';
  }

  // ---- serial connection + line reader ----
  let port = null, writer = null, reader = null;
  let readDone = null;            // promise of the running readLoop()
  let readRestarts = 0;           // consecutive restarts without data (capped)
  let closing = false;            // true while disconnect() tears down
  let lineHandler = null;         // current op's line handler, or null
  let buf = '';                   // text accumulator for line framing

  function log(msg) {
    const el = $('tkLog');
    if (!el) return;
    el.textContent += msg + '\n';
    el.scrollTop = el.scrollHeight;
  }
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const withTimeout = (p, ms) => Promise.race([p, sleep(ms).then(() => { throw new Error('timeout'); })]);

  // One reader for the session. On end/error it EXITS — it never re-acquires a
  // reader in a tight loop (a failing read that rejects instantly would otherwise
  // spin on microtasks and freeze the tab, e.g. while the board reboots).
  async function readLoop() {
    const dec = new TextDecoder();
    let restart = false;
    try {
      reader = port.readable.getReader();
      while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        readRestarts = 0;
        buf += dec.decode(value, { stream: true });
        let nl;
        while ((nl = buf.indexOf('\n')) >= 0) {
          const line = buf.slice(0, nl).replace(/\r$/, '');
          buf = buf.slice(nl + 1);
          if ((line.startsWith('WT ') || line === 'WT') && lineHandler) lineHandler(line);
          // non-WT lines are firmware debug output -> ignore
        }
        if (buf.length > 65536) buf = buf.slice(-4096);   // newline-less flood guard
      }
    } catch (e) {
      // Non-fatal errors (e.g. buffer overrun from a boot-log burst) leave a fresh
      // port.readable behind: restart once via a macrotask, never synchronously.
      if (!closing && port && port.readable && ++readRestarts <= 5) restart = true;
      else if (!closing) log('Serial read stopped: ' + e.message);
    } finally {
      try { reader.releaseLock(); } catch (e) {}
      reader = null;
    }
    if (closing) return;
    if (restart) { await sleep(100); if (!closing && port && port.readable) { readDone = readLoop(); return; } }
    if (connected) { log('Piglet disconnected.'); disconnect(); }   // port died under us
  }

  async function writeLine(s) {
    if (!writer) throw new Error('not connected');
    await writer.write(enc.encode(s + '\n'));
  }

  // Run one operation: install a line handler, send the command, resolve when the
  // handler calls done()/fail(). Only one op at a time (the UI serializes).
  function op(sendFn, handlerFactory, timeoutMs) {
    return new Promise((resolve, reject) => {
      let settled = false;
      const t = setTimeout(() => finish(() => reject(new Error('timeout'))), timeoutMs || 15000);
      function finish(fn) { if (settled) return; settled = true; clearTimeout(t); lineHandler = null; fn(); }
      lineHandler = handlerFactory(
        (val) => finish(() => resolve(val)),
        (err) => finish(() => reject(err instanceof Error ? err : new Error(String(err))))
      );
      Promise.resolve(sendFn()).catch((e) => finish(() => reject(e)));
    });
  }

  // ---- protocol commands ----
  // Opening the port can glitch the USB control lines and reset the board, so keep
  // re-sending HELLO (once a second) until the firmware has booted and answers.
  function hello(totalMs) {
    return new Promise((resolve, reject) => {
      const deadline = Date.now() + (totalMs || 15000);
      let finished = false, iv = null;
      const finish = (fn) => { if (finished) return; finished = true; clearInterval(iv); lineHandler = null; fn(); };
      lineHandler = (line) => { if (line.startsWith('WT HELLO')) finish(() => resolve(line.slice(3).trim())); };
      const send = () => {
        if (finished) return;
        if (Date.now() > deadline || closing || !writer) {
          finish(() => reject(new Error('no reply from the Piglet — flash the latest firmware (it adds the toolkit protocol), then try again')));
          return;
        }
        writeLine('WT HELLO').catch(() => {});
      };
      iv = setInterval(send, 1000);
      send();
    });
  }

  async function ls(dir) {
    const files = [], dirs = [];
    return op(() => writeLine('WT LS ' + dir), (done, fail) => (line) => {
      if (line.startsWith('WT F ')) {
        const m = line.match(/^WT F (\d+) (.+)$/);
        if (m) files.push({ size: +m[1], path: m[2] });
      } else if (line.startsWith('WT DIR ')) {
        dirs.push(line.slice(7).trim());
      } else if (line === 'WT END') {
        done({ files, dirs });
      } else if (line.startsWith('WT ERR')) {
        fail(line.slice(3).trim());
      }
    }, 20000);
  }

  async function get(path, onProgress) {
    let size = 0, received = 0;
    const chunks = [];
    return op(() => writeLine('WT GET ' + path), (done, fail) => (line) => {
      if (line.startsWith('WT BEGIN ')) {
        size = +line.slice(9).trim();
      } else if (line.startsWith('WT DATA ')) {
        const bytes = b64decode(line.slice(8));
        chunks.push(bytes); received += bytes.length;
        if (onProgress && size) onProgress(received, size);
      } else if (line.startsWith('WT END ')) {
        const crc = parseInt(line.slice(7).trim(), 16) >>> 0;
        const out = new Uint8Array(received);
        let o = 0; for (const c of chunks) { out.set(c, o); o += c.length; }
        if ((crc32(out) >>> 0) !== crc) { fail('CRC mismatch'); return; }
        done(out);
      } else if (line.startsWith('WT ERR')) {
        fail(line.slice(3).trim());
      }
    }, 120000);
  }

  async function putBytes(cmd, bytes, onProgress) {
    const crc = crc32(bytes);
    const CHUNK = 384;
    return op(() => writeLine(cmd + ' ' + bytes.length + ' ' + hex8(crc)), (done, fail) => (line) => {
      if (line === 'WT READY') {
        (async () => {
          try {
            for (let i = 0; i < bytes.length; i += CHUNK) {
              const slice = bytes.subarray(i, i + CHUNK);
              await writeLine('WT DATA ' + b64encode(slice));
              if (onProgress) onProgress(Math.min(i + CHUNK, bytes.length), bytes.length);
            }
            if (bytes.length === 0) await writeLine('WT DATA ');
          } catch (e) { fail(e); }
        })();
      } else if (line.startsWith('WT OK')) {
        done(line.slice(3).trim());
      } else if (line.startsWith('WT ERR')) {
        fail(line.slice(3).trim());
      }
    }, 120000);
  }
  const put    = (path, bytes, p) => putBytes('WT PUT ' + path, bytes, p);
  const cfgPut = (bytes, p)       => putBytes('WT CFG PUT', bytes, p);

  async function rm(path) {
    return op(() => writeLine('WT RM ' + path), (done, fail) => (line) => {
      if (line.startsWith('WT OK')) done();
      else if (line.startsWith('WT ERR')) fail(line.slice(3).trim());
    }, 15000);
  }
  async function cfgGet() { return get('/wardriver.cfg'); }

  // ---- connection lifecycle ----
  let connected = false, busy = false;
  function setBusy(b) {
    busy = b;
    document.querySelectorAll('#tkPanel button, #tkPanel select, #tkPanel input').forEach(e => { e.disabled = b; });
    if (!b) updateToolbar();   // re-apply selection-based button states
  }

  let connecting = false;
  async function connect() {
    if (connecting) return;
    connecting = true;
    $('tkConnect').disabled = true;
    try {
      port = await navigator.serial.requestPort();
      await port.open({ baudRate: 115200, bufferSize: 65536 });   // native USB-CDC ignores baud
      // On the ESP32 USB-Serial/JTAG, RTS-high + DTR-low resets the chip. Drive both
      // lines to the idle "run" state, dropping RTS FIRST so we never pass through
      // the reset combination (open() itself may already have glitched them).
      try { await port.setSignals({ requestToSend: false }); await port.setSignals({ dataTerminalReady: false }); } catch (e) {}
      closing = false;
      readRestarts = 0;
      writer = port.writable.getWriter();
      readDone = readLoop();
      $('tkStatus').textContent = 'waiting for the Piglet to answer…';
      $('tkStatus').className = 'tkStat';
      const info = await hello(15000);
      connected = true;
      $('tkConnect').textContent = 'Disconnect';
      $('tkStatus').textContent = 'connected · ' + info;
      $('tkStatus').className = 'tkStat ok';
      $('tkPanel').style.display = '';
      log('Connected: ' + info);
      await refreshFiles();
    } catch (e) {
      const msg = e && e.message ? e.message : String(e);
      log('Connect failed: ' + msg);
      await disconnect();
      $('tkStatus').textContent = /No port/i.test(msg) ? 'no port selected' : ('error: ' + msg);
      $('tkStatus').className = 'tkStat bad';
    } finally {
      connecting = false;
      $('tkConnect').disabled = false;
    }
  }

  // Tear down in a fixed order with a timeout on every step, so a dead or
  // rebooting device can never leave the page hanging.
  async function disconnect() {
    if (closing) return;
    closing = true;
    const wasConnected = connected;
    connected = false;
    lineHandler = null;
    if (wasConnected && writer) { try { await withTimeout(writer.write(enc.encode('WT BYE\n')), 500); } catch (e) {} }
    try { if (reader) await withTimeout(reader.cancel(), 1000); } catch (e) {}
    try { if (readDone) await withTimeout(readDone, 1000); } catch (e) {}
    try { if (writer) writer.releaseLock(); } catch (e) {}
    writer = null;
    try { if (port) await withTimeout(port.close(), 1500); } catch (e) {}
    port = null; readDone = null; buf = '';
    closing = false;
    $('tkConnect').textContent = 'Connect over USB';
    $('tkStatus').textContent = 'disconnected';
    $('tkStatus').className = 'tkStat';
    $('tkPanel').style.display = 'none';
  }

  // ---- file explorer UI (traditional file browser) ----
  // Folder navigation (breadcrumbs / Up / double-click), sortable columns,
  // click / Ctrl / Shift selection, toolbar actions on the selection,
  // drag-and-drop upload into the current folder, and a text preview.
  const TEXT_EXT = ['cfg', 'csv', 'txt', 'log', 'json', 'md', 'ini', 'conf'];
  const PREVIEW_MAX = 256 * 1024;
  let cwd = '/';
  let entries = [];             // [{ path, name, dir, size }]
  let view = [];                // entries in display order
  let sortKey = 'name', sortDir = 1;
  const selected = new Set();   // selected paths
  let anchor = -1;              // shift-select anchor index into view

  const baseName = (p) => p.replace(/\/+$/, '').split('/').pop() || '/';
  const extOf = (n) => { const i = n.lastIndexOf('.'); return i > 0 ? n.slice(i + 1).toLowerCase() : ''; };
  const parentOf = (p) => (p.replace(/\/[^/]+\/?$/, '') || '/');
  const joinPath = (dir, name) => (dir === '/' ? '' : dir) + '/' + name;
  function typeOf(e) {
    if (e.dir) return 'Folder';
    const x = extOf(e.name);
    return ({ csv: 'CSV log', cfg: 'Config', txt: 'Text', log: 'Log', json: 'JSON', bin: 'Firmware',
              pcap: 'Capture', pcapng: 'Capture', gz: 'Archive', tmp: 'Temp' })[x] || (x ? x.toUpperCase() + ' file' : 'File');
  }
  const SVG_FOLDER = '<svg viewBox="0 0 24 24"><path fill="#d9a93b" d="M2 6.5A2.5 2.5 0 0 1 4.5 4h4.2l2 2.2h8.8A2.5 2.5 0 0 1 22 8.7v9.8a2.5 2.5 0 0 1-2.5 2.5h-15A2.5 2.5 0 0 1 2 18.5z"/><path fill="#f3c654" d="M2 9.5A1.5 1.5 0 0 1 3.5 8h17A1.5 1.5 0 0 1 22 9.5v9a2.5 2.5 0 0 1-2.5 2.5h-15A2.5 2.5 0 0 1 2 18.5z"/></svg>';
  const SVG_UP = '<svg viewBox="0 0 24 24"><path fill="#d9a93b" d="M2 6.5A2.5 2.5 0 0 1 4.5 4h4.2l2 2.2h8.8A2.5 2.5 0 0 1 22 8.7v9.8a2.5 2.5 0 0 1-2.5 2.5h-15A2.5 2.5 0 0 1 2 18.5z"/><path fill="#f3c654" d="M2 9.5A1.5 1.5 0 0 1 3.5 8h17A1.5 1.5 0 0 1 22 9.5v9a2.5 2.5 0 0 1-2.5 2.5h-15A2.5 2.5 0 0 1 2 18.5z"/><path d="M12 18v-6m-3 3 3-3 3 3" stroke="#7a5a12" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round"/></svg>';
  const SVG_DRIVE = '<svg viewBox="0 0 24 24"><path fill="#8b93b8" d="M7 2h8l4 4v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2z"/><path fill="#5d6488" d="M9 4h1.4v4H9zm2.6 0H13v4h-1.4zm2.6 0h1.4v4h-1.4z"/></svg>';
  function iconOf(e) {
    if (e.dir) return SVG_FOLDER;
    const x = extOf(e.name);
    const [col, lbl] = ({ csv: ['#3fb950', 'CSV'], cfg: ['#e3863b', 'CFG'], ini: ['#e3863b', 'INI'], conf: ['#e3863b', 'CFG'],
      txt: ['#8b93b8', 'TXT'], log: ['#8b93b8', 'LOG'], json: ['#d2a106', '{ }'], bin: ['#a371f7', 'BIN'],
      gz: ['#a371f7', 'GZ'], pcap: ['#2f9bd6', 'PCAP'], pcapng: ['#2f9bd6', 'PCAP'] })[x] || ['#8b93b8', ''];
    return '<svg viewBox="0 0 24 24"><path fill="#e9eaf2" d="M6 1.5h8.5L20 7v13.5A2 2 0 0 1 18 22.5H6a2 2 0 0 1-2-2v-17a2 2 0 0 1 2-2z"/>' +
      '<path fill="#b8bccf" d="M14.5 1.5V7H20z"/>' +
      (lbl ? '<rect x="2" y="12.5" width="' + (lbl.length > 3 ? 20 : 16) + '" height="7" rx="1.5" fill="' + col + '"/>' +
             '<text x="' + (lbl.length > 3 ? 12 : 10) + '" y="18" text-anchor="middle" font-family="system-ui,sans-serif" font-size="5.4" font-weight="700" fill="#fff">' + lbl + '</text>'
           : '<path stroke="' + col + '" stroke-width="1.4" d="M7.5 12h9M7.5 15h9M7.5 18h6"/>') + '</svg>';
  }


  function sortView() {
    const k = sortKey, d = sortDir;
    view = entries.slice().sort((a, b) => {
      if (a.dir !== b.dir) return a.dir ? -1 : 1;             // folders first
      let r = 0;
      if (k === 'size') r = a.size - b.size;
      else if (k === 'type') r = typeOf(a).localeCompare(typeOf(b));
      if (!r) r = a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: 'base' });
      return r * d;
    });
  }

  function renderCrumbs() {
    const c = $('tkCrumbs');
    c.innerHTML = SVG_DRIVE;
    const segs = cwd.split('/').filter(Boolean);
    const wn = $('tkWinName');
    if (wn) wn.innerHTML = (segs.length ? SVG_FOLDER : SVG_DRIVE) + '<span></span>', wn.lastChild.textContent = segs.length ? segs[segs.length - 1] : 'SD card';
    const add = (label, path, isCur) => {
      if (isCur) { const s = document.createElement('span'); s.className = 'cur'; s.textContent = label; c.appendChild(s); return; }
      const b = document.createElement('button'); b.type = 'button'; b.textContent = label;
      b.onclick = () => navigate(path);
      c.appendChild(b);
    };
    add('SD', '/', segs.length === 0);
    segs.forEach((s, i) => {
      const sep = document.createElement('span'); sep.className = 'sep'; sep.textContent = '›'; c.appendChild(sep);
      add(s, '/' + segs.slice(0, i + 1).join('/'), i === segs.length - 1);
    });
  }

  function cell(cls, text) { const s = document.createElement('span'); s.className = cls; s.textContent = text; return s; }
  function nameCell(svg, name) {
    const s = document.createElement('span'); s.className = 'nm'; s.innerHTML = svg;
    const b = document.createElement('b'); b.textContent = name; s.appendChild(b); return s;
  }

  function renderList() {
    sortView();
    const list = $('tkFiles');
    list.innerHTML = '';
    const hd = document.createElement('div'); hd.className = 'tkFr hd';
    const arrow = (k) => (sortKey === k ? (sortDir > 0 ? ' ▴' : ' ▾') : '');
    hd.append(cell('nm', 'Name' + arrow('name')), cell('sz', 'Size' + arrow('size')), cell('ty', 'Type' + arrow('type')));
    [['nm', 'name'], ['sz', 'size'], ['ty', 'type']].forEach(([cls, key]) => {
      hd.querySelector('.' + cls).onclick = () => { if (sortKey === key) sortDir = -sortDir; else { sortKey = key; sortDir = 1; } renderList(); };
    });
    list.appendChild(hd);
    if (cwd !== '/') {
      const up = document.createElement('div'); up.className = 'tkFr';
      up.append(nameCell(SVG_UP, '..'), cell('sz', ''), cell('ty', 'Parent folder'));
      up.ondblclick = () => navigate(parentOf(cwd));
      list.appendChild(up);
    }
    if (!view.length) {
      const e = document.createElement('div'); e.className = 'tkFbEmpty';
      e.textContent = 'This folder is empty. Drop files here to upload.';
      list.appendChild(e);
    }
    view.forEach((it, idx) => {
      const r = document.createElement('div');
      r.className = 'tkFr' + (selected.has(it.path) ? ' sel' : '');
      r.dataset.idx = idx;
      r.title = it.path;
      r.append(nameCell(iconOf(it), it.name), cell('sz', it.dir ? '—' : fmtSize(it.size)), cell('ty', typeOf(it)));
      r.onclick = (ev) => onRowClick(ev, idx);
      r.ondblclick = () => openItem(it);
      list.appendChild(r);
    });
    updateToolbar();
  }

  function paintSelection() {
    $('tkFiles').querySelectorAll('.tkFr[data-idx]').forEach((r) => {
      r.classList.toggle('sel', selected.has(view[+r.dataset.idx].path));
    });
    updateToolbar();
  }

  function onRowClick(ev, idx) {
    if (busy) return;
    const p = view[idx].path;
    if (ev.shiftKey && anchor >= 0) {
      if (!(ev.ctrlKey || ev.metaKey)) selected.clear();
      const a = Math.min(anchor, idx), b = Math.max(anchor, idx);
      for (let i = a; i <= b; i++) selected.add(view[i].path);
    } else if (ev.ctrlKey || ev.metaKey) {
      if (selected.has(p)) selected.delete(p); else selected.add(p);
      anchor = idx;
    } else {
      selected.clear(); selected.add(p); anchor = idx;
    }
    paintSelection();
  }

  const selectedFiles = () => view.filter((e) => !e.dir && selected.has(e.path));

  function updateToolbar() {
    const files = selectedFiles();
    const anySel = view.some((e) => selected.has(e.path));
    if ($('tkDlSel')) $('tkDlSel').disabled = busy || !files.length;
    if ($('tkDelSel')) {
      $('tkDelSel').disabled = busy || !files.length;
      $('tkDelSel').title = anySel && !files.length ? 'Folders can’t be deleted from here' : 'Delete selected (Del)';
    }
    if ($('tkUp')) $('tkUp').disabled = busy || cwd === '/';
    if ($('tkCfgSave')) $('tkCfgSave').disabled = busy || !cfgValid;   // never save a non-text config
    const nd = view.filter((e) => e.dir).length, nf = view.length - nd;
    let s = nd + ' folder' + (nd === 1 ? '' : 's') + ', ' + nf + ' file' + (nf === 1 ? '' : 's');
    if (anySel) {
      const bytes = files.reduce((t, e) => t + e.size, 0);
      const n = view.filter((e) => selected.has(e.path)).length;
      s += ' · ' + n + ' selected' + (files.length ? ' (' + fmtSize(bytes) + ')' : '');
    }
    if ($('tkFbStatus')) $('tkFbStatus').textContent = s;
  }

  async function refreshFiles() {
    if (!connected) return;
    renderCrumbs();
    const list = $('tkFiles');
    list.innerHTML = '';
    const loading = document.createElement('div'); loading.className = 'tkFbEmpty';
    loading.textContent = 'Loading ' + cwd + '…';
    list.appendChild(loading);
    setBusy(true);
    try {
      const { files, dirs } = await ls(cwd);
      entries = dirs.map((p) => ({ path: p, name: baseName(p), dir: true, size: 0 }))
        .concat(files.map((f) => ({ path: f.path, name: baseName(f.path), dir: false, size: f.size })));
      const keep = new Set(entries.map((e) => e.path));
      for (const p of [...selected]) if (!keep.has(p)) selected.delete(p);
      setBusy(false);
      renderList();
    } catch (e) {
      setBusy(false);
      list.innerHTML = '';
      const d = document.createElement('div'); d.className = 'tkFbEmpty tkStat bad';
      d.textContent = 'Could not open ' + cwd + ': ' + e.message;
      list.appendChild(d);
      updateToolbar();
    }
  }

  function navigate(path) {
    if (busy) return;
    cwd = path || '/';
    selected.clear(); anchor = -1;
    refreshFiles();
  }

  function saveBytes(name, bytes) {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([bytes], { type: 'application/octet-stream' }));
    a.download = name;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  }

  function fetchFile(it) {
    return get(it.path, (r, t) => { $('tkStatus').textContent = 'reading ' + it.name + ' ' + fmtSize(r) + ' / ' + fmtSize(t); });
  }

  async function downloadItems(items) {
    if (!items.length || busy) return;
    setBusy(true);
    try {
      for (const it of items) {
        log('GET ' + it.path + ' …');
        const bytes = await fetchFile(it);
        saveBytes(it.name, bytes);
        log('Saved ' + it.name + ' (' + bytes.length + ' B)');
      }
      $('tkStatus').textContent = 'downloaded ' + items.length + ' file' + (items.length === 1 ? '' : 's');
    } catch (e) { log('GET failed: ' + e.message); $('tkStatus').textContent = 'download failed: ' + e.message; }
    finally { setBusy(false); }
  }

  let pvBytes = null, pvName = '';
  async function previewItem(it) {
    setBusy(true);
    try {
      const bytes = await fetchFile(it);
      pvBytes = bytes; pvName = it.name;
      $('tkPvTitle').textContent = it.path + '  (' + fmtSize(bytes.length) + ')';
      $('tkPvBody').textContent = new TextDecoder().decode(bytes);
      $('tkPreview').style.display = '';
      $('tkStatus').textContent = 'previewing ' + it.name;
    } catch (e) { log('GET failed: ' + e.message); $('tkStatus').textContent = 'preview failed: ' + e.message; }
    finally { setBusy(false); }
  }
  function closePreview() { $('tkPreview').style.display = 'none'; pvBytes = null; $('tkFiles').focus(); }

  function openItem(it) {
    if (busy) return;
    if (it.dir) { navigate(it.path); return; }
    if (TEXT_EXT.includes(extOf(it.name)) && it.size <= PREVIEW_MAX) { previewItem(it); return; }
    downloadItems([it]);
  }

  async function deleteSelected() {
    const files = selectedFiles();
    if (!files.length || busy) return;
    const what = files.length === 1 ? files[0].path : files.length + ' files';
    if (!confirm('Permanently delete ' + what + ' from the SD card?\nThis cannot be undone.')) return;
    setBusy(true);
    let failed = 0;
    for (const f of files) {
      try { await rm(f.path); log('Deleted ' + f.path); selected.delete(f.path); }
      catch (e) { failed++; log('RM failed ' + f.path + ': ' + e.message); }
    }
    setBusy(false);
    if (failed) alert(failed + ' file(s) could not be deleted — see the log.');
    refreshFiles();
  }

  async function uploadFiles(fileList) {
    const list = Array.from(fileList || []);
    if (!list.length || busy) return;
    const clash = list.filter((f) => entries.some((e) => !e.dir && e.name === f.name)).map((f) => f.name);
    if (clash.length && !confirm('Replace existing file' + (clash.length > 1 ? 's' : '') + ' in ' + cwd + '?\n' + clash.join('\n'))) return;
    setBusy(true);
    try {
      for (const file of list) {
        const bytes = new Uint8Array(await file.arrayBuffer());
        const path = joinPath(cwd, file.name);
        log('PUT ' + path + ' (' + bytes.length + ' B) …');
        await put(path, bytes, (s, t) => { $('tkStatus').textContent = 'uploading ' + file.name + ' ' + fmtSize(s) + ' / ' + fmtSize(t); });
        log('Uploaded ' + path);
      }
      $('tkStatus').textContent = 'uploaded ' + list.length + ' file' + (list.length === 1 ? '' : 's') + ' to ' + cwd;
    } catch (e) { log('PUT failed: ' + e.message); alert('Upload failed: ' + e.message); }
    finally { setBusy(false); }
    refreshFiles();
  }

  function onListKey(ev) {
    if (busy || $('tkPreview').style.display === '') return;
    const k = ev.key;
    if ((ev.ctrlKey || ev.metaKey) && k.toLowerCase() === 'a') {
      ev.preventDefault(); view.forEach((e) => selected.add(e.path)); paintSelection();
    } else if (k === 'Delete') {
      ev.preventDefault(); deleteSelected();
    } else if (k === 'Backspace') {
      ev.preventDefault(); if (cwd !== '/') navigate(parentOf(cwd));
    } else if (k === 'Enter') {
      const sel = view.filter((e) => selected.has(e.path));
      if (sel.length === 1) { ev.preventDefault(); openItem(sel[0]); }
    } else if (k === 'ArrowDown' || k === 'ArrowUp') {
      if (!view.length) return;
      ev.preventDefault();
      const cur = anchor >= 0 ? anchor : (k === 'ArrowDown' ? -1 : view.length);
      const next = Math.max(0, Math.min(view.length - 1, cur + (k === 'ArrowDown' ? 1 : -1)));
      if (!ev.shiftKey) selected.clear();
      selected.add(view[next].path); anchor = next;
      paintSelection();
      const row = $('tkFiles').querySelector('.tkFr[data-idx="' + next + '"]');
      if (row) row.scrollIntoView({ block: 'nearest' });
    }
  }

  function initBrowser() {
    $('tkUp').onclick = () => navigate(parentOf(cwd));
    $('tkRefresh').onclick = refreshFiles;
    $('tkUploadBtn').onclick = () => $('tkUploadInput').click();
    $('tkUploadInput').onchange = (e) => { uploadFiles(e.target.files); e.target.value = ''; };
    $('tkDlSel').onclick = () => downloadItems(selectedFiles());
    $('tkDelSel').onclick = deleteSelected;
    const list = $('tkFiles');
    list.addEventListener('keydown', onListKey);
    list.addEventListener('click', (ev) => { if (ev.target === list) { selected.clear(); anchor = -1; paintSelection(); } });
    list.addEventListener('dragover', (ev) => { if (!connected || busy) return; ev.preventDefault(); list.classList.add('drop'); });
    list.addEventListener('dragleave', () => list.classList.remove('drop'));
    list.addEventListener('drop', (ev) => { ev.preventDefault(); list.classList.remove('drop'); if (connected && !busy) uploadFiles(ev.dataTransfer.files); });
    $('tkPvClose').onclick = closePreview;
    $('tkPvDl').onclick = () => { if (pvBytes) saveBytes(pvName, pvBytes); };
    $('tkPreview').addEventListener('click', (ev) => { if (ev.target === $('tkPreview')) closePreview(); });
    document.addEventListener('keydown', (ev) => { if (ev.key === 'Escape' && $('tkPreview').style.display === '') closePreview(); });
  }

  // ---- wardriver.cfg editor (form + raw) ----
  // Known scalar keys -> form field spec. Everything in the file is preserved via
  // the raw text (the source of truth); the form patches key=value lines in place.
  const CFG_FIELDS = [
    { k: 'deviceName',        label: 'Device name',           type: 'text' },
    { k: 'board',             label: 'Board',                 type: 'select', opts: ['auto','s3','c5','c6','c3','exp'] },
    { k: 'scanMode',          label: 'Scan mode',             type: 'select', opts: ['aggressive','powersaving'] },
    { k: 'speedUnits',        label: 'Speed units',           type: 'select', opts: ['kmh','mph'] },
    { k: 'scan24',            label: '2.4 GHz scanning',      type: 'bool' },
    { k: 'scan5',             label: '5 GHz scanning (C5)',   type: 'bool' },
    { k: 'bluetoothScan',     label: 'Bluetooth scanning',    type: 'bool' },
    { k: 'homeSsid',          label: 'Home SSID',             type: 'text' },
    { k: 'homePsk',           label: 'Home password',         type: 'text' },
    { k: 'wardriverSsid',     label: 'AP SSID',               type: 'text' },
    { k: 'wardriverPsk',      label: 'AP password',           type: 'text' },
    { k: 'wigleBasicToken',   label: 'WiGLE token',           type: 'text' },
    { k: 'wdgwarsApiKey',     label: 'WDGoWars API key',      type: 'text' },
    { k: 'wardriftApiKey',    label: 'Wardrift API key',      type: 'text' },
    { k: 'meshModeOnBoot',    label: 'Mesh mode on boot',     type: 'select', opts: ['none','core','node'] },
    { k: 'blePersonality',    label: 'Core phone app',        type: 'select', opts: ['biscuit','wardrivego'] },
    { k: 'blePhoneSolo',      label: 'Phone bridge in solo',  type: 'bool' },
    { k: 'uploadOnNetworkSeen', label: 'Upload on home seen', type: 'bool' },
    { k: 'autoStartAfterUpload', label: 'Auto-start after upload', type: 'bool' },
    { k: 'rotateScreen180',   label: 'Rotate screen 180°',    type: 'bool' }
  ];

  let cfgRaw = '';               // whole file text (keeps comments + unknown keys)
  let cfgValid = false;          // false when the card's file isn't readable text

  // A real wardriver.cfg is plain key=value text. Refuse anything else so the
  // editor can never write binary junk back over a config.
  function looksLikeConfig(bytes) {
    if (!bytes.length) return true;
    let printable = 0;
    for (const b of bytes) {
      if (b === 0) return false;
      if ((b >= 32 && b < 127) || b === 9 || b === 10 || b === 13 || b >= 0xC2) printable++;
    }
    if (printable / bytes.length < 0.95) return false;
    return /^\s*[A-Za-z][A-Za-z0-9_]*\s*=/m.test(new TextDecoder().decode(bytes));
  }

  function cfgReadKey(text, key) {
    const m = text.match(new RegExp('^\\s*' + key + '\\s*=\\s*(.*)$', 'm'));
    return m ? m[1].trim() : null;
  }
  function cfgSetKey(text, key, val) {
    const re = new RegExp('^(\\s*' + key + '\\s*=).*$', 'm');
    if (re.test(text)) return text.replace(re, '$1' + val);
    return text.replace(/\s*$/, '') + '\n' + key + '=' + val + '\n';
  }

  function renderCfgForm() {
    const form = $('tkCfgForm');
    form.innerHTML = '';
    for (const f of CFG_FIELDS) {
      const cur = cfgReadKey(cfgRaw, f.k);
      const wrap = document.createElement('label'); wrap.className = 'tkField';
      const lbl = document.createElement('span'); lbl.textContent = f.label;
      let input;
      if (f.type === 'bool') {
        input = document.createElement('input'); input.type = 'checkbox';
        input.checked = (cur === 'true' || cur === '1');
        input.onchange = () => { cfgRaw = cfgSetKey(cfgRaw, f.k, input.checked ? 'true' : 'false'); };
        wrap.classList.add('bool');
      } else if (f.type === 'select') {
        input = document.createElement('select');
        for (const o of f.opts) { const op = document.createElement('option'); op.value = o; op.textContent = o; input.appendChild(op); }
        if (cur != null) input.value = cur;
        input.onchange = () => { cfgRaw = cfgSetKey(cfgRaw, f.k, input.value); };
      } else {
        input = document.createElement('input'); input.type = 'text'; input.value = cur != null ? cur : '';
        input.oninput = () => { cfgRaw = cfgSetKey(cfgRaw, f.k, input.value); };
      }
      wrap.append(lbl, input);
      form.appendChild(wrap);
    }
  }

  async function loadCfg() {
    setBusy(true); $('tkStatus').textContent = 'loading wardriver.cfg\u2026';
    const note = $('tkCfgNote');
    try {
      let bytes;
      try { bytes = await cfgGet(); }
      catch (e) {
        if (!/open/i.test(e.message)) throw e;
        bytes = new Uint8Array(0);           // no file on the card yet: saving creates it
        note.textContent = 'No wardriver.cfg on the card yet \u2014 Save will create one.';
      }
      cfgValid = looksLikeConfig(bytes);
      cfgRaw = cfgValid ? new TextDecoder().decode(bytes) : '';
      $('tkCfgPanel').style.display = '';
      if (!cfgValid) {
        $('tkCfgForm').innerHTML = '';
        note.className = 'tkCfgNote bad';
        note.textContent = 'The wardriver.cfg on the card is not readable text (' + bytes.length +
          ' B of binary data), so it can\u2019t be edited here. Saving is disabled to avoid overwriting it.';
        log('CFG GET: file is not text (' + bytes.length + ' B) \u2014 editing disabled');
        $('tkStatus').textContent = 'wardriver.cfg is not readable text';
        return;
      }
      if (bytes.length) { note.className = 'tkCfgNote'; note.textContent = ''; }
      renderCfgForm();
      $('tkStatus').textContent = 'wardriver.cfg loaded (' + bytes.length + ' B)';
    } catch (e) { log('CFG GET failed: ' + e.message); $('tkStatus').textContent = 'config load failed: ' + e.message; }
    finally { setBusy(false); $('tkCfgSave').disabled = busy || !cfgValid; }
  }

  async function saveCfg() {
    if (!cfgValid) { alert('The config on the card isn\u2019t readable text, so saving is disabled.'); return; }
    const bytes = enc.encode(cfgRaw);
    if (!confirm('Write wardriver.cfg to the SD card and reload it on the device?')) return;
    setBusy(true); $('tkStatus').textContent = 'saving wardriver.cfg…';
    try {
      await cfgPut(bytes);
      $('tkStatus').textContent = 'wardriver.cfg saved + reloaded (' + bytes.length + ' B)';
      log('Saved wardriver.cfg (' + bytes.length + ' B)');
      renderCfgForm();
    } catch (e) { log('CFG PUT failed: ' + e.message); alert('Save failed: ' + e.message); }
    finally { setBusy(false); }
  }

  // ---- wire up ----
  function init() {
    if (!$('tkConnect')) return;
    $('tkConnect').onclick = () => (connected ? disconnect() : connect());
    initBrowser();
    $('tkCfgLoad').onclick = loadCfg;
    $('tkCfgSave').onclick = saveCfg;
    navigator.serial.addEventListener('disconnect', (e) => { if (port && e.target === port) disconnect(); });
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
  else init();
})();
