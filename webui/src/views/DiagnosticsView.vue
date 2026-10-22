<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import FIcon from '../components/FIcon.vue'
import FInfoBar from '../components/FInfoBar.vue'
import PageHeader from '../components/PageHeader.vue'
import ProgressRing from '../components/ProgressRing.vue'
import { api, describeError } from '../api/client'
import type { Capabilities, ConfigView, LogSource, Status } from '../api/types'
import { refreshStatus } from '../stores/service'
import { confirm, copyText, downloadFile, notify, notifyError } from '../stores/ui'
import { formatBytes, formatDuration } from '../utils/format'

interface Diagnostics {
  version?: string
  status?: Status
  config?: ConfigView
  stats?: { records?: unknown[] }
  logs?: { sources?: LogSource[] }
  google_verified?: boolean
}

const data = ref<Diagnostics | null>(null)
const capabilities = ref<Capabilities | null>(null)
const loading = ref(true)
const loadError = ref('')
const exporting = ref(false)
const showRaw = ref(false)
const stopping = ref(false)

async function load() {
  loading.value = true
  try {
    const [d, c] = await Promise.all([api.get<Diagnostics>('/diagnostics'), api.get<Capabilities>('/capabilities').catch(() => null)])
    data.value = d
    capabilities.value = c
    loadError.value = ''
  } catch (e) {
    loadError.value = describeError(e)
  } finally {
    loading.value = false
  }
}
onMounted(load)

const json = computed(() => JSON.stringify(data.value, null, 2))
const sources = computed(() => data.value?.logs?.sources ?? [])
const logBytes = computed(() => sources.value.reduce((sum, s) => sum + s.bytes, 0))

async function exportSnapshot() {
  exporting.value = true
  try {
    const snapshot = await api.post<unknown>('/diagnostics/export')
    downloadFile(`ais2api-diagnostics-${new Date().toISOString().replace(/[:.]/g, '-')}.json`, JSON.stringify(snapshot, null, 2))
    notify('success', '诊断快照已导出', '内容已脱敏，不含密钥与 Cookie。')
  } catch (e) {
    notifyError('导出失败', e)
  } finally {
    exporting.value = false
  }
}

async function stopService() {
  const ok = await confirm({
    title: '停止服务',
    message: '将停止 ais2api 服务进程，所有进行中的请求会被取消。停止后需要在服务器上手动运行 ais2api start 重新启动，本控制台无法远程启动。',
    confirmText: '停止服务',
    danger: true,
    typeToConfirm: 'STOP',
  })
  if (!ok) return
  stopping.value = true
  try {
    await api.post('/service/stop')
    notify('warning', '服务正在停止', '状态指示会在数秒内变为“无法连接”。')
    setTimeout(refreshStatus, 2000)
  } catch (e) {
    notifyError('停止失败', e)
  } finally {
    stopping.value = false
  }
}

const capabilityList = computed(() =>
  Object.entries(capabilities.value ?? {})
    .filter(([key]) => key !== 'api_version')
    .map(([key, value]) => ({ key, value: typeof value === 'object' ? JSON.stringify(value) : String(value) })),
)
</script>

<template>
  <div>
    <PageHeader title="诊断" subtitle="脱敏的运行快照与日志清单。诊断与就绪检查都不代表 Google 上游生成已验证。">
      <button class="btn" :disabled="loading" @click="load"><FIcon name="arrow_clockwise" />刷新</button>
      <button class="btn accent" :disabled="exporting" @click="exportSnapshot">
        <ProgressRing v-if="exporting" class="on-accent" /><FIcon v-else name="arrow_download" />导出快照
      </button>
    </PageHeader>

    <div class="stack" style="--gap: 12px">
      <FInfoBar v-if="loadError" severity="error" title="加载失败" :message="loadError" />

      <template v-if="data">
        <FInfoBar
          v-if="data.google_verified === false"
          severity="info"
          message="google_verified = false：本地检查通过不等于上游可用，真实可用性需通过账号探测和实际生成确认。"
        />

        <section class="settings-group">
          <h3>运行时</h3>
          <div class="card info-grid">
            <div><span class="t-caption t-secondary">版本</span><strong>{{ data.version ?? '—' }}</strong></div>
            <div><span class="t-caption t-secondary">管理 API</span><strong>{{ capabilities?.api_version ?? 'v1' }}</strong></div>
            <div><span class="t-caption t-secondary">运行时间</span><strong>{{ formatDuration(data.status?.uptime_seconds) }}</strong></div>
            <div>
              <span class="t-caption t-secondary">本地就绪</span>
              <strong class="row" style="--gap: 6px">
                <span class="dot" :class="data.status?.ready ? 'success' : 'caution'" />{{ data.status?.ready ? '是' : '否' }}
              </strong>
            </div>
            <div>
              <span class="t-caption t-secondary">日志写入</span>
              <strong class="row" style="--gap: 6px">
                <span class="dot" :class="data.status?.logging_healthy ? 'success' : 'critical'" />
                {{ data.status?.logging_healthy ? '正常' : `积压 ${data.status?.pending_logs ?? 0}` }}
              </strong>
            </div>
            <div><span class="t-caption t-secondary">统计记录</span><strong>{{ data.stats?.records?.length ?? 0 }} 条</strong></div>
            <div class="wide">
              <span class="t-caption t-secondary">配置文件</span>
              <button class="path mono" title="复制路径" @click="copyText(data.config?.config_file ?? '')">
                <span class="break">{{ data.config?.config_file ?? '—' }}</span><FIcon name="copy" :size="14" />
              </button>
            </div>
          </div>
        </section>

        <section class="settings-group">
          <h3>日志文件 · 共 {{ formatBytes(logBytes) }}</h3>
          <div class="card">
            <table class="table">
              <thead>
                <tr>
                  <th>日志源</th>
                  <th class="num">大小</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="s in sources" :key="s.id">
                  <td>
                    <RouterLink :to="{ path: '/logs' }" class="mono">{{ s.id }}</RouterLink>
                  </td>
                  <td class="num">{{ formatBytes(s.bytes) }}</td>
                </tr>
                <tr v-if="!sources.length">
                  <td colspan="2" class="t-secondary">没有日志文件</td>
                </tr>
              </tbody>
            </table>
          </div>
        </section>

        <section v-if="capabilityList.length" class="settings-group">
          <h3>能力</h3>
          <div class="card pad row wrap caps">
            <span v-for="c in capabilityList" :key="c.key" class="badge outline" :title="c.value">
              {{ c.key }}<template v-if="c.value !== 'true'">: {{ c.value.length > 32 ? '…' : c.value }}</template>
            </span>
          </div>
        </section>

        <section class="settings-group">
          <h3>原始快照</h3>
          <div class="card">
            <button class="expander" :aria-expanded="showRaw" @click="showRaw = !showRaw">
              <FIcon name="code" :size="20" />
              <span class="grow">查看 JSON</span>
              <FIcon :name="showRaw ? 'chevron_down' : 'chevron_right'" />
            </button>
            <div v-if="showRaw" class="raw">
              <div class="row" style="justify-content: flex-end; padding: 0 0 8px">
                <button class="btn subtle" @click="copyText(json)"><FIcon name="copy" />复制</button>
              </div>
              <pre class="code-block">{{ json }}</pre>
            </div>
          </div>
        </section>
      </template>
      <div v-else-if="loading" class="stack" style="--gap: 8px">
        <div class="skeleton" style="height: 160px; border-radius: 8px" />
        <div class="skeleton" style="height: 120px; border-radius: 8px" />
      </div>

      <section class="settings-group">
        <h3>危险操作</h3>
        <div class="setting danger-zone">
          <FIcon name="power" :size="20" />
          <div class="setting-text">
            <div>停止服务</div>
            <div class="desc">远程停止 ais2api 进程。停止后无法从本控制台重新启动。</div>
          </div>
          <div class="setting-control">
            <button class="btn danger" :disabled="stopping" @click="stopService"><ProgressRing v-if="stopping" />停止服务</button>
          </div>
        </div>
      </section>
    </div>
  </div>
</template>

<style scoped>
.info-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(180px, 1fr));
  gap: 16px 24px;
  padding: 16px;
}
.info-grid > div {
  display: flex;
  flex-direction: column;
  gap: 2px;
  min-width: 0;
}
.info-grid .wide {
  grid-column: 1 / -1;
}
.path {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  padding: 2px 6px;
  margin-left: -6px;
  border: 0;
  border-radius: var(--radius-control);
  background: transparent;
  color: var(--text-primary);
  text-align: left;
  cursor: pointer;
}
.path:hover {
  background: var(--subtle-secondary);
}
.caps {
  gap: 8px;
}
.expander {
  display: flex;
  align-items: center;
  gap: 16px;
  width: 100%;
  min-height: 56px;
  padding: 12px 16px;
  border: 0;
  background: transparent;
  color: var(--text-primary);
  text-align: left;
  cursor: pointer;
  border-radius: var(--radius-overlay);
}
.expander:hover {
  background: var(--subtle-tertiary);
}
.raw {
  padding: 0 16px 16px;
}
.raw .code-block {
  max-height: 480px;
}
.danger-zone {
  border-color: color-mix(in srgb, var(--critical) 35%, transparent);
}
.danger-zone > .icon {
  color: var(--critical);
}
.on-accent {
  color: var(--text-on-accent);
}
@media (max-width: 640px) {
  .danger-zone {
    flex-wrap: wrap;
  }
  .danger-zone .setting-text {
    flex-basis: calc(100% - 40px);
  }
  .danger-zone .setting-control {
    flex-basis: 100%;
  }
  .danger-zone .btn {
    width: 100%;
  }
}
</style>
