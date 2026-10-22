import { computed, reactive } from 'vue'

const STORAGE_KEY = 'ais2api.session'

interface Persisted {
  baseUrl: string
  adminKey: string
}

export const session = reactive({
  /** Backend origin, e.g. http://127.0.0.1:18451. Empty = same origin. */
  baseUrl: '',
  adminKey: '',
  remember: false,
  /** Set when the backend rejected the stored key, shown on the sign-in page. */
  expiredReason: '',
})

function load() {
  for (const [storage, remember] of [
    [localStorage, true],
    [sessionStorage, false],
  ] as const) {
    try {
      const raw = storage.getItem(STORAGE_KEY)
      if (!raw) continue
      const value = JSON.parse(raw) as Partial<Persisted>
      session.baseUrl = typeof value.baseUrl === 'string' ? value.baseUrl : ''
      session.adminKey = typeof value.adminKey === 'string' ? value.adminKey : ''
      session.remember = remember
      return
    } catch {
      storage.removeItem(STORAGE_KEY)
    }
  }
}
load()

// The last backend address is remembered even after signing out, so the
// sign-in form can prefill it.
const LAST_URL_KEY = 'ais2api.lastBaseUrl'
export const lastBaseUrl = (): string => localStorage.getItem(LAST_URL_KEY) ?? ''

export const isSignedIn = computed(() => session.adminKey.length > 0)

export function normalizeBaseUrl(value: string): string {
  const trimmed = value.trim()
  if (!trimmed) return ''
  const url = new URL(/^https?:\/\//i.test(trimmed) ? trimmed : `http://${trimmed}`)
  return url.origin
}

export function signIn(baseUrl: string, adminKey: string, remember: boolean) {
  session.baseUrl = baseUrl
  session.adminKey = adminKey
  session.remember = remember
  session.expiredReason = ''
  const payload = JSON.stringify({ baseUrl, adminKey } satisfies Persisted)
  localStorage.removeItem(STORAGE_KEY)
  sessionStorage.removeItem(STORAGE_KEY)
  ;(remember ? localStorage : sessionStorage).setItem(STORAGE_KEY, payload)
  localStorage.setItem(LAST_URL_KEY, baseUrl)
}

export function signOut(reason = '') {
  session.adminKey = ''
  session.expiredReason = reason
  localStorage.removeItem(STORAGE_KEY)
  sessionStorage.removeItem(STORAGE_KEY)
}

/** Human-readable backend label for the title bar. */
export const backendLabel = computed(() => {
  if (session.baseUrl) return new URL(session.baseUrl).host
  return `${location.host}（同源）`
})
