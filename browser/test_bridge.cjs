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
  const errorBytes = options.errorBody === undefined ? null : new TextEncoder().encode(options.errorBody);
  const length = errorBytes ? errorBytes.length : 70000;
  const context = vm.createContext({
    Request, Response, URL, TextDecoder, Uint8Array, AbortController, btoa,
    setInterval: callback => { context.install = callback; return 1; }, clearInterval() {},
    setTimeout, clearTimeout,
    location: { href: 'https://aistudio.google.com/prompts/new_chat?authuser=0', origin: 'https://aistudio.google.com' },
    document: { readyState: 'complete', querySelector: () => ({}), querySelectorAll: () => [] },
    default_MakerSuite: { adapter(service, hash) { return service.snapshot(hash); } },
    fetch: async (input) => {
      ++fetches;
      const url = typeof input === 'string' ? input : input.url;
      if (url.endsWith('/ListModels')) return new Response(JSON.stringify([[['models/live-model', null, null, 'Live Model', 'Returned by RPC', 123, 45, ['generateContent']]]]));
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
    const response = await context.fetch('https://alkalimakersuite-pa.clients6.google.com/$rpc/google.internal.alkali.applications.makersuite.v1.MakerSuiteService/GenerateContent', { method: 'POST', headers: { 'content-type': 'application/json+protobuf', 'x-goog-api-key': 'test-key-never-real' }, body: '["real-captured-template"]' });
    assert.equal(await response.text(), '[]');
    await context.default_MakerSuite.adapter({ snapshot: hash => Promise.resolve('ticket:' + hash) }, 'bootstrap');
  }
  return { context, bridge, capture, fetches: () => fetches, reads: () => reads };
}
const tick = () => new Promise(resolve => setTimeout(resolve, 5));

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
  const h = harness(); await h.capture();
  const models = await h.bridge.discoverModels();
  assert.equal(models.length, 1); assert.equal(models[0].id, 'live-model');
  assert.equal(models[0].source, 'live'); assert.equal(models[0].inputTokenLimit, 123);
  h.bridge.dispose();
});

test('missing live service has no synthetic snapshot fallback', async () => {
  const h = harness();
  await assert.rejects(h.bridge.snapshot('content'), /browser_incompatible/);
  await assert.rejects(h.bridge.discoverModels(), /browser_incompatible/);
  h.bridge.dispose();
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
  assert.equal(h.bridge.identity().login, true); h.bridge.dispose();
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
