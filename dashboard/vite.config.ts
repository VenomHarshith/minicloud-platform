import { defineConfig, loadEnv } from 'vite'
import react from '@vitejs/plugin-react'

export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, '../deploy', 'MINICLOUD_')
  const apiToken = env.MINICLOUD_API_TOKEN

  return {
    plugins: [react()],
    server: {
      port: 5173,
      proxy: {
        '/api': {
          target: 'http://127.0.0.1:8090',
          ...(apiToken ? { headers: { Authorization: `Bearer ${apiToken}` } } : {}),
        },
      },
    },
  }
})
