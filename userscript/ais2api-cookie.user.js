// ==UserScript==
// @name         ais2api Cookie Import
// @namespace    https://github.com/mananekoha114/ais2api
// @version      1.1.1
// @description  Import AI Studio accounts with explicit consent and clear results.
// @author       Mana Nekoha
// @license      MPL-2.0
// @match        https://aistudio.google.com/*
// @run-at       document-idle
// @noframes
// @grant        GM_cookie
// @grant        GM_xmlhttpRequest
// @grant        GM_registerMenuCommand
// @grant        GM_getValue
// @grant        GM_setValue
// @grant        GM_deleteValue
// @connect      localhost
// @connect      127.0.0.1
// @connect      *
// ==/UserScript==

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
(function () {
  'use strict';
  if (location.protocol !== 'https:' || location.hostname !== 'aistudio.google.com' || window.top !== window.self) return;
  const VERSION = '1.1.1';
  const DEFAULT_SERVER = 'http://127.0.0.1:18451';
  let busy = false, activeOrigin = null, storageFailed = false;
  let operation = null, ticker = null;

  function serverURL(value) {
    let raw = value.trim();
    // Only loopback addresses can omit their scheme.
    if (/^(?:localhost|127\.0\.0\.1|\[::1\])(?::\d+)?\/?$/i.test(raw)) raw = 'http://' + raw;
    const url = new URL(raw);
    if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password ||
        url.search || url.hash || url.pathname !== '/') throw new Error('invalid server');
    if (url.protocol === 'http:' && !['localhost', '127.0.0.1', '[::1]'].includes(url.hostname))
      throw new Error('HTTP requires loopback');
    return url.origin;
  }
  function inferUser(value) {
    const url = new URL(value);
    const candidate = url.searchParams.get('authuser') ?? (url.pathname.match(/\/u\/(\d+)(?:\/|$)/) || [])[1] ?? '0';
    return /^\d{1,10}$/.test(candidate) ? candidate : '0';
  }
  function validKey(value) {
    return typeof value === 'string' && value.length > 0 && value.length <= 4096 && !/[\s\x00-\x1f\x7f]/.test(value);
  }
  function readSetting(name, fallback) {
    try {
      return GM_getValue(name, fallback);
    } catch (_) { storageFailed = true; return fallback; }
  }
  function writeSetting(name, value) {
    try {
      if (value === null) {
        GM_deleteValue(name);
      } else {
        GM_setValue(name, value);
      }
      return true;
    } catch (_) {
      storageNotice.textContent = '设置保存或清除失败；请检查脚本管理器的存储权限。当前页面仍可导入。';
      return false;
    }
  }

  const host = document.createElement('div');
  const root = host.attachShadow({ mode: 'closed' });
  const style = document.createElement('style');
  style.textContent = `:host{all:initial;position:fixed;z-index:2147483647;color-scheme:dark;font:14px/1.5 system-ui,sans-serif;color:#e9eef8}
*{box-sizing:border-box}[hidden]{display:none!important}
section{display:flex;flex-direction:column;overflow:hidden;background:#151c29;border:1px solid #41506a;border-radius:16px;box-shadow:0 12px 40px #0007}
header{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:12px 16px;background:#202b3d;cursor:grab;touch-action:none;user-select:none}
header strong{font-size:15px}.body{padding:14px 16px;overflow:auto;overscroll-behavior:contain;min-height:0}
label{display:block;margin:10px 0 4px}input,select,textarea,button{font:inherit}
input:not([type=checkbox]),select,textarea{display:block;width:100%;min-width:0;margin-top:5px;padding:10px;border:1px solid #56647b;border-radius:8px;background:#0e1521;color:#f4f7fc}
input:focus,select:focus,textarea:focus,button:focus-visible,a:focus-visible,summary:focus-visible{outline:2px solid #9fbfff;outline-offset:2px}
input[type=checkbox]{width:18px;height:18px;margin:0;accent-color:#aac5ff}.check{display:flex;align-items:center;gap:8px}
button{cursor:pointer;border:1px solid #56647b;border-radius:8px;background:#29364b;color:#f4f7fc;padding:9px 12px;min-height:40px}
button:disabled,input:disabled,select:disabled{opacity:.55;cursor:default}.primary{width:100%;margin-top:12px;background:#b6ceff;color:#101c35;border-color:#b6ceff;font-weight:650}
.row{display:flex;align-items:center;flex-wrap:wrap;gap:8px;margin-top:10px}.row>*{flex:1}header button{padding:4px 10px}
a{color:#bbd1ff;text-decoration:underline;text-underline-offset:3px}
p{margin:8px 0;overflow-wrap:anywhere}.muted{color:#b3bfd3;font-size:12px}.notice{color:#ffd694;font-size:12px}
.status{border-left:3px solid #89abeb;padding:8px 10px;background:#202b3d;border-radius:4px;white-space:pre-wrap}
.status[data-kind=success]{border-color:#90d6aa}.status[data-kind=error],.status[data-kind=unconfirmed]{border-color:#ffbf83}
details{margin-top:10px}summary{cursor:pointer;padding:5px 0}ul{padding:0;list-style:none;margin:8px 0}
li{padding:9px 0;border-bottom:1px solid #344158;overflow-wrap:anywhere}li strong,li span{display:block}
.bubble{width:52px;height:52px;border-radius:50%;background:#b6ceff;color:#101c35;box-shadow:0 6px 24px #0006;touch-action:none;font-weight:700}
textarea{resize:vertical;font-size:12px;min-height:100px}`;
  root.appendChild(style);
  function element(tag, text, parent, id) {
    const node = document.createElement(tag);
    if (text) node.textContent = text;
    if (id) node.id = 'ais2-' + id;
    parent.appendChild(node);
    return node;
  }
  function button(text, parent, id) {
    const node = element('button', text, parent, id); node.type = 'button'; return node;
  }
  const panel = element('section', '', root, 'panel');
  panel.tabIndex = -1;
  panel.setAttribute('aria-label', 'ais2api 账号导入');
  const header = element('header', '', panel);
  element('strong', 'ais2api · 账号导入', header);
  const collapse = button('收起', header, 'collapse');
  const content = element('div', '', panel); content.className = 'body';
  function field(label, type, parent, id) {
    const wrap = element('label', label, parent);
    const input = element('input', '', wrap, id);
    input.type = type; input.autocomplete = 'off'; input.spellcheck = false;
    return input;
  }
  const service = field('服务地址', 'text', content, 'service');
  service.value = readSetting('serverURL', DEFAULT_SERVER);
  service.placeholder = DEFAULT_SERVER;
  const key = field('导入密钥', 'password', content, 'key'); key.maxLength = 4096;
  key.placeholder = '在管理页「服务配置」中设置';
  const rememberLabel = element('label', '', content); rememberLabel.className = 'check';
  const remember = element('input', '', rememberLabel, 'remember'); remember.type = 'checkbox';
  element('span', '在此设备记住这个服务的密钥', rememberLabel);
  const settingsRow = element('div', '', content); settingsRow.className = 'row';
  const check = button('检查服务', settingsRow, 'check');
  const forget = button('清除密钥', settingsRow, 'forget');
  const configLink = element('a', '打开服务配置', content, 'config-link');
  const storageNotice = element('p', '', content, 'storage-notice'); storageNotice.className = 'notice';
  const memoryHint = element('p', '勾选后存入本机脚本管理器；Cookie 不会保存。', content); memoryHint.className = 'muted';
  const modeLabel = element('label', '导入范围', content);
  const mode = element('select', '', modeLabel, 'mode');
  const currentOption = element('option', '当前账号', mode); currentOption.value = 'current';
  const allOption = element('option', '批量识别账号', mode); allOption.value = 'all';
  mode.value = 'current';
  const modeHint = element('p', '', content, 'mode-hint'); modeHint.className = 'muted';
  const advanced = element('details', '', content);
  element('summary', '高级设置', advanced);
  const auth = field('账号索引（留空时跟随当前页面）', 'text', advanced, 'auth');
  auth.inputMode = 'numeric'; auth.maxLength = 10; auth.placeholder = '自动';
  const email = field('预期邮箱（可选，用于确认身份）', 'email', advanced, 'email'); email.maxLength = 320;
  const push = button('读取并导入当前账号', content, 'import'); push.className = 'primary';
  const status = element('p', '准备就绪。点击导入后才会读取 Cookie。', content, 'status');
  status.className = 'status'; status.setAttribute('role', 'status'); status.setAttribute('aria-live', 'polite');
  const elapsed = element('p', '', content, 'elapsed'); elapsed.className = 'muted';
  const accounts = element('ul', '', content, 'accounts');
  const accountsLink = element('a', '打开账号列表', content, 'accounts-link');
  const detailPanel = element('details', '', content);
  element('summary', '读取详情与错误摘要', detailPanel);
  const cookieInfo = element('p', '尚未读取 Cookie。', detailPanel, 'cookie-info'); cookieInfo.className = 'muted';
  const copy = button('复制诊断摘要', detailPanel, 'copy'); copy.disabled = true;
  const copyNotice = element('p', '', detailPanel, 'copy-notice'); copyNotice.className = 'muted';
  const copyText = element('textarea', '', detailPanel, 'copy-text');
  copyText.readOnly = true; copyText.hidden = true; copyText.setAttribute('aria-label', '诊断摘要');
  const note = element('p', '导入验证登录身份；生成能力请在服务中另行检查。', content); note.className = 'muted';
  const bubble = button('导入', root, 'bubble'); bubble.className = 'bubble';
  bubble.setAttribute('aria-label', '打开 ais2api 账号导入');
  (document.body || document.documentElement).appendChild(host);
  for (const link of [configLink, accountsLink]) { link.target = '_blank'; link.rel = 'noopener noreferrer'; }

  function setStatus(message, kind = '') {
    status.textContent = message; status.setAttribute('data-kind', kind);
  }
  function keyName(origin) { return 'importKey:' + origin; }
  function syncService() {
    if (busy) return activeOrigin;
    let origin = null;
    try { origin = serverURL(service.value); } catch (_) { /* An unfinished address has no credential scope. */ }
    if (origin !== activeOrigin) {
      key.value = ''; remember.checked = false; accounts.replaceChildren();
      if (origin) {
        const savedKey = readSetting(keyName(origin), '');
        if (validKey(savedKey)) { key.value = savedKey; remember.checked = true; }
      }
      activeOrigin = origin;
    }
    for (const [link, path] of [[configLink, '/config'], [accountsLink, '/accounts']]) {
      link.hidden = !origin;
      if (origin) link.href = origin + path; else link.removeAttribute('href');
    }
    if (storageFailed) storageNotice.textContent = '无法读取已保存设置，请检查管理器存储权限；可在本页重新填写。';
    return origin;
  }
  function saveKey() {
    const origin = syncService();
    if (!origin) { remember.checked = false; storageNotice.textContent = '请先填写有效的服务地址。'; return; }
    if (remember.checked && !validKey(key.value.trim())) {
      remember.checked = false; storageNotice.textContent = '请先填写导入密钥，再选择记住。'; return;
    }
    if (writeSetting(keyName(origin), remember.checked ? key.value.trim() : null))
      storageNotice.textContent = remember.checked ? '已记住此服务的导入密钥。' : '已取消记住；当前页面的输入仍保留。';
    else remember.checked = false;
  }
  service.addEventListener('input', () => {
    if (busy) return;
    syncService();
    operation = null; copy.disabled = true; copyText.hidden = true; copyNotice.textContent = ''; elapsed.textContent = '';
    cookieInfo.textContent = '尚未读取 Cookie。';
    setStatus('服务地址已更改，请检查服务或开始导入。');
  });
  service.addEventListener('change', () => {
    const origin = syncService();
    if (origin && !busy) { service.value = origin; writeSetting('serverURL', origin); }
  });
  remember.addEventListener('change', saveKey);
  key.addEventListener('change', () => { if (remember.checked && !busy) saveKey(); });
  forget.addEventListener('click', () => {
    if (busy) return;
    const origin = syncService(); key.value = ''; remember.checked = false;
    if (origin && writeSetting(keyName(origin), null)) storageNotice.textContent = '此服务的导入密钥已清除。';
  });
  function updateControls() {
    for (const node of [service, key, remember, forget, mode, push, check]) node.disabled = busy;
    const all = mode.value === 'all';
    auth.disabled = busy || all; email.disabled = busy || all;
    modeHint.textContent = all ? '从第一个登录账号依次识别，受服务端扫描数量和时间上限限制。' : '自动跟随点击导入时的页面账号；必要时可在高级设置中指定。';
    push.textContent = busy ? '请稍候…' : all ? '读取并批量导入' : '读取并导入当前账号';
  }
  mode.addEventListener('change', updateControls);
  syncService(); updateControls();

  function place(position) {
    const view = window.visualViewport;
    const xOffset = view ? view.offsetLeft : 0, yOffset = view ? view.offsetTop : 0;
    const width = view ? view.width : window.innerWidth, height = view ? view.height : window.innerHeight;
    panel.style.maxHeight = Math.max(80, height - 16) + 'px';
    panel.style.width = Math.max(80, Math.min(360, width - 16)) + 'px';
    const rect = host.getBoundingClientRect();
    const valid = position && Number.isFinite(position.x) && Number.isFinite(position.y);
    const x = valid ? position.x : xOffset + width - rect.width - 16;
    const y = valid ? position.y : yOffset + height - rect.height - 16;
    host.style.left = Math.max(xOffset + 8, Math.min(x, xOffset + width - rect.width - 8)) + 'px';
    host.style.top = Math.max(yOffset + 8, Math.min(y, yOffset + height - rect.height - 8)) + 'px';
  }
  function position() { return { x: parseFloat(host.style.left), y: parseFloat(host.style.top) }; }
  function setOpen(open, persist = true) {
    const previous = position();
    panel.hidden = !open; bubble.hidden = open;
    if (persist) writeSetting('panelOpen', open);
    place(previous);
    if (persist) (open ? panel : bubble).focus({ preventScroll: true });
  }
  setOpen(readSetting('panelOpen', true) !== false, false);
  place(readSetting('panelPosition', null));
  collapse.addEventListener('click', () => setOpen(false));
  let drag = null, suppressClick = false;
  for (const handle of [header, bubble]) handle.addEventListener('pointerdown', event => {
    if (event.button !== 0 || (handle === header && event.target.closest('button'))) return;
    drag = { id: event.pointerId, x: event.clientX, y: event.clientY, start: position(), moved: false, handle };
    suppressClick = false;
    event.preventDefault();
  });
  window.addEventListener('pointermove', event => {
    if (!drag || drag.id !== event.pointerId) return;
    const dx = event.clientX - drag.x, dy = event.clientY - drag.y;
    if (Math.abs(dx) + Math.abs(dy) > 6) drag.moved = true;
    if (drag.moved) { place({ x: drag.start.x + dx, y: drag.start.y + dy }); event.preventDefault(); }
  }, { passive: false });
  function endDrag(event) {
    if (!drag || drag.id !== event.pointerId) return;
    suppressClick = drag.moved && drag.handle === bubble;
    if (drag.moved) writeSetting('panelPosition', position());
    drag = null;
  }
  window.addEventListener('pointerup', endDrag);
  window.addEventListener('pointercancel', endDrag);
  bubble.addEventListener('click', () => {
    if (suppressClick) { suppressClick = false; return; }
    setOpen(true);
  });
  const resize = () => place(position());
  window.addEventListener('resize', resize);
  if (window.visualViewport) {
    window.visualViewport.addEventListener('resize', resize);
    window.visualViewport.addEventListener('scroll', resize);
  }
  if (typeof ResizeObserver === 'function') new ResizeObserver(resize).observe(panel);
  if (typeof GM_registerMenuCommand === 'function') GM_registerMenuCommand('显示 ais2api 导入面板', () => setOpen(true));

  const failureMessages = Object.freeze({
    cookie_browser_not_configured: '后端未配置浏览器，请在程序目录执行 ais2api doctor。',
    cookie_browser_launch: '后端浏览器启动失败，请在程序目录执行 ais2api doctor。',
    cookie_browser_timeout: '后端浏览器等待超时，请检查网络连接与 AI Studio 是否可访问。',
    cookie_browser_disconnected: '后端浏览器连接断开，请检查服务日志。',
    cookie_browser_incompatible: '后端未识别到已登录的 AI Studio 页面，请检查网络及网页适配情况。',
    cookie_import_disabled: '服务端未启用 Cookie 导入，请在服务配置中设置导入密钥。',
    cookie_import_authentication: '导入密钥错误。请填写 cookie_import_key，不是管理密码或生成接口的 api_key。',
    cookie_not_authenticated: 'Cookie 可能已过期或缺少认证项。请重新登录 AI Studio，并检查管理器的 Cookie 读取权限。',
    identity_unverified: '无法确认账号身份，请检查当前页面账号、高级设置中的索引或预期邮箱。',
    cookie_region_blocked: '当前账号或服务地区无法访问 AI Studio。',
    cookie_import_timeout: '服务端身份验证超时，请先核对账号列表，再决定是否重试。',
    queue_timeout: '导入排队超时，请稍后重试。',
    queue_full: '服务端导入队列已满，请稍后重试。',
    cookie_import_limit: 'Cookie 条数或数据大小超过服务端限制。',
    cookie_import_origin: '服务端拒绝此来源，请检查导入服务的来源配置。'
  });
  function collect(currentURL) {
    return new Promise((resolve, reject) => {
      let complete = false;
      const fallback = () => {
        if (complete) return;
        complete = true; clearTimeout(timer);
        // Header compatibility mode cannot recover cookie scope or attributes.
        try { resolve({ cookies: document.cookie }); }
        catch (_) { reject(new Error('cookie read failed')); }
      };
      const timer = setTimeout(fallback, 4000);
      try {
        GM_cookie.list({ url: currentURL }, (records, error) => {
          if (complete) return;
          try {
            if (error || !Array.isArray(records)) { fallback(); return; }
            const cookies = []; let partitioned = 0; let expired = 0;
            for (const record of records) {
              if (!record || typeof record !== 'object') continue;
              if (record.partitionKey != null || record.partitionKeyOpaque === true || record.partitioned === true || record.firstPartyDomain) { partitioned++; continue; }
              const expiry = record.expirationDate ?? record.expiry ?? record.expires;
              if (typeof expiry === 'number' && expiry > 0 && expiry <= Date.now() / 1000) { expired++; continue; }
              if (typeof record.name !== 'string' || typeof record.value !== 'string') continue;
              // Explicit fields avoid transferring manager-internal metadata.
              const out = { name: record.name, value: record.value, domain: record.domain || location.hostname, path: record.path || '/' };
              if (record.hostOnly === true) out.domain = out.domain.replace(/^\.+/, '');
              for (const property of ['secure', 'httpOnly'])
                if (record[property] !== undefined) out[property] = record[property];
              const sameSites = { strict: 'Strict', lax: 'Lax', none: 'None', no_restriction: 'None' };
              const sameSite = String(record.sameSite).toLowerCase();
              if (Object.prototype.hasOwnProperty.call(sameSites, sameSite)) out.sameSite = sameSites[sameSite];
              if (record.session !== true && typeof expiry === 'number' && Number.isFinite(expiry)) out.expires = expiry;
              cookies.push(out);
            }
            complete = true; clearTimeout(timer);
            resolve({ cookies, partitioned, expired });
          } catch (_) { fallback(); }
        });
      } catch (_) { fallback(); }
    });
  }
  function requestJSON(url, token, payload, timeout) {
    return new Promise(resolve => {
      let request;
      const done = result => { clearTimeout(timer); resolve(result); };
      // Tampermonkey's fetch mode may ignore its own timeout option.
      const timer = setTimeout(() => {
        done({ kind: 'unconfirmed', code: 'timeout' });
        try { if (request) request.abort(); } catch (_) { /* Outcome remains unconfirmed. */ }
      }, timeout);
      const headers = payload ? { 'Content-Type': 'application/json', Authorization: 'Bearer ' + token } : {};
      try {
        request = GM_xmlhttpRequest({
          method: payload ? 'POST' : 'GET', url, anonymous: true, redirect: 'error', timeout, headers,
          ...(payload ? { data: JSON.stringify(payload) } : {}),
          onload: response => {
            const http = Number.isInteger(response.status) && response.status >= 100 && response.status <= 599 ? response.status : 0;
            if (response.finalUrl && response.finalUrl !== url) { done({ kind: 'unconfirmed', code: 'redirect', http }); return; }
            try {
              if (typeof response.responseText !== 'string' || response.responseText.length > 1024 * 1024) throw new Error('invalid response');
              const data = JSON.parse(response.responseText);
              if (http !== 200) {
                const reason = data && data.error && data.error.reason;
                if (typeof reason === 'string' && Object.prototype.hasOwnProperty.call(failureMessages, reason))
                  done({ kind: 'rejected', code: reason, http });
                else done({ kind: 'unconfirmed', code: 'http_error', http });
                return;
              }
              done({ kind: 'ok', data, http });
            } catch (_) { done({ kind: 'unconfirmed', code: 'invalid_response', http }); }
          },
          onerror: () => done({ kind: 'unconfirmed', code: 'network' }),
          ontimeout: () => done({ kind: 'unconfirmed', code: 'timeout' }),
          onabort: () => done({ kind: 'unconfirmed', code: 'aborted' })
        });
      } catch (_) { done({ kind: 'unconfirmed', code: 'request_error' }); }
    });
  }
  function validResult(result) {
    if (!result || !Number.isSafeInteger(result.imported_count) || result.imported_count < 1 || result.imported_count > 32 ||
        !Array.isArray(result.accounts) || result.accounts.length !== result.imported_count ||
        !result.accounts.every(account => account && ['id', 'name', 'email'].every(field => typeof account[field] === 'string' && account[field].length > 0 && account[field].length <= 4096)) ||
        result.verification !== 'browser_identity_only' || result.generation_verified !== false) return false;
    const details = result.details;
    if (details === undefined) return true;
    if (!details || typeof details !== 'object' || Array.isArray(details)) return false;
    if (details.truncated !== undefined && typeof details.truncated !== 'boolean') return false;
    if (details.verified !== undefined && details.verified !== result.imported_count) return false;
    return ['attempted', 'verified', 'skipped_host_cookies', 'skipped_partition_cookies'].every(field =>
      details[field] === undefined || (Number.isSafeInteger(details[field]) && details[field] >= 0));
  }
  function updateElapsed() {
    if (operation) elapsed.textContent = (busy ? '已等待 ' : '用时 ') + Math.max(0, Math.floor((Date.now() - operation.started) / 1000)) + ' 秒';
  }
  function begin(action) {
    operation = { action, phase: 'started', code: '', started: Date.now(), duration_ms: 0, http: 0 };
    busy = true; accounts.replaceChildren(); copyText.hidden = true; copyNotice.textContent = ''; copy.disabled = true;
    cookieInfo.textContent = action === 'health' ? '此检查不读取 Cookie。' : '尚未读取 Cookie。';
    updateControls(); updateElapsed(); ticker = setInterval(updateElapsed, 1000);
  }
  function finish() {
    clearInterval(ticker); ticker = null;
    operation.duration_ms = Math.max(0, Date.now() - operation.started);
    busy = false;
    copy.disabled = false; updateControls(); updateElapsed(); resize();
  }
  function notSent(action, code, message) {
    operation = { action, phase: 'not_sent', code, started: Date.now(), duration_ms: 0, http: 0 };
    accounts.replaceChildren(); copy.disabled = false; copyText.hidden = true; copyNotice.textContent = '';
    cookieInfo.textContent = '尚未读取 Cookie。'; updateElapsed(); setStatus(message, 'error');
  }
  function destination(action) {
    const origin = syncService();
    if (!origin) { notSent(action, 'invalid_server', '请输入完整 HTTPS 服务地址，或本机地址，例如 127.0.0.1:18451；不要附带路径、查询或密码。'); return null; }
    service.value = origin; writeSetting('serverURL', origin);
    return origin;
  }
  check.addEventListener('click', async () => {
    if (busy) return;
    const origin = destination('health'); if (!origin) return;
    begin('health'); operation.phase = 'checking_service';
    setStatus('正在检查服务连接…');
    try {
      const result = await requestJSON(origin + '/health', '', null, 10000);
      operation.http = result.http || 0;
      if (result.kind === 'ok' && result.data && result.data.status === 'alive') {
        operation.phase = 'complete'; operation.code = 'service_alive';
        setStatus('服务可连接。导入密钥和 Google 账号会在实际导入时校验。', 'success');
      } else {
        operation.phase = 'failed'; operation.code = result.code || 'invalid_response';
        setStatus('未能确认服务可连接。请检查服务是否启动、地址及管理器的跨域连接权限。此检查没有读取 Cookie。', 'error');
      }
    } catch (_) {
      operation.phase = 'failed'; operation.code = 'unexpected';
      setStatus('连接检查出现异常，请复制诊断摘要并检查脚本管理器。', 'error');
    } finally { finish(); }
  });
  push.addEventListener('click', async () => {
    if (busy) return;
    const origin = destination('import'); if (!origin) return;
    const token = key.value.trim();
    if (!validKey(token)) { notSent('import', 'invalid_key', '请填写有效的导入密钥；不要使用管理密码或 API 密钥。'); return; }
    const all = mode.value === 'all', selected = auth.value.trim();
    if (!all && selected && !/^\d{1,10}$/.test(selected)) { notSent('import', 'invalid_index', '账号索引须为最多 10 位的非负整数，留空可自动识别。'); return; }
    if (!all && email.value.trim().length > 320) { notSent('import', 'invalid_email', '预期邮箱过长，请检查输入。'); return; }
    const currentURL = location.href;
    const payload = all ? {} : { auth_user: selected || inferUser(currentURL) };
    if (!all && email.value.trim()) payload.email = email.value.trim();
    if (remember.checked) saveKey();
    begin('import'); operation.phase = 'reading_cookies';
    setStatus('正在读取当前登录 Cookie…'); cookieInfo.textContent = '正在读取 Cookie。';
    let dispatched = false;
    try {
      const gathered = await collect(currentURL);
      const structured = Array.isArray(gathered.cookies);
      const count = structured ? gathered.cookies.length : gathered.cookies.trim() ? gathered.cookies.split(';').filter(pair => pair.trim()).length : 0;
      cookieInfo.textContent = !structured
        ? '来源：document.cookie。域、路径、有效期和安全属性未知，无法判断分区，不包含 HttpOnly Cookie；不能保证认证完整。'
        : '来源：GM_cookie；读取 ' + count + ' 项；排除分区 ' + gathered.partitioned + ' 项、过期 ' + gathered.expired + ' 项。管理器仍可能限制 HttpOnly 读取。';
      if (!count) {
        operation.phase = 'not_sent'; operation.code = 'no_cookies';
        setStatus('没有可导入 Cookie，请先登录 AI Studio 并检查管理器权限。尚未发送请求，密钥已保留。', 'error'); return;
      }
      payload.cookies = gathered.cookies;
      operation.phase = 'waiting_for_service';
      setStatus('已读取 ' + count + ' 项，等待服务校验账号…' + (!structured ? '\n当前读取能力受限，详情中可查看原因。' : ''));
      dispatched = true;
      const result = await requestJSON(origin + '/api/accounts/probe-import', token, payload, 300000);
      operation.http = result.http || 0;
      if (result.kind === 'ok' && validResult(result.data)) {
        operation.phase = 'complete'; operation.code = 'imported';
        const data = result.data;
        for (const account of data.accounts) {
          const row = element('li', '', accounts);
          element('strong', account.email, row); element('span', account.name, row).className = 'muted';
        }
        const details = data.details || {};
        const extra = details.truncated ? '\n已达到服务端扫描上限，可能还有账号未被识别。' : '';
        setStatus('成功导入 / 更新 ' + data.imported_count + ' 个账号。' + extra, 'success');
        for (const [field, label] of [['skipped_host_cookies', '服务端跳过来源未知项'], ['skipped_partition_cookies', '服务端跳过分区项']])
          if (details[field]) cookieInfo.textContent += ' ' + label + '：' + details[field] + '。';
      } else if (result.kind === 'rejected') {
        operation.phase = 'rejected'; operation.code = result.code;
        setStatus(failureMessages[result.code] + '\n密钥输入已保留。', 'error');
      } else {
        operation.phase = 'unconfirmed'; operation.code = result.code || 'invalid_response';
        setStatus('未能确认导入结果，可能已经导入。请先打开账号列表核对，再决定是否重试；也可复制诊断摘要。', 'unconfirmed');
      }
    } catch (_) {
      operation.phase = dispatched ? 'unconfirmed' : 'not_sent'; operation.code = 'unexpected';
      setStatus(dispatched ? '导入结果尚未确认，请先核对账号列表并复制诊断摘要。' : '读取 Cookie 时出现异常，尚未发送请求。请检查管理器权限，密钥已保留。', 'error');
    } finally { finish(); }
  });
  copy.addEventListener('click', async () => {
    if (!operation || busy) return;
    // Only locally assigned, allowlisted fields leave this panel.
    const report = JSON.stringify({ script_version: VERSION, action: operation.action, phase: operation.phase,
      code: operation.code, http_status: operation.http, duration_ms: operation.duration_ms }, null, 2);
    copyText.value = report;
    try {
      await navigator.clipboard.writeText(report);
      copyText.hidden = true; copyNotice.textContent = '已复制。摘要不含 Cookie、密钥、邮箱或服务端原文。';
    } catch (_) {
      copyText.hidden = false; copyText.focus(); copyText.select();
      copyNotice.textContent = '自动复制不可用，请复制下方已选中的摘要。';
    }
  });
})();
