<script setup lang="ts">
// WinUI MenuFlyout on pointer devices; an action sheet on narrow screens.
import { nextTick, onBeforeUnmount, ref } from 'vue'
import FIcon from './FIcon.vue'
import type { IconName } from '../icons'
import { useMediaQuery } from '../utils/media'

export interface MenuItem {
  label?: string
  icon?: IconName
  danger?: boolean
  disabled?: boolean
  divider?: boolean
  action?: () => void
}

withDefaults(defineProps<{ items: MenuItem[]; label?: string; icon?: IconName; title?: string }>(), {
  label: '更多操作',
  icon: 'more_horizontal',
})

const open = ref(false)
const trigger = ref<HTMLButtonElement>()
const menu = ref<HTMLElement>()
const position = ref<Record<string, string>>({})
const narrow = useMediaQuery('(max-width: 640px)')

function place() {
  const rect = trigger.value?.getBoundingClientRect()
  const panel = menu.value
  if (!rect || !panel || narrow.value) return
  const width = panel.offsetWidth
  const height = panel.offsetHeight
  let left = rect.right - width
  if (left < 8) left = Math.min(rect.left, window.innerWidth - width - 8)
  let top = rect.bottom + 4
  if (top + height > window.innerHeight - 8) top = Math.max(8, rect.top - height - 4)
  position.value = { left: `${left}px`, top: `${top}px` }
}

function onOutside(event: Event) {
  const target = event.target as Node
  if (menu.value?.contains(target) || trigger.value?.contains(target)) return
  hide()
}
function onKey(event: KeyboardEvent) {
  if (event.key === 'Escape') {
    hide()
    trigger.value?.focus()
    return
  }
  if (event.key !== 'ArrowDown' && event.key !== 'ArrowUp') return
  const items = [...(menu.value?.querySelectorAll<HTMLButtonElement>('button.item:not(:disabled)') ?? [])]
  const index = items.indexOf(document.activeElement as HTMLButtonElement)
  const next = event.key === 'ArrowDown' ? (index + 1) % items.length : (index - 1 + items.length) % items.length
  items[next]?.focus()
  event.preventDefault()
}

async function show() {
  open.value = true
  await nextTick()
  place()
  menu.value?.querySelector<HTMLButtonElement>('button.item:not(:disabled)')?.focus()
  document.addEventListener('pointerdown', onOutside, true)
  document.addEventListener('keydown', onKey, true)
  window.addEventListener('resize', hide)
  window.addEventListener('scroll', hide, true)
}
function hide() {
  open.value = false
  document.removeEventListener('pointerdown', onOutside, true)
  document.removeEventListener('keydown', onKey, true)
  window.removeEventListener('resize', hide)
  window.removeEventListener('scroll', hide, true)
}
function run(item: MenuItem) {
  hide()
  item.action?.()
}
onBeforeUnmount(hide)
defineExpose({ show })
</script>

<template>
  <button
    ref="trigger"
    class="btn subtle"
    :class="{ 'icon-only': !$slots.default }"
    :aria-label="label"
    :title="title ?? label"
    aria-haspopup="menu"
    :aria-expanded="open"
    @click.stop="open ? hide() : show()"
  >
    <FIcon :name="icon" />
    <slot />
  </button>
  <Teleport to="body">
    <Transition :name="narrow ? 'sheet' : 'flyout'">
      <div v-if="open" class="layer" :class="{ narrow }" @click.self="hide">
        <div ref="menu" class="menu" role="menu" :style="position">
          <template v-for="(item, index) in items" :key="index">
            <div v-if="item.divider" class="sep" role="separator" />
            <button v-else class="item" role="menuitem" :class="{ danger: item.danger }" :disabled="item.disabled" @click="run(item)">
              <FIcon v-if="item.icon" :name="item.icon" />
              <span>{{ item.label }}</span>
            </button>
          </template>
        </div>
      </div>
    </Transition>
  </Teleport>
</template>

<style scoped>
.layer {
  position: fixed;
  inset: 0;
  z-index: 90;
  pointer-events: none;
}
.layer.narrow {
  pointer-events: auto;
  background: var(--smoke);
  display: flex;
  align-items: flex-end;
}
.menu {
  position: fixed;
  pointer-events: auto;
  min-width: 200px;
  max-width: min(320px, calc(100vw - 16px));
  padding: 4px;
  border-radius: var(--radius-overlay);
  border: 1px solid var(--surface-stroke);
  background: var(--acrylic);
  backdrop-filter: blur(30px) saturate(150%);
  -webkit-backdrop-filter: blur(30px) saturate(150%);
  box-shadow: var(--shadow-flyout);
}
.narrow .menu {
  position: static;
  width: 100%;
  max-width: none;
  border-radius: 12px 12px 0 0;
  padding: 8px 8px calc(8px + var(--safe-bottom));
}
.item {
  display: flex;
  align-items: center;
  gap: 12px;
  width: 100%;
  min-height: 32px;
  padding: 6px 12px;
  border: 0;
  border-radius: var(--radius-control);
  background: transparent;
  color: var(--text-primary);
  text-align: left;
  cursor: pointer;
}
.narrow .item {
  min-height: 44px;
}
.item:hover,
.item:focus-visible {
  background: var(--subtle-secondary);
  outline: none;
}
.item:active {
  background: var(--subtle-tertiary);
  color: var(--text-secondary);
}
.item:disabled {
  color: var(--text-disabled);
  cursor: default;
  background: transparent;
}
.item.danger {
  color: var(--critical);
}
.sep {
  height: 1px;
  margin: 4px -4px;
  background: var(--divider);
}
.flyout-enter-active {
  transition:
    opacity var(--motion-fast) linear,
    transform var(--motion-normal) var(--ease-decelerate);
}
.flyout-enter-from {
  opacity: 0;
  transform: translateY(-8px);
}
.sheet-enter-active,
.sheet-leave-active {
  transition: opacity var(--motion-normal) linear;
}
.sheet-enter-active .menu {
  transition: transform var(--motion-slow) var(--ease-decelerate);
}
.sheet-enter-from,
.sheet-leave-to {
  opacity: 0;
}
.sheet-enter-from .menu {
  transform: translateY(100%);
}
</style>
