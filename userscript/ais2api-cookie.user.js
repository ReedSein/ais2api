// ==UserScript==
// @name         ais2api Cookie Import
// @namespace    https://github.com/mananekoha114/ais2api
// @version      1.0.1
// @description  Explicitly import the current AI Studio browser identity into ais2api.
// @author       Mana Nekoha
// @license      MPL-2.0
// @match        https://aistudio.google.com/*
// @grant        GM_cookie
// @grant        GM_xmlhttpRequest
// @grant        GM_registerMenuCommand
// @grant        GM_getValue
// @grant        GM_setValue
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
  if (location.protocol !== 'https:' || location.hostname !== 'aistudio.google.com') return;
  const currentURL = location.href;
  function serverURL(value) {
    const url = new URL(value);
    if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password ||
        url.search || url.hash || url.pathname !== '/') throw new Error('invalid server');
    if (url.protocol === 'http:' && !['localhost', '127.0.0.1', '[::1]'].includes(url.hostname))
      throw new Error('HTTP requires loopback');
    return url.origin;
  }
  function inferUser(value) {
    const url = new URL(value);
    const candidate = url.searchParams.get('authuser') ?? (url.pathname.match(/\/u\/(\d+)(?:\/|$)/) || [])[1] ?? '0';
    return /^\d+$/.test(candidate) ? candidate : '0';
  }
  const host = document.createElement('div');
  const root = host.attachShadow({ mode: 'closed' });
  const style = document.createElement('style');
  style.textContent = ':host{position:fixed;right:16px;bottom:16px;z-index:2147483647}section{background:#202124;color:#fff;padding:16px;border:1px solid #777;border-radius:10px;font:14px sans-serif;width:300px}label,input,button{display:block;box-sizing:border-box;width:100%;margin:7px 0}input,button{padding:7px}p{white-space:pre-wrap;overflow-wrap:anywhere}';
  root.appendChild(style);
  const panel = document.createElement('section');
  root.appendChild(panel);
  function element(tag, text) { const node = document.createElement(tag); node.textContent = text; panel.appendChild(node); return node; }
  element('strong', 'ais2api · 当前浏览器身份导入');
  function field(label, type, value) {
    const wrap = element('label', label);
    const input = document.createElement('input'); input.type = type; input.value = value;
    input.autocomplete = 'off'; wrap.appendChild(input); return input;
  }
  const saved = typeof GM_getValue === 'function' ? GM_getValue('serverURL', 'http://127.0.0.1:18451') : 'http://127.0.0.1:18451';
  const service = field('服务地址（仅 origin）', 'text', saved);
  const key = field('cookie_import_key（不保存）', 'password', '');
  const auth = field('当前账号索引 auth_user', 'text', inferUser(currentURL));
  const email = field('期望邮箱（可选，不保存）', 'text', '');
  element('p', '仅在点击后读取 Cookie。身份验证不代表生成请求已通过。管理器可能无法读取 HttpOnly Cookie。');
  const push = element('button', '读取并导入当前账号');
  const status = element('p', '尚未读取 Cookie 或发送请求。');
  const hide = element('button', '隐藏');
  hide.addEventListener('click', () => { host.hidden = true; });
  (document.body || document.documentElement).appendChild(host);
  if (typeof GM_registerMenuCommand === 'function') GM_registerMenuCommand('显示 ais2api 导入面板', () => { host.hidden = false; });
  let busy = false;
  const failureMessages = Object.freeze({
    cookie_browser_not_configured: '后端未配置浏览器，请在程序目录执行 ais2api doctor。',
    cookie_browser_launch: '后端浏览器启动失败，请在程序目录执行 ais2api doctor。',
    cookie_browser_timeout: '后端浏览器等待超时，请检查网络连接与 AI Studio 是否可访问。',
    cookie_browser_disconnected: '后端浏览器连接断开，请检查服务日志后重试。',
    cookie_browser_incompatible: '后端未识别到已登录的 AI Studio 页面，请检查网络及网页适配情况。',
    cookie_import_disabled: '服务端未启用 Cookie 导入，请配置 cookie_import_key。',
    cookie_import_authentication: '导入密钥错误，请填写 cookie_import_key（不是生成接口的 api_key）。',
    cookie_not_authenticated: 'Cookie 未通过登录验证，可能已过期或缺少认证 Cookie，请重新登录并检查管理器权限。',
    identity_unverified: '无法确认账号身份，请检查当前账号索引、期望邮箱及 Cookie 完整性。',
    cookie_region_blocked: '当前账号或服务地区无法访问 AI Studio。',
    cookie_import_timeout: '服务端身份验证超时，请稍后重试。',
    queue_timeout: '导入排队超时，请稍后重试。',
    queue_full: '服务端导入队列已满，请稍后重试。',
    cookie_import_limit: 'Cookie 条数或数据大小超过服务端限制。',
    cookie_import_origin: '服务端拒绝此来源，请检查导入服务的来源配置。'
  });
  function collect() {
    return new Promise(resolve => {
      let complete = false;
      const finish = (records, source) => {
        if (complete) return; complete = true; clearTimeout(timer);
        const cookies = []; let partitioned = 0; let expired = 0;
        for (const record of records) {
          if (record.partitionKey != null || record.partitionKeyOpaque === true || record.partitioned === true || record.firstPartyDomain) { partitioned++; continue; }
          const expiry = record.expirationDate ?? record.expiry ?? record.expires;
          if (typeof expiry === 'number' && expiry > 0 && expiry <= Date.now() / 1000) { expired++; continue; }
          if (typeof record.name !== 'string' || typeof record.value !== 'string') continue;
          // Explicit fields avoid transferring manager-internal metadata.
          const out = { name: record.name, value: record.value, domain: record.domain || location.hostname, path: record.path || '/' };
          if (record.hostOnly === true) out.domain = out.domain.replace(/^\.+/, '');
          for (const property of ['secure', 'httpOnly'])
            if (record[property] !== undefined) out[property] = record[property];
          const sameSite = { strict: 'Strict', lax: 'Lax', none: 'None', no_restriction: 'None' }[String(record.sameSite).toLowerCase()];
          if (sameSite) out.sameSite = sameSite;
          if (record.session !== true && typeof expiry === 'number' && Number.isFinite(expiry)) out.expires = expiry;
          cookies.push(out);
        }
        resolve({ cookies, partitioned, expired, source });
      };
      const fallback = () => {
        if (complete) return;
        complete = true; clearTimeout(timer);
        // Header compatibility mode cannot recover cookie scope or attributes.
        resolve({ cookies: document.cookie, source: 'document.cookie', metadataUnknown: true });
      };
      const timer = setTimeout(fallback, 4000);
      try {
        if (typeof GM_cookie === 'undefined' || typeof GM_cookie.list !== 'function') { fallback(); return; }
        GM_cookie.list({ url: currentURL }, (records, error) => {
          if (complete) return;
          if (error || !Array.isArray(records)) fallback(); else finish(records, 'GM_cookie');
        });
      } catch (_) { fallback(); }
    });
  }
  function send(url, token, payload) {
    return new Promise((resolve, reject) => {
      let settled = false; let request;
      const done = (ok, value) => {
        if (settled) return; settled = true; clearTimeout(timer);
        if (ok) resolve(value);
        else {
          const failure = new Error('request failed');
          if (Object.prototype.hasOwnProperty.call(failureMessages, value)) failure.reason = value;
          reject(failure);
        }
      };
      const timer = setTimeout(() => { done(false); try { if (request) request.abort(); } catch (_) {} }, 300000);
      try {
        request = GM_xmlhttpRequest({ method: 'POST', url: url + '/api/accounts/probe-import',
          anonymous: true, redirect: 'error', timeout: 300000,
          headers: { 'Content-Type': 'application/json', Authorization: 'Bearer ' + token },
          data: JSON.stringify(payload),
          onload: response => {
            if (response.finalUrl && response.finalUrl !== url + '/api/accounts/probe-import') { done(false); return; }
            try {
              const result = JSON.parse(response.responseText);
              if (response.status !== 200) { done(false, result && result.error && result.error.reason); return; }
              if (!Number.isSafeInteger(result.imported_count) || result.imported_count < 1 || result.imported_count > 32 ||
                  !Array.isArray(result.accounts) || result.accounts.length !== result.imported_count ||
                  !result.accounts.every(account => account && ['id', 'name', 'email'].every(field => typeof account[field] === 'string' && account[field].length > 0)) ||
                  result.verification !== 'browser_identity_only' || result.generation_verified !== false) { done(false); return; }
              done(true, result.imported_count);
            } catch (_) { done(false); }
          }, onerror: () => done(false), ontimeout: () => done(false), onabort: () => done(false)
        });
      } catch (_) { done(false); }
    });
  }
  push.addEventListener('click', async () => {
    if (busy) return;
    let url;
    try { url = serverURL(service.value.trim()); } catch (_) { status.textContent = '服务地址无效。仅允许无路径、查询、用户信息的 HTTPS origin 或本机 HTTP origin。'; return; }
    if (!key.value.trim()) { status.textContent = '请填写 cookie_import_key。'; return; }
    if (!/^\d+$/.test(auth.value.trim())) { status.textContent = '账号索引必须是非负整数。'; return; }
    busy = true; push.disabled = true;
    const token = key.value.trim(); key.value = '';
    let counts = '';
    try {
      const gathered = await collect();
      counts = gathered.metadataUnknown
        ? '来源：document.cookie（有损 Cookie 头）；域、路径、有效期与安全属性未知，无法判断分区，且不含 HttpOnly，不能保证完整认证。'
        : `来源：${gathered.source}；排除分区 ${gathered.partitioned}；过期 ${gathered.expired}。`;
      status.textContent = counts + ' 正在验证浏览器身份…';
      if (!gathered.cookies.length) { status.textContent = counts + ' 没有可导入 Cookie。'; return; }
      const payload = { cookies: gathered.cookies, auth_user: auth.value.trim() };
      if (email.value.trim()) payload.email = email.value.trim();
      if (typeof GM_setValue === 'function') { GM_setValue('serverURL', url); GM_setValue('auth_user', payload.auth_user); }
      const count = await send(url, token, payload);
      status.textContent = counts + ` 已导入 ${count} 个账号。仅验证浏览器身份，未验证生成能力。`;
    } catch (failure) {
      const message = failure && Object.prototype.hasOwnProperty.call(failureMessages, failure.reason)
        ? failureMessages[failure.reason]
        : '导入失败或超时。请检查服务、导入密钥与浏览器账号；响应内容不会显示。';
      status.textContent = counts + ' ' + message;
    }
    finally { busy = false; push.disabled = false; }
  });
})();
