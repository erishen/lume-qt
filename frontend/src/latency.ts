/**
 * 链路延迟统计（术中化改造的第一块基石）。
 *
 * 为什么不能只测「C++ emit → JS 回调」：那只是链路里的一小段。真正决定
 * 临床可接受度的是「采集 → 像素上屏」的端到端年龄，中间还有 IPC、
 * 渲染进程、合成与排队。为此 C++ 侧给每个实时载荷打了采集时刻 tsUs
 * （epoch 微秒，system_clock），这里用同一时基换算成年龄。
 *
 * 时基：performance.timeOrigin + performance.now() = epoch 毫秒，精度亚毫秒，
 * 与 C++ 的 system_clock 同源（都是系统墙钟），差值即链路年龄。
 */

export type Channel = 'ecg' | 'eeg' | 'emg' | 'neural' | 'vitals' | 'volume'

/** epoch 微秒（与 C++ Bridge::nowMicros 对齐） */
export function nowMicros(): number {
  return (performance.timeOrigin + performance.now()) * 1000
}

const CAPACITY = 512

interface Series {
  /** 样本到达 JS 回调时的年龄（ms） */
  receipt: number[]
  /** 样本真正落到画布/上屏时的年龄（ms） */
  painted: number[]
}

const series: Record<Channel, Series> = {
  ecg: { receipt: [], painted: [] },
  eeg: { receipt: [], painted: [] },
  emg: { receipt: [], painted: [] },
  neural: { receipt: [], painted: [] },
  vitals: { receipt: [], painted: [] },
  volume: { receipt: [], painted: [] },
}

function push(ring: number[], value: number) {
  ring.push(value)
  if (ring.length > CAPACITY) ring.splice(0, ring.length - CAPACITY)
}

/** 记录一次「采集 → 到达 JS」的年龄；tsUs 缺失时忽略（纯浏览器演示无桥接） */
export function recordReceipt(channel: Channel, tsUs: number | undefined) {
  if (typeof tsUs !== 'number' || !Number.isFinite(tsUs) || tsUs <= 0) return
  push(series[channel].receipt, nowMicros() - tsUs)
}

/** 记录一次「采集 → 本帧绘制」的年龄；在真正 draw 的那一帧调用 */
export function recordPainted(channel: Channel, tsUs: number | undefined) {
  if (typeof tsUs !== 'number' || !Number.isFinite(tsUs) || tsUs <= 0) return
  push(series[channel].painted, nowMicros() - tsUs)
}

export interface ChannelLatency {
  channel: Channel
  receiptP50: number
  receiptP99: number
  paintedP50: number
  paintedP99: number
  samples: number
}

function pct(sorted: number[], q: number): number {
  if (sorted.length === 0) return 0
  const i = Math.min(sorted.length - 1, Math.floor(sorted.length * q))
  return sorted[i]
}

function summarize(list: number[]) {
  const sorted = list.slice().sort((a, b) => a - b)
  return {
    p50: pct(sorted, 0.5),
    p99: pct(sorted, 0.99),
    max: sorted.length > 0 ? sorted[sorted.length - 1] : 0,
    n: sorted.length,
  }
}

/** 读取当前统计快照（给延迟看板按 1Hz 轮询） */
export function snapshot(): ChannelLatency[] {
  return (Object.keys(series) as Channel[]).map((channel) => {
    const s = series[channel]
    const r = summarize(s.receipt)
    const p = summarize(s.painted)
    return {
      channel,
      receiptP50: r.p50,
      receiptP99: r.p99,
      paintedP50: p.p50,
      paintedP99: p.p99,
      samples: Math.max(r.n, p.n),
    }
  })
}
