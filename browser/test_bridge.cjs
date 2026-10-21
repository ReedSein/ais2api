// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Development-only tests. Node is not a production runtime dependency.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

function harness(options = {}) {
  let fetches = 0, reads = 0, sourceOffset = 0;
  class FakeXHR extends EventTarget {
    open(method, url) { this.method = method; this.url = url; }
    dispatchEvent(event) { super.dispatchEvent(event); this['on' + event.type]?.(event); return true; }
    abort() { this.aborted = true; }
    setRequestHeader() {}
    send() { this.sent = true; }
  }
  const errorBytes = options.errorBody === undefined ? null : new TextEncoder().encode(options.errorBody);
  const length = errorBytes ? errorBytes.length : 70000;
  const context = vm.createContext({
    Request, Response, URL, TextDecoder, TextEncoder, Blob, DOMException, Event, Uint8Array, AbortController, btoa, XMLHttpRequest: FakeXHR,
    setInterval: callback => { context.install = callback; return 1; }, clearInterval() {},
    setTimeout: (fn, ms) => setTimeout(fn, options.fastDiscoveryWait && ms === 100 ? 0 : options.fastDiscoveryTimeout && ms === 10000 ? 5 : ms), clearTimeout,
    location: { href: 'https://aistudio.google.com/prompts/new_chat?authuser=0', origin: 'https://aistudio.google.com', hostname: 'aistudio.google.com' },
    document: { readyState: 'complete', querySelector: () => ({}), querySelectorAll: () => [] },
    default_MakerSuite: options.exports || { adapter(service, hash) { return service.snapshot(hash); } },
    fetch: async (input, init) => {
      ++fetches;
      if (options.observeFetch) options.observeFetch(input, init);
      const url = typeof input === 'string' ? input : input.url;
      if (url.includes('/ListModels')) {
        if (options.discoveryFetch) return options.discoveryFetch(input, init);
        return new Response(JSON.stringify([[['models/live-model', null, null, 'Live Model', 'Returned by RPC', 123, 45, ['generateContent']]]]));
      }
      return new Response(new ReadableStream({
        type: 'bytes',
        pull(controller) {
          ++reads;
          if (sourceOffset === length) { controller.close(); if (controller.byobRequest) controller.byobRequest.respond(0); return; }
          const target = controller.byobRequest.view;
          const size = Math.min(target.byteLength, length - sourceOffset);
          for (let i = 0; i < size; ++i) target[i] = errorBytes ? errorBytes[sourceOffset + i] : (sourceOffset + i) % 251;
          sourceOffset += size;
          controller.byobRequest.respond(size);
        }
      }), { status: options.httpStatus || 200, headers: options.retryAfter ? { 'retry-after': options.retryAfter } : {} });
    }
  });
  vm.runInContext(fs.readFileSync(__dirname + '/bridge.js', 'utf8'), context, { filename: 'bridge.js' });
  context.install();
  const bridge = context.__ais2bridge;
  async function capture() {
    const response = await context.fetch('https://alkalimakersuite-pa.clients6.google.com/$rpc/google.internal.alkali.applications.makersuite.v1.MakerSuiteService/GenerateContent', { method: 'POST', headers: { 'content-type': 'application/json+protobuf', 'x-goog-api-key': 'test-key-never-real' }, body: '["real-captured-template",null,null,null,"ticket:bootstrap"]' });
    assert.equal(await response.text(), '[]');
    await context.default_MakerSuite.adapter({ snapshot: hash => Promise.resolve(hash === 'a'.repeat(64) ? 'ticket:bootstrap' : 'ticket:' + hash) }, 'a'.repeat(64));

  }
  return { context, bridge, capture, fetches: () => fetches, reads: () => reads };
}
const tick = () => new Promise(resolve => setTimeout(resolve, 5));
const modelsUrl = 'https://alkalimakersuite-pa.clients6.google.com/$rpc/google.internal.alkali.applications.makersuite.v1.MakerSuiteService/ListModels';
function captureModelsXHR(h, headers = {}, url = modelsUrl) {
  const xhr = new h.context.XMLHttpRequest();
  xhr.open('POST', url);
  for (const [key, value] of Object.entries(headers)) xhr.setRequestHeader(key, value);
  xhr.send('[]');
  assert.equal(xhr.sent, true);
}

test('live capture, fresh snapshot service and secret-free capability report', async () => {
  const h = harness(); await h.capture();
  assert.equal(h.fetches(), 0, 'bootstrap generation must not reach upstream');
  assert.equal(await h.bridge.snapshot('content-hash-one'), 'ticket:content-hash-one');
  assert.equal(await h.bridge.snapshot('content-hash-two'), 'ticket:content-hash-two');
  assert.equal(JSON.stringify(h.bridge.capability()).includes('test-key'), false);
  h.bridge.dispose();
});

test('BYOB relay waits for consumer credit and caps chunks at 32 KiB', async () => {
  const h = harness(); await h.capture(); h.bridge.start({ body: '[]' });
  await tick();
  const first = h.bridge.poll().pending;
  assert.equal(first.sequence, 1); assert.equal(Buffer.from(first.bytes, 'base64').length, 32768);
  await tick(); assert.equal(h.bridge.poll().pending.sequence, 1); assert.equal(h.reads(), 1);
  assert.throws(() => h.bridge.ack(2), /invalid_credit/);
  const output = [];
  for (let sequence = 1; sequence <= 3; ++sequence) {
    const pending = h.bridge.poll().pending;
    assert.equal(pending.sequence, sequence);
    output.push(Buffer.from(pending.bytes, 'base64'));
    h.bridge.ack(sequence); await tick();
  }
  const all = Buffer.concat(output);
  assert.equal(all.length, 70000);
  for (let i = 0; i < all.length; ++i) assert.equal(all[i], i % 251);
  assert.equal(h.bridge.poll().done, true); h.bridge.dispose();
});

test('abort releases pending credit without reading the remaining response', async () => {
  const h = harness(); await h.capture(); h.bridge.start({ body: '[]' }); await tick();
  h.bridge.abort(); await tick();
  assert.equal(h.bridge.poll().done, true); assert.equal(h.bridge.poll().pending, null); assert.equal(h.reads(), 1); h.bridge.dispose();
});

test('ListModels uses captured RPC endpoint and returns only real response rows', async () => {
  const h = harness();
  await h.context.fetch(modelsUrl, { method: 'POST', headers: { 'x-goog-api-key': 'page-test-key' }, body: '[]' });
  const models = await h.bridge.discoverModels('0');
  assert.equal(models.length, 1); assert.equal(models[0].id, 'live-model');
  assert.equal(models[0].source, 'live'); assert.equal(models[0].inputTokenLimit, 123);
  h.bridge.dispose();
});

test('missing live service has no synthetic snapshot fallback', async () => {
  const h = harness({ fastDiscoveryWait: true });
  await assert.rejects(h.bridge.snapshot('content'), /browser_incompatible/);
  await assert.rejects(h.bridge.discoverModels('0'), /no live ListModels request captured/);
  h.bridge.dispose();
});

test('XHR discovery works without generation, binds verified slot and keeps headers private', async () => {
  let request;
  const h = harness({ discoveryFetch: async (url, init) => {
    request = { url, init };
    return new Response('[[["models/xhr-model"]]]');
  } });
  captureModelsXHR(h, { 'X-Goog-Api-Key': 'page-only-key', 'Content-Type': 'application/json+protobuf' });
  assert.equal(h.bridge.capability().template, null);
  assert.equal(h.bridge.capability().discovery, true);
  assert.equal(JSON.stringify(h.bridge.capability()).includes('page-only-key'), false);
  const models = await h.bridge.discoverModels('2');
  assert.equal(models[0].id, 'xhr-model');
  assert.equal(request.url, modelsUrl);
  assert.equal(request.init.method, 'POST');
  assert.equal(request.init.body, '[]');
  assert.equal(request.init.credentials, 'include');
  assert.equal(request.init.headers['x-goog-authuser'], '2');
  assert.equal(request.init.headers['x-goog-api-key'], 'page-only-key');
  h.bridge.dispose();
});

test('discovery waits briefly for actual XHR and never substitutes a generation template', async () => {
  const h = harness({ fastDiscoveryWait: true });
  setTimeout(() => captureModelsXHR(h), 5);
  assert.equal((await h.bridge.discoverModels('0'))[0].id, 'live-model');
  h.bridge.dispose();
  const generation = harness({ fastDiscoveryWait: true });
  await generation.capture();
  await assert.rejects(generation.bridge.discoverModels('0'), /no live ListModels request captured/);
  generation.bridge.dispose();
});

test('discovery rejects account conflicts before sending any request', async () => {
  for (const [headers, url] of [
    [{ 'X-Goog-AuthUser': '1' }, modelsUrl],
    [{ 'x-goog-authuser': '0, 1' }, modelsUrl],
    [{}, modelsUrl + '?authuser=1'],
    [{}, modelsUrl + '?authuser=0&authuser=1']
  ]) {
    const h = harness(); captureModelsXHR(h, headers, url);
    await assert.rejects(h.bridge.discoverModels('0'), /identity_mismatch/);
    assert.equal(h.fetches(), 0); h.bridge.dispose();
  }
  const h = harness();
  await assert.rejects(h.bridge.discoverModels('../0'), /invalid discovery auth_user/);
  h.bridge.dispose();
});

test('only actual POST ListModels on the exact HTTPS service can supply discovery credentials', async () => {
  for (const url of [modelsUrl.replace('https:', 'http:'), modelsUrl.replace('/$rpc/', '/other/'), modelsUrl.replace('.com/', '.com.evil/')]) {
    const h = harness({ fastDiscoveryWait: true }); captureModelsXHR(h, {}, url);
    assert.equal(h.bridge.capability().discovery, false);
    await assert.rejects(h.bridge.discoverModels('0'), /no live ListModels request captured/);
    h.bridge.dispose();
  }
  const h = harness({ fastDiscoveryWait: true });
  await h.context.fetch(modelsUrl);
  assert.equal(h.bridge.capability().discovery, false); h.bridge.dispose();
});

test('discovery bounds responses and classifies HTTP, wire and timeout failures', async () => {
  for (const [respond, expected] of [
    [() => new Response('private error', { status: 403 }), /ListModels_http_403/],
    [() => new Response('x'.repeat(1024 * 1024 + 1)), /ListModels_response_too_large/],
    [() => new Response('invalid'), /invalid ListModels JSON/],
    [() => new Response('{}'), /unknown ListModels wire shape/],
    [() => { const error = new Error('private'); error.name = 'AbortError'; throw error; }, /ListModels_timeout/]
  ]) {
    const h = harness({ discoveryFetch: respond }); captureModelsXHR(h);
    await assert.rejects(h.bridge.discoverModels('0'), expected); h.bridge.dispose();
  }
});

test('discovery request deadline actively aborts a stalled upstream fetch', async () => {
  let aborted = false;
  const h = harness({ fastDiscoveryTimeout: true, discoveryFetch: (url, init) => new Promise((resolve, reject) => {
    init.signal.addEventListener('abort', () => {
      aborted = true;
      const error = new Error('aborted'); error.name = 'AbortError'; reject(error);
    }, { once: true });
  }) });
  captureModelsXHR(h);
  await assert.rejects(h.bridge.discoverModels('0'), /ListModels_timeout/);
  assert.equal(aborted, true); h.bridge.dispose();
});

test('configured relay credit can be smaller than 32 KiB', async () => {
  const h = harness(); await h.capture(); h.bridge.start({ body: '[]', chunkBytes: 4096 }); await tick();
  assert.equal(Buffer.from(h.bridge.poll().pending.bytes, 'base64').length, 4096);
  assert.equal(h.reads(), 1); h.bridge.abort(); await tick(); h.bridge.dispose();
});

test('identity probe reports ambiguity and Google sign-in redirects', () => {
  const h = harness();
  const element = email => ({ getAttribute: name => name === 'data-email' ? email : null });
  h.context.document.querySelectorAll = () => [element('User@Example.com')];
  assert.equal(h.bridge.identity().email, 'user@example.com');
  h.context.document.querySelectorAll = () => [element('first@example.com'), element('other@example.com')];
  assert.equal(h.bridge.identity().email, ''); assert.equal(h.bridge.identity().identityAmbiguous, true);
  h.context.location.href = 'https://accounts.google.com/ServiceLogin';
  h.context.location.hostname = 'accounts.google.com';
  assert.equal(h.bridge.identity().login, true);
  assert.equal(h.bridge.identity().ready, false); h.bridge.dispose();
});

test('a textarea on any other origin cannot make the account ready', () => {
  const h = harness();
  assert.equal(h.bridge.identity().ready, true);
  for (const hostname of ['accounts.google.com', 'example.com', 'aistudio.google.com.example.com']) {
    h.context.location.href = `https://${hostname}/`;
    h.context.location.hostname = hostname;
    assert.equal(h.bridge.identity().ready, false);
  }
  h.context.location.href = 'https://aistudio.google.com/prompts/new_chat';
  h.context.location.hostname = 'aistudio.google.com';
  assert.equal(h.bridge.identity().ready, true);
  h.context.document.readyState = 'loading';
  assert.equal(h.bridge.identity().ready, false);
  h.bridge.dispose();
});

test('account import verifies identity independently of the prompt editor', () => {
  const h = harness();
  h.context.document.querySelector = () => null;
  const badge = { getAttribute: name => ({ href:'https://accounts.google.com/SignOutOptions', 'aria-label':'Google Account: User (user@example.com)' })[name] ?? null };
  const content = { getAttribute: name => name === 'data-email' ? 'unrelated@example.com' : null };
  h.context.document.querySelectorAll = selector => selector === 'a[href]' ? [badge] : [badge, content];
  const result = h.bridge.identity();
  assert.equal(result.identityReady, true); assert.equal(result.ready, false);
  assert.equal(result.email, 'user@example.com'); assert.equal(result.identityAmbiguous, false);
  h.context.location.href = 'https://accounts.google.com/ServiceLogin';
  assert.equal(h.bridge.identity().identityReady, false); h.bridge.dispose();
});

test('hidden account menu entries do not override a visible identity and ambiguity fails closed', () => {
  const h = harness();
  const account = (email, hidden = false) => ({ hidden, getAttribute: name => name === 'data-email' ? email : null });
  let entries = [account('one@example.com'), account('two@example.com', true)];
  h.context.document.querySelectorAll = selector => selector === 'a[href]' ? [] : entries;
  assert.equal(h.bridge.identity().identityReady, true);
  assert.equal(h.bridge.identity().email, 'one@example.com');
  entries[1].hidden = false;
  assert.equal(h.bridge.identity().identityReady, false);
  assert.equal(h.bridge.identity().identityAmbiguous, true);
  assert.equal(h.bridge.identity().email, ''); h.bridge.dispose();
});

test('non-200 metadata preserves Retry-After and bounds private error text', async () => {
  const h = harness({ httpStatus: 429, retryAfter: '75', errorBody: 'x'.repeat(20000) });
  await h.capture(); h.bridge.start({ body: '[]' }); await tick();
  const result = h.bridge.poll();
  assert.equal(result.status, 429); assert.equal(result.retryAfter, 75);
  assert.equal(result.errorBody.length, 8192); assert.equal(result.done, true);
  assert.equal(result.pending, null); assert.equal(h.reads(), 1); h.bridge.dispose();
});

test('HTTP 200 RPC error body is relayed intact for native status classification', async () => {
  const wire = '[[[16,"authentication expired"]]]';
  const h = harness({ errorBody: wire, retryAfter: '30' });
  await h.capture(); h.bridge.start({ body: '[]' }); await tick();
  const result = h.bridge.poll();
  assert.equal(result.status, 200); assert.equal(result.retryAfter, 30);
  assert.equal(Buffer.from(result.pending.bytes, 'base64').toString(), wire);
  h.bridge.abort(); await tick(); h.bridge.dispose();
});

const generationUrl = modelsUrl.replace('/ListModels', '/GenerateContent');
test('XHR captures its live template and private headers without sending bootstrap upstream', async () => {
  let request;
  const h = harness({ observeFetch: (input, init) => { request = { input, init }; } });
  const xhr = new h.context.XMLHttpRequest();
  const events = [];
  for (const type of ['readystatechange', 'load', 'loadend']) xhr.addEventListener(type, () => events.push(type + ':' + xhr.readyState));
  xhr.open('POST', generationUrl);
  xhr.setRequestHeader('X-Test-Page', 'private-header');
  xhr.send('["models/actual-page-model",[[[[null,"probe"]],"user"]],null,null,"actual-ticket"]');
  assert.equal(xhr.sent, undefined);
  await tick();
  assert.equal(xhr.status, 200); assert.equal(xhr.responseText, '[]'); assert.equal(xhr.response, '[]');
  assert.equal(xhr.getResponseHeader('Content-Type'), 'application/json+protobuf');
  assert.deepEqual(events, ['readystatechange:2', 'readystatechange:3', 'readystatechange:4', 'load:4', 'loadend:4']);
  const capability = h.bridge.capability();
  assert.equal(capability.templateTransport, 'xhr');
  assert.equal(capability.template.body[0], 'models/actual-page-model');
  assert.equal(JSON.stringify(capability).includes('private-header'), false);
  await h.context.default_MakerSuite.adapter({ snapshot: () => 'actual-ticket' }, 'a'.repeat(64));
  // Observe the replay parameters through the existing native-fetch test seam.
  h.context.fetch = () => { throw new Error('bridge must use its captured native transport'); };
  h.bridge.start({ body: '[]' }); await tick();
  assert.equal(h.fetches(), 1);
  assert.equal(request.input, generationUrl);
  assert.equal(request.init.headers['x-test-page'], 'private-header');
  h.bridge.abort(); h.bridge.dispose();
});

test('XHR prompt saves are acknowledged locally, including JSON responses and property listeners', async () => {
  const h = harness();
  for (const action of ['SavePrompt', 'CreatePrompt']) {
    const xhr = new h.context.XMLHttpRequest(); let loaded = 0;
    xhr.open('POST', generationUrl.replace('GenerateContent', action)); xhr.responseType = 'json';
    xhr.onload = () => { ++loaded; };
    xhr.send('[]'); await tick();
    assert.equal(xhr.sent, undefined); assert.equal(loaded, 1); assert.equal(xhr.status, 200);
    assert.equal(JSON.stringify(xhr.response), '[]');
    assert.throws(() => xhr.responseText, { name: 'InvalidStateError' });
  }
  assert.equal(h.bridge.capability().template, null); assert.equal(h.fetches(), 0); h.bridge.dispose();
});

test('intercepted XHR abort cancels completion and reopening restores native response views', async () => {
  const h = harness(); const xhr = new h.context.XMLHttpRequest(); let loaded = false, aborted = false;
  xhr.open('POST', generationUrl); xhr.onload = () => { loaded = true; }; xhr.onabort = () => { aborted = true; };
  xhr.send('[]'); xhr.abort(); await tick();
  assert.equal(loaded, false); assert.equal(aborted, true); assert.equal(xhr.status, 0);
  xhr.open('POST', modelsUrl); xhr.send('[]');
  assert.equal(xhr.sent, true); assert.equal(Object.hasOwn(xhr, 'responseText'), false); h.bridge.dispose();
});

test('snapshot adapter discovery skips getters and immutable exports and preserves receiver, arguments and return identity', async () => {
  let getters = 0;
  const exports = {
    tag: 'receiver',
    direct(service, hash, extra) { return service.snapshot({ content: hash, receiver: this.tag, extra }); },
    indirect(service, hash) { return Promise.resolve(service.snapshot).then(fn => fn(hash)); }
  };
  Object.defineProperty(exports, 'getter', { enumerable: true, get() { ++getters; throw new Error('must not evaluate'); } });
  Object.defineProperty(exports, 'locked', { enumerable: true, value: function(service, hash) { return service.snapshot(hash); } });
  const h = harness({ exports });
  assert.equal(getters, 0); assert.equal(h.bridge.capability().snapshotCandidates, 3); assert.equal(h.bridge.capability().snapshotHooked, 2);
  const object = { opaque: true }; let input;
  const service = { snapshot(value) { input = value; return object; } };
  assert.equal(exports.direct(service, 'seed', 17), object);
  assert.equal(input.receiver, 'receiver'); assert.equal(input.extra, 17);
  service.snapshot = value => { input = value; return 'fresh-ticket'; };
  exports.direct(service, 'a'.repeat(64), 17);
  const xhr = new h.context.XMLHttpRequest(); xhr.open('POST', generationUrl); xhr.send('[null,null,null,null,"fresh-ticket"]');
  assert.equal(await h.bridge.snapshot('fresh'), 'fresh-ticket');
  assert.equal(input.content, 'fresh'); assert.equal(input.receiver, 'receiver');
  assert.equal(input.extra, 17);
  const sentinel = Promise.resolve('promise-ticket');
  assert.equal(exports.direct({ snapshot: () => sentinel }, 'seed'), sentinel);
  const promise = exports.indirect({ snapshot: hash => 'async:' + hash }, 'next');
  assert.equal(await promise, 'async:next');
  assert.equal(await h.bridge.snapshot('later'), 'fresh-ticket', 'background adapters cannot replace the selected generation service');
  h.bridge.dispose();
});


test('snapshot export proxies preserve construction, prototypes, metadata and static behavior', () => {
  class SnapshotClient {
    static token = 'static-token';
    constructor(service) { this.service = service; this.target = new.target; }
    getSnapshot(hash) { return this.service.snapshot(hash); }
  }
  function RelatedClient(service, extra) { this.service = service; this.extra = extra; this.target = new.target; }
  RelatedClient.prototype.run = function(hash) { return this.service.snapshot(hash); };
  // Its body contains the production-style snapshot reference, while the export
  // remains a constructor rather than the two-argument signing adapter.
  function SnapshotFactory(service) { this.service = service; this.target = new.target; this.method = service.snapshot; if (!this.method) return service.snapshot(); }
  SnapshotFactory.staticValue = { marker: true };
  const exports = { SnapshotClient, RelatedClient, SnapshotFactory };
  const h = harness({ exports });
  assert.notEqual(exports.SnapshotClient, SnapshotClient);
  assert.notEqual(exports.SnapshotFactory, SnapshotFactory);
  for (const [key, original] of [['SnapshotClient', SnapshotClient], ['SnapshotFactory', SnapshotFactory]]) {
    const proxy = exports[key];
    assert.equal(proxy.prototype, original.prototype); assert.equal(proxy.name, original.name); assert.equal(proxy.length, original.length);
    assert.deepEqual(Object.getOwnPropertyNames(proxy), Object.getOwnPropertyNames(original));
    const instance = new proxy({ snapshot: hash => hash });
    assert.equal(instance instanceof original, true); assert.equal(instance instanceof proxy, true); assert.equal(instance.target, proxy);
    const alternateTarget = function Alternate() {};
    const alternate = Reflect.construct(proxy, [{ snapshot: hash => hash }], alternateTarget);
    assert.equal(Object.getPrototypeOf(alternate), alternateTarget.prototype); assert.equal(alternate.target, alternateTarget);
  }
  assert.equal(exports.SnapshotClient.token, 'static-token'); assert.equal(exports.SnapshotFactory.staticValue, SnapshotFactory.staticValue);
  assert.throws(() => exports.SnapshotClient({ snapshot() {} }), TypeError);
  const before = exports.SnapshotClient; h.context.install(); assert.equal(exports.SnapshotClient, before);
  h.bridge.dispose();
});

test('snapshot observation cannot turn unusual return values into page failures', () => {
  let current;
  const exports = { adapter(service) { if (service.snapshot) return current; } };
  const h = harness({ exports });
  current = Object.defineProperty({}, 'then', { get() { throw new Error('getter failure'); } });
  assert.equal(exports.adapter({ snapshot: true }), current);
  current = { then() { throw new Error('then failure'); } };
  assert.equal(exports.adapter({ snapshot: true }), current);
  const failure = new Error('original failure');
  const service = Object.defineProperty({}, 'snapshot', { get() { throw failure; } });
  assert.throws(() => exports.adapter(service), error => error === failure);
  h.bridge.dispose();
});
