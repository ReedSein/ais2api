<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import EmptyState from '../components/EmptyState.vue'
import FIcon from '../components/FIcon.vue'
import FInfoBar from '../components/FInfoBar.vue'
import PageHeader from '../components/PageHeader.vue'
import ProgressRing from '../components/ProgressRing.vue'
import { api, describeError, runOperation } from '../api/client'
import type { ConfigView, Model } from '../api/types'
import { copyText, notify, notifyError } from '../stores/ui'
import { formatCompact, modelId } from '../utils/format'

const models = ref<Model[]>([])
const aliases = ref<Record<string, string>>({})
const loading = ref(true)
const refreshing = ref(false)
const elapsed = ref(0)
const loadError = ref('')
const query = ref('')

async function load() {
  try {
    const [m, c] = await Promise.all([api.get<{ models: Model[] }>('/models'), api.get<ConfigView>('/config').catch(() => null)])
    models.value = m.models ?? []
    aliases.value = c?.config.model_aliases ?? {}
    loadError.value = ''
  } catch (e) {
    loadError.value = describeError(e)
  } finally {
    loading.value = false
  }
}
onMounted(load)

async function refresh() {
  refreshing.value = true
  elapsed.value = 0
  try {
    const result = await runOperation<{ models?: Model[] }>('/models/refresh', { onPoll: (ms) => (elapsed.value = Math.round(ms / 1000)) })
    models.value = result.models ?? models.value
    notify('success', `已发现 ${result.models?.length ?? 0} 个模型`)
  } catch (e) {
    notifyError('模型发现失败', e)
  } finally {
    refreshing.value = false
  }
}

const filtered = computed(() => {
  const q = query.value.trim().toLowerCase()
  if (!q) return models.value
  return models.value.filter((m) => [m.name, m.displayName, m.description].some((v) => typeof v === 'string' && v.toLowerCase().includes(q)))
})

const configuredOnly = computed(() => models.value.length > 0 && models.value.every((m) => m.source === 'configured'))
const aliasEntries = computed(() => Object.entries(aliases.value))

const methods = (m: Model) => (Array.isArray(m.supportedGenerationMethods) ? m.supportedGenerationMethods : [])
</script>

<template>
  <div>
    <PageHeader title="模型" subtitle="来自实时发现、缓存或显式配置的模型。网关不内置声称可用的模型清单。">
      <button class="btn accent" :disabled="refreshing" @click="refresh">
        <ProgressRing v-if="refreshing" class="on-accent" /><FIcon v-else name="arrow_sync" />
        {{ refreshing ? `发现中 ${elapsed}s` : '实时发现' }}
      </button>
    </PageHeader>

    <div class="stack" style="--gap: 12px">
      <FInfoBar v-if="loadError" severity="error" title="加载失败" :message="loadError" />
      <FInfoBar v-if="refreshing" severity="info" message="正在通过浏览器访问 AI Studio 获取模型列表，冷启动可能需要十几秒。" />
      <FInfoBar
        v-if="configuredOnly && !refreshing"
        severity="info"
        message="当前仅显示 configured_models 中显式配置的模型，尚无实时发现的缓存。"
      />

      <div class="row wrap">
        <div class="input-group search">
          <span class="lead"><FIcon name="search" /></span>
          <input v-model="query" class="input has-lead" type="search" placeholder="搜索模型" aria-label="搜索模型" />
        </div>
        <span class="t-secondary t-caption">{{ filtered.length }} / {{ models.length }} 个模型</span>
      </div>

      <div v-if="loading" class="card-grid" style="--min: 280px">
        <div v-for="i in 6" :key="i" class="skeleton" style="height: 120px; border-radius: 8px" />
      </div>
      <div v-else-if="!models.length" class="card">
        <EmptyState icon="cube" title="没有缓存的模型" message="执行一次实时发现，或在服务配置中添加 configured_models。">
          <button class="btn accent" :disabled="refreshing" @click="refresh"><FIcon name="arrow_sync" />实时发现</button>
        </EmptyState>
      </div>
      <div v-else class="card-grid" style="--min: 300px">
        <article v-for="m in filtered" :key="m.name" class="card model">
          <header class="row">
            <h2 class="t-body-strong grow truncate" :title="m.displayName || modelId(m.name)">{{ m.displayName || modelId(m.name) }}</h2>
            <span v-if="m.source" class="badge outline">{{ m.source === 'configured' ? '配置' : m.source === 'cache' ? '缓存' : m.source }}</span>
          </header>
          <button class="id-btn mono" :title="'复制 ' + modelId(m.name)" @click="copyText(modelId(m.name), '模型 ID 已复制')">
            <span class="truncate">{{ modelId(m.name) }}</span>
            <FIcon name="copy" :size="14" />
          </button>
          <p v-if="m.description" class="desc t-secondary t-caption">{{ m.description }}</p>
          <footer class="row wrap t-caption t-secondary limits">
            <span v-if="m.inputTokenLimit" title="输入 token 上限"><FIcon name="arrow_down" :size="12" /> 输入 {{ formatCompact(m.inputTokenLimit) }}</span>
            <span v-if="m.outputTokenLimit" title="输出 token 上限"><FIcon name="arrow_upload" :size="12" /> 输出 {{ formatCompact(m.outputTokenLimit) }}</span>
            <span v-for="method in methods(m).slice(0, 3)" :key="method" class="badge">{{ method }}</span>
          </footer>
        </article>
      </div>

      <section v-if="aliasEntries.length" class="card pad stack" style="--gap: 8px">
        <div class="row">
          <h2 class="t-body-strong grow">模型别名</h2>
          <RouterLink to="/config" class="t-caption">在服务配置中编辑</RouterLink>
        </div>
        <div class="table-wrap">
          <table class="table">
            <thead>
              <tr>
                <th>请求名</th>
                <th>实际模型</th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="[alias, target] in aliasEntries" :key="alias">
                <td class="mono break">{{ alias }}</td>
                <td class="mono break">{{ target }}</td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>
    </div>
  </div>
</template>

<style scoped>
.search {
  flex: 1 1 260px;
  max-width: 420px;
}
.model {
  display: flex;
  flex-direction: column;
  gap: 8px;
  padding: 14px 16px;
  min-width: 0;
}
.id-btn {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  max-width: 100%;
  align-self: flex-start;
  padding: 2px 6px;
  margin-left: -6px;
  border: 0;
  border-radius: var(--radius-control);
  background: transparent;
  color: var(--text-secondary);
  cursor: pointer;
}
.id-btn:hover {
  background: var(--subtle-secondary);
  color: var(--text-primary);
}
.desc {
  display: -webkit-box;
  -webkit-line-clamp: 2;
  line-clamp: 2;
  -webkit-box-orient: vertical;
  overflow: hidden;
}
.limits {
  margin-top: auto;
  gap: 6px 12px;
}
.limits > span:not(.badge) {
  display: inline-flex;
  align-items: center;
  gap: 4px;
}
.on-accent {
  color: var(--text-on-accent);
}
</style>
