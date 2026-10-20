// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

/* ais2api page bridge. No credentials, snapshots or response bodies are persisted. */
(() => {
  if (globalThis.__ais2bridge) return;
  const nativeFetch = globalThis.fetch.bind(globalThis);
  const state = { template: null, snapshotCall: null, models: [], transfer: null, discoveryRequest: null };
  const fail = message => { throw new Error(message); };
  function install() {
    const exports = globalThis.default_MakerSuite;
    if (!exports) return false;
    for (const key of Object.keys(exports)) {
      const fn = exports[key];
      if (typeof fn !== 'function' || fn.__ais2wrapped) continue;
      // Discover the exported service adapter by behavior visible in its source,
      // then capture the actual service argument when the page invokes it.
      if (!/\.snapshot\s*\(/.test(Function.prototype.toString.call(fn))) continue;
      function wrapped(...args) {
        const result = Reflect.apply(fn, this, args);
        if (args[0]) {
          state.snapshotCall = hash => Reflect.apply(fn, this, [args[0], hash]);
        }
        return result;
      }
      wrapped.__ais2wrapped = true;
      try { exports[key] = wrapped; } catch (_) { /* immutable export */ }
    }
    return true;
  }
  const installTimer = setInterval(install, 250);
  globalThis.fetch = async function(input, init) {
    const req = new Request(input, init);
    const url = new URL(req.url);
    if (url.hostname === 'alkalimakersuite-pa.clients6.google.com' && /\/ListModels$/.test(url.pathname)) {
      const headers = {};
      req.headers.forEach((value, key) => { headers[key] = value; });
      state.discoveryRequest = { url: req.url, headers };
    }
    if (url.hostname === 'alkalimakersuite-pa.clients6.google.com' && /\/GenerateContent$/.test(url.pathname)) {
      const text = await req.clone().text();
      let body;
      try { body = JSON.parse(text); } catch (_) { return nativeFetch(req); }
      if (Array.isArray(body)) {
        const headers = {};
        req.headers.forEach((v, k) => { headers[k] = v; });
        state.template = { url: req.url, headers, body };
        // Bootstrap only needs the real page-generated request, not its output.
        return new Response('[]', { status: 200, headers: { 'content-type': 'application/json+protobuf' } });
      }
    }
    if (url.hostname === 'alkalimakersuite-pa.clients6.google.com' && /\/(SavePrompt|CreatePrompt)$/.test(url.pathname)) {
      return new Response('[]', { status: 200, headers: { 'content-type': 'application/json+protobuf' } });
    }
    return nativeFetch(req);
  };
  function identity() {
    const url = new URL(location.href);
    const login = url.hostname === 'accounts.google.com' || /\/signin|\/ServiceLogin/.test(url.pathname);
    const candidates = [...document.querySelectorAll('[data-email], [aria-label*="@"], img[alt*="@"]')];
    const emails = new Set();
    for (const el of candidates) {
      const label = [el.getAttribute('data-email'), el.getAttribute('aria-label'), el.getAttribute('alt')].filter(Boolean).join(' ');
      for (const match of label.matchAll(/[A-Z0-9.!#$%&'*+/=?^_`{|}~-]+@[A-Z0-9.-]+\.[A-Z]{2,}/ig)) emails.add(match[0].toLowerCase());
    }
    return { login, regionBlocked: url.pathname.includes('available-regions'), ready: document.readyState === 'complete' && !!document.querySelector('textarea'), email: emails.size === 1 ? [...emails][0] : '', identityAmbiguous: emails.size > 1 };
  }
  function bootstrap() {
    install();
    const input = document.querySelector('textarea');
    if (!input) return false;
    const setter = Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value').set;
    setter.call(input, 'Reply with the single digit 1.');
    input.dispatchEvent(new Event('input', { bubbles: true }));
    return true;
  }
  function run() {
    const button = [...document.querySelectorAll('button')].find(el => !el.disabled && /^(Run|Build)(\s|$)/.test((el.innerText || el.getAttribute('aria-label') || '').trim()));
    if (!button) return false;
    button.click();
    return true;
  }
  async function snapshot(hash) {
    if (!state.snapshotCall || !state.template) fail('browser_incompatible: snapshot service or live template unavailable');
    const value = await state.snapshotCall(hash);
    if (typeof value !== 'string' || !value.length) fail('browser_incompatible: snapshot service returned no ticket');
    return value;
  }
  async function discoverModels() {
    const captured = state.discoveryRequest || state.template;
    if (!captured) fail('browser_incompatible: no live RPC credentials captured');
    const url = new URL(captured.url);
    url.pathname = url.pathname.replace(/\/(GenerateContent|ListModels)$/, '/ListModels');
    if (url.hostname !== 'alkalimakersuite-pa.clients6.google.com' || !/\/ListModels$/.test(url.pathname)) fail('browser_incompatible: invalid discovery endpoint');
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 10000);
    try {
      const response = await nativeFetch(url.href, { method: 'POST', headers: captured.headers, credentials: 'include', body: '[]', signal: controller.signal });
      if (!response.ok) fail('ListModels_http_' + response.status);
      const reader = response.body.getReader();
      const decoder = new TextDecoder();
      let text = '', size = 0;
      try {
        while (true) {
          const { value, done } = await reader.read();
          if (done) break;
          size += value.byteLength;
          if (size > 1024 * 1024) fail('ListModels_response_too_large');
          text += decoder.decode(value, { stream: true });
        }
        text += decoder.decode();
      } finally { await reader.cancel(); }
      const raw = JSON.parse(text);
      if (!Array.isArray(raw) || !Array.isArray(raw[0])) fail('browser_incompatible: unknown ListModels wire shape');
      state.models = raw[0].flatMap(row => {
        if (!Array.isArray(row) || typeof row[0] !== 'string' || !row[0].startsWith('models/')) return [];
        return [{ id: row[0].slice(7), name: row[0], displayName: typeof row[3] === 'string' ? row[3] : row[0].slice(7), description: typeof row[4] === 'string' ? row[4] : '', inputTokenLimit: Number.isFinite(row[5]) ? row[5] : 0, outputTokenLimit: Number.isFinite(row[6]) ? row[6] : 0, supportedGenerationMethods: Array.isArray(row[7]) ? row[7] : [], source: 'live' }];
      });
      return state.models;
    } finally { clearTimeout(timer); }
  }
  function abort() {
    const t = state.transfer;
    if (!t) return;
    t.controller.abort();
    t.pending = null;
    if (t.credit) { t.credit(); t.credit = null; }
  }
  function start(args) {
    abort();
    const chunkBytes = Number.isSafeInteger(args.chunkBytes) && args.chunkBytes > 0 ? Math.min(32768, args.chunkBytes) : 32768;
    const t = { controller: new AbortController(), pending: null, sequence: 0, credit: null, done: false, error: '', errorBody: '', status: 0, retryAfter: 0 };
    state.transfer = t;
    void (async () => {
      let reader;
      try {
        const response = await nativeFetch(state.template.url, { method: 'POST', credentials: 'include', headers: state.template.headers, body: args.body, signal: t.controller.signal });
        t.status = response.status;
        const retry = response.headers.get('retry-after');
        if (retry) {
          const seconds = /^\d+$/.test(retry.trim()) ? Number(retry) : Math.ceil((Date.parse(retry) - Date.now()) / 1000);
          if (Number.isFinite(seconds) && seconds > 0) t.retryAfter = Math.min(86400, seconds);
        }
        if (!response.ok) {
          // Retain at most 8 KiB for protocol classification, never a raw dump.
          if (response.body) {
            try { reader = response.body.getReader({ mode: 'byob' }); } catch (_) {}
            if (reader) {
              const decoder = new TextDecoder();
              let remaining = 8192;
              while (remaining) {
                const { value, done } = await reader.read(new Uint8Array(remaining));
                if (value) { t.errorBody += decoder.decode(value, { stream: true }); remaining -= value.byteLength; }
                if (done) break;
              }
              t.errorBody += decoder.decode();
            } else { try { await response.body.cancel(); } catch (_) {} }
          }
          fail('upstream_http_' + response.status);
        }
        if (!response.body) fail('empty_upstream_body');
        // Require the byte reader: each application read is limited to 32 KiB.
        // Do not fall back to a reader that can allocate an arbitrary chunk.
        try { reader = response.body.getReader({ mode: 'byob' }); }
        catch (_) { fail('browser_incompatible: fetch BYOB reader unavailable'); }
        while (true) {
          const { value, done } = await reader.read(new Uint8Array(chunkBytes));
          if (value && value.byteLength) {
            if (t.controller.signal.aborted) fail('cancelled');
            let binary = '';
            for (const byte of value) binary += String.fromCharCode(byte);
            const sequence = ++t.sequence;
            await new Promise(resolve => { t.credit = resolve; t.pending = { sequence, bytes: btoa(binary) }; });
            if (t.controller.signal.aborted) fail('cancelled');
          }
          if (done) break;
        }
      } catch (error) { t.error = error.name === 'AbortError' ? 'cancelled' : String(error.message || error); }
      finally { if (reader) { try { await reader.cancel(); } catch (_) {} } t.done = true; }
    })();
    return true;
  }
  function ack(sequence) {
    const t = state.transfer;
    if (!t || !t.pending || t.pending.sequence !== sequence) fail('invalid_credit');
    t.pending = null;
    const resolve = t.credit; t.credit = null; resolve();
    return true;
  }
  globalThis.__ais2bridge = {
    identity, bootstrap, run, snapshot, start, ack, abort, discoverModels,
    poll: () => { const t = state.transfer; return t ? { pending: t.pending, done: t.done, error: t.error, errorBody: t.errorBody, status: t.status, retryAfter: t.retryAfter } : null; },
    capability: () => ({ template: state.template ? { body: state.template.body } : null, snapshot: !!state.snapshotCall }),
    models: () => state.models,
    dispose: () => { abort(); clearInterval(installTimer); }
  };
})()
