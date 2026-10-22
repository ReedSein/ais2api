import { reactive } from 'vue'
import { describeError } from '../api/client'

// ---------- Toast notifications (WinUI "InfoBar" style, transient) ----------

export type Severity = 'info' | 'success' | 'warning' | 'error'

export interface Toast {
  id: number
  severity: Severity
  title: string
  message?: string
}

export const toasts = reactive<Toast[]>([])
let nextToast = 1

export function notify(severity: Severity, title: string, message?: string, timeout = severity === 'error' ? 8000 : 4000) {
  const id = nextToast++
  toasts.push({ id, severity, title, message })
  if (toasts.length > 4) toasts.shift()
  if (timeout > 0) setTimeout(() => dismissToast(id), timeout)
}

export function dismissToast(id: number) {
  const index = toasts.findIndex((toast) => toast.id === id)
  if (index >= 0) toasts.splice(index, 1)
}

export const notifyError = (title: string, error: unknown) => notify('error', title, describeError(error))

// ---------- Confirmation dialog (promise-based ContentDialog) ----------

export interface ConfirmRequest {
  title: string
  message: string
  confirmText?: string
  cancelText?: string
  danger?: boolean
  /** Require typing this text before the primary button enables. */
  typeToConfirm?: string
}

export const confirmState = reactive<{ open: boolean; request: ConfirmRequest | null; resolve: ((ok: boolean) => void) | null }>({
  open: false,
  request: null,
  resolve: null,
})

export function confirm(request: ConfirmRequest): Promise<boolean> {
  confirmState.resolve?.(false)
  return new Promise((resolve) => {
    confirmState.request = request
    confirmState.resolve = resolve
    confirmState.open = true
  })
}

export function settleConfirm(ok: boolean) {
  confirmState.resolve?.(ok)
  confirmState.resolve = null
  confirmState.open = false
}

// ---------- Clipboard ----------

export async function copyText(text: string, label = '已复制到剪贴板') {
  try {
    if (navigator.clipboard && window.isSecureContext) {
      await navigator.clipboard.writeText(text)
    } else {
      // Plain-HTTP LAN access (e.g. a phone) has no async clipboard API.
      const area = document.createElement('textarea')
      area.value = text
      area.setAttribute('readonly', '')
      area.style.position = 'fixed'
      area.style.opacity = '0'
      document.body.appendChild(area)
      area.select()
      const ok = document.execCommand('copy')
      area.remove()
      if (!ok) throw new Error('copy failed')
    }
    notify('success', label)
  } catch {
    notify('warning', '无法自动复制', '请手动选择文本复制。')
  }
}

export function downloadFile(name: string, content: BlobPart, type = 'application/json') {
  const url = URL.createObjectURL(new Blob([content], { type }))
  const link = document.createElement('a')
  link.href = url
  link.download = name
  document.body.appendChild(link)
  link.click()
  link.remove()
  setTimeout(() => URL.revokeObjectURL(url), 1000)
}
