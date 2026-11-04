import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import { resolve } from 'node:path'

// 前端构建产物直接输出到 resources/web，
// 由 Qt 侧的 AppSchemeHandler 通过 app:// scheme 提供。
export default defineConfig({
  plugins: [react()],
  // 使用根相对路径，配合 app://app/ 前缀加载静态资源
  base: '/',
  build: {
    outDir: resolve(__dirname, '../resources/web'),
    emptyOutDir: true,
    sourcemap: false,
    // three.js 独立分包约 520KB，调高警告阈值避免误报
    chunkSizeWarningLimit: 600,
    rollupOptions: {
      output: {
        // 生成简洁的文件名
        entryFileNames: 'assets/[name].[hash].js',
        chunkFileNames: 'assets/[name].[hash].js',
        assetFileNames: 'assets/[name].[hash][extname]',
        // 将 three.js 独立分包，避免主 bundle 超过 500KB
        manualChunks: {
          three: ['three'],
        },
      },
    },
  },
  server: {
    // 开发服务器端口（若通过 Qt 加载 http://localhost:5173 开发）
    port: 5173,
    strictPort: false,
  },
})
