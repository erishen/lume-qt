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
}

export interface Intervention {
  type: string
  label: string
  intensity: number
  frequency: number
  duration: number
  status: string
  time: string
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

  // ---- DICOM 体积加载 ----
  loadDicomVolume(directory: string): void
  dicomSamplePath(): Promise<string>

  dataFromCpp: QtSignal<(payload: Record<string, unknown>) => void>
  ecgSample: QtSignal<(value: number) => void>
  vitalsReady: QtSignal<(vitals: Vitals) => void>
  eegSample: QtSignal<(value: number) => void>
  emgSample: QtSignal<(value: number) => void>
  neuralSpike: QtSignal<(value: number) => void>
  interventionReady: QtSignal<(intervention: Intervention) => void>
  dicomVolumeReady: QtSignal<(meta: DicomVolumeMeta, base64: string) => void>
  dicomVolumeError: QtSignal<(message: string) => void>
}

/** DICOM 体积元数据（与 C++ DicomReader 输出对应） */
export interface DicomVolumeMeta {
  dims: number[]       // [d,d,d]
  spacing: number[]    // [resX, resY, resZ]
  min: number
  max: number
  slices: number
  source: string
}

/** qwebchannel.js 暴露的信号对象 */
export interface QtSignal<T extends (...args: never[]) => void> {
  connect(callback: T): void
  disconnect(callback?: T): void
}

let cached: Bridge | null = null

/**
 * 建立与 C++ 侧的 QWebChannel 连接。
 * 仅在 Qt WebEngine 环境下可用；纯浏览器中 resolve 为 null。
 */
export function connectBridge(): Promise<Bridge | null> {
  if (cached) return Promise.resolve(cached)

  return new Promise((resolve) => {
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
}

export function getBridge(): Bridge | null {
  return cached
}
