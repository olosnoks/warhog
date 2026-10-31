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
  let port = null, writer = null, reader = null, readLoopRunning = false;
  let lineHandler = null;         // current op's line handler, or null
  let buf = '';                   // text accumulator for line framing

  function log(msg) {
    const el = $('tkLog');
    if (!el) return;
    el.textContent += msg + '\n';
    el.scrollTop = el.scrollHeight;
  }

  async function readLoop() {
    const dec = new TextDecoder();
    readLoopRunning = true;
    try {
      while (port && port.readable) {
        reader = port.readable.getReader();
        try {
          while (true) {
            const { value, done } = await reader.read();
            if (done) break;
            buf += dec.decode(value, { stream: true });
            let nl;
            while ((nl = buf.indexOf('\n')) >= 0) {
              let line = buf.slice(0, nl).replace(/\r$/, '');
              buf = buf.slice(nl + 1);
              if (line.startsWith('WT ') || line === 'WT') {
                if (lineHandler) lineHandler(line);
              }
              // non-WT lines are firmware debug output -> ignore
            }
          }
        } catch (e) { /* reader cancelled on disconnect */ }
        finally { try { reader.releaseLock(); } catch (e) {} reader = null; }
      }
    } finally { readLoopRunning = false; }
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
  async function hello() {
    return op(() => writeLine('WT HELLO'), (done, fail) => (line) => {
      if (line.startsWith('WT HELLO')) done(line.slice(3).trim());
    }, 5000);
  }
  async function bye() { try { await writeLine('WT BYE'); } catch (e) {} }

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
  function setBusy(b) { busy = b; document.querySelectorAll('#tkPanel button, #tkPanel select, #tkPanel input').forEach(e => { e.disabled = b; }); }

  async function connect() {
    try {
      port = await navigator.serial.requestPort();
      await port.open({ baudRate: 115200 });   // native USB-CDC ignores baud
      writer = port.writable.getWriter();
      readLoop();
      const info = await hello();
      connected = true;
      $('tkConnect').textContent = 'Disconnect';
      $('tkStatus').textContent = 'connected · ' + info;
      $('tkStatus').className = 'tkStat ok';
      $('tkPanel').style.display = '';
      log('Connected: ' + info);
      await refreshFiles();
    } catch (e) {
      log('Connect failed: ' + e.message);
      $('tkStatus').textContent = e.message.includes('No port') ? 'no port selected' : ('error: ' + e.message);
      $('tkStatus').className = 'tkStat bad';
      await disconnect();
    }
  }

  async function disconnect() {
    try { if (connected) await bye(); } catch (e) {}
    connected = false;
    try { if (reader) await reader.cancel(); } catch (e) {}
    try { if (writer) { writer.releaseLock(); } } catch (e) {}
    writer = null;
    try { if (port) await port.close(); } catch (e) {}
    port = null; lineHandler = null; buf = '';
    $('tkConnect').textContent = 'Connect over USB';
    $('tkStatus').textContent = 'disconnected';
    $('tkStatus').className = 'tkStat';
    $('tkPanel').style.display = 'none';
  }

  // ---- file explorer UI ----
  const FOLDERS = ['/logs', '/uploaded', '/archived', '/'];

  async function refreshFiles() {
    if (!connected) return;
    const dir = $('tkFolder').value;
    const list = $('tkFiles');
    list.innerHTML = '<div class="muted">Listing ' + dir + '…</div>';
    setBusy(true);
    try {
      const { files } = await ls(dir);
      files.sort((a, b) => a.path.localeCompare(b.path));
      if (!files.length) { list.innerHTML = '<div class="muted">No files in ' + dir + '.</div>'; return; }
      list.innerHTML = '';
      for (const f of files) {
        const row = document.createElement('div');
        row.className = 'tkFile';
        const name = document.createElement('span'); name.className = 'tkName'; name.textContent = f.path;
        const size = document.createElement('span'); size.className = 'tkSize'; size.textContent = fmtSize(f.size);
        const dl = document.createElement('button'); dl.className = 'tkMini'; dl.textContent = 'Download';
        dl.onclick = () => downloadFile(f);
        const del = document.createElement('button'); del.className = 'tkMini danger'; del.textContent = 'Delete';
        del.onclick = () => deleteFile(f);
        row.append(name, size, dl, del);
        list.appendChild(row);
      }
    } catch (e) { list.innerHTML = '<div class="tkStat bad">Error: ' + e.message + '</div>'; }
    finally { setBusy(false); }
  }

  async function downloadFile(f) {
    setBusy(true); log('GET ' + f.path + ' …');
    try {
      const bytes = await get(f.path, (r, t) => { $('tkStatus').textContent = 'downloading ' + fmtSize(r) + ' / ' + fmtSize(t); });
      const blob = new Blob([bytes], { type: 'application/octet-stream' });
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      a.download = f.path.split('/').pop();
      a.click(); URL.revokeObjectURL(a.href);
      $('tkStatus').textContent = 'downloaded ' + a.download + ' (' + fmtSize(bytes.length) + ')';
      log('Saved ' + a.download + ' (' + bytes.length + ' B)');
    } catch (e) { log('GET failed: ' + e.message); $('tkStatus').textContent = 'download failed: ' + e.message; }
    finally { setBusy(false); }
  }

  async function deleteFile(f) {
    if (!confirm('Delete ' + f.path + ' from the SD card? This cannot be undone.')) return;
    setBusy(true);
    try { await rm(f.path); log('Deleted ' + f.path); await refreshFiles(); }
    catch (e) { log('RM failed: ' + e.message); alert('Delete failed: ' + e.message); setBusy(false); }
  }

  async function uploadFiles(fileList) {
    if (!fileList || !fileList.length) return;
    const folder = $('tkUploadFolder').value;
    setBusy(true);
    try {
      for (const file of fileList) {
        const bytes = new Uint8Array(await file.arrayBuffer());
        const path = (folder === '/' ? '' : folder) + '/' + file.name;
        log('PUT ' + path + ' (' + bytes.length + ' B) …');
        await put(path, bytes, (s, t) => { $('tkStatus').textContent = 'uploading ' + file.name + ' ' + fmtSize(s) + '/' + fmtSize(t); });
        log('Uploaded ' + path);
      }
      $('tkStatus').textContent = 'upload complete';
      await refreshFiles();
    } catch (e) { log('PUT failed: ' + e.message); alert('Upload failed: ' + e.message); setBusy(false); }
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

  let cfgRaw = '';

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
        input.onchange = () => { cfgRaw = cfgSetKey(cfgRaw, f.k, input.checked ? 'true' : 'false'); $('tkCfgRaw').value = cfgRaw; };
        wrap.classList.add('bool');
      } else if (f.type === 'select') {
        input = document.createElement('select');
        for (const o of f.opts) { const op = document.createElement('option'); op.value = o; op.textContent = o; input.appendChild(op); }
        if (cur != null) input.value = cur;
        input.onchange = () => { cfgRaw = cfgSetKey(cfgRaw, f.k, input.value); $('tkCfgRaw').value = cfgRaw; };
      } else {
        input = document.createElement('input'); input.type = 'text'; input.value = cur != null ? cur : '';
        input.oninput = () => { cfgRaw = cfgSetKey(cfgRaw, f.k, input.value); $('tkCfgRaw').value = cfgRaw; };
      }
      wrap.append(lbl, input);
      form.appendChild(wrap);
    }
  }

  async function loadCfg() {
    setBusy(true); $('tkStatus').textContent = 'loading wardriver.cfg…';
    try {
      const bytes = await cfgGet();
      cfgRaw = new TextDecoder().decode(bytes);
      $('tkCfgRaw').value = cfgRaw;
      renderCfgForm();
      $('tkCfgPanel').style.display = '';
      $('tkStatus').textContent = 'wardriver.cfg loaded (' + bytes.length + ' B)';
    } catch (e) { log('CFG GET failed: ' + e.message); $('tkStatus').textContent = 'config load failed: ' + e.message; }
    finally { setBusy(false); }
  }

  async function saveCfg() {
    // raw textarea is authoritative on save
    cfgRaw = $('tkCfgRaw').value;
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
    $('tkRefresh').onclick = refreshFiles;
    $('tkFolder').onchange = refreshFiles;
    $('tkUploadBtn').onclick = () => $('tkUploadInput').click();
    $('tkUploadInput').onchange = (e) => { uploadFiles(e.target.files); e.target.value = ''; };
    $('tkCfgLoad').onclick = loadCfg;
    $('tkCfgSave').onclick = saveCfg;
    $('tkCfgTabForm').onclick = () => { $('tkCfgForm').style.display = ''; $('tkCfgRawWrap').style.display = 'none'; $('tkCfgTabForm').classList.add('on'); $('tkCfgTabRaw').classList.remove('on'); renderCfgForm(); };
    $('tkCfgTabRaw').onclick  = () => { cfgRaw = $('tkCfgRaw').value; $('tkCfgForm').style.display = 'none'; $('tkCfgRawWrap').style.display = ''; $('tkCfgTabRaw').classList.add('on'); $('tkCfgTabForm').classList.remove('on'); };
    navigator.serial.addEventListener('disconnect', (e) => { if (port && e.target === port) disconnect(); });
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
  else init();
})();
