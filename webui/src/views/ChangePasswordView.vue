<script setup lang="ts">
// Replaces the management password through POST /auth/password. While the
// service still uses the default password ("forced" mode) every other
// management route is refused, so this page has no way back except signing out.
import { computed, onMounted, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import FIcon from '../components/FIcon.vue'
import FInfoBar from '../components/FInfoBar.vue'
import ProgressRing from '../components/ProgressRing.vue'
import SecretInput from '../components/SecretInput.vue'
import { api, describeError } from '../api/client'
import { session, signIn, signOut } from '../stores/session'
import { notify } from '../stores/ui'

const DEFAULT_PASSWORD = 'admin'

const route = useRoute()
const router = useRouter()
const password = ref('')
const confirmation = ref('')
const busy = ref(false)
const error = ref('')

const forced = computed(() => session.mustChangePassword)

// Refresh the forced state from the service (e.g. after a page reload).
onMounted(async () => {
  try {
    const check = await api.get<{ password_change_required?: boolean }>('/auth/check')
    session.mustChangePassword = check.password_change_required === true
  } catch {
    /* 401 signs out via the client; other errors leave the current state */
  }
})

const rules = computed(() => [
  { ok: password.value.length >= 8 && password.value.length <= 4096, text: '8–4096 个字符' },
  { ok: password.value.length > 0 && /^[!-~]+$/.test(password.value), text: '仅可见 ASCII 字符，不含空格' },
  { ok: password.value.length > 0 && password.value !== DEFAULT_PASSWORD, text: '不能是默认密码 admin' },
  { ok: password.value.length > 0 && password.value !== session.adminKey, text: '与当前密码不同' },
  { ok: password.value.length > 0 && password.value === confirmation.value, text: '两次输入一致' },
])
const valid = computed(() => rules.value.every((rule) => rule.ok))

async function submit() {
  if (!valid.value || busy.value) return
  error.value = ''
  busy.value = true
  const next = password.value
  try {
    await api.post('/auth/password', { password: next })
    // The old password stops working immediately; continue with the new one.
    signIn(session.baseUrl, next, session.remember)
    session.mustChangePassword = false
    password.value = ''
    confirmation.value = ''
    notify('success', '管理密码已修改', '其他设备上使用旧密码的会话需要重新登录。')
    const redirect = typeof route.query.redirect === 'string' && route.query.redirect.startsWith('/') ? route.query.redirect : '/'
    router.replace(redirect)
  } catch (e) {
    error.value = describeError(e)
  } finally {
    busy.value = false
  }
}

function leave() {
  if (forced.value) signOut()
  else router.back()
}
</script>

<template>
  <div class="change-password">
    <form class="panel card" novalidate @submit.prevent="submit">
      <div class="brand">
        <span class="glyph"><FIcon name="lock_closed" :size="24" /></span>
        <div>
          <h1 class="t-subtitle">{{ forced ? '设置新的管理密码' : '修改管理密码' }}</h1>
          <p class="t-secondary">用于登录管理面板和调用管理 API</p>
        </div>
      </div>

      <FInfoBar
        v-if="forced"
        severity="warning"
        title="正在使用默认密码"
        message="默认密码 admin 是公开的。修改后才能继续使用管理面板，旧密码会立即失效。"
      />
      <FInfoBar v-if="error" severity="error" :message="error" />

      <div class="field">
        <label for="new-password">新密码</label>
        <SecretInput id="new-password" v-model="password" autocomplete="new-password" :invalid="password.length > 0 && !rules[0].ok" />
      </div>
      <div class="field">
        <label for="confirm-password">确认新密码</label>
        <SecretInput
          id="confirm-password"
          v-model="confirmation"
          autocomplete="new-password"
          :invalid="confirmation.length > 0 && confirmation !== password"
        />
      </div>

      <ul class="rules" aria-label="密码要求">
        <li v-for="rule in rules" :key="rule.text" :class="{ ok: rule.ok }">
          <FIcon :name="rule.ok ? 'checkmark_circle' : 'circle'" :filled="rule.ok" :size="14" />
          <span>{{ rule.text }}</span>
        </li>
      </ul>

      <p class="t-caption t-secondary">修改后，其他设备或标签页中使用旧密码的会话会被要求重新登录。</p>

      <div class="actions">
        <button class="btn accent lg" type="submit" :disabled="!valid || busy">
          <ProgressRing v-if="busy" :size="16" class="on-accent" />
          <span>{{ busy ? '正在保存…' : '修改密码' }}</span>
        </button>
        <button class="btn lg" type="button" :disabled="busy" @click="leave">{{ forced ? '退出登录' : '取消' }}</button>
      </div>
    </form>
  </div>
</template>

<style scoped>
.change-password {
  min-height: 100dvh;
  display: grid;
  place-items: center;
  padding: 24px 16px calc(24px + var(--safe-bottom));
}
.panel {
  width: min(440px, 100%);
  padding: 32px;
  display: flex;
  flex-direction: column;
  gap: 16px;
  box-shadow: var(--shadow-dialog);
  background: var(--solid-tertiary);
}
[data-theme='dark'] .panel {
  background: #2b2b2b;
}
.brand {
  display: flex;
  align-items: center;
  gap: 16px;
  margin-bottom: 8px;
}
.glyph {
  display: grid;
  place-items: center;
  width: 40px;
  height: 40px;
  border-radius: var(--radius-overlay);
  background: var(--accent);
  color: var(--text-on-accent);
}
.rules {
  list-style: none;
  margin: 0;
  padding: 0;
  display: flex;
  flex-direction: column;
  gap: 4px;
  font: var(--type-caption);
  color: var(--text-secondary);
}
.rules li {
  display: flex;
  align-items: center;
  gap: 8px;
}
.rules li.ok {
  color: var(--text-primary);
}
.rules li.ok .icon {
  color: var(--success);
}
.actions {
  display: flex;
  gap: 8px;
}
.actions .btn {
  flex: 1 1 0;
}
.on-accent {
  color: var(--text-on-accent);
}
@media (max-width: 640px) {
  .panel {
    padding: 24px 20px;
    box-shadow: none;
  }
}
</style>
