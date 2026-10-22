import { defineConfig, loadEnv } from 'vite'
import vue from '@vitejs/plugin-vue'
import pkg from './package.json' with { type: 'json' }

// The dev server proxies the management API so the console is same-origin and
// does not need an `admin_origins` entry. ais2api treats a request as same-origin
// when `Origin` equals `http://` + `Host`, so both headers are rewritten to the
// backend's own address.
export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, process.cwd(), '')
  const target = new URL(env.AIS2API_URL || 'http://127.0.0.1:18451')
  const backendOrigin = target.origin

  return {
    plugins: [vue()],
    define: {
      __APP_VERSION__: JSON.stringify(pkg.version),
      __DEV_BACKEND_ORIGIN__: JSON.stringify(target.origin),
    },
    server: {
      port: 5173,
      proxy: {
        '/api/admin/': {
          target: target.origin,
          changeOrigin: true,
          configure(proxy) {
            proxy.on('proxyReq', (req) => {
              if (req.getHeader('origin')) req.setHeader('origin', backendOrigin)
              req.removeHeader('referer')
              req.removeHeader('cookie')
            })
          },
        },
      },
    },
    preview: {
      port: 5173,
    },
    build: {
      target: 'es2022',
      chunkSizeWarningLimit: 600,
    },
  }
})
