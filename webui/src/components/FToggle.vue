<script setup lang="ts">
// WinUI ToggleSwitch: 40×20 pill, knob grows on hover, On/Off content label.
withDefaults(defineProps<{ onText?: string; offText?: string; disabled?: boolean; label?: string }>(), {
  onText: '开',
  offText: '关',
})
const model = defineModel<boolean>({ default: false })
</script>

<template>
  <label class="toggle" :class="{ on: model, disabled }">
    <input v-model="model" type="checkbox" role="switch" :aria-checked="model" :aria-label="label" :disabled="disabled" />
    <span class="track" aria-hidden="true"><span class="knob" /></span>
    <span v-if="onText || offText" class="text">{{ model ? onText : offText }}</span>
  </label>
</template>

<style scoped>
.toggle {
  display: inline-flex;
  align-items: center;
  gap: 12px;
  min-height: 32px;
  cursor: pointer;
  user-select: none;
  position: relative;
}
.toggle input {
  position: absolute;
  opacity: 0;
  width: 40px;
  height: 20px;
  margin: 0;
  cursor: pointer;
}
.track {
  position: relative;
  width: 40px;
  height: 20px;
  flex: none;
  border-radius: 10px;
  border: 1px solid var(--control-strong-stroke);
  background: var(--control-alt-secondary);
  transition: background-color var(--motion-fast) linear, border-color var(--motion-fast) linear;
}
.knob {
  position: absolute;
  top: 50%;
  left: 3px;
  width: 12px;
  height: 12px;
  margin-top: -6px;
  border-radius: 6px;
  background: var(--text-secondary);
  transition:
    left var(--motion-normal) var(--ease-decelerate),
    width var(--motion-fast) var(--ease-decelerate),
    height var(--motion-fast) var(--ease-decelerate),
    margin var(--motion-fast) var(--ease-decelerate);
}
.toggle:hover .track {
  background: var(--control-alt-tertiary);
}
.toggle:hover .knob {
  width: 14px;
  height: 14px;
  margin-top: -7px;
}
.toggle:active .knob {
  width: 17px;
}
.on .track {
  background: var(--accent);
  border-color: var(--accent);
}
.on:hover .track {
  background: var(--accent-hover);
}
.on .knob {
  left: calc(100% - 15px);
  background: var(--text-on-accent);
}
.on:hover .knob {
  left: calc(100% - 17px);
}
.on:active .knob {
  left: calc(100% - 20px);
}
.toggle input:focus-visible + .track {
  outline: 2px solid var(--text-primary);
  outline-offset: 2px;
}
.text {
  min-width: 1.5em;
}
.disabled {
  opacity: 0.45;
  pointer-events: none;
}
</style>
