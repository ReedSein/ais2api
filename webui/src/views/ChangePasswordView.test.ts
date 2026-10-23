// Forced replacement of the default management password: the service refuses
// every management route except /auth/check and /auth/password until it changes.
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { createMemoryHistory, createRouter, type Router } from 'vue-router'
import ChangePasswordView from './ChangePasswordView.vue'
import { api } from '../api/client'
import { session } from '../stores/session'
import { toasts } from '../stores/ui'

let forced: boolean
let passwordResult: { status: number; reason?: string }
let calls: { method: string; path: string; auth: string; body?: unknown }[]

const json = (body: unknown, status = 200) => new Response(JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json' } })
const error = (status: number, reason: string) => json({ error: { code: status, message: 'synthetic', status: 'FAILED_PRECONDITION', reason } }, status)

function fakeFetch(input: RequestInfo | URL, init: RequestInit = {}): Promise<Response> {
  const path = new URL(String(input), 'http://console.test').pathname.replace('/api/admin/v1', '')
  const method = init.method ?? 'GET'
  const auth = new Headers(init.headers).get('Authorization') ?? ''
  calls.push({ method, path, auth, body: init.body ? JSON.parse(String(init.body)) : undefined })
  if (method === 'GET' && path === '/auth/check') return Promise.resolve(json({ authenticated: true, password_change_required: forced }))
  if (method === 'POST' && path === '/auth/password')
    return Promise.resolve(passwordResult.status === 200 ? json({ changed: true }) : error(passwordResult.status, passwordResult.reason ?? ''))
  if (forced) return Promise.resolve(error(403, 'admin_password_change_required'))
  return Promise.resolve(json({}))
}

let wrapper: VueWrapper | undefined
let router: Router

async function mountView() {
  router = createRouter({
    history: createMemoryHistory(),
    routes: [
      { path: '/', name: 'dashboard', component: { template: '<p>dashboard</p>' } },
      { path: '/login', name: 'login', component: { template: '<p>login</p>' } },
      { path: '/change-password', name: 'change-password', component: ChangePasswordView },
    ],
  })
  await router.push('/change-password')
  await router.isReady()
  wrapper = mount(ChangePasswordView, { attachTo: document.body, global: { plugins: [router] } })
  await flushPromises()
  return wrapper
}

async function fill(next: string, confirm = next) {
  await wrapper!.find('#new-password').setValue(next)
  await wrapper!.find('#confirm-password').setValue(confirm)
}
const submitButton = () => wrapper!.find<HTMLButtonElement>('button[type="submit"]')

beforeEach(() => {
  forced = true
  passwordResult = { status: 200 }
  calls = []
  toasts.splice(0)
  session.baseUrl = ''
  session.adminKey = 'admin'
  session.remember = false
  session.mustChangePassword = true
  vi.stubGlobal('fetch', vi.fn(fakeFetch))
})

afterEach(() => {
  wrapper?.unmount()
  wrapper = undefined
  document.body.innerHTML = ''
  sessionStorage.clear()
  localStorage.clear()
  vi.unstubAllGlobals()
})

describe('client', () => {
  it('flags the session when the service requires a password change', async () => {
    session.mustChangePassword = false
    await expect(api.get('/status')).rejects.toMatchObject({ status: 403, reason: 'admin_password_change_required' })
    expect(session.mustChangePassword).toBe(true)
    expect(session.adminKey).toBe('admin')
  })
})

describe('forced password change', () => {
  it('only offers changing the password or signing out', async () => {
    await mountView()
    expect(wrapper!.text()).toContain('正在使用默认密码')
    expect(wrapper!.text()).toContain('退出登录')
    expect(wrapper!.text()).not.toContain('取消')
  })

  it('enables submission only for an acceptable new password', async () => {
    await mountView()
    expect(submitButton().element.disabled).toBe(true)
    for (const [next, confirm] of [
      ['admin', 'admin'],
      ['short', 'short'],
      ['has space1', 'has space1'],
      ['long-enough-1', 'long-enough-2'],
    ]) {
      await fill(next, confirm)
      expect(submitButton().element.disabled, next).toBe(true)
    }
    await fill('long-enough-1')
    expect(submitButton().element.disabled).toBe(false)
  })

  it('switches the session to the new password and leaves the forced page', async () => {
    await mountView()
    await fill('new-admin-password')
    await submitButton().trigger('submit')
    await flushPromises()
    const change = calls.find((c) => c.method === 'POST' && c.path === '/auth/password')
    expect(change?.body).toEqual({ password: 'new-admin-password' })
    expect(change?.auth).toBe('Bearer admin')
    expect(session.adminKey).toBe('new-admin-password')
    expect(session.mustChangePassword).toBe(false)
    expect(JSON.parse(sessionStorage.getItem('ais2api.session') ?? '{}').adminKey).toBe('new-admin-password')
    expect(router.currentRoute.value.name).toBe('dashboard')
    expect(toasts.map((t) => t.title)).toContain('管理密码已修改')
  })

  it('keeps the old session and shows the reason when the service rejects the password', async () => {
    passwordResult = { status: 400, reason: 'admin_password_invalid' }
    await mountView()
    await fill('rejected-by-server')
    await submitButton().trigger('submit')
    await flushPromises()
    expect(wrapper!.text()).toContain('新密码不符合要求')
    expect(session.adminKey).toBe('admin')
    expect(session.mustChangePassword).toBe(true)
    expect(router.currentRoute.value.name).toBe('change-password')
  })

  it('explains that the default password is limited to local networks', async () => {
    passwordResult = { status: 403, reason: 'admin_default_password_remote' }
    await mountView()
    await fill('new-admin-password')
    await submitButton().trigger('submit')
    await flushPromises()
    expect(wrapper!.text()).toContain('只能在本机或局域网内使用')
  })
})

describe('voluntary password change', () => {
  it('reads the state from the service and allows cancelling', async () => {
    forced = false
    session.adminKey = 'current-password'
    session.mustChangePassword = false
    await mountView()
    expect(calls.some((c) => c.path === '/auth/check')).toBe(true)
    expect(wrapper!.text()).not.toContain('正在使用默认密码')
    expect(wrapper!.text()).toContain('取消')
    await fill('current-password')
    expect(submitButton().element.disabled).toBe(true)
  })

  it('restores the forced state after a reload', async () => {
    session.mustChangePassword = false
    await mountView()
    expect(session.mustChangePassword).toBe(true)
    expect(wrapper!.text()).toContain('正在使用默认密码')
  })
})
