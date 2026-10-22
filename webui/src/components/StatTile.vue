<script setup lang="ts">
import FIcon from './FIcon.vue'
import type { IconName } from '../icons'

// Stat tile: label · value · optional caption. Status, when present, is shown
// with icon + text (never colour alone).
defineProps<{
  label: string
  value: string
  caption?: string
  icon?: IconName
  status?: { tone: 'success' | 'caution' | 'critical' | 'neutral'; text: string; icon: IconName }
  to?: string
}>()
</script>

<template>
  <component :is="to ? 'RouterLink' : 'div'" :to="to" class="tile card" :class="{ link: to }">
    <div class="top">
      <span class="label t-secondary">{{ label }}</span>
      <FIcon v-if="icon" :name="icon" class="t-tertiary" />
    </div>
    <div class="value">{{ value }}</div>
    <div class="bottom">
      <span v-if="status" class="badge" :class="status.tone !== 'neutral' ? status.tone : ''">
        <FIcon :name="status.icon" filled />{{ status.text }}
      </span>
      <span v-if="caption" class="t-caption t-secondary truncate">{{ caption }}</span>
    </div>
  </component>
</template>

<style scoped>
.tile {
  display: flex;
  flex-direction: column;
  gap: 4px;
  padding: 14px 16px 16px;
  min-width: 0;
  color: inherit;
  text-decoration: none;
  transition: background-color var(--motion-fast) linear;
}
.tile.link:hover {
  background: var(--control-secondary);
  text-decoration: none;
}
.top {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
}
.value {
  font: 600 28px/36px var(--font-display);
  letter-spacing: -0.01em;
  overflow-wrap: anywhere;
}
.bottom {
  display: flex;
  align-items: center;
  gap: 8px;
  min-height: 22px;
  min-width: 0;
}
</style>
