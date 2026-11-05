// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
//
// The web side of FileStore.h (linked with --pre-js): the autosave in the browser's private file area (OPFS),
// exports handed to the browser (a download, or the share sheet on a tablet: "Save to Files" on an iPad), and files
// picked or dropped on the page. Files travel through Emscripten's in-memory file system (FS) under /tmp/files: the
// C++ side writes or reads them there, then calls in here. Incoming files wait in a queue the main loop drains
// (SculptureFiles.take), so nothing calls back into C++ in the middle of a frame.
var SculptureFiles = (() => {
  const AUTOSAVE = 'autosave.sfproj';
  const queue = [];   // {kind: 'restore' | 'import', path}
  const notices = []; // messages for the top bar (errors, mostly)
  let serial = 0;
  let state = 'idle'; // 'off' (no OPFS here), 'idle', 'writing', 'saved', 'error'
  let savedAt = '';   // local time of the last finished autosave ("15:04")
  let restoring = false;
  let pending = null; // the newest autosave waiting for the write in progress
  let inFlight = null; // the autosave being written the normal way (redone the quick way if the page goes away)
  let writable = null; // that write's stream (dropped then, so it can't land after the quick one)
  let commits = 0;     // autosaves handed over so far
  let worker = null;
  let leaving = false; // the page is going to the background or away: write the quickest way
  let answer = null;   // askText's answer, waiting for the program

  const opfs = () => window.isSecureContext && !!(navigator.storage && navigator.storage.getDirectory);
  const touch = () => (navigator.maxTouchPoints || 0) > 0;
  // Share sheets and file pickers need a recent tap; a button in the app runs a frame later, which usually still
  // counts (navigator.userActivation says). When it doesn't, the overlay asks for one more tap.
  const needsTap = () => navigator.userActivation ? !navigator.userActivation.isActive : touch();
  const note = (text) => { notices.push(String(text)); console.warn('files: ' + text); };

  function mkdirs(path) {
    let at = '';
    for (const part of path.split('/').filter(Boolean)) {
      at += '/' + part;
      try { FS.mkdir(at); } catch (e) { /* exists */ }
    }
  }
  // Copies bytes into the in-memory file system under their own name (the model is named after the file).
  function put(name, bytes) {
    const dir = '/tmp/files/in' + (++serial);
    mkdirs(dir);
    const path = dir + '/' + name.replace(/[\/\\]/g, '_');
    FS.writeFile(path, bytes);
    return path;
  }
  function take(path) {
    const bytes = FS.readFile(path);
    try { FS.unlink(path); } catch (e) { /* gone */ }
    return bytes;
  }
  function clock() {
    const d = new Date();
    return String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0');
  }

  // A small worker that writes with a synchronous access handle: for Safari before 26 (no createWritable on the
  // page), and for a page that is going away (see pump). It writes "<name>.part" and then moves it over the old
  // file, so a write that gets cut off leaves the last good autosave in place.
  function startWorker() {
    if (worker) return;
    const code = `onmessage = async (e) => {
      try {
        const dir = await navigator.storage.getDirectory();
        const h = await dir.getFileHandle(e.data.name + '.part', {create: true});
        const a = await h.createSyncAccessHandle();
        a.truncate(0); a.write(e.data.bytes, {at: 0}); a.flush(); a.close();
        if (h.move) await h.move(e.data.name);
        else {
          const f = await dir.getFileHandle(e.data.name, {create: true});
          const b = await f.createSyncAccessHandle();
          b.truncate(0); b.write(e.data.bytes, {at: 0}); b.flush(); b.close();
        }
        postMessage({ok: true});
      } catch (err) { postMessage({ok: false, error: String(err)}); }
    };`;
    worker = new Worker(URL.createObjectURL(new Blob([code], {type: 'text/javascript'})));
    worker.onmessage = (e) => {
      const w = waiting.shift();
      if (w) e.data.ok ? w.resolve() : w.reject(new Error(e.data.error));
    };
  }
  const waiting = []; // the worker answers in order
  function workerWrite(name, bytes) {
    startWorker();
    return new Promise((resolve, reject) => {
      waiting.push({resolve, reject});
      worker.postMessage({name, bytes}, [bytes.buffer]);
    });
  }
  async function writeOpfs(name, bytes, quick) {
    // A page that is going away is torn down mid-write: the worker's write starts at once and needs no more turns of
    // the page's event loop, so it has the best chance to finish.
    if (quick) return workerWrite(name, bytes);
    const dir = await navigator.storage.getDirectory();
    const h = await dir.getFileHandle(name, {create: true});
    if (h.createWritable) { // (writes to a swap file and swaps it in on close: a cut-off save keeps the old one)
      const w = writable = await h.createWritable();
      try {
        await w.write(bytes);
        await w.close();
      } finally {
        if (writable === w) writable = null;
      }
    } else {
      await workerWrite(name, bytes);
    }
  }
  async function pump() {
    if ((state === 'writing' && !leaving) || !pending) return; // (going away: don't wait for the write under way)
    const bytes = pending, quick = leaving;
    pending = null;
    state = 'writing';
    if (!quick) inFlight = bytes;
    try {
      await writeOpfs(AUTOSAVE, bytes, quick);
      if (!quick) inFlight = null;
      state = 'saved';
      savedAt = clock();
    } catch (e) {
      if (!quick && inFlight !== bytes) { // (dropped for the quick write that took over)
      } else {
        state = 'error';
        note('autosave: ' + e);
      }
    }
    if (pending) pump();
  }

  // A small box over the page with one button that does `go` inside a real tap.
  function overlay(message, okLabel, cancelLabel, go) {
    const old = document.getElementById('files-overlay');
    if (old) old.remove();
    const box = document.createElement('div');
    box.id = 'files-overlay';
    box.style.cssText = 'position:fixed;inset:0;display:flex;align-items:center;justify-content:center;' +
      'background:rgba(0,0,0,0.45);z-index:10;touch-action:manipulation;';
    const card = document.createElement('div');
    card.style.cssText = 'background:#33363c;color:#e6e8ec;border-radius:14px;padding:20px 22px;min-width:260px;' +
      'max-width:80vw;font:17px system-ui,sans-serif;box-shadow:0 8px 30px rgba(0,0,0,0.5);text-align:center;';
    const text = document.createElement('div');
    text.textContent = message;
    text.style.cssText = 'margin-bottom:16px;word-break:break-all;';
    const row = document.createElement('div');
    row.style.cssText = 'display:flex;gap:12px;justify-content:center;';
    const button = (label, primary, act) => {
      const b = document.createElement('button');
      b.textContent = label;
      b.style.cssText = 'font:inherit;padding:10px 22px;border:0;border-radius:10px;color:#fff;' +
        (primary ? 'background:#408ce6;' : 'background:#4a4e56;');
      b.addEventListener('click', (e) => { e.stopPropagation(); box.remove(); act(); });
      row.appendChild(b);
    };
    button(cancelLabel, false, () => {});
    button(okLabel, true, go);
    card.appendChild(text);
    card.appendChild(row);
    box.appendChild(card);
    box.addEventListener('pointerdown', (e) => e.stopPropagation());
    document.body.appendChild(box);
  }

  function mimeOf(name) {
    const ext = name.toLowerCase().split('.').pop();
    return {glb: 'model/gltf-binary', obj: 'model/obj', stl: 'model/stl'}[ext] || 'application/octet-stream';
  }
  function download(file) {
    const url = URL.createObjectURL(file);
    const a = document.createElement('a');
    a.href = url;
    a.download = file.name;
    a.style.display = 'none';
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 60000);
  }

  async function readInto(file, kind) {
    try {
      const bytes = new Uint8Array(await file.arrayBuffer());
      queue.push({kind, path: put(file.name || 'model', bytes)});
    } catch (e) {
      note('read ' + file.name + ': ' + e);
    }
  }

  return {
    // Once, when the program starts: drag and drop, saving when the page goes to the background, and (unless the
    // start options say otherwise) reading back the last autosave.
    init(restore) {
      this.watchFullscreen();
      if (!opfs()) state = 'off';
      else {
        if (navigator.storage.persist) navigator.storage.persist().catch(() => {}); // (asks the browser to keep it)
        startWorker(); // (ready for a quick save when the page goes away)
      }
      const hidden = () => {
        leaving = true;
        setTimeout(() => { leaving = false; }, 0); // (back to the safer write once the page runs again)
        const f = typeof _sculpture_page_hidden === 'function' ? _sculpture_page_hidden : Module['_sculpture_page_hidden'];
        // A normal write still under way is dropped (a stream closing late would land over the quick write)...
        const unfinished = inFlight;
        inFlight = null;
        if (writable) writable.abort().catch(() => {});
        const before = commits;
        if (f) f(); // (a changed model comes back through commit(), written the quick way)
        // ...and when nothing newer came, its bytes go the quick way.
        if (commits === before && unfinished && unfinished.byteLength > 0) {
          pending = unfinished.slice();
          pump();
        }
      };
      document.addEventListener('visibilitychange', () => { if (document.visibilityState === 'hidden') hidden(); });
      window.addEventListener('pagehide', hidden);
      window.addEventListener('dragover', (e) => e.preventDefault());
      window.addEventListener('drop', (e) => {
        e.preventDefault();
        const f = e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files[0];
        if (f) readInto(f, 'import');
      });
      if (restore) this.restore();
    },
    async restore() {
      if (!opfs() || restoring) return;
      restoring = true;
      try {
        const dir = await navigator.storage.getDirectory();
        const h = await dir.getFileHandle(AUTOSAVE);
        const file = await h.getFile();
        if (file.size > 0) queue.push({kind: 'restore', path: put(AUTOSAVE, new Uint8Array(await file.arrayBuffer()))});
      } catch (e) {
        if (e.name !== 'NotFoundError') note('restore: ' + e);
      }
      restoring = false;
    },
    restoring() { return restoring; },
    // "kind\npath" of the next incoming file, or null.
    take() {
      const next = queue.shift();
      return next ? next.kind + '\n' + next.path : null;
    },
    notice() { return notices.length ? notices.shift() : null; },
    available() { return opfs(); },
    state() { return state; },
    savedAt() { return savedAt; },
    // The C++ side wrote a project at `path`: move it into OPFS (the newest one wins if a write is under way).
    commit(path) {
      commits++;
      try {
        pending = take(path);
      } catch (e) {
        note('autosave: ' + e);
        return;
      }
      if (!opfs()) return;
      pump();
    },
    // An export at `path`: hand it to the browser as `name`.
    deliver(path, name, okLabel, cancelLabel) {
      let bytes;
      try {
        bytes = take(path);
      } catch (e) {
        note('export: ' + e);
        return;
      }
      const file = new File([bytes], name, {type: mimeOf(name)});
      const share = touch() && !!navigator.canShare && navigator.canShare({files: [file]});
      const go = () => {
        if (!share) return download(file);
        navigator.share({files: [file]}).catch((e) => { if (e.name !== 'AbortError') download(file); });
      };
      if (needsTap()) overlay(name, okLabel, cancelLabel, go);
      else go();
    },
    // Opens the browser's file picker; the chosen file arrives through take().
    pick(message, okLabel, cancelLabel) {
      const old = document.getElementById('files-input');
      if (old) old.remove();
      const input = document.createElement('input');
      input.type = 'file';
      input.id = 'files-input';
      input.style.display = 'none';
      // (iPadOS greys out files whose extension it doesn't know, such as .sfproj: no filter on touch devices.)
      if (!touch()) input.accept = '.sfproj,.glb,.gltf,.obj,.stl';
      input.addEventListener('change', () => {
        const f = input.files && input.files[0];
        if (f) readInto(f, 'import');
        input.remove();
      });
      document.body.appendChild(input);
      // iPadOS opens a file picker only from inside the tap itself: a frame later it ignores input.click() without a
      // word, even while navigator.userActivation still says the tap counts. Touch devices always get
      // the one-tap box.
      if (touch() || needsTap()) overlay(message, okLabel, cancelLabel, () => input.click());
      else input.click();
    },
    // Full screen (the Fullscreen API; iPadOS has it with the webkit prefix). Entering needs the tap itself on a
    // tablet, as the file picker does: touch devices get the one-tap box.
    runningAsApp() { return window.matchMedia('(display-mode: standalone)').matches || navigator.standalone === true; },
    fullscreenAvailable() {
      const d = document.documentElement;
      return !this.runningAsApp() && !!(d.requestFullscreen || d.webkitRequestFullscreen);
    },
    isFullscreen() { return !!(document.fullscreenElement || document.webkitFullscreenElement); },
    // After entering or leaving full screen the page's size settles over a moment (iPad): tell the program a few
    // times that the window changed, so the canvas follows (main.cpp leaves full screen changes to resizes).
    watchFullscreen() {
      const nudge = () => [50, 300, 800].forEach((ms) => setTimeout(() => window.dispatchEvent(new Event('resize')), ms));
      document.addEventListener('fullscreenchange', nudge);
      document.addEventListener('webkitfullscreenchange', nudge);
    },
    toggleFullscreen(message, okLabel, cancelLabel) {
      if (this.isFullscreen()) {
        const exit = document.exitFullscreen || document.webkitExitFullscreen;
        if (exit) Promise.resolve(exit.call(document)).catch((e) => note('full screen: ' + e));
        return;
      }
      const go = () => {
        const d = document.documentElement;
        try {
          const r = d.requestFullscreen ? d.requestFullscreen({navigationUI: 'hide'}) : d.webkitRequestFullscreen();
          if (r && r.catch) r.catch((e) => note('full screen: ' + e.message));
        } catch (e) {
          note('full screen: ' + e);
        }
      };
      if (touch() || needsTap()) overlay(message, okLabel, cancelLabel, go);
      else go();
    },
    // A name for a layer or a Part: a box with a text field (tapping it brings up the tablet's keyboard). The
    // answer waits in `answer` until the program takes it: "1\n" + the text, or "0\n" when cancelled.
    askText(title, current, okLabel, cancelLabel) {
      const old = document.getElementById('files-overlay');
      if (old) old.remove();
      answer = null;
      const box = document.createElement('div');
      box.id = 'files-overlay';
      box.style.cssText = 'position:fixed;inset:0;display:flex;align-items:flex-start;justify-content:center;' +
        'padding-top:12vh;background:rgba(0,0,0,0.45);z-index:10;touch-action:manipulation;';
      const card = document.createElement('div');
      card.style.cssText = 'background:#33363c;color:#e6e8ec;border-radius:14px;padding:20px 22px;min-width:300px;' +
        'max-width:80vw;font:17px system-ui,sans-serif;box-shadow:0 8px 30px rgba(0,0,0,0.5);text-align:center;';
      const text = document.createElement('div');
      text.textContent = title;
      text.style.cssText = 'margin-bottom:12px;';
      const input = document.createElement('input');
      input.type = 'text';
      input.value = current;
      input.maxLength = 60;
      input.style.cssText = 'font:inherit;width:100%;box-sizing:border-box;padding:10px 12px;margin-bottom:16px;' +
        'border:1px solid #5a5e66;border-radius:8px;background:#24262a;color:#fff;';
      const row = document.createElement('div');
      row.style.cssText = 'display:flex;gap:12px;justify-content:center;';
      const finish = (ok) => {
        box.remove();
        answer = ok ? '1\n' + input.value.trim() : '0\n';
      };
      const button = (label, primary, ok) => {
        const b = document.createElement('button');
        b.textContent = label;
        b.style.cssText = 'font:inherit;padding:10px 22px;border:0;border-radius:10px;color:#fff;' +
          (primary ? 'background:#408ce6;' : 'background:#4a4e56;');
        b.addEventListener('click', (e) => { e.stopPropagation(); finish(ok); });
        row.appendChild(b);
      };
      button(cancelLabel, false, false);
      button(okLabel, true, true);
      input.addEventListener('keydown', (e) => {
        e.stopPropagation(); // (the program's keyboard shortcuts stay out of the field)
        if (e.key === 'Enter') finish(true);
        if (e.key === 'Escape') finish(false);
      });
      input.addEventListener('keyup', (e) => e.stopPropagation());
      card.appendChild(text);
      card.appendChild(input);
      card.appendChild(row);
      box.appendChild(card);
      box.addEventListener('pointerdown', (e) => e.stopPropagation());
      document.body.appendChild(box);
      if (!touch()) { // (on a tablet the keyboard only comes up for a tap on the field itself)
        input.focus();
        input.select();
      }
    },
    takeAnswer() {
      const a = answer;
      answer = null;
      return a;
    },
    remove(path) {
      try { FS.unlink(path); } catch (e) { /* gone */ }
      try { FS.rmdir(path.substring(0, path.lastIndexOf('/'))); } catch (e) { /* not empty, or not ours */ }
    },
  };
})();
