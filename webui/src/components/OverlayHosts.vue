<script setup lang="ts">
// Global toast stack and the shared confirmation ContentDialog.
import { computed, ref, watch } from 'vue'
import FDialog from './FDialog.vue'
import FInfoBar from './FInfoBar.vue'
import { confirmState, dismissToast, settleConfirm, toasts } from '../stores/ui'

const typed = ref('')
watch(
  () => confirmState.open,
  () => (typed.value = ''),
)
const canConfirm = computed(() => !confirmState.request?.typeToConfirm || typed.value === confirmState.request.typeToConfirm)
const dialogOpen = computed({
  get: () => confirmState.open,
  set: (value) => !value && settleConfirm(false),
})
</script>

<template>
  <div class="toasts" aria-live="polite">
    <TransitionGroup name="toast">
      <FInfoBar
        v-for="toast in toasts"
        :key="toast.id"
        class="toast"
        :severity="toast.severity"
        :title="toast.title"
        :message="toast.message"
        closable
        @close="dismissToast(toast.id)"
      />
    </TransitionGroup>
  </div>

  <FDialog v-model:open="dialogOpen" :title="confirmState.request?.title" :width="440">
    <p class="message">{{ confirmState.request?.message }}</p>
    <div v-if="confirmState.request?.typeToConfirm" class="field">
      <label for="confirm-typed">
        请输入 <code>{{ confirmState.request.typeToConfirm }}</code> 以确认
      </label>
      <input id="confirm-typed" v-model="typed" class="input" autocomplete="off" @keydown.enter="canConfirm && settleConfirm(true)" />
    </div>
    <template #footer>
      <button class="btn" :class="confirmState.request?.danger ? 'danger' : 'accent'" :disabled="!canConfirm" @click="settleConfirm(true)">
        {{ confirmState.request?.confirmText ?? '确定' }}
      </button>
      <button class="btn" @click="settleConfirm(false)">{{ confirmState.request?.cancelText ?? '取消' }}</button>
    </template>
  </FDialog>
</template>

<style scoped>
.toasts {
  position: fixed;
  z-index: 120;
  right: 16px;
  bottom: calc(16px + var(--safe-bottom));
  width: min(400px, calc(100vw - 32px));
  display: flex;
  flex-direction: column;
  gap: 8px;
  pointer-events: none;
}
.toast {
  pointer-events: auto;
  box-shadow: var(--shadow-flyout);
  background-color: var(--solid-tertiary);
  background-image: linear-gradient(var(--ib-bg), var(--ib-bg));
  border-color: var(--surface-stroke);
}
.toast-enter-active {
  transition:
    opacity var(--motion-normal) linear,
    transform var(--motion-slow) var(--ease-decelerate);
}
.toast-leave-active {
  transition: opacity var(--motion-fast) linear;
}
.toast-enter-from {
  opacity: 0;
  transform: translateY(16px);
}
.toast-leave-to {
  opacity: 0;
}
.message {
  white-space: pre-line;
  color: var(--text-primary);
}
@media (max-width: 640px) {
  .toasts {
    left: 16px;
    right: 16px;
    width: auto;
    top: calc(56px + var(--safe-top));
    bottom: auto;
  }
}
</style>
