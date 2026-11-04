import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import { resolve } from 'node:path'
import { existsSync, copyFileSync, mkdirSync } from 'node:fs'

// QWebChannel 的 JS 运行时由 Qt 安装提供。生产构建时 CMake 已把它拷到
// resources/web/；但 Vite dev server 的静态根是 frontend/，取不到该文件，
// 因此这里同步一份到 frontend/public/（Vite 会自动以 / 为路径提供）。
// 该文件是生成物，已加入 .gitignore，不纳入版本控制。
function syncQWebChannelJs() {
  const candidates = [
    process.env.QTRS_QWEBCHANNEL_JS,
    // 首选 CMake 拷贝好的那份（与生产构建严格同源、跨平台）
    resolve(__dirname, '../resources/web/qwebchannel.js'),
    // 未经 CMake 构建时，回退到常见的 Qt 安装位置
    '/opt/homebrew/opt/qt/share/qt6/webchannel/qwebchannel.js',
    '/opt/homebrew/opt/qtwebchannel/share/qt6/webchannel/qwebchannel.js',
    '/usr/share/qt6/webchannel/qwebchannel.js',
    '/usr/lib/qt6/webchannel/qwebchannel.js',
  ].filter((p): p is string => Boolean(p))

  const src = candidates.find((p) => existsSync(p))
  if (!src) {
    console.warn('[vite] 未找到 qwebchannel.js：dev 模式下 Qt 桥接不可用。'
      + '请先执行 make cpp，或设置 QTRS_QWEBCHANNEL_JS 指向该文件。')
    return
  }
  try {
    const publicDir = resolve(__dirname, 'public')
    mkdirSync(publicDir, { recursive: true })
    // 无条件覆盖，保证与当前 Qt 版本同步
    copyFileSync(src, resolve(publicDir, 'qwebchannel.js'))
  } catch (e) {
    console.warn('[vite] 同步 qwebchannel.js 失败：', e)
  }
}
syncQWebChannelJs()

// 前端构建产物直接输出到 resources/web，
// 由 Qt 侧的 AppSchemeHandler 通过 app:// scheme 提供。
export default defineConfig({
  plugins: [react()],
  // 使用根相对路径，配合 app://app/ 前缀加载静态资源
  base: '/',
  // dev server 的静态资源目录（存放 qwebchannel.js 等运行时文件）
  publicDir: resolve(__dirname, 'public'),
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
    // 开发服务器端口。必须固定：Qt 侧 QTRS_DEV_URL 指向的就是这个端口，
    // 若被占用而自动换端口，应用会静默连到旧的 dev server 实例。
    port: 5173,
    strictPort: true,
  },
})
