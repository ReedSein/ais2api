<script setup lang="ts">
import FIcon from '../components/FIcon.vue'
import FToggle from '../components/FToggle.vue'
import PageHeader from '../components/PageHeader.vue'
import type { IconName } from '../icons'
import { backendLabel, session, signOut } from '../stores/session'
import { ACCENTS, theme, type ThemeMode } from '../stores/theme'
import { confirm } from '../stores/ui'

const modes: { value: ThemeMode; label: string; icon: IconName }[] = [
  { value: 'light', label: '浅色', icon: 'weather_sunny' },
  { value: 'dark', label: '深色', icon: 'weather_moon' },
  { value: 'system', label: '跟随系统', icon: 'dark_theme' },
]

async function logout() {
  if (await confirm({ title: '退出登录', message: '将清除本设备保存的管理密钥。', confirmText: '退出' })) signOut()
}

const appVersion = __APP_VERSION__
</script>

<template>
  <div>
    <PageHeader title="设置" subtitle="控制台的外观与连接偏好，仅保存在当前浏览器中。" />

    <div class="stack" style="--gap: 4px">
      <section class="settings-group">
        <h3>外观</h3>
        <div class="setting stacked">
          <FIcon name="dark_theme" :size="20" />
          <div class="setting-text">
            <div>应用主题</div>
            <div class="desc">选择浅色、深色，或跟随操作系统的设置。</div>
          </div>
          <div class="setting-control">
            <div class="mode-grid" role="radiogroup" aria-label="应用主题">
              <button
                v-for="m in modes"
                :key="m.value"
                class="mode"
                role="radio"
                :aria-checked="theme.mode === m.value"
                :class="[m.value, { selected: theme.mode === m.value }]"
                @click="theme.mode = m.value"
              >
                <span class="preview" aria-hidden="true"><span class="bar" /><span class="win"><span class="accent-chip" /></span></span>
                <span class="row" style="--gap: 6px"><FIcon :name="m.icon" />{{ m.label }}</span>
              </button>
            </div>
          </div>
        </div>
        <div class="setting stacked">
          <FIcon name="color" :size="20" />
          <div class="setting-text">
            <div>强调色</div>
            <div class="desc">用于按钮、选中指示和图表。</div>
          </div>
          <div class="setting-control">
            <div class="swatches" role="radiogroup" aria-label="强调色">
              <button
                v-for="(preset, key) in ACCENTS"
                :key="key"
                class="swatch"
                role="radio"
                :aria-checked="theme.accent === key"
                :aria-label="preset.label"
                :title="preset.label"
                :style="{ background: preset.swatch }"
                @click="theme.accent = key"
              >
                <FIcon v-if="theme.accent === key" name="checkmark" />
              </button>
            </div>
          </div>
        </div>
        <div class="setting">
          <FIcon name="sparkle" :size="20" />
          <div class="setting-text">
            <div>Mica 背景</div>
            <div class="desc">以强调色淡染窗口背景，模拟 Windows 11 的 Mica 材质。低性能设备可关闭。</div>
          </div>
          <div class="setting-control"><FToggle v-model="theme.mica" label="Mica 背景" /></div>
        </div>
      </section>

      <section class="settings-group">
        <h3>连接</h3>
        <div class="setting">
          <FIcon name="server" :size="20" />
          <div class="setting-text">
            <div>后端</div>
            <div class="desc mono break">{{ session.baseUrl || backendLabel }}</div>
          </div>
          <div class="setting-control">
            <span class="badge">{{ session.baseUrl ? '直接连接' : '同源连接' }}</span>
          </div>
        </div>
        <div class="setting">
          <FIcon name="lock_closed" :size="20" />
          <div class="setting-text">
            <div>管理密钥存储</div>
            <div class="desc">{{ session.remember ? '保存在本设备（localStorage），关闭浏览器后仍保持登录。' : '仅保存在当前标签页（sessionStorage）。' }}</div>
          </div>
          <div class="setting-control">
            <button class="btn" @click="logout"><FIcon name="sign_out" />退出登录</button>
          </div>
        </div>
        <div class="setting">
          <FIcon name="key_reset" :size="20" />
          <div class="setting-text">
            <div>管理密码</div>
            <div class="desc">修改登录管理面板使用的密码（config.json 中的 admin_key），立即生效。</div>
          </div>
          <div class="setting-control">
            <RouterLink class="btn" to="/change-password">修改密码</RouterLink>
          </div>
        </div>
      </section>

      <section class="settings-group">
        <h3>关于</h3>
        <div class="setting">
          <img src="/favicon.svg" alt="" width="20" height="20" />
          <div class="setting-text">
            <div>ais2api 控制台 {{ appVersion }}</div>
            <div class="desc">基于 Windows 11 Fluent Design 的 ais2api 管理界面，使用管理 API v1。图标来自 Fluent UI System Icons（MIT）。</div>
          </div>
        </div>
      </section>
    </div>
  </div>
</template>

<style scoped>
.mode-grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 12px;
  width: 100%;
  max-width: 520px;
}
.mode {
  display: flex;
  flex-direction: column;
  gap: 8px;
  padding: 8px 8px 10px;
  border: 1px solid var(--control-stroke);
  border-radius: var(--radius-overlay);
  background: var(--control-default);
  color: var(--text-primary);
  cursor: pointer;
}
.mode:hover {
  background: var(--control-secondary);
}
.mode.selected {
  border-color: var(--accent);
  box-shadow: 0 0 0 1px var(--accent);
}
.preview {
  position: relative;
  display: block;
  height: 64px;
  border-radius: 4px;
  overflow: hidden;
  background: #f3f3f3;
}
.dark .preview {
  background: #202020;
}
.system .preview {
  background: linear-gradient(135deg, #f3f3f3 50%, #202020 50%);
}
.preview .win {
  position: absolute;
  left: 18%;
  top: 22%;
  right: 10%;
  bottom: -10%;
  border-radius: 4px;
  background: #fff;
  box-shadow: 0 2px 6px rgba(0, 0, 0, 0.15);
}
.dark .preview .win {
  background: #2c2c2c;
}
.system .preview .win {
  background: linear-gradient(135deg, #fff 50%, #2c2c2c 50%);
}
.accent-chip {
  position: absolute;
  left: 10px;
  bottom: 18px;
  width: 36px;
  height: 10px;
  border-radius: 3px;
  background: var(--accent);
}
.preview .bar {
  position: absolute;
  left: 6px;
  top: 8px;
  width: 8%;
  height: 50%;
  border-radius: 2px;
  background: rgba(128, 128, 128, 0.35);
}
.swatches {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}
.swatch {
  display: grid;
  place-items: center;
  width: 40px;
  height: 40px;
  border-radius: var(--radius-overlay);
  border: 2px solid transparent;
  color: #fff;
  cursor: pointer;
  box-shadow: inset 0 0 0 1px rgba(0, 0, 0, 0.1);
}
.swatch[aria-checked='true'] {
  border-color: var(--text-primary);
}
@media (max-width: 640px) {
  .setting {
    flex-wrap: wrap;
  }
  .setting .setting-text {
    flex-basis: calc(100% - 40px);
  }
  .mode-grid {
    gap: 8px;
  }
}
</style>
