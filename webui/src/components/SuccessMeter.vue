<script setup lang="ts">
import { computed } from 'vue'
import { formatPercent } from '../utils/format'

// Meter: fill carries severity (accent → caution → critical), the unfilled
// track is a lighter step of the accent ramp. The percentage is always printed
// beside it, so state is never conveyed by colour alone.
const props = defineProps<{ success: number; failure: number; compact?: boolean }>()
const total = computed(() => props.success + props.failure)
const ratio = computed(() => (total.value ? props.success / total.value : 0))
const tone = computed(() => (!total.value ? 'empty' : ratio.value >= 0.9 ? 'good' : ratio.value >= 0.6 ? 'warn' : 'bad'))
</script>

<template>
  <div class="meter" :class="[tone, { compact }]" :title="`成功 ${success} · 失败 ${failure}`">
    <div
      class="track"
      role="meter"
      aria-valuemin="0"
      aria-valuemax="100"
      :aria-valuenow="Math.round(ratio * 100)"
      :aria-label="`成功率 ${formatPercent(ratio)}`"
    >
      <div class="fill" :style="{ width: `${ratio * 100}%` }" />
    </div>
    <span class="value tabular">{{ total ? formatPercent(ratio) : '—' }}</span>
  </div>
</template>

<style scoped>
.meter {
  display: flex;
  align-items: center;
  gap: 8px;
  min-width: 120px;
}
.track {
  flex: 1;
  height: 6px;
  border-radius: 3px;
  background: var(--accent-track);
  overflow: hidden;
}
.fill {
  height: 100%;
  border-radius: 3px;
  background: var(--accent);
  transition: width var(--motion-slow) var(--ease-decelerate);
}
.warn .fill {
  background: var(--caution);
}
.bad .fill {
  background: var(--critical);
}
.empty .track {
  background: var(--subtle-secondary);
}
.value {
  width: 3.6em;
  text-align: right;
  color: var(--text-secondary);
  font: var(--type-caption);
}
.compact {
  min-width: 90px;
}
</style>
