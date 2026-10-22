<script setup lang="ts">
import { computed, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import FIcon from '../components/FIcon.vue'
import FInfoBar from '../components/FInfoBar.vue'
import ProgressRing from '../components/ProgressRing.vue'
import SecretInput from '../components/SecretInput.vue'
import { api, describeError } from '../api/client'
import { lastBaseUrl, normalizeBaseUrl, session, signIn } from '../stores/session'

const route = useRoute()
const router = useRouter()

const baseUrl = ref(lastBaseUrl())
const adminKey = ref('')
const remember = ref(false)
const busy = ref(false)
const error = ref('')
const showHelp = ref(false)
const consoleOrigin = location.origin

const crossOrigin = computed(() => {
  try {
    const value = normalizeBaseUrl(baseUrl.value)
    return value !== '' && value !== location.origin
  } catch {
    return false
  }
})

async function submit() {
  error.value = ''
  let base: string
  try {
    base = normalizeBaseUrl(baseUrl.value)
  } catch {
    error.value = '后端地址格式无效，例如 http://127.0.0.1:18451'
    return
  }
  const key = adminKey.value.trim()
  if (!key) {
    error.value = '请输入管理密钥（config.json 中的 admin_key）。'
    return
  }
  busy.value = true
  try {
    await api.get('/auth/check', { adminKey: key, baseUrl: base })
    signIn(base === location.origin ? '' : base, key, remember.value)
    const redirect = typeof route.query.redirect === 'string' && route.query.redirect.startsWith('/') ? route.query.redirect : '/'
    router.replace(redirect)
  } catch (e) {
    error.value = describeError(e)
  } finally {
    busy.value = false
  }
}
</script>

<template>
  <div class="login">
    <form class="panel card" @submit.prevent="submit">
      <div class="brand">
        <img src="/favicon.svg" alt="" width="40" height="40" />
        <div>
          <h1 class="t-subtitle">ais2api 控制台</h1>
          <p class="t-secondary">使用管理密钥连接到网关</p>
        </div>
      </div>

      <FInfoBar v-if="session.expiredReason && !error" severity="warning" :message="session.expiredReason" />
      <FInfoBar v-if="error" severity="error" :message="error" />

      <div class="field">
        <label for="base-url">后端地址</label>
        <div class="input-group">
          <span class="lead"><FIcon name="globe" /></span>
          <input
            id="base-url"
            v-model="baseUrl"
            class="input has-lead"
            inputmode="url"
            autocomplete="url"
            autocapitalize="off"
            spellcheck="false"
            placeholder="留空 = 当前 ais2api 服务"
          />
        </div>
        <span class="hint">直接连接时填写如 <code>http://127.0.0.1:18451</code>。</span>
      </div>

      <FInfoBar v-if="crossOrigin" severity="info">
        跨域直连需要在后端 <code>admin_origins</code> 中加入 <code>{{ consoleOrigin }}</code>。
      </FInfoBar>

      <div class="field">
        <label for="admin-key">管理密钥</label>
        <SecretInput id="admin-key" v-model="adminKey" placeholder="admin_key" autocomplete="current-password" />
      </div>

      <label class="check">
        <input v-model="remember" type="checkbox" />
        <span>在此设备上记住<span class="t-secondary t-caption block">否则关闭浏览器标签页后需重新登录</span></span>
      </label>

      <button class="btn accent lg block" type="submit" :disabled="busy">
        <ProgressRing v-if="busy" :size="16" class="on-accent" />
        <span>{{ busy ? '正在验证…' : '连接' }}</span>
      </button>

      <button type="button" class="hyperlink help-toggle" :aria-expanded="showHelp" @click="showHelp = !showHelp">
        <FIcon :name="showHelp ? 'chevron_down' : 'chevron_right'" :size="12" />
        如何启用管理 API？
      </button>
      <div v-if="showHelp" class="help t-secondary">
        <ol>
          <li>在 ais2api 的 <code>config.json</code> 中设置独立的 <code>admin_key</code>（不能与 <code>api_key</code> 相同用途）。</li>
          <li>执行 <code>ais2api config reload</code> 或重启服务。</li>
          <li>
            从 ais2api 服务地址打开内置控制台，后端地址留空即可，无需配置跨域。开发时也可通过
            <code>npm run dev</code> 的代理访问；跨域连接需在 <code>admin_origins</code> 中列出控制台的完整来源。
          </li>
        </ol>
      </div>
    </form>
  </div>
</template>

<style scoped>
.login {
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
.block {
  display: block;
}
.on-accent {
  color: var(--text-on-accent);
}
.help-toggle {
  align-self: flex-start;
}
.help ol {
  margin: 0;
  padding-left: 20px;
  display: flex;
  flex-direction: column;
  gap: 6px;
}
@media (max-width: 640px) {
  .panel {
    padding: 24px 20px;
    box-shadow: none;
  }
}
</style>
