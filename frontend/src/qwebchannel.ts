/**
 * QWebChannel 桥接层：将 C++ 侧的 Bridge 对象（见 src/bridge.h）暴露为类型安全的 JS 接口。
 *
 * 用法：
 *   const bridge = await connectBridge()
 *   const info = await bridge.getPlatformInfo()
 *   bridge.dataFromCpp.connect(handler)
 */

// ---- qwebchannel.js 全局声明（由 Qt 提供，经 index.html <script> 引入）----
declare global {
  interface Window {
    qt?: { webChannelTransport?: unknown }
    // qwebchannel.js 在非模块脚本中挂到全局
    QWebChannel: new (
      transport: unknown,
      callback: (channel: { objects: Record<string, unknown> }) => void,
    ) => void
  }
}

export interface Vitals {
  hr: number
  spo2: number
  rr: number
  sys: number
  dia: number
  temp: number
  time: string
  /** 采集时刻（epoch 微秒），用于端到端延迟统计 */
  tsUs?: number
}

export interface Intervention {
  type: string
  label: string
  intensity: number
  frequency: number
  duration: number
  status: string
  time: string
  tsUs?: number
}

/** 当前设备数据源信息（与 C++ Bridge::providerInfo 对应） */
export interface ProviderInfo {
  kind: string
  description: string
  /** true = 时间戳来自设备时钟（与宿主墙钟有未知常量偏移） */
  hasHardwareClock: boolean
}

export interface Bridge {
  appName: string
  platform: string

  getPlatformInfo(): Promise<string>
  showMessage(message: string): Promise<string>
  add(a: number, b: number): Promise<number>
  setDataFromCpp(data: unknown): void

  // ---- 医疗实时数据模拟控制 ----
  startMedicalSimulation(): void
  stopMedicalSimulation(): void

  // ---- 神经内科介入模拟控制 ----
  startNeurologySimulation(): void
  stopNeurologySimulation(): void

  /** 追加一条结构化审计事件（type 必填，其余字段原样落盘） */
  logAuditEvent(event: Record<string, unknown>): void

  /** 当前设备数据源与时基信息 */
  providerInfo(): Promise<ProviderInfo>

  // ---- PACS（DICOM 网络）----
  /** C-ECHO 连通性验证（失败不抛，结果走 pacsStatus 信号） */
  pacsProbe(host: string, port: number, calledAe: string): void
  /** STUDY 级 C-FIND，枚举该 AE 上的检查 */
  pacsFindStudies(host: string, port: number, calledAe: string): void
  /** 最近一次 PACS 状态快照（空对象表示从未配置/探测过） */
  pacsState(): Promise<Partial<PacsStatus> & { count?: number }>

  // ---- DICOM 体积加载 ----
  loadDicomVolume(directory: string): void
  dicomSamplePath(): Promise<string>

  dataFromCpp: QtSignal<(payload: Record<string, unknown>) => void>
  // 标量信号的第二个参数是采集时刻（epoch 微秒）
  ecgSample: QtSignal<(value: number, tsUs: number) => void>
  vitalsReady: QtSignal<(vitals: Vitals) => void>
  eegSample: QtSignal<(value: number, tsUs: number) => void>
  emgSample: QtSignal<(value: number, tsUs: number) => void>
  neuralSpike: QtSignal<(value: number, tsUs: number) => void>
  interventionReady: QtSignal<(intervention: Intervention) => void>
  // 体积数据不走桥接：信号只给 URL，前端 fetch 该虚拟资源拿 ArrayBuffer
  dicomVolumeReady: QtSignal<(meta: DicomVolumeMeta, volumeUrl: string) => void>
  dicomVolumeError: QtSignal<(message: string) => void>
  pacsStatus: QtSignal<(status: PacsStatus) => void>
  pacsStudies: QtSignal<(studies: PacsStudy[], meta: PacsQueryMeta) => void>
}

/** PACS 连通性结果（与 C++ Bridge::pacsStatus 对应） */
export interface PacsStatus {
  ok: boolean
  host: string
  port: number
  calledAe: string
  /** C-ECHO 往返毫秒；失败为 -1 */
  roundTripMs: number
  message: string
  tsUs: number
}

/** C-FIND 返回的一例检查 */
export interface PacsStudy {
  patientName?: string
  patientId?: string
  studyDate?: string
  studyTime?: string
  studyInstanceUid?: string
  accession?: string
  modality?: string
  series?: number
}

/** C-FIND 汇总 */
export interface PacsQueryMeta {
  ok: boolean
  count: number
  host: string
  calledAe: string
  message: string
  tsUs: number
}

/** DICOM 体积元数据（与 C++ DicomReader 输出对应） */
export interface DicomVolumeMeta {
  dims: number[]       // [d,d,d]
  spacing: number[]    // [resX, resY, resZ]
  min: number
  max: number
  slices: number
  /** 因矩阵尺寸不一致被剔除的切片数（C++ 侧计入 meta.skipped） */
  skipped: number
  source: string
  /** 解析完成时刻（epoch 微秒） */
  tsUs?: number
}

/** qwebchannel.js 暴露的信号对象 */
export interface QtSignal<T extends (...args: never[]) => void> {
  connect(callback: T): void
  disconnect(callback?: T): void
}

let cached: Bridge | null = null
// 必须缓存「进行中」的连接 Promise，而不只是已连接的对象。
// React.StrictMode 在 development 构建（Vite dev server）下会 double-invoke effect，
// 若两次调用各 new 一个 QWebChannel，它们会共享同一个 qt.webChannelTransport，
// 后注册实例的 onmessage 会收到前一个实例的响应，导致
// "channel.execCallbacks[message.id] is not a function" 且 bridge 永远连不上。
// 生产构建（app://）不 double-invoke，所以该问题只在 dev 模式暴露。
let connecting: Promise<Bridge | null> | null = null

/**
 * 建立与 C++ 侧的 QWebChannel 连接。
 * 仅在 Qt WebEngine 环境下可用；纯浏览器中 resolve 为 null。
 * 幂等：并发/重复调用复用同一个连接，不会产生第二个 QWebChannel 实例。
 */
export function connectBridge(): Promise<Bridge | null> {
  if (connecting) return connecting

  connecting = new Promise((resolve) => {
    // 非 Qt WebEngine（如纯浏览器预览）时不建立连接
    if (!window.qt?.webChannelTransport) {
      resolve(null)
      return
    }

    new window.QWebChannel(window.qt.webChannelTransport, (channel) => {
      cached = channel.objects.bridge as unknown as Bridge
      resolve(cached)
    })
  })
  return connecting
}

export function getBridge(): Bridge | null {
  return cached
}
