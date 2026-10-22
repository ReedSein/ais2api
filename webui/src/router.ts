import { createRouter, createWebHistory, type RouteRecordRaw } from 'vue-router'
import { isSignedIn } from './stores/session'
import type { IconName } from './icons'

declare module 'vue-router' {
  interface RouteMeta {
    title?: string
    icon?: IconName
    public?: boolean
    /** Navigation pane placement. */
    nav?: 'main' | 'footer'
  }
}

export const routes: RouteRecordRaw[] = [
  { path: '/login', name: 'login', component: () => import('./views/LoginView.vue'), meta: { title: '登录', public: true } },
  { path: '/', name: 'dashboard', component: () => import('./views/DashboardView.vue'), meta: { title: '概览', icon: 'home', nav: 'main' } },
  { path: '/accounts', name: 'accounts', component: () => import('./views/AccountsView.vue'), meta: { title: '账号', icon: 'people', nav: 'main' } },
  { path: '/models', name: 'models', component: () => import('./views/ModelsView.vue'), meta: { title: '模型', icon: 'cube', nav: 'main' } },
  { path: '/keys', name: 'keys', component: () => import('./views/KeysView.vue'), meta: { title: 'API 密钥', icon: 'key_multiple', nav: 'main' } },
  { path: '/stats', name: 'stats', component: () => import('./views/StatsView.vue'), meta: { title: '用量统计', icon: 'data_bar_vertical', nav: 'main' } },
  { path: '/logs', name: 'logs', component: () => import('./views/LogsView.vue'), meta: { title: '日志', icon: 'document_text', nav: 'main' } },
  { path: '/config', name: 'config', component: () => import('./views/ConfigView.vue'), meta: { title: '服务配置', icon: 'options', nav: 'main' } },
  { path: '/diagnostics', name: 'diagnostics', component: () => import('./views/DiagnosticsView.vue'), meta: { title: '诊断', icon: 'stethoscope', nav: 'main' } },
  { path: '/settings', name: 'settings', component: () => import('./views/SettingsView.vue'), meta: { title: '设置', icon: 'settings', nav: 'footer' } },
  { path: '/:pathMatch(.*)*', redirect: '/' },
]

export const router = createRouter({
  history: createWebHistory(),
  routes,
  scrollBehavior: () => ({ top: 0 }),
})

router.beforeEach((to) => {
  if (!to.meta.public && !isSignedIn.value) return { name: 'login', query: to.fullPath !== '/' ? { redirect: to.fullPath } : {} }
  if (to.name === 'login' && isSignedIn.value) return { name: 'dashboard' }
})

router.afterEach((to) => {
  document.title = to.meta.title ? `${to.meta.title} · ais2api 控制台` : 'ais2api 控制台'
})
