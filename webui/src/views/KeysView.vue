<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import EmptyState from '../components/EmptyState.vue'
import FDialog from '../components/FDialog.vue'
import FIcon from '../components/FIcon.vue'
import FInfoBar from '../components/FInfoBar.vue'
import FMenu from '../components/FMenu.vue'
import PageHeader from '../components/PageHeader.vue'
import ProgressRing from '../components/ProgressRing.vue'
import SecretInput from '../components/SecretInput.vue'
import { api, describeError, enc } from '../api/client'
import type { Key, KeyCreated, KeyList } from '../api/types'
import { session } from '../stores/session'
import { confirm, copyText, notify, notifyError } from '../stores/ui'
import { formatTimestamp } from '../utils/format'

const list = ref<KeyList | null>(null)
const loading = ref(true)
const loadError = ref('')
const busy = ref(false)

async function load() {
  try {
    list.value = await api.get<KeyList>('/keys')
    loadError.value = ''
  } catch (e) {
    loadError.value = describeError(e)
  } finally {
    loading.value = false
  }
}
onMounted(load)

const managed = computed(() => list.value?.keys.filter((k) => k.source === 'managed') ?? [])
const configKeySet = computed(() => !!list.value?.config_key_configured)

function randomKey() {
  const bytes = crypto.getRandomValues(new Uint8Array(24))
  return `sk-${Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('')}`
}

async function guarded(task: () => Promise<unknown>, success: string) {
  busy.value = true
  try {
    await task()
    notify('success', success)
    await load()
  } catch (e) {
    notifyError('操作失败', e)
  } finally {
    busy.value = false
  }
}

// ---------- Create managed key ----------
const createOpen = ref(false)
const createName = ref('')
const created = ref<KeyCreated | null>(null)
function openCreate() {
  createName.value = ''
  created.value = null
  createOpen.value = true
}
async function submitCreate() {
  busy.value = true
  try {
    created.value = await api.post<KeyCreated>('/keys', { name: createName.value.trim() || '未命名密钥' })
    await load()
  } catch (e) {
    notifyError('创建失败', e)
  } finally {
    busy.value = false
  }
}

// ---------- Rename ----------
const renameKey = ref<Key | null>(null)
const renameValue = ref('')
const renameOpen = computed({ get: () => !!renameKey.value, set: (v) => !v && (renameKey.value = null) })
function openRename(key: Key) {
  renameKey.value = key
  renameValue.value = key.name ?? ''
}
async function submitRename() {
  const key = renameKey.value
  if (!key) return
  renameKey.value = null
  await guarded(() => api.patch(`/keys/${enc(key.id)}`, { name: renameValue.value.trim() }), '名称已更新')
}

async function revoke(key: Key) {
  const ok = await confirm({
    title: '撤销密钥',
    message: `撤销后，使用「${key.name || key.id}」的客户端将立即无法访问生成接口。`,
    confirmText: '撤销',
    danger: true,
  })
  if (ok) await guarded(() => api.delete(`/keys/${enc(key.id)}`), '密钥已撤销')
}

// ---------- Config key ----------
const configOpen = ref(false)
const configValue = ref('')
const configValid = computed(() => /^[!-~]{1,4096}$/.test(configValue.value))
function openConfigKey() {
  configValue.value = randomKey()
  configOpen.value = true
}
async function submitConfigKey() {
  const value = configValue.value
  configOpen.value = false
  await guarded(async () => {
    await api.put('/keys/config', { api_key: value })
    await copyText(value, '新密钥已复制到剪贴板')
  }, '配置密钥已更新')
}
async function clearConfigKey() {
  const ok = await confirm({
    title: '清除配置密钥',
    message: '将把 config.json 中的 api_key 置空，使用它的客户端会立即失去访问权限。面板创建的密钥不受影响。',
    confirmText: '清除',
    danger: true,
  })
  if (ok) await guarded(() => api.delete('/keys/config'), '配置密钥已清除')
}

const endpoint = computed(() => session.baseUrl || (import.meta.env.DEV ? __DEV_BACKEND_ORIGIN__ : location.origin))
</script>

<template>
  <div>
    <PageHeader title="API 密钥" subtitle="生成接口（OpenAI / Gemini 兼容）使用的密钥。与管理密钥相互独立。">
      <button class="btn accent" @click="openCreate"><FIcon name="add" />创建密钥</button>
    </PageHeader>

    <div class="stack" style="--gap: 12px">
      <FInfoBar v-if="loadError" severity="error" title="加载失败" :message="loadError" />
      <FInfoBar
        v-if="list && !list.configured"
        severity="warning"
        title="尚无可用的 API 密钥"
        message="在配置或创建至少一个密钥之前，生成和模型接口会拒绝所有请求。"
      />

      <section class="settings-group">
        <h3>配置文件密钥</h3>
        <div class="setting">
          <FIcon name="lock_closed" :size="20" />
          <div class="setting-text">
            <div>config.json · api_key</div>
            <div class="desc">保存在配置文件中的单个密钥，以明文存储；轮换后旧值立即失效。</div>
          </div>
          <div class="setting-control">
            <span v-if="loading" class="skeleton" style="width: 64px; height: 22px" />
            <span v-else class="badge" :class="configKeySet ? 'success' : 'caution'">
              <FIcon :name="configKeySet ? 'checkmark_circle' : 'warning'" filled />{{ configKeySet ? '已配置' : '未配置' }}
            </span>
            <button class="btn" :disabled="busy" @click="openConfigKey">{{ configKeySet ? '轮换' : '设置' }}</button>
            <button v-if="configKeySet" class="btn subtle icon-only" aria-label="清除配置密钥" title="清除" :disabled="busy" @click="clearConfigKey">
              <FIcon name="delete" />
            </button>
          </div>
        </div>
      </section>

      <section class="settings-group">
        <h3>面板创建的密钥</h3>
        <div v-if="loading" class="skeleton" style="height: 68px; border-radius: 8px" />
        <div v-else-if="!managed.length" class="card">
          <EmptyState icon="key_multiple" title="还没有创建密钥" message="为不同的客户端分别创建密钥，便于单独撤销。">
            <button class="btn" @click="openCreate"><FIcon name="add" />创建密钥</button>
          </EmptyState>
        </div>
        <template v-else>
          <div v-for="key in managed" :key="key.id" class="setting">
            <FIcon name="key_multiple" :size="20" />
            <div class="setting-text">
              <div class="truncate">{{ key.name || '未命名密钥' }}</div>
              <div class="desc">
                <span class="mono">{{ key.id }}</span> · 创建于 {{ formatTimestamp(key.created) }}
              </div>
            </div>
            <div class="setting-control">
              <FMenu
                :items="[
                  { label: '重命名', icon: 'edit', action: () => openRename(key) },
                  { divider: true },
                  { label: '撤销密钥', icon: 'delete', danger: true, action: () => revoke(key) },
                ]"
              />
            </div>
          </div>
        </template>
      </section>

      <section class="settings-group">
        <h3>接入示例</h3>
        <div class="card pad stack" style="--gap: 8px">
          <p class="t-secondary t-caption">
            支持 <code>Authorization: Bearer</code>、<code>x-goog-api-key</code>、<code>x-api-key</code> 或 <code>?key=</code>。移动设备需使用可访问的服务地址。
          </p>
          <pre class="code-block">curl {{ endpoint }}/v1/chat/completions \
  -H "Authorization: Bearer YOUR_KEY" -H "Content-Type: application/json" \
  -d '{"model":"MODEL_ID","messages":[{"role":"user","content":"hello"}],"stream":true}'</pre>
          <div class="row wrap t-caption t-secondary">
            <span>OpenAI Base URL：<code>{{ endpoint }}/v1</code></span>
            <span>Gemini Base URL：<code>{{ endpoint }}/v1beta</code></span>
          </div>
        </div>
      </section>
    </div>

    <!-- Create -->
    <FDialog v-model:open="createOpen" :title="created ? '密钥已创建' : '创建密钥'" :width="480" :dismissible="!busy">
      <template v-if="!created">
        <div class="field">
          <label for="key-name">名称</label>
          <input id="key-name" v-model="createName" class="input" maxlength="128" placeholder="例如：SillyTavern" autofocus @keydown.enter="submitCreate" />
          <span class="hint">仅用于区分不同客户端。</span>
        </div>
      </template>
      <template v-else>
        <FInfoBar severity="warning" message="这是唯一一次显示完整密钥，关闭后无法再次查看。" />
        <div class="field">
          <label>{{ created.name }}</label>
          <div class="secret mono break">{{ created.secret }}</div>
        </div>
      </template>
      <template #footer>
        <template v-if="!created">
          <button class="btn accent" :disabled="busy" @click="submitCreate"><ProgressRing v-if="busy" />创建</button>
          <button class="btn" @click="createOpen = false">取消</button>
        </template>
        <template v-else>
          <button class="btn accent" @click="copyText(created.secret, '密钥已复制')"><FIcon name="copy" />复制</button>
          <button class="btn" @click="createOpen = false">完成</button>
        </template>
      </template>
    </FDialog>

    <!-- Rename -->
    <FDialog v-model:open="renameOpen" title="重命名密钥" :width="400">
      <div class="field">
        <label for="key-rename">名称</label>
        <input id="key-rename" v-model="renameValue" class="input" maxlength="128" autofocus @keydown.enter="submitRename" />
      </div>
      <template #footer>
        <button class="btn accent" @click="submitRename">保存</button>
        <button class="btn" @click="renameKey = null">取消</button>
      </template>
    </FDialog>

    <!-- Config key -->
    <FDialog v-model:open="configOpen" :title="configKeySet ? '轮换配置密钥' : '设置配置密钥'" :width="520">
      <p class="t-secondary">新密钥会写入 config.json 并立即生效。已为你生成一个随机密钥，也可以输入自己的值（最多 4096 个可见 ASCII 字符，不含空白）。</p>
      <div class="field">
        <label for="config-key">新密钥</label>
        <SecretInput id="config-key" v-model="configValue" :invalid="!configValid" />
        <button class="hyperlink" type="button" @click="configValue = randomKey()"><FIcon name="arrow_sync" :size="14" />重新生成</button>
      </div>
      <template #footer>
        <button class="btn accent" :disabled="!configValid" @click="submitConfigKey">保存并复制</button>
        <button class="btn" @click="configOpen = false">取消</button>
      </template>
    </FDialog>
  </div>
</template>

<style scoped>
.secret {
  padding: 10px 12px;
  border-radius: var(--radius-control);
  background: var(--control-alt-secondary);
  border: 1px solid var(--card-stroke);
  user-select: all;
}
@media (max-width: 640px) {
  .setting {
    flex-wrap: wrap;
  }
  .setting .setting-text {
    flex-basis: calc(100% - 40px);
  }
  .setting .setting-control {
    margin-left: auto;
  }
}
</style>
