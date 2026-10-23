import { defineConfig } from 'vitest/config'
import vue from '@vitejs/plugin-vue'

// Component interaction tests run in happy-dom with a mocked fetch; they never
// contact a real ais2api service.
export default defineConfig({
  plugins: [vue()],
  define: {
    __APP_VERSION__: JSON.stringify('test'),
    __DEV_BACKEND_ORIGIN__: JSON.stringify('http://127.0.0.1:18451'),
  },
  test: {
    environment: 'happy-dom',
    include: ['src/**/*.test.ts'],
  },
})
