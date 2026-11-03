// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
// --pre-js for programs run under Node.js (the upstream unit tests): mounts the repository at /repo, so the
// tests' data folder (SF_TEST_DATA_DIR = /repo/tests/data) is the real one. The build lives in
// <repo>/build/<name>/, two folders down; SCULPTURE_REPO overrides that.
// Some tests build paths from __FILE__ ("E:/AI/SculptureABC/tests/..."): with no leading slash that is relative
// to the working directory "/", so the repository is mounted at "/E:/AI/SculptureABC" too.
Module.preRun = Module.preRun || [];
Module.preRun.push(() => {
  const root = process.env.SCULPTURE_REPO || require('path').resolve(__dirname, '..', '..');
  FS.mkdir('/repo');
  FS.mount(NODEFS, { root }, '/repo');
  const asFile = '/' + root.split(String.fromCharCode(92)).join('/'); // backslash: no escapes in a pre-js
  FS.mkdirTree(asFile);
  FS.mount(NODEFS, { root }, asFile);
});
