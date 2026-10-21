// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Mana Nekoha (mananekoha114)
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(require('node:path').join(__dirname, 'ais2api-cookie.user.js'), 'utf8');
function setup(options = {}) {
  const nodes = [], requests = [], stored = [], timers = new Map(); let serial = 0, cookieReads = 0;
  class Node {
    constructor(tag) { this.tag = tag; this.children = []; this.value = ''; this.listeners = {}; nodes.push(this); }
    appendChild(node) { this.children.push(node); return node; }
    attachShadow() { return new Node('shadow'); }
    addEventListener(event, fn) { this.listeners[event] = fn; }
  }
  const body = new Node('body');
  const document = { body, createElement: tag => new Node(tag), get cookie() { cookieReads++; return options.cookie ?? 'fallback=ok; empty='; } };
  const context = {
    URL, document, location: new URL(options.url || 'https://aistudio.google.com/u/2/prompts/new'),
    setTimeout: (fn, ms) => { const id = ++serial; timers.set(id, { fn, ms }); return id; }, clearTimeout: id => timers.delete(id),
    GM_getValue: (_, fallback) => fallback, GM_setValue: (...args) => stored.push(args), GM_registerMenuCommand: () => {},
    GM_cookie: { list: (query, cb) => { assert.equal(query.url, context.location.href); if (options.hang) return; cb(options.records || [], options.error); if (options.double) cb(options.records || []); } },
    GM_xmlhttpRequest: request => { requests.push(request); return { abort() {} }; }
  };
  vm.runInNewContext(source, context);
  const inputs = nodes.filter(n => n.tag === 'input');
  const push = nodes.find(n => n.tag === 'button' && n.textContent === '读取并导入当前账号');
  const status = nodes.find(n => n.tag === 'p' && n.textContent.startsWith('尚未'));
  return { inputs, push, status, requests, stored, timers, click: () => push.listeners.click(), reads: () => cookieReads };
}
const flush = async () => { await Promise.resolve(); await Promise.resolve(); };
(async () => {
  const records = [
    {name:'SID',value:'secret',domain:'.google.com',path:'/',httpOnly:true,secure:true,sameSite:'lax',expirationDate:Date.now()/1000+100},
    {name:'SID',value:'',domain:'.aistudio.google.com',path:'/u',hostOnly:true,session:true,expirationDate:Date.now()/1000+200,sameSite:'unspecified'},
    {name:'none',value:'n',domain:'.google.com',path:'/',sameSite:'no_restriction',expiry:9999999999},
    {name:'strict',value:'s',domain:'.google.com',path:'/',sameSite:'Strict',expires:9999999998},
    {name:'unknown',value:'u',sameSite:'unexpected'},
    {name:'partition',value:'hidden',partitionKey:{topLevelSite:'https://google.com'}},
    {name:'old',value:'expired',expirationDate:1}
  ];
  const env = setup({records,double:true});
  assert.equal(env.requests.length,0); assert.equal(env.reads(),0); assert.equal(env.inputs[2].value,'2');
  await env.click(); assert.equal(env.requests.length,0); // no key
  env.inputs[1].value='import-secret';
  const pending = env.click(); await flush(); await env.click();
  assert.equal(env.requests.length,1);
  const req = env.requests[0], payload = JSON.parse(req.data);
  assert.equal(req.anonymous,true); assert.equal(req.redirect,'error'); assert.equal(req.timeout,300000);
  assert.deepEqual(payload, { cookies: [
    {name:'SID',value:'secret',domain:'.google.com',path:'/',secure:true,httpOnly:true,sameSite:'Lax',expires:records[0].expirationDate},
    {name:'SID',value:'',domain:'aistudio.google.com',path:'/u'},
    {name:'none',value:'n',domain:'.google.com',path:'/',sameSite:'None',expires:9999999999},
    {name:'strict',value:'s',domain:'.google.com',path:'/',sameSite:'Strict',expires:9999999998},
    {name:'unknown',value:'u',domain:'aistudio.google.com',path:'/'}
  ], auth_user:'2' });
  assert.equal(payload.auth_user,'2'); assert.equal(env.reads(),0); assert.equal(env.inputs[1].value,'');
  assert(!JSON.stringify(env.stored).includes('secret'));
  req.onload({status:200, responseText:JSON.stringify({imported_count:1,accounts:[{id:'account-1',name:'AI Studio',email:'test@example.com'}],verification:'browser_identity_only',generation_verified:false})});
  req.onerror(); await pending;
  assert.match(env.status.textContent,/已导入 1/); assert.match(env.status.textContent,/分区 1/); assert.equal(env.timers.size,0);
  for (const invalid of ['http://example.com','https://user:pass@example.com','https://example.com/a','https://example.com/?x=1','https://example.com/#x','file:///']) {
    const test = setup(); test.inputs[0].value=invalid; test.inputs[1].value='key'; await test.click(); assert.equal(test.requests.length,0); assert.equal(test.reads(),0);
  }
  const fallback = setup({hang:true,url:'https://aistudio.google.com/?authuser=4'});
  fallback.inputs[1].value='key'; const waiting = fallback.click();
  assert.equal(fallback.inputs[2].value,'4');
  [...fallback.timers.values()].find(t=>t.ms===4000).fn(); await flush();
  assert.equal(fallback.requests.length,1); assert.equal(JSON.parse(fallback.requests[0].data).cookies,'fallback=ok; empty=');
  assert.match(fallback.status.textContent,/属性未知/); assert.match(fallback.status.textContent,/无法判断分区/);
  fallback.requests[0].onload({status:500,responseText:'SECRET BACKEND FAILURE'}); await waiting;
  assert(!fallback.status.textContent.includes('SECRET')); assert.equal(fallback.push.disabled,false);
  const timed = setup({records}); timed.inputs[1].value='key'; const timeoutPending=timed.click(); await flush();
  [...timed.timers.values()].find(t=>t.ms===300000).fn(); await timeoutPending;
  timed.requests[0].onload({status:200,responseText:'secret'});
  assert.match(timed.status.textContent,/失败或超时/); assert.equal(timed.timers.size,0); assert.equal(timed.push.disabled,false);
  const redirected = setup({records}); redirected.inputs[0].value='https://example.com'; redirected.inputs[1].value='key';
  const redirectPending=redirected.click(); await flush();
  redirected.requests[0].onload({status:200,finalUrl:'https://elsewhere.example/',responseText:'secret'}); await redirectPending;
  assert.match(redirected.status.textContent,/失败/);
  for (const reason of ['cookie_import_authentication', 'cookie_not_authenticated', 'cookie_browser_incompatible', 'cookie_browser_timeout', 'unknown-secret-reason']) {
    const rejected = setup({records}); rejected.inputs[1].value='key';
    const rejectedPending=rejected.click(); await flush();
    rejected.requests[0].onload({status:401,responseText:JSON.stringify({error:{reason,message:'SECRET SERVER MESSAGE'}})});
    await rejectedPending;
    assert(!rejected.status.textContent.includes('SECRET')); assert(!rejected.status.textContent.includes('unknown-secret'));
    if (reason==='cookie_import_authentication') assert.match(rejected.status.textContent,/cookie_import_key（不是生成接口的 api_key）/);
    if (reason==='cookie_not_authenticated') assert.match(rejected.status.textContent,/可能已过期/);
    if (reason==='cookie_browser_incompatible') assert.match(rejected.status.textContent,/网页适配/);
    if (reason==='cookie_browser_timeout') assert.match(rejected.status.textContent,/浏览器等待超时/);
    if (reason==='unknown-secret-reason') assert.match(rejected.status.textContent,/失败或超时/);
  }
  const malformed = setup({records}); malformed.inputs[1].value='key';
  const malformedPending=malformed.click(); await flush();
  malformed.requests[0].onload({status:200,responseText:JSON.stringify({imported_count:2,accounts:[{}],verification:'browser_identity_only',generation_verified:false})});
  await malformedPending; assert.match(malformed.status.textContent,/失败或超时/);
  assert(!source.includes('innerHTML')); assert(!source.includes('console.')); assert(!source.includes('eval('));
  console.log('Cookie helper VM tests passed');
})().catch(error => { console.error(error); process.exitCode=1; });
