// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Mana Nekoha (mananekoha114)
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(require('node:path').join(__dirname, 'ais2api-cookie.user.js'), 'utf8');

function setup(options = {}) {
  const nodes = [], requests = [], stored = [], timers = new Map(), events = {}, menus = [], copied = [];
  const storage = options.storage || new Map();
  let serial = 0, cookieReads = 0, gmReads = 0, now = Date.now(), aborts = 0;
  class Node {
    constructor(tag) {
      this.tag = tag; this.children = []; this.value = ''; this.listeners = {};
      this.style = {}; this.attributes = {}; this.hidden = false; this.disabled = false;
      this.checked = false; this._text = '';
      nodes.push(this);
    }
    appendChild(node) { node.parentNode = this; this.children.push(node); return node; }
    attachShadow() { return new Node('shadow'); }
    addEventListener(event, fn) { (this.listeners[event] ||= []).push(fn); }
    setAttribute(name, value) { this.attributes[name] = String(value); }
    getAttribute(name) { return this.attributes[name]; }
    removeAttribute(name) { delete this.attributes[name]; if (name === 'href') delete this.href; }
    replaceChildren(...children) { this._text = ''; this.children = []; children.forEach(child => this.appendChild(child)); }
    set textContent(value) { this._text = String(value); this.children = []; }
    get textContent() { return this._text + this.children.map(child => child.textContent).join(''); }
    focus() { this.focused = true; }
    select() { this.selected = true; }
    closest(tag) { return this.tag === tag ? this : this.parentNode?.closest(tag); }
    getBoundingClientRect() {
      const panel = nodes.find(node => node.id === 'ais2-panel');
      const width = panel && !panel.hidden ? parseFloat(panel.style.width) || 360 : 52;
      const height = panel && !panel.hidden ? Math.min(700, parseFloat(panel.style.maxHeight) || 700) : 52;
      const x = parseFloat(this.style.left) || 0, y = parseFloat(this.style.top) || 0;
      return { x, y, left: x, top: y, right: x + width, bottom: y + height, width, height };
    }
  }
  const body = new Node('body');
  const document = { body, createElement: tag => new Node(tag), get cookie() { cookieReads++; return options.cookie ?? 'fallback=synthetic; empty='; } };
  const context = {
    URL, document, location: new URL(options.url || 'https://aistudio.google.com/u/2/prompts/new'),
    innerWidth: options.width || 400, innerHeight: options.height || 800,
    Date: class extends Date { static now() { return now; } },
    setTimeout: (fn, ms) => { const id = ++serial; timers.set(id, { fn, ms, repeat: false }); return id; },
    clearTimeout: id => timers.delete(id),
    setInterval: (fn, ms) => { const id = ++serial; timers.set(id, { fn, ms, repeat: true }); return id; },
    clearInterval: id => timers.delete(id),
    addEventListener: (event, fn) => (events[event] ||= []).push(fn),
    navigator: { clipboard: { async writeText(value) { if (options.clipboardFail) throw new Error('blocked'); copied.push(value); } } },
    GM_getValue: (name, fallback) => {
      if (options.readFail) throw new Error('blocked');
      return storage.has(name) ? structuredClone(storage.get(name)) : fallback;
    },
    GM_setValue: (name, value) => {
      if (options.writeFail) throw new Error('blocked');
      storage.set(name, structuredClone(value)); stored.push([name, value]);
    },
    GM_deleteValue: name => { if (options.deleteFail) throw new Error('blocked'); storage.delete(name); },
    GM_registerMenuCommand: (name, fn) => menus.push({ name, fn }),
    GM_cookie: { list: (query, cb) => {
      gmReads++; assert.equal(query.url, context.location.href);
      if (options.hang) return;
      cb(options.records || [], options.error);
      if (options.double) cb(options.records || []);
    } },
    GM_xmlhttpRequest: request => {
      if (options.requestThrow) throw new Error('synthetic private error');
      requests.push(request); return { abort() { aborts++; } };
    }
  };
  context.window = context; context.self = context; context.top = options.frame ? {} : context;
  if (options.noGM) delete context.GM_cookie;
  vm.runInNewContext(source, context);
  const get = id => nodes.find(node => node.id === 'ais2-' + id);
  const fire = async (node, event = 'click', args = {}) => {
    if (event === 'click' && node.disabled) return;
    for (const fn of node.listeners[event] || []) await fn({ target: node, preventDefault() {}, ...args });
  };
  return {
    nodes, context, requests, stored, storage, timers, menus, copied, get, fire,
    click: id => fire(get(id)),
    set: async (id, value) => { get(id).value = value; await fire(get(id), 'input'); await fire(get(id), 'change'); },
    tick(ms) {
      const pair = [...timers].find(([, timer]) => timer.ms === ms); assert(pair, 'expected timer ' + ms);
      const [id, timer] = pair; if (!timer.repeat) timers.delete(id);
      now += ms; timer.fn();
    },
    dispatch(event, args) { for (const fn of events[event] || []) fn({ preventDefault() {}, ...args }); },
    reads: () => ({ document: cookieReads, gm: gmReads }), aborts: () => aborts
  };
}
const flush = async () => { for (let i = 0; i < 8; i++) await Promise.resolve(); };
const records = [
  {name:'SID',value:'synthetic-secret',domain:'.google.com',path:'/',httpOnly:true,secure:true,sameSite:'lax',expirationDate:Date.now()/1000+10000},
  {name:'SID',value:'',domain:'.aistudio.google.com',path:'/u',hostOnly:true,session:true,expirationDate:Date.now()/1000+10000,sameSite:'unspecified'},
  {name:'none',value:'n',domain:'.google.com',path:'/',sameSite:'no_restriction',expiry:9999999999},
  {name:'strict',value:'s',domain:'.google.com',path:'/',sameSite:'Strict',expires:9999999998},
  {name:'unknown',value:'u',sameSite:'__proto__'},
  {name:'partition',value:'hidden',partitionKey:{topLevelSite:'https://google.com'}},
  {name:'opaque',value:'hidden',partitionKeyOpaque:true},
  {name:'old',value:'expired',expirationDate:1}
];
function success(extra = {}) {
  return {imported_count:1,accounts:[{id:'synthetic-id',name:'Synthetic Account',email:'synthetic@example.test'}],
    verification:'browser_identity_only',generation_verified:false,...extra};
}
function respond(env, body, status = 200, extra = {}) {
  env.requests.at(-1).onload({status,responseText:JSON.stringify(body),...extra});
}
async function pendingImport(env) {
  if (!env.get('key').value) env.get('key').value = 'synthetic-import-key';
  const done = env.click('import'); await flush(); return {done};
}
let scenarios = 0;
const suiteTimeout = setTimeout(() => {
  console.error('Cookie helper VM tests did not complete'); process.exitCode = 1;
}, 5000);
async function scenario(name, fn) {
  try { await fn(); scenarios++; }
  catch (error) { error.message = name + ': ' + error.message; throw error; }
}
(async () => {
  await scenario('explicit consent and initial UI', async () => {
    const env = setup({records});
    assert.deepEqual(env.reads(),{document:0,gm:0}); assert.equal(env.requests.length,0);
    assert.equal(env.get('mode').value,'current'); assert.equal(env.get('auth').value,'');
    assert.equal(env.get('remember').checked,false); assert.equal(env.get('key').value,'');
    assert.equal(env.get('config-link').href,'http://127.0.0.1:18451/config');
    await env.click('import'); assert.equal(env.reads().gm,0); assert.equal(env.requests.length,0);
    assert.equal(setup({frame:true}).nodes.length,1);
  });
  await scenario('structured metadata, identities and reentrancy', async () => {
    const env = setup({records,double:true});
    const {done} = await pendingImport(env); await env.click('import');
    assert.equal(env.requests.length,1);
    const req = env.requests[0], payload = JSON.parse(req.data);
    assert.equal(req.anonymous,true); assert.equal(req.redirect,'error'); assert.equal(req.timeout,300000);
    assert.equal(payload.auth_user,'2'); assert.equal(env.reads().document,0);
    assert.deepEqual(payload.cookies,[
      {name:'SID',value:'synthetic-secret',domain:'.google.com',path:'/',secure:true,httpOnly:true,sameSite:'Lax',expires:records[0].expirationDate},
      {name:'SID',value:'',domain:'aistudio.google.com',path:'/u'},
      {name:'none',value:'n',domain:'.google.com',path:'/',sameSite:'None',expires:9999999999},
      {name:'strict',value:'s',domain:'.google.com',path:'/',sameSite:'Strict',expires:9999999998},
      {name:'unknown',value:'u',domain:'aistudio.google.com',path:'/'}
    ]);
    assert.equal(env.get('key').value,'synthetic-import-key');
    assert(!JSON.stringify(env.stored).includes('synthetic'));
    respond(env,success()); req.onerror(); await done;
    assert.match(env.get('status').textContent,/导入 \/ 更新 1/);
    assert.match(env.get('accounts').textContent,/synthetic@example\.test/);
    assert.match(env.get('accounts').textContent,/Synthetic Account/);
    assert.match(env.get('cookie-info').textContent,/分区 2/);
    assert.equal(env.timers.size,0); assert.equal(env.get('import').disabled,false);
  });
  await scenario('current URL and explicit index', async () => {
    const env = setup({records});
    env.context.location = new URL('https://aistudio.google.com/?authuser=4');
    let {done} = await pendingImport(env);
    assert.equal(JSON.parse(env.requests[0].data).auth_user,'4'); respond(env,success()); await done;
    env.get('auth').value='7'; env.get('email').value='expected@example.test';
    ({done} = await pendingImport(env));
    assert.equal(JSON.parse(env.requests[1].data).auth_user,'7');
    assert.equal(JSON.parse(env.requests[1].data).email,'expected@example.test');
    respond(env,success()); await done;
  });
  await scenario('bulk import ignores single-account constraints and shows limit', async () => {
    const env = setup({records}); await env.set('mode','all');
    env.get('auth').value='invalid'; env.get('email').value='single@example.test';
    assert.equal(env.get('auth').disabled,true); assert.equal(env.get('email').disabled,true);
    const {done} = await pendingImport(env);
    const payload=JSON.parse(env.requests[0].data);
    assert.deepEqual(Object.keys(payload),['cookies']);
    respond(env,success({details:{truncated:true,attempted:1,verified:1,skipped_host_cookies:2}})); await done;
    assert.match(env.get('status').textContent,/扫描上限/);
    assert.match(env.get('cookie-info').textContent,/来源未知项：2/);
    assert.equal(env.get('auth').disabled,true);
  });
  await scenario('no cookies preserves key for retry', async () => {
    const env=setup({records:[],cookie:''}); env.get('key').value='synthetic-key';
    await env.click('import');
    assert.equal(env.requests.length,0); assert.equal(env.get('key').value,'synthetic-key');
    assert.match(env.get('status').textContent,/尚未发送请求/);
    assert.equal(env.get('import').disabled,false); assert.equal(env.timers.size,0);
  });
  await scenario('URL and input validation before reading credentials', async () => {
    for (const invalid of ['http://example.com','https://user:pass@example.com','https://example.com/a','https://example.com/?x=1','https://example.com/#x','file:///','example.com']) {
      const env=setup({records}); await env.set('service',invalid); env.get('key').value='synthetic-key';
      await env.click('import'); assert.equal(env.requests.length,0); assert.equal(env.reads().gm,0);
    }
    const env=setup({records}); await env.set('service','127.0.0.1:18451/');
    env.get('key').value='synthetic-key'; env.get('auth').value='12345678901';
    await env.click('import'); assert.equal(env.requests.length,0);
    env.get('auth').value=''; env.get('key').value='key\ninjected-header';
    await env.click('import'); assert.equal(env.requests.length,0);
  });
  await scenario('fallback watchdog, progress and cleanup', async () => {
    const env=setup({hang:true});
    const {done}=await pendingImport(env);
    assert.match(env.get('status').textContent,/正在读取/);
    env.tick(1000); assert.match(env.get('elapsed').textContent,/1 秒/);
    env.tick(4000); await flush();
    assert.equal(env.requests.length,1); assert.equal(JSON.parse(env.requests[0].data).cookies,'fallback=synthetic; empty=');
    assert.match(env.get('cookie-info').textContent,/不包含 HttpOnly/);
    respond(env,{error:{reason:'cookie_not_authenticated',message:'PRIVATE BODY'}},400); await done;
    assert.match(env.get('status').textContent,/过期/); assert(!env.get('status').textContent.includes('PRIVATE'));
    assert.equal(env.timers.size,0);
    const missing=setup({noGM:true,cookie:''}); missing.get('key').value='key'; await missing.click('import');
    assert.equal(missing.reads().document,1); assert.equal(missing.requests.length,0);
  });
  await scenario('health is a separate check without Cookie or bearer', async () => {
    const env=setup({records}); env.get('key').value='synthetic-key';
    const done=env.click('check'); await flush();
    assert.equal(env.requests[0].url,'http://127.0.0.1:18451/health');
    assert.equal(env.requests[0].method,'GET'); assert.deepEqual(Object.keys(env.requests[0].headers),[]);
    assert.deepEqual(env.reads(),{document:0,gm:0});
    respond(env,{status:'alive'}); await done;
    assert.match(env.get('status').textContent,/实际导入时校验/);
    assert.equal(env.get('key').value,'synthetic-key'); assert.equal(env.timers.size,0);
  });
  await scenario('malformed manager metadata cannot leave collection pending', async () => {
    const env=setup({records:[{name:'SID',value:'synthetic',domain:42,hostOnly:true}]});
    const {done}=await pendingImport(env);
    assert.equal(env.requests.length,1);
    assert.equal(env.reads().document,1);
    assert.match(env.get('cookie-info').textContent,/document.cookie/);
    respond(env,success()); await done;
    assert.equal(env.timers.size,0);
  });
  await scenario('opt-in memory, per-origin isolation, reload and forgetting', async () => {
    const storage=new Map(), env=setup({storage});
    env.get('key').value='synthetic-key-A';
    assert(!storage.has('importKey:http://127.0.0.1:18451'));
    env.get('remember').checked=true; await env.fire(env.get('remember'),'change');
    assert.equal(storage.get('importKey:http://127.0.0.1:18451'),'synthetic-key-A');
    const reload=setup({storage}); assert.equal(reload.get('key').value,'synthetic-key-A'); assert(reload.get('remember').checked);
    await env.set('service','https://another.example');
    assert.equal(env.get('key').value,''); assert.equal(env.get('remember').checked,false);
    env.get('key').value='synthetic-key-B'; env.get('remember').checked=true; await env.fire(env.get('remember'),'change');
    await env.set('service','127.0.0.1:18451');
    assert.equal(env.get('key').value,'synthetic-key-A');
    await env.click('forget');
    assert.equal(env.get('key').value,''); assert.equal(storage.has('importKey:http://127.0.0.1:18451'),false);
    assert.equal(storage.get('importKey:https://another.example'),'synthetic-key-B');
    assert.equal(setup({storage}).get('key').value,'');
  });
  await scenario('uncheck forgets persistence while preserving in-page input', async () => {
    const storage=new Map([['importKey:http://127.0.0.1:18451','synthetic-key']]), env=setup({storage});
    env.get('remember').checked=false; await env.fire(env.get('remember'),'change');
    assert.equal(env.get('key').value,'synthetic-key'); assert.equal(storage.size,0);
  });
  await scenario('storage failures are visible', async () => {
    const env=setup({readFail:true}); assert.match(env.get('storage-notice').textContent,/无法读取/);
    const blocked=setup({writeFail:true}); blocked.get('key').value='synthetic-key'; blocked.get('remember').checked=true;
    await blocked.fire(blocked.get('remember'),'change');
    assert.equal(blocked.get('remember').checked,false); assert.match(blocked.get('storage-notice').textContent,/失败/);
    const retained=new Map([['importKey:http://127.0.0.1:18451','synthetic-key']]);
    const deletion=setup({storage:retained,deleteFail:true}); await deletion.click('forget');
    assert(retained.has('importKey:http://127.0.0.1:18451'));
    assert.match(deletion.get('storage-notice').textContent,/清除失败/);
  });
  await scenario('unconfirmed outcomes do not retry or leak server details', async () => {
    for (const failure of ['network','timeout','aborted','redirect','json','shape','unknown','throw','watchdog']) {
      const env=setup({records,requestThrow:failure==='throw'}); const {done}=await pendingImport(env);
      const req=env.requests[0];
      if (failure==='network') req.onerror();
      if (failure==='timeout') req.ontimeout();
      if (failure==='aborted') req.onabort();
      if (failure==='redirect') respond(env,success(),200,{finalUrl:'https://elsewhere.example/'});
      if (failure==='json') req.onload({status:200,responseText:'PRIVATE RESPONSE'});
      if (failure==='shape') respond(env,success({accounts:[{}]}));
      if (failure==='unknown') respond(env,{error:{reason:'PRIVATE REASON',message:'PRIVATE MESSAGE'}},500);
      if (failure==='watchdog') { env.tick(300000); assert.equal(env.aborts(),1); req.onload({status:200,responseText:'PRIVATE'}); }
      await done;
      assert.match(env.get('status').textContent,/未能确认|尚未确认/);
      assert.equal(env.requests.length,failure==='throw'?0:1);
      assert.equal(env.get('key').value,'synthetic-import-key'); assert.equal(env.timers.size,0);
      await env.click('copy'); const report=env.copied[0];
      assert(report); assert(!/PRIVATE|synthetic|example|Cookie|Bearer/.test(report));
      assert.deepEqual(Object.keys(JSON.parse(report)),['script_version','action','phase','code','http_status','duration_ms']);
    }
  });
  await scenario('known rejection offers concrete action', async () => {
    const env=setup({records}); const {done}=await pendingImport(env);
    respond(env,{error:{reason:'cookie_import_authentication',message:'PRIVATE MESSAGE'}},401); await done;
    assert.match(env.get('status').textContent,/导入密钥错误/);
    assert.match(env.get('status').textContent,/已保留/); assert.equal(env.get('status').getAttribute('data-kind'),'error');
    await env.click('copy'); assert.equal(JSON.parse(env.copied[0]).code,'cookie_import_authentication');
  });
  await scenario('untrusted identity text and malformed success metadata', async () => {
    const env=setup({records}); let {done}=await pendingImport(env);
    respond(env,success({accounts:[{id:'x',email:'<img src=x onerror=alert(1)>',name:'<script>bad</script>'}]})); await done;
    assert.equal(env.get('accounts').children[0].children[0].textContent,'<img src=x onerror=alert(1)>');
    ({done}=await pendingImport(env)); respond(env,success({details:{truncated:'yes'}})); await done;
    assert.match(env.get('status').textContent,/未能确认/);
  });
  await scenario('clipboard fallback offers selectable redacted text', async () => {
    const env=setup({clipboardFail:true}); env.get('key').value='synthetic-key'; await env.click('import'); await env.click('copy');
    assert.equal(env.get('copy-text').hidden,false); assert.equal(env.get('copy-text').selected,true);
    assert.equal(JSON.parse(env.get('copy-text').value).code,'no_cookies');
    assert(!env.get('copy-text').value.includes('synthetic'));
  });
  await scenario('diagnostics follow the latest action and reset on destination changes', async () => {
    const env=setup({records}); const {done}=await pendingImport(env); respond(env,success()); await done;
    env.get('key').value=''; await env.click('import'); await env.click('copy');
    assert.equal(JSON.parse(env.copied.at(-1)).code,'invalid_key');
    assert.equal(env.get('accounts').children.length,0);
    await env.set('service','https://another.example');
    assert.equal(env.get('copy').disabled,true); assert.equal(env.get('elapsed').textContent,'');
    assert.match(env.get('cookie-info').textContent,/尚未读取/);
  });
  await scenario('collapse, reopen, position persistence and narrow viewport', async () => {
    const storage=new Map(), env=setup({storage,width:300,height:450});
    await env.click('collapse'); assert.equal(env.get('panel').hidden,true); assert.equal(env.get('bubble').hidden,false);
    assert.equal(storage.get('panelOpen'),false);
    const collapsed=setup({storage,width:300,height:450}); assert.equal(collapsed.get('panel').hidden,true);
    collapsed.menus[0].fn(); assert.equal(collapsed.get('panel').hidden,false);
    await env.fire(env.get('bubble'),'pointerdown',{button:0,pointerId:1,clientX:100,clientY:100});
    env.dispatch('pointermove',{pointerId:1,clientX:-1000,clientY:-1000});
    env.dispatch('pointerup',{pointerId:1});
    assert.equal(typeof storage.get('panelPosition'),'object');
    await env.click('bubble'); assert.equal(env.get('panel').hidden,true); // drag does not open
    await env.click('bubble'); assert.equal(env.get('panel').hidden,false);
    env.context.innerWidth=240; env.context.innerHeight=320; env.dispatch('resize',{});
    const host=env.nodes.find(node=>node.tag==='div');
    const rect=host.getBoundingClientRect();
    assert(rect.left>=8 && rect.top>=8); assert(rect.right<=232 && rect.bottom<=312);
    const restored=setup({storage,width:300,height:450});
    const restoredHost=restored.nodes.find(node=>node.tag==='div');
    assert.equal(parseFloat(restoredHost.style.left),8);
  });
  assert(!source.includes('innerHTML')); assert(!source.includes('console.')); assert(!source.includes('eval('));
  console.log('Cookie helper VM tests passed (' + scenarios + ' scenarios)');
})().catch(error => { console.error(error); process.exitCode=1; }).finally(() => clearTimeout(suiteTimeout));
