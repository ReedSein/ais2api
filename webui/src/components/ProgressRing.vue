<script setup lang="ts">
withDefaults(defineProps<{ size?: number; label?: string }>(), { size: 16, label: '加载中' })
</script>

<template>
  <svg class="ring" :width="size" :height="size" viewBox="0 0 16 16" role="progressbar" :aria-label="label">
    <circle cx="8" cy="8" r="7" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" />
  </svg>
</template>

<style scoped>
.ring {
  flex: none;
  color: var(--accent);
  animation: spin 2s linear infinite;
}
.ring circle {
  stroke-dasharray: 44;
  stroke-dashoffset: 30;
  transform-origin: center;
  animation: arc 1.6s var(--ease-point) infinite alternate;
}
@keyframes spin {
  to {
    transform: rotate(360deg);
  }
}
@keyframes arc {
  from {
    stroke-dashoffset: 40;
  }
  to {
    stroke-dashoffset: 12;
  }
}
</style>
