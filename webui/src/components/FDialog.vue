<script setup lang="ts">
// WinUI ContentDialog: smoke layer, 8px rounded card, footer band with the
// primary/secondary buttons. On phones it docks to the bottom as a sheet.
import { nextTick, onBeforeUnmount, ref, watch } from 'vue'

const props = withDefaults(defineProps<{ title?: string; width?: number; dismissible?: boolean }>(), {
  width: 480,
  dismissible: true,
})
const open = defineModel<boolean>('open', { default: false })
const emit = defineEmits<{ close: [] }>()
const panel = ref<HTMLElement>()
let restoreFocus: Element | null = null

function close() {
  if (!props.dismissible) return
  open.value = false
  emit('close')
}

function onKey(event: KeyboardEvent) {
  if (!open.value) return
  if (event.key === 'Escape') {
    event.stopPropagation()
    close()
  } else if (event.key === 'Tab' && panel.value) {
    const focusable = panel.value.querySelectorAll<HTMLElement>(
      'button:not([disabled]), [href], input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])',
    )
    if (!focusable.length) return
    const first = focusable[0]
    const last = focusable[focusable.length - 1]
    if (event.shiftKey && document.activeElement === first) {
      last.focus()
      event.preventDefault()
    } else if (!event.shiftKey && document.activeElement === last) {
      first.focus()
      event.preventDefault()
    }
  }
}

watch(
  open,
  async (value) => {
    if (value) {
      restoreFocus = document.activeElement
      document.addEventListener('keydown', onKey, true)
      document.body.style.overflow = 'hidden'
      await nextTick()
      const target = panel.value?.querySelector<HTMLElement>('[autofocus], input, textarea, select') ?? panel.value
      target?.focus()
    } else {
      document.removeEventListener('keydown', onKey, true)
      document.body.style.overflow = ''
      if (restoreFocus instanceof HTMLElement) restoreFocus.focus()
    }
  },
  { immediate: true },
)

onBeforeUnmount(() => {
  document.removeEventListener('keydown', onKey, true)
  document.body.style.overflow = ''
})
</script>

<template>
  <Teleport to="body">
    <Transition name="dialog">
      <div v-if="open" class="smoke" @mousedown.self="close">
        <div ref="panel" class="dialog" role="dialog" aria-modal="true" :aria-label="title" tabindex="-1" :style="{ '--w': `${width}px` }">
          <div class="body">
            <h2 v-if="title" class="t-subtitle title">{{ title }}</h2>
            <slot />
          </div>
          <footer v-if="$slots.footer" class="footer">
            <slot name="footer" />
          </footer>
        </div>
      </div>
    </Transition>
  </Teleport>
</template>

<style scoped>
.smoke {
  position: fixed;
  inset: 0;
  z-index: 100;
  display: grid;
  place-items: center;
  padding: 24px;
  background: var(--smoke);
}
.dialog {
  display: flex;
  flex-direction: column;
  width: min(var(--w), 100%);
  max-height: min(760px, calc(100dvh - 48px));
  border-radius: var(--radius-overlay);
  border: 1px solid var(--surface-stroke);
  background: var(--solid-base);
  box-shadow: var(--shadow-dialog);
  overflow: hidden;
}
[data-theme='dark'] .dialog {
  background: #2b2b2b;
}
.body {
  padding: 24px;
  overflow: auto;
  display: flex;
  flex-direction: column;
  gap: 12px;
  background: var(--layer-alt);
}
[data-theme='dark'] .body {
  background: #2b2b2b;
}
.title {
  margin-bottom: 4px;
}
.footer {
  display: flex;
  gap: 8px;
  padding: 24px;
  border-top: 1px solid var(--card-stroke);
  background: var(--solid-base);
}
[data-theme='dark'] .footer {
  background: #202020;
}
.footer :deep(.btn) {
  flex: 1 1 0;
}

.dialog-enter-active {
  transition: opacity var(--motion-normal) linear;
}
.dialog-enter-active .dialog {
  transition: transform var(--motion-slow) var(--ease-decelerate);
}
.dialog-leave-active {
  transition: opacity var(--motion-fast) linear;
}
.dialog-enter-from,
.dialog-leave-to {
  opacity: 0;
}
.dialog-enter-from .dialog {
  transform: scale(1.05);
}

@media (max-width: 640px) {
  .smoke {
    place-items: end stretch;
    padding: 0;
  }
  .dialog {
    width: 100%;
    max-height: calc(100dvh - 32px - var(--safe-top));
    border-radius: 12px 12px 0 0;
    border-bottom: 0;
  }
  .body {
    padding: 20px 16px;
  }
  .footer {
    padding: 16px 16px calc(16px + var(--safe-bottom));
  }
  .dialog-enter-from .dialog {
    transform: translateY(40px);
  }
}
</style>
