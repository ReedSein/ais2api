import { reactive, watchEffect } from 'vue'

export type ThemeMode = 'system' | 'light' | 'dark'

interface AccentPreset {
  label: string
  /** Swatch shown in the picker (Windows base accent). */
  swatch: string
  light: { fill: string; text: string }
  dark: { fill: string; text: string }
  /** Secondary hue for the Mica wash. */
  alt: string
}

// Light themes use the darker accent steps, dark themes the lighter ones —
// the same mapping WinUI applies to the system accent color.
export const ACCENTS: Record<string, AccentPreset> = {
  blue: { label: '默认蓝', swatch: '#0078d4', light: { fill: '#005fb8', text: '#003e92' }, dark: { fill: '#60cdff', text: '#99ebff' }, alt: '#8a63d2' },
  teal: { label: '海沫青', swatch: '#00b7c3', light: { fill: '#00727c', text: '#00525a' }, dark: { fill: '#5ad8e0', text: '#a5f0f4' }, alt: '#2f8fdc' },
  purple: { label: '薰衣草紫', swatch: '#8764b8', light: { fill: '#744da9', text: '#4f2f86' }, dark: { fill: '#c3a6ff', text: '#e0d0ff' }, alt: '#d0569b' },
  plum: { label: '梅子红', swatch: '#c30052', light: { fill: '#b0004a', text: '#7a0033' }, dark: { fill: '#ff99c2', text: '#ffc8de' }, alt: '#7d4ec5' },
  slate: { label: '石板灰', swatch: '#68768a', light: { fill: '#4a5a70', text: '#2f3d50' }, dark: { fill: '#a9c1de', text: '#d3e1f1' }, alt: '#5b8ab8' },
}

const STORAGE_KEY = 'ais2api.theme'

function loadPrefs() {
  try {
    const raw = JSON.parse(localStorage.getItem(STORAGE_KEY) ?? '{}')
    return {
      mode: (['system', 'light', 'dark'].includes(raw.mode) ? raw.mode : 'system') as ThemeMode,
      accent: typeof raw.accent === 'string' && raw.accent in ACCENTS ? (raw.accent as string) : 'blue',
      mica: raw.mica !== false,
    }
  } catch {
    return { mode: 'system' as ThemeMode, accent: 'blue', mica: true }
  }
}

const media = window.matchMedia('(prefers-color-scheme: dark)')

export const theme = reactive({
  ...loadPrefs(),
  systemDark: media.matches,
  get resolved(): 'light' | 'dark' {
    return this.mode === 'system' ? (this.systemDark ? 'dark' : 'light') : this.mode
  },
})

media.addEventListener('change', (event) => (theme.systemDark = event.matches))

const rgb = (hex: string) => {
  const n = parseInt(hex.slice(1), 16)
  return `${(n >> 16) & 255}, ${(n >> 8) & 255}, ${n & 255}`
}

watchEffect(() => {
  const root = document.documentElement
  const resolved = theme.resolved
  const preset = ACCENTS[theme.accent] ?? ACCENTS.blue
  const accent = preset[resolved]
  root.dataset.theme = resolved
  root.dataset.mica = theme.mica ? 'on' : 'off'
  root.style.setProperty('--accent', accent.fill)
  root.style.setProperty('--accent-hover', `rgba(${rgb(accent.fill)}, 0.9)`)
  root.style.setProperty('--accent-pressed', `rgba(${rgb(accent.fill)}, 0.8)`)
  root.style.setProperty('--accent-text', accent.text)
  root.style.setProperty('--accent-rgb', rgb(accent.fill))
  root.style.setProperty('--accent-alt-rgb', rgb(preset.alt))
  // Track lighter step of the same ramp (meters and progress).
  root.style.setProperty('--accent-track', `rgba(${rgb(accent.fill)}, ${resolved === 'dark' ? 0.22 : 0.18})`)
  document.querySelectorAll('meta[name="theme-color"]').forEach((meta) => meta.setAttribute('content', resolved === 'dark' ? '#202020' : '#f3f3f3'))
  localStorage.setItem(STORAGE_KEY, JSON.stringify({ mode: theme.mode, accent: theme.accent, mica: theme.mica }))
})
