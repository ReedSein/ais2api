<script setup lang="ts">
import FIcon from './FIcon.vue'
import type { IconName } from '../icons'

defineProps<{ icon: IconName; title: string; message?: string }>()
</script>

<template>
  <div class="empty">
    <span class="glyph"><FIcon :name="icon" :size="32" /></span>
    <strong>{{ title }}</strong>
    <p v-if="message" class="t-secondary">{{ message }}</p>
    <div v-if="$slots.default" class="row wrap actions"><slot /></div>
  </div>
</template>

<style scoped>
.empty {
  display: flex;
  flex-direction: column;
  align-items: center;
  text-align: center;
  gap: 8px;
  padding: 48px 16px;
}
.glyph {
  display: grid;
  place-items: center;
  width: 64px;
  height: 64px;
  margin-bottom: 4px;
  border-radius: 50%;
  background: var(--subtle-secondary);
  color: var(--text-secondary);
}
p {
  max-width: 48ch;
}
.actions {
  margin-top: 8px;
  justify-content: center;
}
</style>
