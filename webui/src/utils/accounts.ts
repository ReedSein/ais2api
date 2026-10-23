import type { Account } from '../api/types'
import type { IconName } from '../icons'
import { isIndefinite } from './format'

export interface AccountState {
  tone: 'success' | 'caution' | 'critical' | 'neutral'
  text: string
  icon: IconName
  /** Can be picked for generation right now. */
  available: boolean
}

export const now = () => Math.floor(Date.now() / 1000)

/** Cooldowns that are still running (`until` in the future). */
export const activeCooldowns = (account: Account, at = now()) => (account.failures ?? []).filter((f) => f.until > at)

export function accountState(account: Account, at = now()): AccountState {
  if (account.disabled) return { tone: 'neutral', text: '已禁用', icon: 'person_prohibited', available: false }
  if (account.expired) return { tone: 'critical', text: '登录过期', icon: 'error_circle', available: false }
  const cooling = activeCooldowns(account, at)
  const global = cooling.find((f) => f.model === '*')
  // Permission failures never expire on their own; they need a manual clear/reset.
  if (global && isIndefinite(global.until)) return { tone: 'critical', text: '权限受限', icon: 'person_prohibited', available: false }
  if (global) return { tone: 'caution', text: '冷却中', icon: 'timer', available: false }
  if (cooling.length) return { tone: 'caution', text: `${cooling.length} 个模型冷却`, icon: 'timer', available: true }
  return { tone: 'success', text: '可用', icon: 'checkmark_circle', available: true }
}

export const accountLabel = (account: Account | undefined, fallbackId?: string | null) =>
  account ? account.name || account.email || account.id : fallbackId || '—'
