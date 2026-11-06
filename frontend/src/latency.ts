/**
 * 链路延迟统计（术中化改造的第一块基石）。
 *
 * 为什么不能只测「C++ emit → JS 回调」：那只是链路里的一小段。真正决定
 * 临床可接受度的是「采集 → 像素上屏」的端到端年龄，中间还有 IPC、
 * 渲染进程、合成与排队。为此 C++ 侧给每个实时载荷打了采集时刻 tsUs
 * （epoch 微秒，system_clock），这里用同一时基换算成年龄。
 *
 * 时基：performance.timeOrigin + performance.now() = epoch 毫秒，精度亚毫秒。
 * 时间戳分两种：来自宿主墙钟（与上面同源，直接相减即年龄）；来自设备时钟
 * （与宿主有未知常量偏移，必须先估偏移再算，见 setClockSource）。
 */

export type Channel = 'ecg' | 'eeg' | 'emg' | 'neural' | 'vitals' | 'volume' | 'alarm'

/** 通道健康状态：正常 / 延迟超预算 / 已中断（画面是冻结帧） */
export type SignalState = 'ok' | 'slow' | 'stale' | 'pending'

export interface ChannelHealth {
  channel: Channel
  /** 距最近一次收到样本的时长（ms）；从未收到为 Infinity */
  ageMs: number
  /** 判定中断的阈值（ms） */
  staleAfterMs: number
  /** 最近窗口的到达中位数：判「慢」看它，避免一次卡顿把状态钉在告警里 */
  receiptP50: number
  receiptP99: number
  state: SignalState
}

/**
 * 中断阈值：按各通道采样周期的 5–8 倍留余量，宁可晚报也不误报。
 * 这是「术中」与「演示」的分界线之一——信号停了必须显式告知，而不是继续画旧波形。
 */
const STALE_AFTER_MS: Record<Channel, number> = {
  ecg: 250,
  eeg: 400,
  emg: 700,
  neural: 600,
  vitals: 4000,
  // 体积是一次性加载，解析本身耗时数百毫秒，给足余量
  volume: 30000,
  // 报警是事件型通道：没有报警发生是正常状态，不能因为「很久没来事件」判中断
  // （该通道由 channelHealth 单独排除，这里给 Infinity 只是让表完整）
  alarm: Number.POSITIVE_INFINITY,
}

/** 到达延迟预算：超过即视为「慢」，与看板标红阈值保持一致 */
const SLOW_BUDGET_MS = 100

/** 体积是一次性解析，2 s 量级是正常的；不能套波形那 100 ms 的链路预算 */
const SLOW_BUDGET_BY_CHANNEL: Partial<Record<Channel, number>> = {
  volume: 15000,
  alarm: 200,
}

/** 事件型通道：只记「这一次花了多久」，没有持续流的「年龄/中断」语义 */
const EVENT_ONLY_CHANNELS: Channel[] = ['alarm']

/** 健康判定只看最近这么多个样本（看板统计仍用完整窗口） */
const RECENT_WINDOW = 30

/** epoch 微秒（与 C++ Bridge::nowMicros 对齐） */
export function nowMicros(): number {
  return (performance.timeOrigin + performance.now()) * 1000
}

const CAPACITY = 512

// ---------------------------------------------------------------------------
// 时基：宿主时钟 / 设备时钟
// ---------------------------------------------------------------------------
/**
 * 单侧测量拿不到「时钟偏移」和「真实延迟」各自的值，只能拿到二者之和：
 *   raw = 宿主now − tsUs = 偏移 + 真实延迟
 * 偏移短期恒定、真实延迟有下界，所以最近窗口内 raw 的最小值就是偏移的下界估计。
 * 扣掉它，剩下的残差才是链路延迟（相对值，比真实延迟略小）。
 * 时间戳本来就来自宿主墙钟时（hasHardwareClock = false），偏移≈0，直接相减即可。
 */
export type ClockMode = 'host' | 'device'

let clockMode: ClockMode = 'host'
let clockSamples = 0
let clockOffsetUs = 0
const OFFSET_WINDOW = 120
const offsetRing: number[] = []

export interface ClockStatus {
  mode: ClockMode
  /** 估计出的「宿主 − 设备」时钟偏移（ms）；host 模式恒为 0 */
  offsetMs: number
  /** 参与偏移估计的样本数 */
  samples: number
}

/** 由 C++ 侧 providerInfo().hasHardwareClock 决定；切换时丢弃旧样本 */
export function setClockSource(hasHardwareClock: boolean) {
  const mode: ClockMode = hasHardwareClock ? 'device' : 'host'
  if (clockMode === mode) return
  clockMode = mode
  offsetRing.length = 0
  clockSamples = 0
  clockOffsetUs = 0
}

export function clockStatus(): ClockStatus {
  return { mode: clockMode, offsetMs: clockOffsetUs / 1000, samples: clockSamples }
}

/**
 * 时间戳固定来自宿主时钟的通道。DICOM 体积的 tsUs 是 C++ 侧解析完成时刻
 * （Bridge::nowMicros，宿主墙钟），不随设备时基变化——按设备时钟去扣偏移会得到
 * 5 000 000 ms 这种假值。
 */
const HOST_STAMPED_CHANNELS: Channel[] = ['volume']

function isHostStamped(channel: Channel): boolean {
  return HOST_STAMPED_CHANNELS.includes(channel)
}

/** 把原始差值换算成「扣掉时钟偏移后的链路延迟」 */
function residualUs(channel: Channel, raw: number): number {
  if (clockMode === 'host' || isHostStamped(channel)) return raw
  push(offsetRing, raw, OFFSET_WINDOW)
  clockSamples += 1
  let min = offsetRing[0]
  for (const v of offsetRing) {
    if (v < min) min = v
  }
  clockOffsetUs = min
  return raw - min
}

interface Series {
  /** 样本到达 JS 回调时的年龄（ms） */
  receipt: number[]
  /** 样本真正落到画布/上屏时的年龄（ms） */
  painted: number[]
}

const lastReceiptUs: Record<Channel, number> = {
  ecg: 0,
  eeg: 0,
  emg: 0,
  neural: 0,
  vitals: 0,
  volume: 0,
  alarm: 0,
}

const series: Record<Channel, Series> = {
  ecg: { receipt: [], painted: [] },
  eeg: { receipt: [], painted: [] },
  emg: { receipt: [], painted: [] },
  neural: { receipt: [], painted: [] },
  vitals: { receipt: [], painted: [] },
  volume: { receipt: [], painted: [] },
  alarm: { receipt: [], painted: [] },
}

function push(ring: number[], value: number, capacity = CAPACITY) {
  ring.push(value)
  if (ring.length > capacity) ring.splice(0, ring.length - capacity)
}

/** 记录一次「采集 → 到达 JS」的年龄；tsUs 缺失时忽略（纯浏览器演示无桥接） */
export function recordReceipt(channel: Channel, tsUs: number | undefined) {
  if (typeof tsUs !== 'number' || !Number.isFinite(tsUs) || tsUs <= 0) return
  const now = nowMicros()
  lastReceiptUs[channel] = now
  push(series[channel].receipt, residualUs(channel, now - tsUs))
}

/**
 * 记录事件型通道（体积这类一次性加载）的单次耗时。
 * 它没有「年龄」语义：把发布时刻当采集时刻会算出数百秒的假延迟，
 * 这里直接记本次请求花了多久；同时刷新 lastReceipt 供健康判定用。
 */
export function recordSpan(channel: Channel, phase: 'receipt' | 'painted', ms: number) {
  if (!Number.isFinite(ms) || ms < 0) return
  lastReceiptUs[channel] = nowMicros()
  push(series[channel][phase], ms)
}

/** 记录一次「采集 → 本帧绘制」的年龄；在真正 draw 的那一帧调用 */
export function recordPainted(channel: Channel, tsUs: number | undefined) {
  if (typeof tsUs !== 'number' || !Number.isFinite(tsUs) || tsUs <= 0) return
  push(series[channel].painted, residualUs(channel, nowMicros() - tsUs))
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
  // 事件型通道也要进表：报警呈现是术中必须盯的指标，只是不参与「中断」判定。
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

/**
 * 通道健康度：给看门狗条轮询。
 * 「从未收到」判为 pending 而不是 stale —— 启动时 ECG/体积本来就要等一会儿。
 */
export function channelHealth(): ChannelHealth[] {
  const now = nowMicros()
  return (Object.keys(series) as Channel[])
    .filter((channel) => !EVENT_ONLY_CHANNELS.includes(channel))
    .map((channel) => {
      const s = series[channel]
      // 判「慢」只看最近 30 个样本：512 窗口会把启动期（窗口未激活/解析中）的积压
      // 一直算在里面，让健康状态卡在告警里出不来——那是测量噪声，不是当前链路状态。
      const r = summarize(s.receipt.slice(-RECENT_WINDOW))
      const last = lastReceiptUs[channel]
      const ageMs = last > 0 ? (now - last) / 1000 : Number.POSITIVE_INFINITY
      const staleAfterMs = STALE_AFTER_MS[channel]
      const budgetMs = SLOW_BUDGET_BY_CHANNEL[channel] ?? SLOW_BUDGET_MS
      let state: SignalState = 'pending'
      if (last > 0) {
        // 体积没有「持续更新」语义：一次加载完成后停在原地是正常的，
        // 用年龄判 stale 会每隔 30 s 就报一次假中断。
        if (channel !== 'volume' && ageMs > staleAfterMs) state = 'stale'
        else if (r.p50 > budgetMs) state = 'slow'
        else state = 'ok'
      }
      return { channel, ageMs, staleAfterMs, receiptP50: r.p50, receiptP99: r.p99, state }
    })
}

// ---------------------------------------------------------------------------
// 帧预算：可见时的 rAF 间隔。P99 超预算意味着掉帧（不是「JS 算得慢」，而是没排上帧）。
// ---------------------------------------------------------------------------

export interface FrameStats {
  p50: number
  p99: number
  /** 超过 33ms（30fps 预算）的帧占比 */
  overBudgetRatio: number
  samples: number
}

const FRAME_CAPACITY = 240
const frameRing: number[] = []
const FRAME_BUDGET_MS = 33

/** 记录一次可见帧的 rAF 间隔（ms） */
export function recordFrame(intervalMs: number) {
  if (!Number.isFinite(intervalMs) || intervalMs <= 0) return
  push(frameRing, intervalMs, FRAME_CAPACITY)
}

export function frameStats(): FrameStats {
  const s = summarize(frameRing)
  const over = frameRing.filter((v) => v > FRAME_BUDGET_MS).length
  return {
    p50: s.p50,
    p99: s.p99,
    overBudgetRatio: s.n > 0 ? over / s.n : 0,
    samples: s.n,
  }
}
