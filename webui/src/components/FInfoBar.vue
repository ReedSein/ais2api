<script setup lang="ts">
import { computed } from 'vue'
import FIcon from './FIcon.vue'
import type { IconName } from '../icons'

const props = withDefaults(
  defineProps<{ severity?: 'info' | 'success' | 'warning' | 'error'; title?: string; message?: string; closable?: boolean }>(),
  { severity: 'info' },
)
defineEmits<{ close: [] }>()

const icon = computed<IconName>(
  () => ({ info: 'info', success: 'checkmark_circle', warning: 'warning', error: 'error_circle' })[props.severity] as IconName,
)
</script>

<template>
  <div class="infobar" :class="severity" :role="severity === 'error' ? 'alert' : 'status'">
    <span class="glyph"><FIcon :name="icon" :size="16" filled /></span>
    <div class="content">
      <strong v-if="title" class="title">{{ title }}</strong>
      <span v-if="message" class="message">{{ message }}</span>
      <slot />
    </div>
    <div v-if="$slots.actions" class="actions"><slot name="actions" /></div>
    <button v-if="closable" class="btn subtle icon-only close" aria-label="关闭" @click="$emit('close')">
      <FIcon name="dismiss" />
    </button>
  </div>
</template>

<style scoped>
.infobar {
  --ib-bg: var(--attention-bg);
  --ib-icon: var(--accent);
  display: flex;
  align-items: flex-start;
  gap: 12px;
  padding: 8px 8px 8px 16px;
  min-height: 48px;
  border: 1px solid var(--card-stroke);
  border-radius: var(--radius-control);
  background: var(--ib-bg);
}
.success {
  --ib-bg: var(--success-bg);
  --ib-icon: var(--success);
}
.warning {
  --ib-bg: var(--caution-bg);
  --ib-icon: var(--caution);
}
.error {
  --ib-bg: var(--critical-bg);
  --ib-icon: var(--critical);
}
.glyph {
  display: flex;
  margin-top: 8px;
  color: var(--ib-icon);
}
.content {
  flex: 1;
  min-width: 0;
  padding: 6px 0;
  display: flex;
  flex-wrap: wrap;
  column-gap: 12px;
  row-gap: 2px;
  overflow-wrap: anywhere;
}
.title {
  font-weight: 600;
}
.actions {
  display: flex;
  gap: 8px;
  align-self: center;
}
.close {
  align-self: flex-start;
}
</style>
