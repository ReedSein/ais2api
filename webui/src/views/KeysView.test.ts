// Interaction tests for the config-key effective state (frontend handoff
// 2026-10-23): PUT/DELETE /keys/config succeed with HTTP 200 whether or not the
// running service applied the change, so the UI must follow ConfigSaved.
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import KeysView from './KeysView.vue'
import { session } from '../stores/session'
import { confirmState, settleConfirm, toasts } from '../stores/ui'

interface Backend {
  restartRequired: boolean
  configKeyConfigured: boolean
  /** Response for PUT/DELETE /keys/config: a ConfigSaved body or an HTTP error. */
  configKeyResult: { applied: boolean } | { status: number; reason: string }
}

let backend: Backend
let calls: { method: string; path: string; body?: unknown }[]

const json = (body: unknown, status = 200) =>
  new Response(JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json' } })

function fakeFetch(input: RequestInfo | URL, init: RequestInit = {}): Promise<Response> {
  const url = new URL(String(input), 'http://console.test')
  const path = url.pathname.replace('/api/admin/v1', '')
  const method = init.method ?? 'GET'
  calls.push({ method, path, body: init.body ? JSON.parse(String(init.body)) : undefined })
  if (method === 'GET' && path === '/keys')
    return Promise.resolve(
      json({
        source: 'config_and_managed',
        configured: true,
        config_key_configured: backend.configKeyConfigured,
        keys: [
          { id: 'config', name: 'Configured API key', source: 'config' },
          { id: 'managed_1', name: 'SillyTavern', created: 1792646460, source: 'managed' },
        ],
      }),
    )
  if (method === 'GET' && path === '/config')
    return Promise.resolve(
      json({
        config: { port: 18451, api_key: '***' },
        ...(backend.restartRequired ? { saved_config: { port: 18471, api_key: '***' } } : {}),
        config_file: 'config.json',
        restart_required: backend.restartRequired,
      }),
    )
  if ((method === 'PUT' || method === 'DELETE') && path === '/keys/config') {
    const result = backend.configKeyResult
    if ('status' in result)
      return Promise.resolve(json({ error: { code: result.status, message: 'synthetic failure', status: 'INTERNAL', reason: result.reason } }, result.status))
    // Mirror the backend: a pending (restart-only) config keeps the change unapplied.
    if (result.applied && method === 'DELETE') backend.configKeyConfigured = false
    return Promise.resolve(json({ saved: true, applied: result.applied, restart_required: !result.applied }))
  }
  if (method === 'DELETE' && path === '/keys/managed_1') return Promise.resolve(json({ ok: true }))
  return Promise.resolve(json({ error: { code: 404, message: 'unexpected route', status: 'NOT_FOUND', reason: 'route_not_found' } }, 404))
}

let wrapper: VueWrapper | undefined

async function mountView() {
  wrapper = mount(KeysView, { attachTo: document.body, global: { stubs: { RouterLink: { template: '<a><slot /></a>' } } } })
  await flushPromises()
  return wrapper
}

const page = () => document.body.textContent ?? ''
const dialog = () => document.body.querySelector<HTMLElement>('.dialog')
const button = (root: ParentNode, text: string) => {
  const found = [...root.querySelectorAll<HTMLButtonElement>('button')].find((b) => b.textContent?.trim().includes(text))
  if (!found) throw new Error(`button not found: ${text}`)
  return found
}
const toastTitles = () => toasts.map((t) => `${t.severity}:${t.title}`)

async function rotate() {
  button(document.body, '轮换').click()
  await flushPromises()
  button(dialog()!, '保存').click()
  await flushPromises()
}

async function clear(accept = true) {
  document.body.querySelector<HTMLButtonElement>('[aria-label="清除配置密钥"]')!.click()
  await flushPromises()
  expect(confirmState.open).toBe(true)
  const message = confirmState.request?.message ?? ''
  settleConfirm(accept)
  await flushPromises()
  return message
}

beforeEach(() => {
  backend = { restartRequired: false, configKeyConfigured: true, configKeyResult: { applied: true } }
  calls = []
  toasts.splice(0)
  session.adminKey = 'synthetic-admin-key'
  session.baseUrl = ''
  vi.stubGlobal('fetch', vi.fn(fakeFetch))
})

afterEach(() => {
  wrapper?.unmount()
  wrapper = undefined
  document.body.innerHTML = ''
  vi.unstubAllGlobals()
})

describe('config key rotation', () => {
  it('reports an applied rotation as effective', async () => {
    await mountView()
    await rotate()
    expect(calls.some((c) => c.method === 'PUT' && c.path === '/keys/config')).toBe(true)
    expect(toastTitles()).toEqual(['success:配置密钥已更新并生效'])
    expect(dialog()?.textContent).toContain('配置密钥已生效')
    expect(dialog()?.textContent).toContain('旧配置密钥已失效')
    expect(dialog()?.textContent).not.toContain('尚未生效')
  })

  it('reports a pending rotation as saved but not effective', async () => {
    backend.restartRequired = true
    backend.configKeyResult = { applied: false }
    await mountView()
    expect(page()).toContain('有已保存但尚未生效的配置')
    await rotate()
    expect(toastTitles()).toEqual(['warning:配置密钥已保存，重启后生效'])
    const text = dialog()?.textContent ?? ''
    expect(text).toContain('配置密钥已保存，尚未生效')
    expect(text).toContain('重启前旧密钥继续有效，新密钥会返回 401')
    expect(text).toContain('新密钥（尚未生效）')
    expect(text).not.toContain('旧配置密钥已失效')
  })

  it('warns before saving when a restart is already pending', async () => {
    backend.restartRequired = true
    await mountView()
    button(document.body, '轮换').click()
    await flushPromises()
    expect(dialog()?.textContent).toContain('在重启服务前不会生效')
  })

  it('shows an error and no success on HTTP failure', async () => {
    backend.configKeyResult = { status: 500, reason: 'config_write' }
    await mountView()
    await rotate()
    expect(toastTitles()).toEqual(['error:更新配置密钥失败'])
    expect(dialog()?.textContent).toContain('轮换配置密钥')
    expect(dialog()?.textContent).not.toContain('已生效')
  })
})

describe('config key clearing', () => {
  it('reports an applied clear as effective', async () => {
    await mountView()
    const message = await clear()
    expect(message).not.toContain('立即失去访问权限')
    expect(message).toContain('以服务返回的状态为准')
    expect(calls.some((c) => c.method === 'DELETE' && c.path === '/keys/config')).toBe(true)
    expect(toastTitles()).toEqual(['success:配置密钥已清除并生效'])
    expect(page()).toContain('未配置')
  })

  it('does not claim revocation while a restart is pending', async () => {
    backend.restartRequired = true
    backend.configKeyResult = { applied: false }
    await mountView()
    const message = await clear()
    expect(message).toContain('在重启服务前不会生效，旧配置密钥在此期间仍可访问')
    expect(toastTitles()).toEqual(['warning:配置密钥已保存，重启后生效'])
    expect(toasts[0].message).toContain('重启前旧配置密钥仍然有效')
    // The running service still uses the old key, so it stays "effective".
    expect(page()).toContain('已生效')
    expect(page()).toContain('待重启')
  })

  it('sends nothing when the confirmation is cancelled', async () => {
    await mountView()
    await clear(false)
    expect(calls.some((c) => c.method === 'DELETE')).toBe(false)
    expect(toasts).toHaveLength(0)
  })

  it('shows an error and no success on HTTP failure', async () => {
    backend.configKeyResult = { status: 409, reason: 'invalid_state' }
    await mountView()
    await clear()
    expect(toastTitles()).toEqual(['error:清除配置密钥失败'])
  })
})

describe('managed keys', () => {
  it('keeps the immediate revoke behaviour', async () => {
    backend.restartRequired = true
    await mountView()
    ;(document.body.querySelector('.setting-control button[aria-haspopup="menu"]') as HTMLButtonElement).click()
    await flushPromises()
    button(document.body, '撤销密钥').click()
    await flushPromises()
    expect(confirmState.request?.message).toContain('立即无法访问')
    settleConfirm(true)
    await flushPromises()
    expect(calls.some((c) => c.method === 'DELETE' && c.path === '/keys/managed_1')).toBe(true)
    expect(toastTitles()).toEqual(['success:密钥已撤销'])
  })
})
