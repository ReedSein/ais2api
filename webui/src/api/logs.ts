import { ApiError, rawRequest } from './client'
import type { LogPage, RawLogPage } from './types'

export type LogBatch = LogPage | RawLogPage

export const isRaw = (batch: LogBatch): batch is RawLogPage => 'encoding' in batch

/**
 * Reads `/logs/stream` with an authenticated fetch (EventSource cannot send an
 * Authorization header). SSE frames may be split across network chunks, so
 * input is buffered until a blank line. Resolves with the last handled cursor
 * when the server closes the stream.
 */
export async function readLogStream(options: {
  source?: string
  cursor: string
  signal: AbortSignal
  onOpen?: () => void
  onBatch: (batch: LogBatch, cursor: string) => void
}): Promise<string> {
  let cursor = options.cursor
  const response = await rawRequest('GET', '/logs/stream', {
    query: { source: options.source, limit: 500 },
    headers: { Accept: 'text/event-stream', 'Last-Event-ID': cursor },
    signal: options.signal,
  })
  if (!response.body) throw new ApiError('浏览器不支持流式读取', 0, 'stream_unsupported')
  options.onOpen?.()
  const reader = response.body.getReader()
  const decoder = new TextDecoder()
  let buffered = ''
  try {
    for (;;) {
      const { value, done } = await reader.read()
      buffered += done ? decoder.decode() : decoder.decode(value, { stream: true })
      buffered = buffered.replace(/\r\n/g, '\n')
      let end: number
      while ((end = buffered.indexOf('\n\n')) >= 0) {
        const frame = buffered.slice(0, end)
        buffered = buffered.slice(end + 2)
        let id: string | undefined
        let event = 'message'
        const data: string[] = []
        for (const line of frame.split('\n')) {
          if (line.startsWith('id:')) id = line.slice(3).trim()
          else if (line.startsWith('event:')) event = line.slice(6).trim()
          else if (line.startsWith('data:')) data.push(line.slice(5).replace(/^ /, ''))
        }
        if (event === 'logs' && data.length) {
          const batch = JSON.parse(data.join('\n')) as LogBatch
          const next = id ?? batch.next_cursor
          options.onBatch(batch, next)
          cursor = next
        }
      }
      if (done) return cursor
    }
  } finally {
    await reader.cancel().catch(() => {})
    reader.releaseLock()
  }
}

/** Base64 → bytes without going through a lossy string decode. */
export function base64ToBytes(data: string): Uint8Array {
  const binary = atob(data)
  const bytes = new Uint8Array(binary.length)
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i)
  return bytes
}
