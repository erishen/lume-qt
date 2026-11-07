/**
 * 顶层 Tab 可见性由 Qt 壳下发。
 *
 * Qt WebEngine 不会因为 QStackedWidget 隐藏页面就自动暂停 React 定时器/绘制循环；
 * 原生页在前台时，Web 页仍可能收到 QWebChannel 信号并占用渲染进程。壳在切页时
 * 执行 `runJavaScript` 设置该标志，前端所有“持续工作”的循环在这里统一让路。
 * 未设置时默认可见，保证纯浏览器演示（vite dev）不受影响。
 */
declare global {
  interface Window {
    __lumeQtActive?: boolean
  }
}

export function webTabActive(): boolean {
  if (typeof window === 'undefined') return true
  return window.__lumeQtActive !== false
}
