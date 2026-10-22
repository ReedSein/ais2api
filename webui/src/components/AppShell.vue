<script setup lang="ts">
// WinUI NavigationView with its three adaptive display modes:
//   ≥1008px  Expanded — inline pane, user can collapse it to the compact rail
//   641–1007 Compact  — 48px icon rail, opens as an acrylic overlay
//   ≤640px   Minimal  — no rail; hamburger in the title bar opens a drawer
import { computed, ref, watch } from 'vue'
import { useRoute } from 'vue-router'
import FIcon from './FIcon.vue'
import { routes } from '../router'
import { backendLabel, signOut } from '../stores/session'
import { refreshStatus, service } from '../stores/service'
import { confirm } from '../stores/ui'
import { useMediaQuery, usePolling } from '../utils/media'

const route = useRoute()
const wide = useMediaQuery('(min-width: 1008px)')
const narrow = useMediaQuery('(max-width: 640px)')
const mode = computed(() => (wide.value ? 'expanded' : narrow.value ? 'minimal' : 'compact'))

const PANE_KEY = 'ais2api.paneCollapsed'
const collapsed = ref(localStorage.getItem(PANE_KEY) === '1')
const overlayOpen = ref(false)

const paneExpanded = computed(() => (mode.value === 'expanded' ? !collapsed.value : overlayOpen.value))
const paneOverlay = computed(() => mode.value !== 'expanded' && overlayOpen.value)

function togglePane() {
  if (mode.value === 'expanded') {
    collapsed.value = !collapsed.value
    localStorage.setItem(PANE_KEY, collapsed.value ? '1' : '0')
  } else overlayOpen.value = !overlayOpen.value
}

watch(() => route.fullPath, () => (overlayOpen.value = false))
watch(mode, () => (overlayOpen.value = false))

const mainItems = routes.filter((r) => r.meta?.nav === 'main')
const footerItems = routes.filter((r) => r.meta?.nav === 'footer')

refreshStatus()
usePolling(refreshStatus, 5000)

const health = computed(() => {
  const status = service.status
  if (!service.reachable) return { tone: 'critical', text: '无法连接' }
  if (!status) return { tone: '', text: '连接中' }
  if (status.logging_healthy === false) return { tone: 'caution', text: '日志积压' }
  if (status.ready) return { tone: 'success', text: status.active ? '生成中' : '就绪' }
  return { tone: 'caution', text: '未就绪' }
})

async function logout() {
  if (await confirm({ title: '退出登录', message: '将清除本设备保存的管理密钥。', confirmText: '退出' })) signOut()
}

function onKey(event: KeyboardEvent) {
  if (event.key === 'Escape' && overlayOpen.value) overlayOpen.value = false
}
</script>

<template>
  <div class="shell" :class="[mode, { 'pane-open': paneExpanded, overlay: paneOverlay }]" @keydown="onKey">
    <a class="skip" href="#main">跳到主要内容</a>
    <header class="titlebar">
      <button
        class="btn subtle icon-only pane-toggle"
        :aria-label="paneExpanded ? '收起导航' : '展开导航'"
        :aria-expanded="paneExpanded"
        aria-controls="nav-pane"
        @click="togglePane"
      >
        <FIcon name="line_horizontal_3" />
      </button>
      <img class="app-icon" src="/favicon.svg" alt="" width="16" height="16" />
      <span class="app-title">ais2api 控制台</span>
      <span class="backend t-caption t-secondary truncate" :title="backendLabel">{{ backendLabel }}</span>
      <span class="spacer" />
      <RouterLink to="/" class="health" :title="service.lastError || health.text">
        <span class="dot" :class="health.tone" />
        <span class="t-caption">{{ health.text }}</span>
      </RouterLink>
    </header>

    <nav id="nav-pane" class="pane" aria-label="主导航">
      <div class="pane-scroll">
        <RouterLink
          v-for="item in mainItems"
          :key="item.path"
          :to="item.path"
          class="nav-item"
          :title="!paneExpanded ? String(item.meta?.title) : undefined"
          active-class=""
          exact-active-class="selected"
        >
          <FIcon :name="item.meta!.icon!" :filled="route.path === item.path" />
          <span class="nav-label">{{ item.meta?.title }}</span>
        </RouterLink>
      </div>
      <div class="pane-footer">
        <button class="nav-item" :title="!paneExpanded ? '退出登录' : undefined" @click="logout">
          <FIcon name="sign_out" />
          <span class="nav-label">退出登录</span>
        </button>
        <RouterLink
          v-for="item in footerItems"
          :key="item.path"
          :to="item.path"
          class="nav-item"
          :title="!paneExpanded ? String(item.meta?.title) : undefined"
          active-class=""
          exact-active-class="selected"
        >
          <FIcon :name="item.meta!.icon!" :filled="route.path === item.path" />
          <span class="nav-label">{{ item.meta?.title }}</span>
        </RouterLink>
      </div>
    </nav>
    <div v-if="paneOverlay" class="pane-dismiss" aria-hidden="true" @click="overlayOpen = false" />

    <main id="main" class="content" tabindex="-1">
      <div class="page">
        <RouterView v-slot="{ Component, route: r }">
          <Transition name="page" mode="out-in">
            <component :is="Component" :key="r.path" />
          </Transition>
        </RouterView>
      </div>
    </main>
  </div>
</template>

<style scoped>
.shell {
  --pane-w: var(--pane-compact);
  min-height: 100dvh;
  display: grid;
  grid-template-columns: var(--pane-w) minmax(0, 1fr);
  grid-template-rows: var(--titlebar-height) minmax(0, 1fr);
  grid-template-areas:
    'title title'
    'pane content';
}
.shell.expanded.pane-open {
  --pane-w: var(--pane-expanded);
}
.shell.minimal {
  --pane-w: 0px;
}

.skip {
  position: absolute;
  left: 8px;
  top: -40px;
  z-index: 200;
  padding: 6px 12px;
  border-radius: var(--radius-control);
  background: var(--accent);
  color: var(--text-on-accent);
}
.skip:focus {
  top: 8px;
}

/* ---------- Title bar ---------- */
.titlebar {
  grid-area: title;
  position: sticky;
  top: 0;
  z-index: 30;
  display: flex;
  align-items: center;
  gap: 12px;
  height: var(--titlebar-height);
  padding: 0 12px 0 4px;
  padding-top: var(--safe-top);
  box-sizing: content-box;
}
.minimal .titlebar {
  background: var(--acrylic-base);
  backdrop-filter: blur(30px) saturate(150%);
  -webkit-backdrop-filter: blur(30px) saturate(150%);
  border-bottom: 1px solid var(--divider);
}
.pane-toggle {
  width: 40px;
  height: 36px;
  min-width: 40px;
}
.app-icon {
  flex: none;
}
.app-title {
  font: var(--type-caption);
  white-space: nowrap;
}
.backend {
  min-width: 0;
  max-width: 260px;
}
.health {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  padding: 4px 10px;
  border-radius: var(--radius-pill);
  color: var(--text-secondary);
  text-decoration: none;
  white-space: nowrap;
}
.health:hover {
  background: var(--subtle-secondary);
  text-decoration: none;
}

/* ---------- Navigation pane ---------- */
.pane {
  grid-area: pane;
  position: sticky;
  top: var(--titlebar-height);
  z-index: 20;
  height: calc(100dvh - var(--titlebar-height));
  width: var(--pane-w);
  display: flex;
  flex-direction: column;
  padding: 4px 0 calc(4px + var(--safe-bottom));
  overflow: hidden;
  transition: width var(--motion-normal) var(--ease-decelerate);
}
.shell.overlay .pane {
  position: fixed;
  left: 0;
  top: var(--titlebar-height);
  width: var(--pane-expanded);
  max-width: calc(100vw - 48px);
  z-index: 50;
  background: var(--acrylic);
  backdrop-filter: blur(30px) saturate(150%);
  -webkit-backdrop-filter: blur(30px) saturate(150%);
  border-right: 1px solid var(--surface-stroke);
  border-top: 1px solid var(--surface-stroke);
  border-radius: 0 var(--radius-overlay) 0 0;
  box-shadow: var(--shadow-flyout);
  animation: pane-in var(--motion-slow) var(--ease-decelerate);
}
.shell.minimal .pane {
  visibility: hidden;
}
.shell.minimal.overlay .pane {
  visibility: visible;
  top: calc(var(--titlebar-height) + var(--safe-top));
  height: calc(100dvh - var(--titlebar-height) - var(--safe-top));
}
@keyframes pane-in {
  from {
    transform: translateX(-24px);
    opacity: 0;
  }
}
.pane-dismiss {
  position: fixed;
  inset: 0;
  z-index: 40;
}
.minimal .pane-dismiss {
  background: var(--smoke);
}
.pane-scroll {
  flex: 1;
  overflow-y: auto;
  overflow-x: hidden;
  display: flex;
  flex-direction: column;
  gap: 2px;
}
.pane-footer {
  display: flex;
  flex-direction: column;
  gap: 2px;
  padding-top: 4px;
  border-top: 1px solid transparent;
}
.nav-item {
  position: relative;
  display: flex;
  align-items: center;
  gap: 16px;
  height: 36px;
  margin: 0 4px;
  padding: 0 12px;
  border: 0;
  border-radius: var(--radius-control);
  background: transparent;
  color: var(--text-primary);
  text-decoration: none;
  white-space: nowrap;
  cursor: pointer;
  font: var(--type-body);
  text-align: left;
  transition: background-color var(--motion-fast) linear;
}
.nav-item:hover {
  background: var(--subtle-secondary);
  text-decoration: none;
}
.nav-item:active {
  background: var(--subtle-tertiary);
  color: var(--text-secondary);
}
.nav-item.selected {
  background: var(--subtle-secondary);
}
.nav-item.selected::before {
  content: '';
  position: absolute;
  left: 0;
  top: 50%;
  width: 3px;
  height: 16px;
  margin-top: -8px;
  border-radius: 2px;
  background: var(--accent);
  animation: pill var(--motion-normal) var(--ease-decelerate);
}
@keyframes pill {
  from {
    transform: scaleY(0);
  }
}
.nav-label {
  overflow: hidden;
  text-overflow: ellipsis;
  transition: opacity var(--motion-fast) linear;
}
.shell:not(.pane-open) .nav-label {
  opacity: 0;
}
@media (pointer: coarse) {
  .nav-item {
    height: 44px;
  }
}

/* ---------- Content layer ---------- */
.content {
  grid-area: content;
  min-width: 0;
  min-height: calc(100dvh - var(--titlebar-height));
  background: var(--layer-default);
  border-top: 1px solid var(--card-stroke);
  border-left: 1px solid var(--card-stroke);
  border-top-left-radius: var(--radius-overlay);
}
.content:focus {
  outline: none;
}
.minimal .content {
  border: 0;
  border-radius: 0;
}
.page {
  max-width: var(--content-max);
  margin: 0 auto;
  padding: 28px 40px calc(40px + var(--safe-bottom));
}
@media (max-width: 1007px) {
  .page {
    padding: 24px 24px calc(32px + var(--safe-bottom));
  }
}
@media (max-width: 640px) {
  .page {
    padding: 16px 16px calc(24px + var(--safe-bottom));
  }
  .backend {
    display: none;
  }
}

.page-enter-active {
  transition:
    opacity var(--motion-normal) linear,
    transform var(--motion-slow) var(--ease-decelerate);
}
.page-leave-active {
  transition: opacity var(--motion-fast) linear;
}
.page-enter-from {
  opacity: 0;
  transform: translateY(24px);
}
.page-leave-to {
  opacity: 0;
}
</style>
