import { onBeforeUnmount, onMounted, ref, type Ref } from 'vue'

export function useMediaQuery(query: string): Ref<boolean> {
  const list = window.matchMedia(query)
  const matches = ref(list.matches)
  const update = (event: MediaQueryListEvent) => (matches.value = event.matches)
  onMounted(() => list.addEventListener('change', update))
  onBeforeUnmount(() => list.removeEventListener('change', update))
  return matches
}

/** Re-runs `task` every `ms` while the page is visible; pauses in background tabs. */
export function usePolling(task: () => unknown, ms: number) {
  let timer: ReturnType<typeof setInterval> | undefined
  const start = () => {
    stop()
    timer = setInterval(() => {
      if (document.visibilityState === 'visible') task()
    }, ms)
  }
  const stop = () => timer && clearInterval(timer)
  const onVisible = () => document.visibilityState === 'visible' && task()
  onMounted(() => {
    start()
    document.addEventListener('visibilitychange', onVisible)
  })
  onBeforeUnmount(() => {
    stop()
    document.removeEventListener('visibilitychange', onVisible)
  })
}
