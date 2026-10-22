<script setup lang="ts">
// PasswordBox with a reveal button. Secrets never get autocomplete or spellcheck.
import { ref } from 'vue'
import FIcon from './FIcon.vue'

defineProps<{ placeholder?: string; id?: string; invalid?: boolean; autocomplete?: string }>()
const model = defineModel<string>({ default: '' })
const revealed = ref(false)
</script>

<template>
  <div class="input-group">
    <input
      :id="id"
      v-model="model"
      class="input mono"
      :type="revealed ? 'text' : 'password'"
      :placeholder="placeholder"
      :aria-invalid="invalid || undefined"
      :autocomplete="autocomplete ?? 'off'"
      autocapitalize="off"
      spellcheck="false"
    />
    <span class="trail">
      <button
        type="button"
        class="btn subtle icon-only"
        :aria-label="revealed ? '隐藏' : '显示'"
        :aria-pressed="revealed"
        @click="revealed = !revealed"
      >
        <FIcon :name="revealed ? 'eye_off' : 'eye'" />
      </button>
    </span>
  </div>
</template>
