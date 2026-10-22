import { reactive } from 'vue'
import { api } from '../api/client'
import type { Status } from '../api/types'

// Live service status shared by the title bar and the dashboard.
export const service = reactive({
  status: null as Status | null,
  reachable: true,
  lastError: '',
  updatedAt: 0,
})

let inflight: Promise<void> | null = null

export function refreshStatus(): Promise<void> {
  inflight ??= api
    .get<Status>('/status')
    .then((status) => {
      service.status = status
      service.reachable = true
      service.lastError = ''
      service.updatedAt = Date.now()
    })
    .catch((error: unknown) => {
      service.reachable = false
      service.lastError = error instanceof Error ? error.message : String(error)
    })
    .finally(() => (inflight = null))
  return inflight
}
