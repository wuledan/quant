import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

export default defineConfig({
  plugins: [react()],
  server: {
    host: '0.0.0.0',
    watch: {
      ignored: ['**/py/**'],
    },
    proxy: {
      // C++ StrategyApi — :9191
      '/api/cpp': {
        target: 'http://192.168.3.10:9191',
        changeOrigin: true,
        rewrite: (path) => path.replace(/^\/api\/cpp/, '/api'),
      },
      // Python FastAPI — :8000
      '/api/py': {
        target: 'http://192.168.3.10:8000',
        changeOrigin: true,
        rewrite: (path) => path.replace(/^\/api\/py/, '/api'),
      },
      // Legacy /api/v1 still points to Python for other endpoints
      '/api/v1': {
        target: 'http://192.168.3.10:8000',
        changeOrigin: true,
      },
      '/ws': {
        target: 'ws://192.168.3.10:8282',
        ws: true,
      },
    },
  },
})
