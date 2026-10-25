<script setup lang="ts">
// Two import paths:
//  · 直接导入   POST /accounts/import        — stores credentials as given
//  · 验证后导入 POST /accounts/probe-import  — opens a browser, confirms the
//    Google identity, then merges (same flow as the userscript)
import { computed, ref, watch } from 'vue'
import FDialog from './FDialog.vue'
import FIcon from './FIcon.vue'
import FInfoBar from './FInfoBar.vue'
import ProgressRing from './ProgressRing.vue'
import { api, describeError } from '../api/client'
import { notify } from '../stores/ui'

const open = defineModel<boolean>('open', { default: false })
const emit = defineEmits<{ imported: [] }>()

const mode = ref<'direct' | 'verify'>('direct')
const text = ref('')
const name = ref('')
const authUser = ref('')
const email = ref('')
const busy = ref(false)
const error = ref('')
const fileInput = ref<HTMLInputElement>()
let controller: AbortController | null = null

watch(open, (value) => {
  if (value) {
    text.value = ''
    name.value = ''
    authUser.value = ''
    email.value = ''
    error.value = ''
  } else controller?.abort()
})

function parsed(): unknown {
  const trimmed = text.value.trim()
  if (/^[[{]/.test(trimmed)) {
    try {
      return JSON.parse(trimmed)
    } catch {
      /* fall through: treat as plain Cookie text */
    }
  }
  return trimmed
}

const canSubmit = computed(() => text.value.trim().length > 0 && !busy.value && (!authUser.value || /^\d{1,2}$/.test(authUser.value)))

async function pickFile(event: Event) {
  const file = (event.target as HTMLInputElement).files?.[0]
  if (!file) return
  if (file.size > 1024 * 1024) {
    error.value = '文件超过 1 MiB。'
    return
  }
  text.value = await file.text()
  if (!name.value) name.value = file.name.replace(/\.(json|txt)$/i, '')
  ;(event.target as HTMLInputElement).value = ''
}

async function submit() {
  error.value = ''
  busy.value = true
  controller = new AbortController()
  try {
    if (mode.value === 'direct') {
      const body: Record<string, unknown> = { input: parsed() }
      if (name.value.trim()) body.name = name.value.trim()
      const result = await api.post<{ imported?: number }>('/accounts/import', body, { signal: controller.signal })
      notify('success', `已导入 ${result.imported ?? 0} 个账号`, '建议随后执行一次账号探测以确认登录状态。')
    } else {
      const value = parsed()
      const body: Record<string, unknown> = {}
      if (value && typeof value === 'object' && !Array.isArray(value) && ('origins' in value || 'cookies' in value)) body.storage_state = value
      else body.cookies = value
      if (authUser.value !== '') body.auth_user = authUser.value
      if (email.value.trim()) body.email = email.value.trim()
      if (name.value.trim()) body.name_prefix = name.value.trim()
      const result = await api.post<{ imported_count?: number; details?: { truncated?: boolean } }>('/accounts/probe-import', body, {
        signal: controller.signal,
      })
      notify(
        'success',
        `已验证并导入 ${result.imported_count ?? 0} 个账号`,
        result.details?.truncated ? '已达到单次探测上限，可能还有更多账号索引未检查。' : '仅验证了浏览器登录身份，生成能力需另行确认。',
        7000,
      )
    }
    emit('imported')
    open.value = false
  } catch (e) {
    if (!(e instanceof DOMException && e.name === 'AbortError')) error.value = describeError(e)
  } finally {
    busy.value = false
    controller = null
  }
}
</script>

<template>
  <FDialog v-model:open="open" title="导入账号" :width="600" :dismissible="!busy">
    <div class="segmented" role="tablist" aria-label="导入方式">
      <button role="tab" :aria-selected="mode === 'direct'" :disabled="busy" @click="mode = 'direct'">
        <FIcon name="arrow_upload" />直接导入
      </button>
      <button role="tab" :aria-selected="mode === 'verify'" :disabled="busy" @click="mode = 'verify'">
        <FIcon name="shield" />验证后导入
      </button>
    </div>

    <p class="t-secondary t-caption">
      <template v-if="mode === 'direct'">
        支持 Cookie 请求头、Netscape Cookie 文本、Cookie JSON、storage state 和凭据包。凭据保存在后端数据库，不会出现在列表中。
      </template>
      <template v-else>
        后端会启动浏览器确认 Google 账号身份后再保存；重复导入会更新同一账号。未指定账号索引时依次探测 0–7，单个索引未登录会继续，连续三次失败后停止，最长可能需要数分钟。storage state 中的 localStorage 会被丢弃，仅保留 Cookie。
      </template>
    </p>

    <FInfoBar v-if="error" severity="error" :message="error" />

    <div class="field">
      <div class="row">
        <label for="import-text" class="grow">凭据内容</label>
        <button type="button" class="btn subtle" :disabled="busy" @click="fileInput?.click()">
          <FIcon name="document_arrow_down" />从文件读取
        </button>
        <input ref="fileInput" type="file" accept=".json,.txt,text/plain,application/json" hidden @change="pickFile" />
      </div>
      <textarea
        id="import-text"
        v-model="text"
        class="textarea mono"
        rows="8"
        :disabled="busy"
        spellcheck="false"
        autocapitalize="off"
        placeholder="SID=…; __Secure-1PSID=…   或粘贴 JSON"
      />
    </div>

    <div class="grid">
      <div class="field">
        <label for="import-name">{{ mode === 'direct' ? '名称（可选）' : '名称前缀（可选）' }}</label>
        <input id="import-name" v-model="name" class="input" maxlength="128" :disabled="busy" />
      </div>
      <template v-if="mode === 'verify'">
        <div class="field">
          <label for="import-auth">账号索引 authuser（可选）</label>
          <input
            id="import-auth"
            v-model="authUser"
            class="input"
            inputmode="numeric"
            placeholder="0"
            :aria-invalid="authUser !== '' && !/^\d{1,2}$/.test(authUser)"
            :disabled="busy"
          />
        </div>
        <div class="field span">
          <label for="import-email">预期邮箱（可选，用于核对身份）</label>
          <input id="import-email" v-model="email" class="input" type="email" autocomplete="off" :disabled="busy" />
        </div>
      </template>
    </div>

    <div v-if="busy" class="row t-secondary">
      <ProgressRing />
      <span>{{ mode === 'verify' ? '正在启动浏览器验证身份，请稍候…' : '正在导入…' }}</span>
    </div>

    <template #footer>
      <button class="btn accent" :disabled="!canSubmit" @click="submit">{{ mode === 'verify' ? '验证并导入' : '导入' }}</button>
      <button class="btn" @click="busy ? controller?.abort() : (open = false)">{{ busy ? '取消请求' : '取消' }}</button>
    </template>
  </FDialog>
</template>

<style scoped>
.grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 12px;
}
.grid .span {
  grid-column: 1 / -1;
}
@media (max-width: 640px) {
  .grid {
    grid-template-columns: minmax(0, 1fr);
  }
}
.segmented {
  align-self: flex-start;
}
</style>
