// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Optional offline browser regression: AIS2API_TEST_BROWSER=/path/to/chrome
// node browser/test_cookie_runtime.cjs (Node with built-in WebSocket required).
// This tests CDP cookie attributes and the real page bridge, not the C++
// header converter. C++ conversion is covered by test_cookie_import.cpp.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawn, execFile } = require('node:child_process');

const timeoutMs = 10000;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
function deadline(promise, label, ms = timeoutMs) {
  let timer;
  return Promise.race([promise, new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error(`${label} timed out`)), ms);
  })]).finally(() => clearTimeout(timer));
}

class CDP {
  constructor(url) {
    this.socket = new WebSocket(url);
    this.pending = new Map();
    this.handlers = new Map();
    this.nextId = 0;
    this.closed = false;
    this.opened = new Promise((resolve, reject) => {
      this.socket.addEventListener('open', resolve, { once: true });
      this.socket.addEventListener('error', () => reject(new Error('CDP connection failed')), { once: true });
      this.socket.addEventListener('close', () => reject(new Error('CDP closed before opening')), { once: true });
    });
    this.socket.addEventListener('message', event => {
      let message;
      try { message = JSON.parse(String(event.data)); }
      catch (_) { this.fail(new Error('Invalid CDP response')); return; }
      if (message.id) {
        const request = this.pending.get(message.id);
        if (!request) return;
        this.pending.delete(message.id);
        clearTimeout(request.timer);
        if (message.error) request.reject(new Error(`CDP ${request.method} failed`));
        else request.resolve(message.result || {});
      } else {
        for (const handler of this.handlers.get(message.method) || []) handler(message);
      }
    });
    this.socket.addEventListener('close', () => this.fail(new Error('CDP socket closed')));
    this.socket.addEventListener('error', () => this.fail(new Error('CDP socket failed')));
  }
  fail(error) {
    this.closed = true;
    for (const request of this.pending.values()) {
      clearTimeout(request.timer);
      request.reject(error);
    }
    this.pending.clear();
  }
  on(method, handler) {
    if (!this.handlers.has(method)) this.handlers.set(method, []);
    this.handlers.get(method).push(handler);
  }
  call(method, params = {}, sessionId) {
    if (this.closed) return Promise.reject(new Error('CDP is closed'));
    return new Promise((resolve, reject) => {
      const id = ++this.nextId;
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`CDP ${method} timed out`));
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, timer, method });
      try { this.socket.send(JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) })); }
      catch (_) {
        clearTimeout(timer);
        this.pending.delete(id);
        reject(new Error(`CDP ${method} send failed`));
      }
    });
  }
  close() { this.fail(new Error('CDP test finished')); this.socket.close(); }
}

async function main() {
  const executable = process.env.AIS2API_TEST_BROWSER;
  if (!executable) throw new Error('Set AIS2API_TEST_BROWSER to an existing CloakBrowser/Chromium executable');
  if (typeof WebSocket !== 'function') throw new Error('Use Node with a built-in WebSocket implementation');
  assert.ok(fs.statSync(executable).isFile(), 'Browser executable must be a file');
  const temporaryRoot = fs.realpathSync(os.tmpdir());
  const profile = fs.mkdtempSync(path.join(temporaryRoot, 'ais2api-cookie-runtime-'));
  let child, cdp, exited = false, interceptionError;
  try {
    let resolveEndpoint, rejectEndpoint;
    const endpoint = new Promise((resolve, reject) => { resolveEndpoint = resolve; rejectEndpoint = reject; });
    child = spawn(executable, [
      `--user-data-dir=${profile}`, '--headless=new', '--remote-debugging-address=127.0.0.1',
      '--remote-debugging-port=0', '--host-resolver-rules=MAP * ~NOTFOUND',
      '--disable-background-networking', '--disable-component-update', '--disable-sync',
      '--disable-default-apps', '--no-first-run', '--no-default-browser-check',
      '--proxy-server=direct://', '--disable-quic', '--disable-extensions', 'about:blank'
    ], { stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true });
    let output = '';
    const capture = chunk => {
      output = (output + chunk.toString()).slice(-16384);
      const match = output.match(/DevTools listening on (ws:\/\/127\.0\.0\.1:\d+\/devtools\/browser\/[^\s]+)/);
      if (match) resolveEndpoint(match[1]);
    };
    child.stdout.on('data', capture);
    child.stderr.on('data', capture);
    child.once('error', () => rejectEndpoint(new Error('Browser could not start')));
    child.once('exit', () => { exited = true; rejectEndpoint(new Error('Browser exited before CDP became ready')); });
    cdp = new CDP(await deadline(endpoint, 'Browser startup', 30000));
    await deadline(cdp.opened, 'CDP connection');
    const { browserContextId } = await cdp.call('Target.createBrowserContext', { disposeOnDetach: true });
    const { targetId } = await cdp.call('Target.createTarget', { url: 'about:blank', browserContextId });
    const { sessionId } = await cdp.call('Target.attachToTarget', { targetId, flatten: true });
    const call = (method, params) => cdp.call(method, params, sessionId);
    await call('Page.enable');
    await call('Runtime.enable');
    const html = '<!doctype html><meta charset="utf-8"><title>Offline cookie regression</title>' +
      '<a href="https://accounts.google.com/SignOutOptions" aria-label="Google Account: Synthetic (cookie-runtime@example.test)">Synthetic account</a>';
    cdp.on('Fetch.requestPaused', message => {
      if (message.sessionId !== sessionId) return;
      const { requestId, request } = message.params;
      // Fulfill only our synthetic document; fail every other request locally.
      const operation = request.url === 'https://aistudio.google.com/'
        ? call('Fetch.fulfillRequest', { requestId, responseCode: 200,
          responseHeaders: [{ name: 'Content-Type', value: 'text/html; charset=utf-8' }],
          body: Buffer.from(html).toString('base64') })
        : call('Fetch.failRequest', { requestId, errorReason: 'BlockedByClient' });
      operation.catch(error => { interceptionError = error; });
    });
    await call('Fetch.enable', { patterns: [{ urlPattern: '*', requestStage: 'Request' }] });
    await call('Page.addScriptToEvaluateOnNewDocument', { source: fs.readFileSync(path.join(__dirname, 'bridge.js'), 'utf8') });
    // Synthetic, non-secret values matching the header import correction.
    await cdp.call('Storage.setCookies', { browserContextId, cookies: [
      ...['SID', 'APISID', 'SAPISID', '__Secure-1PAPISID', '__Secure-3PAPISID'].map(name =>
        ({ name, value: `synthetic-${name}`, domain: '.google.com', path: '/', secure: true, httpOnly: false })),
      { name: 'HSID', value: 'synthetic-httponly', domain: '.google.com', path: '/', secure: true, httpOnly: true }
    ] });
    const navigation = await call('Page.navigate', { url: 'https://aistudio.google.com/' });
    assert.ok(!navigation.errorText, 'Synthetic navigation must succeed');
    let result;
    const until = Date.now() + timeoutMs;
    while (Date.now() < until) {
      if (interceptionError) throw interceptionError;
      const evaluated = await call('Runtime.evaluate', {
        expression: 'document.readyState === "complete" && !!globalThis.__ais2bridge ? ({identity: globalThis.__ais2bridge.identity(), cookies: document.cookie, hasEditor: !!document.querySelector("textarea")}) : null',
        returnByValue: true
      });
      if (evaluated.exceptionDetails) throw new Error('Synthetic page evaluation failed');
      result = evaluated.result.value;
      if (result) break;
      await pause(50);
    }
    assert.ok(result, 'Synthetic document and bridge must finish loading');
    assert.equal(result.hasEditor, false);
    assert.equal(result.identity.identityReady, true);
    assert.equal(result.identity.ready, false);
    assert.equal(result.identity.email, 'cookie-runtime@example.test');
    assert.equal(result.identity.identityAmbiguous, false);
    assert.equal(result.identity.login, false);
    const readable = new Map(result.cookies.split(';').map(pair => {
      const index = pair.indexOf('=');
      return [pair.slice(0, index).trim(), pair.slice(index + 1)];
    }));
    for (const name of ['SID', 'APISID', 'SAPISID', '__Secure-1PAPISID', '__Secure-3PAPISID'])
      assert.equal(readable.get(name), `synthetic-${name}`, `${name} must remain JS readable`);
    assert.equal(readable.has('HSID'), false, 'HttpOnly HSID must remain hidden from JS');
    const stored = await cdp.call('Storage.getCookies', { browserContextId });
    assert.equal(stored.cookies.find(cookie => cookie.name === 'HSID').httpOnly, true);
    assert.equal(stored.cookies.find(cookie => cookie.name === 'SAPISID').httpOnly, false);
    // Native authorization must see rotated HttpOnly signing cookies through
    // the selected page session, without reading another account's store.
    await cdp.call('Storage.setCookies', { browserContextId, cookies: [
      { name: 'SAPISID', value: 'synthetic-rotated-httponly', domain: '.google.com', path: '/', secure: true, httpOnly: true }
    ] });
    const other = await cdp.call('Target.createBrowserContext', { disposeOnDetach: true });
    await cdp.call('Storage.setCookies', { browserContextId: other.browserContextId, cookies: [
      { name: 'SAPISID', value: 'synthetic-other-account', domain: '.google.com', path: '/', secure: true, httpOnly: true }
    ] });
    const signing = await call('Network.getCookies', { urls: ['https://aistudio.google.com/'] });
    const signingCookie = signing.cookies.find(cookie => cookie.name === 'SAPISID');
    assert.equal(signingCookie.value, 'synthetic-rotated-httponly');
    assert.equal(signingCookie.httpOnly, true);
    const pageCookies = await call('Runtime.evaluate', { expression: 'document.cookie', returnByValue: true });
    assert.equal(pageCookies.result.value.includes('SAPISID='), false);
    await cdp.call('Target.disposeBrowserContext', { browserContextId: other.browserContextId });
    if (interceptionError) throw interceptionError;
    await cdp.call('Target.disposeBrowserContext', { browserContextId });
    console.log('PASS: offline browser cookie attributes, identity and isolated HttpOnly signing-cookie refresh');
  } finally {
    if (cdp) {
      try { await cdp.call('Browser.close'); } catch (_) { /* Browser.close may close its own socket. */ }
      cdp.close();
    }
    if (child && child.pid && !exited) {
      if (process.platform === 'win32') {
        await new Promise(resolve => execFile('taskkill.exe', ['/PID', String(child.pid), '/T', '/F'],
          { windowsHide: true, timeout: 5000, maxBuffer: 4096 }, () => resolve()));
      } else child.kill('SIGKILL');
      await deadline(new Promise(resolve => { if (exited) resolve(); else child.once('exit', resolve); }), 'Browser cleanup', 5000).catch(() => {});
    }
    // Delete only this invocation's mkdtemp directory after checking its parent.
    assert.equal(path.dirname(profile), temporaryRoot);
    assert.ok(path.basename(profile).startsWith('ais2api-cookie-runtime-'));
    fs.rmSync(profile, { recursive: true, force: true, maxRetries: 10, retryDelay: 100 });
  }
}

main().catch(error => { console.error(`FAIL: ${error.message}`); process.exitCode = 1; });
