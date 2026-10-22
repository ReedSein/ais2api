<script setup lang="ts">
defineProps<{ title: string; subtitle?: string }>()
</script>

<template>
  <header class="page-header">
    <div class="heading">
      <h1 class="t-title">{{ title }}</h1>
      <p v-if="subtitle" class="t-secondary subtitle">{{ subtitle }}</p>
    </div>
    <div v-if="$slots.default" class="commands" role="toolbar" :aria-label="`${title}操作`">
      <slot />
    </div>
  </header>
</template>

<style scoped>
.page-header {
  display: flex;
  align-items: flex-end;
  flex-wrap: wrap;
  gap: 12px 24px;
  margin-bottom: 20px;
}
.heading {
  flex: 1 1 280px;
  min-width: 0;
}
.subtitle {
  margin-top: 4px;
  max-width: 72ch;
}
.commands {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 8px;
}
@media (max-width: 640px) {
  .t-title {
    font: var(--type-subtitle);
    font-size: 24px;
    line-height: 32px;
  }
  .commands {
    width: 100%;
  }
}
</style>
