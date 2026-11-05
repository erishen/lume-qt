/**
 * 帧率限制 + 视口可见性判断的公用小工具。
 *
 * 页面里同时跑着两个 WebGL 场景和 5 个 2D canvas。它们原先各自按 rAF 全速重绘，
 * 即使整块面板滚出视口、或窗口被切到后台也不停，导致滚动与其它区域卡顿。
 */

/** 创建帧率限制器：回调返回 true 表示「本帧应该绘制」。
 *  容差 1ms：在高刷/60Hz 屏幕上按整数毫秒取整时不会把每一帧都误判为过快。 */
export function createFrameLimiter(targetFps: number): (now: number) => boolean {
  const interval = 1000 / targetFps
  let last = -Infinity
  return (now: number) => {
    if (now - last < interval - 1) return false
    last = now
    return true
  }
}

/** 监听元素是否在视口内（含 100px 预取边距），返回取消监听的函数。 */
export function observeInViewport(el: Element, cb: (inView: boolean) => void): () => void {
  const io = new IntersectionObserver(
    (entries) => {
      for (const e of entries) cb(e.isIntersecting)
    },
    { rootMargin: '100px' },
  )
  io.observe(el)
  return () => io.disconnect()
}
