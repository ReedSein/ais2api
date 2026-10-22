<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import EmptyState from '../components/EmptyState.vue'
import FIcon from '../components/FIcon.vue'
import FInfoBar from '../components/FInfoBar.vue'
import PageHeader from '../components/PageHeader.vue'
import StatTile from '../components/StatTile.vue'
import SuccessMeter from '../components/SuccessMeter.vue'
import { api, describeError } from '../api/client'
import type { Account, StatRecord } from '../api/types'
import { confirm, downloadFile, notify, notifyError } from '../stores/ui'
import { accountLabel } from '../utils/accounts'
import { formatCompact, formatNumber, formatPercent, modelId, usageTotals } from '../utils/format'

const records = ref<StatRecord[]>([])
const accounts = ref<Account[]>([])
const loading = ref(true)
const loadError = ref('')
const group = ref<'model' | 'account' | 'detail'>('model')

async function load() {
  try {
    const [s, a] = await Promise.all([api.get<{ records: StatRecord[] }>('/stats'), api.get<{ accounts: Account[] }>('/accounts')])
    records.value = s.records ?? []
    accounts.value = a.accounts ?? []
    loadError.value = ''
  } catch (e) {
    loadError.value = describeError(e)
  } finally {
    loading.value = false
  }
}
onMounted(load)

const accountName = (id: string) => accountLabel(accounts.value.find((a) => a.id === id), id)

interface Row {
  key: string
  label: string
  sub?: string
  success: number
  failure: number
  prompt: number
  output: number
  total: number
}

const rows = computed<Row[]>(() => {
  const map = new Map<string, Row>()
  for (const record of records.value) {
    const key = group.value === 'model' ? record.model : group.value === 'account' ? record.account : `${record.account}\u0000${record.model}`
    const label = group.value === 'account' ? accountName(record.account) : modelId(record.model)
    const sub = group.value === 'detail' ? accountName(record.account) : undefined
    const row = map.get(key) ?? { key, label, sub, success: 0, failure: 0, prompt: 0, output: 0, total: 0 }
    const usage = usageTotals(record.usage)
    row.success += record.success
    row.failure += record.failure
    row.prompt += usage.prompt
    row.output += usage.output
    row.total += usage.total
    map.set(key, row)
  }
  return [...map.values()].sort((a, b) => b.success + b.failure - (a.success + a.failure))
})

const maxRequests = computed(() => Math.max(1, ...rows.value.map((r) => r.success + r.failure)))

const totals = computed(() =>
  rows.value.reduce(
    (acc, r) => ({
      success: acc.success + r.success,
      failure: acc.failure + r.failure,
      prompt: acc.prompt + r.prompt,
      output: acc.output + r.output,
      total: acc.total + r.total,
    }),
    { success: 0, failure: 0, prompt: 0, output: 0, total: 0 },
  ),
)
const requests = computed(() => totals.value.success + totals.value.failure)

async function resetStats() {
  const ok = await confirm({
    title: '重置统计',
    message: '将清空全部账号与模型的请求计数和用量记录。此操作无法撤销。',
    confirmText: '重置',
    danger: true,
    typeToConfirm: '重置',
  })
  if (!ok) return
  try {
    await api.delete('/stats')
    notify('success', '统计已重置')
    await load()
  } catch (e) {
    notifyError('重置失败', e)
  }
}

function exportCsv() {
  const head = ['account_id', 'account', 'model', 'success', 'failure', 'prompt_tokens', 'output_tokens', 'total_tokens']
  const quote = (v: string | number) => `"${String(v).replace(/"/g, '""')}"`
  const lines = records.value.map((r) => {
    const u = usageTotals(r.usage)
    return [r.account, accountName(r.account), r.model, r.success, r.failure, u.prompt, u.output, u.total].map(quote).join(',')
  })
  downloadFile(`ais2api-stats-${new Date().toISOString().slice(0, 10)}.csv`, '﻿' + [head.join(','), ...lines].join('\r\n'), 'text/csv')
}
</script>

<template>
  <div>
    <PageHeader title="用量统计" subtitle="按账号与模型累计的请求结果和上游返回的 token 用量。">
      <button class="btn" :disabled="!records.length" @click="exportCsv"><FIcon name="arrow_download" />导出 CSV</button>
      <button class="btn" @click="load"><FIcon name="arrow_clockwise" />刷新</button>
      <button class="btn subtle" :disabled="!records.length" @click="resetStats"><FIcon name="delete" />重置</button>
    </PageHeader>

    <div class="stack" style="--gap: 12px">
      <FInfoBar v-if="loadError" severity="error" title="加载失败" :message="loadError" />

      <section class="card-grid kpis" style="--min: 180px" aria-label="合计">
        <StatTile label="请求总数" :value="formatCompact(requests)" icon="data_bar_vertical" :caption="`${rows.length} 个${group === 'account' ? '账号' : '分组'}`" />
        <StatTile
          label="成功率"
          :value="requests ? formatPercent(totals.success / requests) : '—'"
          icon="checkmark_circle"
          :caption="`失败 ${formatNumber(totals.failure)} 次`"
        />
        <StatTile label="输入 Tokens" :value="formatCompact(totals.prompt)" icon="arrow_upload" />
        <StatTile label="输出 Tokens" :value="formatCompact(totals.output)" icon="arrow_download" :caption="`合计 ${formatCompact(totals.total)}`" />
      </section>

      <section class="card" aria-labelledby="breakdown-title">
        <div class="row wrap head">
          <h2 id="breakdown-title" class="t-body-strong grow">分组明细</h2>
          <div class="segmented" role="group" aria-label="分组方式">
            <button :aria-pressed="group === 'model'" @click="group = 'model'">按模型</button>
            <button :aria-pressed="group === 'account'" @click="group = 'account'">按账号</button>
            <button :aria-pressed="group === 'detail'" @click="group = 'detail'">账号 × 模型</button>
          </div>
        </div>

        <div v-if="loading" class="stack" style="--gap: 8px; padding: 0 16px 16px">
          <div v-for="i in 4" :key="i" class="skeleton" style="height: 36px" />
        </div>
        <EmptyState v-else-if="!rows.length" icon="data_bar_vertical" title="暂无统计" message="产生生成请求后，这里会显示各模型与账号的成功率与用量。" />
        <div v-else class="table-wrap">
          <table class="table stats">
            <thead>
              <tr>
                <th>{{ group === 'account' ? '账号' : '模型' }}</th>
                <th class="bar-col">请求数</th>
                <th>成功率</th>
                <th class="num">输入</th>
                <th class="num">输出</th>
                <th class="num">合计 Tokens</th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="row in rows" :key="row.key">
                <td class="name">
                  <div class="truncate" :class="{ mono: group !== 'account' }" :title="row.label">{{ row.label }}</div>
                  <div v-if="row.sub" class="t-caption t-secondary truncate">{{ row.sub }}</div>
                </td>
                <td class="bar-col">
                  <div class="bar" :title="`成功 ${row.success} · 失败 ${row.failure}`">
                    <div class="bar-track">
                      <div class="bar-fill" :style="{ width: `${((row.success + row.failure) / maxRequests) * 100}%` }" />
                    </div>
                    <span class="tabular">{{ formatNumber(row.success + row.failure) }}</span>
                  </div>
                </td>
                <td><SuccessMeter :success="row.success" :failure="row.failure" compact /></td>
                <td class="num">{{ formatCompact(row.prompt) }}</td>
                <td class="num">{{ formatCompact(row.output) }}</td>
                <td class="num">{{ formatCompact(row.total) }}</td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>
      <p class="t-caption t-tertiary">Token 数来自上游 usageMetadata 的累计值；失败请求不计用量。</p>
    </div>
  </div>
</template>

<style scoped>
.head {
  padding: 16px 16px 8px;
}
.stats {
  min-width: 720px;
}
.name {
  max-width: 280px;
}
.bar-col {
  width: 30%;
}
.bar {
  display: flex;
  align-items: center;
  gap: 8px;
}
.bar-track {
  flex: 1;
  height: 12px;
}
.bar-fill {
  height: 100%;
  min-width: 2px;
  border-radius: 0 4px 4px 0;
  background: var(--accent);
  transition: width var(--motion-slow) var(--ease-decelerate);
}
tr:hover .bar-fill {
  background: var(--accent-hover);
}
.bar span {
  width: 4.5em;
  text-align: right;
  font: var(--type-caption);
  color: var(--text-secondary);
}
</style>
