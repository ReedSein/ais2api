<script setup lang="ts">
import { computed } from 'vue'
import { iconMarkup, type IconName } from '../icons'

const props = withDefaults(defineProps<{ name: IconName; size?: number; filled?: boolean; label?: string }>(), {
  size: 16,
  filled: false,
})
const markup = computed(() => iconMarkup(props.name, props.filled ? 'filled' : 'regular'))
</script>

<template>
  <svg
    class="icon"
    viewBox="0 0 20 20"
    :width="size"
    :height="size"
    fill="currentColor"
    :role="label ? 'img' : undefined"
    :aria-label="label"
    :aria-hidden="label ? undefined : 'true'"
    focusable="false"
    v-html="markup"
  />
</template>

<style scoped>
.icon {
  flex: none;
  display: inline-block;
  vertical-align: middle;
}
</style>
