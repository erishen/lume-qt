import { useEffect, useRef, useState } from 'react'
import type { Bridge, DicomVolumeMeta, Intervention, PacsQueryMeta, PacsRetrieveStatus, PacsStatus, PacsStudy, Vitals } from './qwebchannel'
import Neuro3D from './Neuro3D'
import Volume3D from './Volume3D'
import { createSyntheticBrain } from './syntheticVolume'
import { createFrameLimiter, observeInViewport } from './framePacing'
import {
  channelHealth,
  clockStatus,
  frameStats,
  recordFrame,
  recordPainted,
  recordReceipt,
  setClockSource,
  snapshot,
  type ChannelHealth,
  type ChannelLatency,
  type FrameStats,
} from './latency'

// ECG 波形缓冲长度（点），对应画布横向采样
const ECG_BUFFER = 600

/**
 * MedicalMonitor — 医疗影像实时数据模拟面板。
 *
 * 数据源：C++ 侧 Bridge 经 QWebChannel 推送
 *  - ecgSample(double)：每 40ms 一个心电图样本
 *  - vitalsReady(map)：每 600ms 一次生命体征快照
 *
 * 渲染：
 *  - Canvas 实时滚动 ECG 波形
 *  - Canvas 模拟超声实时扫描影像
 *  - 体征数值卡（含颜色告警）
 */
export default function MedicalMonitor({ bridge }: { bridge: Bridge | null }) {
  const [vitals, setVitals] = useState<Vitals | null>(null)
  const rootRef = useRef<HTMLElement | null>(null)
  const ecgRef = useRef<HTMLCanvasElement | null>(null)
  const usRef = useRef<HTMLCanvasElement | null>(null)
  // ECG 样本缓冲（ref 避免频繁 setState）
  const ecgBuf = useRef<number[]>([])
  // 只有新样本到达才重绘 ECG（样本 25Hz，无需按 60Hz 空转）
  const ecgDirty = useRef(true)
  // 最近一个 ECG 样本的采集时刻，用于量「采集 → 上屏」
  const lastEcgTs = useRef<number | undefined>(undefined)

  useEffect(() => {
    // 订阅实时数据（Qt 桥接或纯浏览器演示）
    const onEcg = (value: number, tsUs?: number) => {
      recordReceipt('ecg', tsUs)
      lastEcgTs.current = tsUs
      const buf = ecgBuf.current
      buf.push(value)
      if (buf.length > ECG_BUFFER) buf.splice(0, buf.length - ECG_BUFFER)
      ecgDirty.current = true
    }
    const onVitals = (v: Vitals) => { recordReceipt('vitals', v.tsUs); setVitals(v) }

    // 纯浏览器演示：无 Qt 桥接时用 JS 模拟同源数据（与 C++ 侧一致）
    let ecgTimer: number | undefined
    let vitalsTimer: number | undefined

    if (bridge) {
      bridge.ecgSample.connect(onEcg)
      bridge.vitalsReady.connect(onVitals)
      bridge.startMedicalSimulation()
    } else {
      let phase = 0
      ecgTimer = window.setInterval(() => {
        phase += 0.048
        if (phase >= 1) phase -= 1
        onEcg(synthesizeEcgJs(phase))
      }, 40)

      let hr = 72, spo2 = 98, rr = 16, sys = 118, dia = 76, temp = 36.6
      vitalsTimer = window.setInterval(() => {
        hr = clamp(hr + (Math.random() * 6 - 3) * 0.2, 58, 102)
        spo2 = clamp(spo2 + (Math.random() * 4 - 2) * 0.1, 93, 100)
        rr = clamp(rr + (Math.random() * 4 - 2) * 0.1, 12, 24)
        sys = clamp(sys + (Math.random() * 6 - 3) * 0.3, 108, 142)
        dia = clamp(dia + (Math.random() * 4 - 2) * 0.2, 68, 92)
        temp = clamp(temp + (Math.random() * 4 - 2) * 0.02, 36, 38)
        onVitals({
          hr: Math.round(hr),
          spo2: Math.round(spo2),
          rr: Math.round(rr),
          sys: Math.round(sys),
          dia: Math.round(dia),
          temp: Math.round(temp * 10) / 10,
          time: new Date().toLocaleTimeString('zh-CN', { hour12: false }),
        })
      }, 600)
    }

    return () => {
      if (bridge) {
        bridge.ecgSample.disconnect(onEcg)
        bridge.vitalsReady.disconnect(onVitals)
        bridge.stopMedicalSimulation()
      } else {
        if (ecgTimer !== undefined) clearInterval(ecgTimer)
        if (vitalsTimer !== undefined) clearInterval(vitalsTimer)
      }
    }
  }, [bridge])

  // 渲染循环：Canvas 绘制 ECG + 超声
  useEffect(() => {
    let raf = 0
    let lastT = 0
    let stopObserve: (() => void) | null = null

    const usLimiter = createFrameLimiter(30)
    let inView = true
    if (rootRef.current) stopObserve = observeInViewport(rootRef.current, (v) => { inView = v })

    const draw = (t: number) => {
      raf = requestAnimationFrame(draw)
      const rawMs = t - lastT
      const dt = Math.min(rawMs, 100) / 1000
      lastT = t
      if (!inView || document.hidden) return
      // 只统计真正可见的帧：掉帧（rAF 间隔超 33ms）意味着画面在等帧，不是算得慢
      recordFrame(rawMs)
      if (ecgDirty.current) {
        ecgDirty.current = false
        recordPainted('ecg', lastEcgTs.current)
        drawEcg(ecgRef.current, ecgBuf.current, t)
      }
      if (usLimiter(t)) drawUltrasound(usRef.current, t, dt)
    }
    raf = requestAnimationFrame(draw)
    return () => {
      cancelAnimationFrame(raf)
      stopObserve?.()
    }
  }, [])

  return (
    <section className="card monitor" ref={rootRef}>
      <h2>医疗影像实时数据模拟</h2>
      <p className="monitor-note">数据由 C++ 侧实时生成并经 QWebChannel 推送 · Canvas 渲染</p>

      {/* 生命体征数值卡 */}
      <div className="vitals">
        <Vital label="心率" value={vitals?.hr} unit="bpm" color="var(--c-heart)"
               alert={vitals ? vitals.hr > 100 || vitals.hr < 60 : false} />
        <Vital label="血氧" value={vitals?.spo2} unit="%" color="var(--c-spo2)"
               alert={vitals ? vitals.spo2 < 95 : false} />
        <Vital label="呼吸" value={vitals?.rr} unit="次/分" color="var(--c-rr)" />
        <Vital label="血压" value={vitals ? `${vitals.sys}/${vitals.dia}` : undefined}
               unit="mmHg" color="var(--c-bp)"
               alert={vitals ? vitals.sys > 135 || vitals.dia > 88 : false} />
        <Vital label="体温" value={vitals?.temp} unit="°C" color="var(--c-temp)"
               alert={vitals ? vitals.temp > 37.5 : false} />
      </div>

      <LatencyBoard hasBridge={bridge !== null} bridge={bridge} />

      <WatchdogStrip hasBridge={bridge !== null} bridge={bridge} />

      {/* 波形 + 影像 */}
      <div className="monitor-row">
        <div className="panel">
          <div className="panel-head">
            <span>心电图 ECG · II 导联</span>
            <span className="panel-meta">{vitals?.time ?? '--:--:--'}</span>
          </div>
          <canvas ref={ecgRef} className="ecg-canvas" width={780} height={200} />
        </div>
        <div className="panel">
          <div className="panel-head">
            <span>超声影像 · 实时扫描</span>
            <span className="panel-meta">模拟 B 超</span>
          </div>
          <canvas ref={usRef} className="us-canvas" width={340} height={200} />
        </div>
      </div>

      {/* 神经内科介入模拟面板 */}
      <NeurologyPanel bridge={bridge} />

      {/* DICOM 体渲染面板 */}
      <DicomPanel bridge={bridge} />
    </section>
  )
}

// ---------------------------------------------------------------------------
// 神经内科介入模拟面板
// ---------------------------------------------------------------------------

/** 神经波形缓冲长度 */
const NEURO_BUFFER = 400

function NeurologyPanel({ bridge }: { bridge: Bridge | null }) {
  const [interventions, setInterventions] = useState<Intervention[]>([])
  const eegRef = useRef<HTMLCanvasElement | null>(null)
  const emgRef = useRef<HTMLCanvasElement | null>(null)
  const neuralRef = useRef<HTMLCanvasElement | null>(null)
  // 各波形缓冲（ref 避免频繁 setState）
  const eegBuf = useRef<number[]>([])
  const emgBuf = useRef<number[]>([])
  const neuralBuf = useRef<number[]>([])
  const rootRef = useRef<HTMLDivElement | null>(null)
  // 任一波形有新样本才重绘，避免 60Hz 空转
  const waveDirty = useRef(true)

  useEffect(() => {
    const push = (buf: number[], v: number) => {
      buf.push(v)
      if (buf.length > NEURO_BUFFER) buf.splice(0, buf.length - NEURO_BUFFER)
      waveDirty.current = true
    }
    const onEeg = (v: number, tsUs?: number) => { recordReceipt('eeg', tsUs); push(eegBuf.current, v) }
    const onEmg = (v: number, tsUs?: number) => { recordReceipt('emg', tsUs); push(emgBuf.current, v) }
    const onNeural = (v: number, tsUs?: number) => { recordReceipt('neural', tsUs); push(neuralBuf.current, v) }
    const onIv = (iv: Intervention) =>
      setInterventions((prev) => [...prev, iv].slice(-8))

    // 纯浏览器演示：JS 模拟同源数据
    let eegTimer: number | undefined
    let emgTimer: number | undefined
    let neuralTimer: number | undefined
    let ivTimer: number | undefined

    if (bridge) {
      bridge.eegSample.connect(onEeg)
      bridge.emgSample.connect(onEmg)
      bridge.neuralSpike.connect(onNeural)
      bridge.interventionReady.connect(onIv)
      bridge.startNeurologySimulation()
    } else {
      // 压力模式：?stress=1 把数据生成频率提高 10 倍（模拟医疗大数据量）
      const stress = new URLSearchParams(window.location.search).get('stress') === '1'
      const eegMs = stress ? 5 : 50
      const emgMs = stress ? 10 : 100
      const neuralMs = stress ? 8 : 75
      const ivMs = stress ? 50 : 500
      let phase = 0
      let emgBurst = 0
      let neuralEnv = 0
      eegTimer = window.setInterval(() => {
        phase += 0.5
        onEeg(synthesizeEegJs(phase))
      }, eegMs)
      emgTimer = window.setInterval(() => {
        emgBurst = Math.max(0, Math.min(1, emgBurst + (Math.random() - 0.5) * 0.24))
        if (Math.random() < 0.04) emgBurst = 1
        onEmg(0.5 + (emgBurst * (Math.random() - 0.5) * 1.2))
      }, emgMs)
      neuralTimer = window.setInterval(() => {
        neuralEnv = Math.max(0, Math.min(1, neuralEnv + (Math.random() - 0.5) * 0.16))
        if (Math.random() < 0.02) neuralEnv = 1
        onNeural(0.15 * Math.sin(phase * 0.7) + (Math.random() - 0.5) * 0.1 + neuralEnv * (Math.random() - 0.2) * 0.8)
      }, neuralMs)
      ivTimer = window.setInterval(() => {
        const labels = ['TMS', 'DBS', 'ECT', 'NF']
        const label = labels[Math.floor(Math.random() * labels.length)]
        onIv({
          type: label,
          label,
          intensity: Math.round(30 + Math.random() * 70),
          frequency: Math.round(1 + Math.random() * 129),
          duration: Math.round(5 + Math.random() * 55),
          status: Math.random() < 0.7 ? 'active' : 'completed',
          time: new Date().toLocaleTimeString('zh-CN', { hour12: false }),
        })
      }, ivMs)
    }

    return () => {
      if (bridge) {
        bridge.eegSample.disconnect(onEeg)
        bridge.emgSample.disconnect(onEmg)
        bridge.neuralSpike.disconnect(onNeural)
        bridge.interventionReady.disconnect(onIv)
        bridge.stopNeurologySimulation()
      } else {
        if (eegTimer !== undefined) clearInterval(eegTimer)
        if (emgTimer !== undefined) clearInterval(emgTimer)
        if (neuralTimer !== undefined) clearInterval(neuralTimer)
        if (ivTimer !== undefined) clearInterval(ivTimer)
      }
    }
  }, [bridge])

  // 渲染循环：三条神经波形
  useEffect(() => {
    let raf = 0
    let inView = true
    let stopObserve: (() => void) | null = null
    if (rootRef.current) stopObserve = observeInViewport(rootRef.current, (v) => { inView = v })

    const draw = () => {
      raf = requestAnimationFrame(draw)
      if (!inView || document.hidden || !waveDirty.current) return
      waveDirty.current = false
      drawWave(eegRef.current, eegBuf.current, '#5ee7df', -2, 2)
      drawWave(emgRef.current, emgBuf.current, '#ff8a65', 0, 1)
      drawWave(neuralRef.current, neuralBuf.current, '#ffd166', -1, 1)
    }
    raf = requestAnimationFrame(draw)
    return () => {
      cancelAnimationFrame(raf)
      stopObserve?.()
    }
  }, [])

  return (
    <div className="neuro" ref={rootRef}>
      <h3 className="neuro-title">🧠 神经内科介入监测</h3>
      <div className="neuro-grid">
        <NeuroPanel title="脑电图 EEG · 10μV/格" refCb={eegRef} color="#5ee7df" />
        <NeuroPanel title="肌电图 EMG · 运动单位电位" refCb={emgRef} color="#ff8a65" />
        <NeuroPanel title="神经元活动 · 棘波放电" refCb={neuralRef} color="#ffd166" />
      </div>

      {/* 三维神经脉络 */}
      <div className="neuro-3d-wrap">
        <div className="panel-head">
          <span>🧠 神经脉络 · 实时介入手术可视化</span>
          <span className="panel-meta">拖拽旋转 · 滚轮缩放</span>
        </div>
        <Neuro3D bridge={bridge} />
      </div>
      <div className="neuro-iv">
        <div className="panel-head">
          <span>神经介入事件 · 实时记录</span>
          <span className="panel-meta">
            {bridge ? 'C++ 推送' : 'JS 模拟'}
          </span>
        </div>
        <div className="iv-list">
          {interventions.length === 0 && (
            <p className="iv-empty">等待介入事件…</p>
          )}
          {interventions.slice().reverse().map((iv, i) => (
            <div key={i} className={`iv-item ${iv.status === 'active' ? 'iv-active' : 'iv-done'}`}>
              <span className="iv-badge">{iv.label}</span>
              <span className="iv-meta">
                强度 {iv.intensity}% · {iv.frequency}Hz · {iv.duration}min
              </span>
              <span className="iv-time">{iv.time}</span>
            </div>
          ))}
        </div>
      </div>
    </div>
  )
}

function NeuroPanel({ title, refCb, color }: {
  title: string
  refCb: React.Ref<HTMLCanvasElement>
  color: string
}) {
  return (
    <div className="panel">
      <div className="panel-head">
        <span>{title}</span>
        <span className="panel-meta" style={{ color }}>●</span>
      </div>
      <canvas ref={refCb} className="neuro-canvas" width={340} height={130} />
    </div>
  )
}

/** 通用波形绘制（归一化 min/max 到画布） */
function drawWave(canvas: HTMLCanvasElement | null, data: number[], color: string, lo: number, hi: number) {
  if (!canvas) return
  const ctx = canvas.getContext('2d')
  if (!ctx) return
  const { width: w, height: h } = canvas

  ctx.drawImage(gridLayer(canvas, '#08121a', 'rgba(40,80,110,0.2)', 20, false), 0, 0)

  const n = data.length
  if (n < 2) return

  ctx.strokeStyle = color
  ctx.lineWidth = 1.5
  ctx.beginPath()
  const step = w / (NEURO_BUFFER - 1)
  const startX = w - (n - 1) * step
  for (let i = 0; i < n; i++) {
    const x = startX + i * step
    const norm = (data[i] - lo) / (hi - lo)
    const y = h - 8 - norm * (h - 16)
    if (i === 0) ctx.moveTo(x, y)
    else ctx.lineTo(x, y)
  }
  ctx.stroke()
}

/** JS 版 EEG 合成（与 C++ 侧一致） */
function synthesizeEegJs(phase: number): number {
  let v = 0
  v += 0.8 * Math.sin(phase * 3.0)
  v += 0.4 * Math.sin(phase * 6.5)
  v += 0.2 * Math.sin(phase * 12.0)
  v += 0.1 * Math.sin(phase * 1.2)
  v += (Math.random() - 0.5) * 0.08
  if (Math.random() < 0.03) v += 0.4 + Math.random() * 0.8
  return Math.max(-2, Math.min(2, v))
}

// ---------------------------------------------------------------------------
// 纯浏览器演示数据源（与 C++ 侧 Bridge::synthesizeEcg 逻辑一致）
// ---------------------------------------------------------------------------

/** JS 版心电图合成：P 波 → QRS 波群 → T 波 */
function synthesizeEcgJs(phase: number): number {
  const gauss = (x: number, c: number, w: number, a: number) => {
    const d = (x - c) / w
    return a * Math.exp(-d * d)
  }
  let v = 0
  v += gauss(phase, 0.18, 0.035, 0.12)  // P
  v -= gauss(phase, 0.32, 0.012, 0.10)  // Q
  v += gauss(phase, 0.36, 0.014, 1.0)   // R
  v -= gauss(phase, 0.40, 0.012, 0.22)  // S
  v += gauss(phase, 0.62, 0.045, 0.28)  // T
  v += 0.02 * Math.sin(phase * Math.PI)  // 基线漂移
  v += (Math.random() - 0.5) * 0.012     // 噪声
  return v
}

function clamp(v: number, lo: number, hi: number): number {
  return v < lo ? lo : v > hi ? hi : v
}

/** 生命体征数值卡 */
function Vital({ label, value, unit, color, alert }: {
  label: string
  value?: number | string
  unit: string
  color: string
  alert?: boolean
}) {
  return (
    <div className={`vital ${alert ? 'vital-alert' : ''}`}>
      <span className="vital-label">{label}</span>
      <span className="vital-value" style={{ color }}>
        {value ?? '--'}
        <small className="vital-unit">{unit}</small>
      </span>
    </div>
  )
}

// ---------------------------------------------------------------------------
// 链路延迟看板
// ---------------------------------------------------------------------------

const CHANNEL_LABEL: Record<string, string> = {
  ecg: 'ECG 波形',
  eeg: 'EEG 波形',
  emg: 'EMG 波形',
  neural: '神经元活动',
  vitals: '生命体征',
  volume: 'DICOM 体积',
}

function fmt(ms: number): string {
  return ms > 0 ? ms.toFixed(1) : '—'
}

/**
 * 延迟看板：读 latency.ts 的环形缓冲快照，1Hz 刷新。
 * 「到达」= 采集 → JS 回调；「上屏」= 采集 → 本帧真正 draw（目前只对 ECG 与体积采样）。
 * 窗口底部的原生波形条给出同机对照（不经 WebView）。
 */
/** 信号看门狗：任何通道停止更新或延迟超预算都必须显式告警，而不是继续画旧波形 */
function WatchdogStrip({ hasBridge, bridge }: { hasBridge: boolean; bridge: Bridge | null }) {
  const [tick, setTick] = useState<{ health: ChannelHealth[]; frame: FrameStats } | null>(null)
  const lastBad = useRef('')
  const slowStreak = useRef(0)

  useEffect(() => {
    const update = () => {
      const h = channelHealth()
      setTick({ health: h, frame: frameStats() })
      // 状态变化时落一条日志：术中告警必须可回溯，不能只闪一下
      // 「慢」连续 2 次采样（约 1s）才上报，避免单次抖动刷日志；中断立即上报
      const raw = h.filter((c) => c.state === 'stale' || c.state === 'slow')
      slowStreak.current = raw.some((c) => c.state === 'slow') ? slowStreak.current + 1 : 0
      const badNow = raw.filter((c) => c.state === 'stale' || slowStreak.current >= 2)
      const sig = badNow.map((c) => `${c.channel}:${c.state}`).join(',')
      if (sig !== lastBad.current) {
        lastBad.current = sig
        // 状态变化落结构化台账：WebView 侧与原生条是两个独立判定来源，
        // source 字段让事后复核能分清是谁报的。console 只留给人看。
        bridge?.logAuditEvent({
          type: badNow.length > 0 ? 'watchdog.bad' : 'watchdog.ok',
          source: 'web-frontend',
          states: badNow.map((c) => `${c.channel}:${c.state}`).join(','),
          ageMs: badNow.length > 0 ? Math.round(Math.max(...badNow.map((c) => c.ageMs))) : 0,
          p50Ms: badNow.length > 0 ? Math.round(Math.max(...badNow.map((c) => c.receiptP50))) : 0,
        })
        if (badNow.length > 0) {
          console.warn('[Watchdog] ' + badNow
            .map((c) => `${c.channel}=${c.state}(age=${Math.round(c.ageMs)}ms, p50=${c.receiptP50.toFixed(1)}ms, p99=${c.receiptP99.toFixed(0)}ms)`)
            .join(' '))
        } else {
          console.log('[Watchdog] 信号恢复正常')
        }
      }
    }
    update()
    const timer = window.setInterval(update, 500)
    return () => window.clearInterval(timer)
  }, [bridge])

  if (!hasBridge || !tick) return null

  const bad = tick.health.filter((c) => c.state === 'stale' || c.state === 'slow')

  const okCount = tick.health.filter((c) => c.state === 'ok').length
  const stale = bad.filter((c) => c.state === 'stale')
  const slow = bad.filter((c) => c.state === 'slow')
  const frame = tick.frame

  if (bad.length === 0) {
    return (
      <div className="watchdog watchdog-ok">
        <span className="watchdog-dot" />
        <span>
          信号正常 · {okCount}/{tick.health.length} 通道有数据
          {frame.samples > 0
            ? ` · 帧 P50 ${frame.p50.toFixed(1)}ms / P99 ${frame.p99.toFixed(1)}ms`
            : ''}
        </span>
      </div>
    )
  }

  return (
    <div className="watchdog watchdog-bad">
      <span className="watchdog-dot" />
      <span>
        {stale.length > 0
          ? `信号中断：${stale
              .map((c) => `${CHANNEL_LABEL[c.channel]} ${(c.ageMs / 1000).toFixed(1)}s`)
              .join(' · ')}（画面为冻结帧）`
          : ''}
        {stale.length > 0 && slow.length > 0 ? '；' : ''}
        {slow.length > 0
          ? `延迟超预算：${slow
              .map((c) => `${CHANNEL_LABEL[c.channel]} P50 ${c.receiptP50.toFixed(0)}ms`)
              .join(' · ')}`
          : ''}
      </span>
    </div>
  )
}

function LatencyBoard({ hasBridge, bridge }: { hasBridge: boolean; bridge: Bridge | null }) {
  const [rows, setRows] = useState<ChannelLatency[]>([])
  const [clock, setClock] = useState(clockStatus())
  // PACS 状态：接入现场第一句要问的就是「连上了吗、库里有几例」，
  // 它和延迟指标同屏，免得再去翻日志。
  const [pacs, setPacs] = useState<{ ok: boolean; rttMs: number; count: number; ae: string; message: string } | null>(null)
  // 取回按钮需要 StudyInstanceUID；这里记第一例，够用且不需要另建病人浏览器
  const [studyUid, setStudyUid] = useState('')
  const [retrieving, setRetrieving] = useState(false)
  // 取回失败会按退避自动重试；这里把「第几次 / 为什么」写在按钮上，
  // 否则术中只能看到一个转圈，不知道是在重试还是卡死。
  const [retrieveNote, setRetrieveNote] = useState('')
  const pacsRef = useRef<{ host: string; port: number; ae: string } | null>(null)

  // 时间戳到底来自宿主时钟还是设备时钟，由 C++ 侧 provider 如实声明。
  // 设备时钟与宿主存在未知常量偏移，不先扣掉就会把「时钟差」当成「延迟」。
  useEffect(() => {
    if (!bridge) {
      setClockSource(false)
      setClock(clockStatus())
      return
    }
    let alive = true
    void bridge.providerInfo().then((info) => {
      if (!alive) return
      setClockSource(Boolean(info?.hasHardwareClock))
      setClock(clockStatus())
    })
    return () => {
      alive = false
    }
  }, [bridge])

  useEffect(() => {
    if (!bridge || typeof bridge.pacsStatus?.connect !== 'function') return
    const onStatus = (st: PacsStatus) => {
      pacsRef.current = { host: st.host, port: st.port, ae: st.calledAe }
      setPacs({ ok: st.ok, rttMs: st.roundTripMs, count: 0, ae: st.calledAe, message: st.message })
    }
    const onStudies = (studies: PacsStudy[], meta: PacsQueryMeta) => {
      setPacs((prev) => ({
        ok: meta.ok,
        rttMs: prev?.rttMs ?? -1,
        count: meta.count,
        ae: meta.calledAe,
        message: meta.message,
      }))
      const uid = studies.find((s) => s.studyInstanceUid)?.studyInstanceUid
      setStudyUid(uid ?? '')
    }
    const onRetrieve = (st: PacsRetrieveStatus) => {
      setRetrieving(Boolean(st.retrying))
      setRetrieveNote(
        st.ok
          ? ''
          : st.retrying
            ? `重试中（第 ${(st.attempt ?? 1) + 1}/${st.maxAttempts ?? 3} 次）`
            : st.message,
      )
      setPacs((prev) =>
        prev
          ? {
              ...prev,
              ok: st.ok,
              message: st.ok ? '' : st.message,
            }
          : prev,
      )
    }
    bridge.pacsStatus.connect(onStatus)
    bridge.pacsStudies.connect(onStudies)
    bridge.pacsRetrieve.connect(onRetrieve)
    // 启动即探测，信号往往早于 WebView 订阅，必须主动拉一次当前值
    void bridge.pacsState?.().then((st) => {
      if (!st || typeof st.ok !== 'boolean') return
      setPacs({
        ok: st.ok,
        rttMs: st.roundTripMs ?? -1,
        count: st.count ?? 0,
        ae: st.calledAe ?? '',
        message: st.message ?? '',
      })
      if (st.studyUid) setStudyUid(st.studyUid)
    })
    return () => {
      bridge.pacsStatus.disconnect(onStatus)
      bridge.pacsStudies.disconnect(onStudies)
      bridge.pacsRetrieve.disconnect(onRetrieve)
    }
  }, [bridge])

  useEffect(() => {
    const tick = () => {
      setRows(snapshot())
      setClock(clockStatus())
    }
    tick()
    const timer = window.setInterval(tick, 1000)

    return () => window.clearInterval(timer)
  }, [])

  const active = rows.filter((r) => r.samples > 0)

  return (
    <div className="latency">
      <div className="latency-head">
        <span>链路延迟 · 采集 → 上屏</span>
        <span className="panel-meta">
          {!hasBridge
            ? '无桥接，纯浏览器演示'
            : clock.mode === 'device'
              ? `设备时钟${clock.offsetMs < 0 ? `快 ${(-clock.offsetMs / 1000).toFixed(1)} s` : `慢 ${(clock.offsetMs / 1000).toFixed(1)} s`} · 偏移已扣除`
              : 'C++ 打时间戳 · 宿主同源'}
          {pacs ? (
            <span className={pacs.ok ? 'pacs-chip ok' : 'pacs-chip bad'}>
              {' · PACS '}
              {pacs.ok
                ? `已连通 ${pacs.rttMs} ms · ${pacs.count} 例`
                : `未连通 ${pacs.message || ''}`}
            </span>
          ) : (
            <span className="pacs-chip idle">{' · PACS 未配置'}</span>
          )}
          {pacs?.ok && studyUid && pacsRef.current ? (
            <button
              type="button"
              className="pacs-retrieve"
              title={retrieveNote}
              disabled={retrieving}
              onClick={() => {
                const target = pacsRef.current
                if (!target) return
                setRetrieving(true)
                setRetrieving(true)
                setRetrieveNote('')
                bridge?.pacsRetrieveStudy(target.host, target.port, target.ae, studyUid)
              }}
            >
              {retrieving
                ? retrieveNote
                  ? `重试中…`
                  : '取回中…'
                : '取回该例'}
            </button>
          ) : null}
        </span>
      </div>
      {active.length === 0 ? (
        <p className="iv-empty">等待样本…</p>
      ) : (
        <table className="latency-table">
          <thead>
            <tr>
              <th>通道</th>
              <th>到达 P50</th>
              <th>到达 P99</th>
              <th>上屏 P50</th>
              <th>上屏 P99</th>
              <th>样本</th>
            </tr>
          </thead>
          <tbody>
            {active.map((r) => (
              <tr key={r.channel}>
                <td>{CHANNEL_LABEL[r.channel] ?? r.channel}</td>
                <td>{fmt(r.receiptP50)} ms</td>
                <td className={r.receiptP99 > 100 ? 'lat-warn' : ''}>{fmt(r.receiptP99)} ms</td>
                <td>{fmt(r.paintedP50)} ms</td>
                <td className={r.paintedP99 > 100 ? 'lat-warn' : ''}>{fmt(r.paintedP99)} ms</td>
                <td>{r.samples}</td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
      <p className="latency-note">
        「到达」= 采集 → JS 回调；「上屏」= 采集 → 该帧真正绘制。原生通道（窗口底部波形条）
        走 Qt 信号直连 + QPainter，不经 WebView，可作同机对照。
        {clock.mode === 'device' &&
          ' 时间戳来自设备时钟：单侧测量只能得到「时钟偏移 + 延迟」之和，' +
          '这里用最近 120 个样本的最小值估偏移并扣除，表中数值是扣偏移后的残差（略小于真实延迟，不会虚高）。'}
      </p>
    </div>
  )
}

// ---------------------------------------------------------------------------
// Canvas 渲染
// ---------------------------------------------------------------------------

/**
 * 背景 + 网格是静态的，但原先每帧都重画一遍（十几个 beginPath/stroke）。
 * 尺寸与配色不变时缓存成一张离屏图，每帧只做一次 drawImage。
 */
const gridLayerCache = new WeakMap<HTMLCanvasElement, { key: string; layer: HTMLCanvasElement }>()

function gridLayer(
  canvas: HTMLCanvasElement,
  bg: string,
  grid: string,
  step: number,
  horizontal: boolean,
): HTMLCanvasElement {
  const key = `${canvas.width}x${canvas.height}|${bg}|${grid}|${step}|${horizontal}`
  const hit = gridLayerCache.get(canvas)
  if (hit && hit.key === key) return hit.layer

  const layer = document.createElement('canvas')
  layer.width = canvas.width
  layer.height = canvas.height
  const ctx = layer.getContext('2d')!
  ctx.fillStyle = bg
  ctx.fillRect(0, 0, layer.width, layer.height)
  ctx.strokeStyle = grid
  ctx.lineWidth = 1
  for (let x = 0; x < layer.width; x += step) {
    ctx.beginPath()
    ctx.moveTo(x + 0.5, 0)
    ctx.lineTo(x + 0.5, layer.height)
    ctx.stroke()
  }
  if (horizontal) {
    for (let y = 0; y < layer.height; y += step) {
      ctx.beginPath()
      ctx.moveTo(0, y + 0.5)
      ctx.lineTo(layer.width, y + 0.5)
      ctx.stroke()
    }
  } else {
    ctx.beginPath()
    ctx.moveTo(0, layer.height / 2 + 0.5)
    ctx.lineTo(layer.width, layer.height / 2 + 0.5)
    ctx.stroke()
  }
  gridLayerCache.set(canvas, { key, layer })
  return layer
}

/** 滚动 ECG 波形 */
function drawEcg(canvas: HTMLCanvasElement | null, data: number[], t: number) {
  if (!canvas) return
  const ctx = canvas.getContext('2d')
  if (!ctx) return
  const { width: w, height: h } = canvas

  // 背景 + 网格（静态层，缓存后一次 drawImage 贴上）
  ctx.drawImage(gridLayer(canvas, '#060a10', 'rgba(40,80,110,0.25)', 20, true), 0, 0)

  const mid = h / 2
  const amp = h * 0.38
  const n = data.length

  if (n < 2) return

  // 信号折线
  ctx.strokeStyle = '#33e07a'
  ctx.lineWidth = 1.6
  ctx.beginPath()
  const step = w / (ECG_BUFFER - 1)
  const startX = w - (n - 1) * step
  for (let i = 0; i < n; i++) {
    const x = startX + i * step
    const y = mid - data[i] * amp
    if (i === 0) ctx.moveTo(x, y)
    else ctx.lineTo(x, y)
  }
  ctx.stroke()

  // 扫描头（当前最右端）
  const lastX = w - 2
  const lastY = mid - data[n - 1] * amp
  ctx.fillStyle = '#33e07a'
  ctx.beginPath()
  ctx.arc(lastX, lastY, 2.5, 0, Math.PI * 2)
  ctx.fill()

  // 实时数值（心率来自 vitals 由父组件传，这里画提示）
  void t
}

/** 按画布尺寸缓存径向渐变（createRadialGradient 每帧重建是纯浪费） */
const backdropCache = new WeakMap<HTMLCanvasElement, { w: number; h: number; g: CanvasGradient }>()
function backdropGradient(
  canvas: HTMLCanvasElement,
  ctx: CanvasRenderingContext2D,
  cx: number,
  cy: number,
  radius: number,
): CanvasGradient {
  const hit = backdropCache.get(canvas)
  if (hit && hit.w === canvas.width && hit.h === canvas.height) return hit.g
  const g = ctx.createRadialGradient(cx, cy, 0, cx, cy, radius)
  g.addColorStop(0, '#0d141c')
  g.addColorStop(1, '#05080c')
  backdropCache.set(canvas, { w: canvas.width, h: canvas.height, g })
  return g
}

/** 模拟超声实时扫描影像 */
function drawUltrasound(canvas: HTMLCanvasElement | null, t: number, dt: number) {
  if (!canvas) return
  const ctx = canvas.getContext('2d')
  if (!ctx) return
  const { width: w, height: h } = canvas

  ctx.fillStyle = '#04070a'
  ctx.fillRect(0, 0, w, h)

  // 探头位于底部中央，扇形向上张开（标准 B 超布局，图像居中）
  const cx = w * 0.5
  const cy = h * 0.93
  const radius = Math.min(w, h) * 0.82
  const spread = 0.55 // 半张角（弧度）

  ctx.fillStyle = '#96c3dc'
  ctx.save()
  ctx.beginPath()
  ctx.moveTo(cx, cy)
  ctx.arc(cx, cy, radius, -Math.PI / 2 - spread, -Math.PI / 2 + spread)
  ctx.closePath()
  ctx.clip()

  // 组织底色（径向渐变）：渐变对象按画布尺寸缓存，不必每帧重建
  ctx.fillStyle = backdropGradient(canvas, ctx, cx, cy, radius)
  ctx.fillRect(0, 0, w, h)
  // 组织斑点用固定色 + globalAlpha 控制透明度（不每帧拼 rgba 字符串）
  ctx.fillStyle = '#96c3dc'

  // 组织界面（伪随机斑点，缓慢漂移模拟呼吸/搏动）
  for (let i = 0; i < 70; i++) {
    const seed = i * 137.508
    const angle = -Math.PI / 2 + (Math.sin(seed * 1.7) * 0.5) * spread * 1.6
    const r = ((Math.sin(seed * 1.31 + t * 0.25) * 0.5 + 0.5) * 0.92 + 0.05) * radius
    const x = cx + Math.cos(angle) * r
    const y = cy + Math.sin(angle) * r
    const a = 0.10 + 0.35 * (Math.sin(seed * 2.73 + t * 0.9) * 0.5 + 0.5)
    const s = 1 + (Math.sin(seed + t * 0.6) * 0.5 + 0.5) * 2.4
    ctx.globalAlpha = a
    ctx.beginPath()
    ctx.arc(x, y, s, 0, Math.PI * 2)
    ctx.fill()
  }

  // 血流多普勒（红/蓝斑点，沿扇轴移动）
  for (let i = 0; i < 14; i++) {
    const seed = i * 89.7
    const angle = -Math.PI / 2 + (Math.sin(seed) * 0.5) * spread * 1.2
    const speed = 0.10 + (seed % 7) / 40
    const r = ((t * speed + seed * 0.13) % 1.0) * radius
    const x = cx + Math.cos(angle) * r
    const y = cy + Math.sin(angle) * r
    const a = 0.5 * (1 - r / radius)
    const red = (i % 2) === 0
    ctx.fillStyle = red ? '#ff5a5a' : '#5a96ff'
    ctx.globalAlpha = a
    ctx.beginPath()
    ctx.arc(x, y, 1.6, 0, Math.PI * 2)
    ctx.fill()
  }

  // 扫描线（往复摆动）
  ctx.globalAlpha = 1
  const sweep = -Math.PI / 2 + Math.sin(t * 1.4) * spread * 0.9
  const grad = ctx.createLinearGradient(cx, cy,
    cx + Math.cos(sweep) * radius, cy + Math.sin(sweep) * radius)
  grad.addColorStop(0, 'rgba(180,240,255,0.9)')
  grad.addColorStop(1, 'rgba(180,240,255,0)')
  ctx.strokeStyle = grad
  ctx.lineWidth = 2
  ctx.beginPath()
  ctx.moveTo(cx, cy)
  ctx.lineTo(cx + Math.cos(sweep) * radius, cy + Math.sin(sweep) * radius)
  ctx.stroke()

  ctx.globalAlpha = 1
  ctx.restore()

  // 扇区边界弧
  ctx.strokeStyle = 'rgba(80,130,160,0.55)'
  ctx.lineWidth = 1.5
  ctx.beginPath()
  ctx.arc(cx, cy, radius, -Math.PI / 2 - spread, -Math.PI / 2 + spread)
  ctx.stroke()
  // 探头图标
  ctx.fillStyle = 'rgba(120,180,210,0.9)'
  ctx.fillRect(cx - 3, cy - 4, 6, 8)

  void dt
}

// ---------------------------------------------------------------------------
// DICOM 体渲染面板
// ---------------------------------------------------------------------------

function DicomPanel({ bridge }: { bridge: Bridge | null }) {
  const [volume, setVolume] = useState<{ pixels: Uint8Array | ArrayBuffer; meta: DicomVolumeMeta } | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [loading, setLoading] = useState(false)

  useEffect(() => {
    let mounted = true

    const onReady = (meta: DicomVolumeMeta, volumeUrl: string) => {
      if (!mounted) return
      // 体积走 app:// 虚拟资源的二进制直取：QWebChannel 的文本通道会把
      // 2MB 二进制按 UTF-8 处理而破坏数据（实测量级：丢字节），不能用。
      fetch(volumeUrl)
        .then((resp) => {
          if (!resp.ok) throw new Error(`HTTP ${resp.status}`)
          return resp.arrayBuffer()
        })
        .then((buf) => {
          if (!mounted) return
          recordReceipt('volume', meta.tsUs)
          setLoading(false)
          setVolume({ pixels: buf, meta })
          setError(null)
          console.log('[DicomPanel] 体数据已取回', buf.byteLength, 'bytes', meta.dims)
        })
        .catch((err: unknown) => {
          if (!mounted) return
          setLoading(false)
          setError(`体数据取回失败：${String(err)}`)
        })
    }
    const onError = (msg: string) => {
      if (!mounted) return
      setLoading(false)
      setError(msg)
    }

    if (bridge) {
      bridge.dicomSamplePath().then((path) => {
        if (!mounted) return
        console.log('[DicomPanel] 样例路径:', path)
        bridge!.dicomVolumeReady.connect(onReady)
        bridge!.dicomVolumeError.connect(onError)
        setLoading(true)
        bridge!.loadDicomVolume(path)
      })
    } else {
      // 纯浏览器演示：合成体积
      setLoading(true)
      setTimeout(() => {
        if (!mounted) return
        const syn = createSyntheticBrain(96)
        setVolume(syn)
        setLoading(false)
      }, 300)
    }

    return () => {
      mounted = false
      if (bridge) {
        bridge.dicomVolumeReady.disconnect(onReady)
        bridge.dicomVolumeError.disconnect(onError)
      }
    }
  }, [bridge])

  return (
    <div className="vol-panel">
      <div className="panel-head">
        <span>🏥 DICOM 体渲染 · 三维医学影像</span>
        <span className="panel-meta">
          {bridge ? 'C++ / DCMTK' : 'JS 合成'}
        </span>
      </div>
      {loading && <div className="vol-loading">加载体积数据中…</div>}
      {error && <div className="vol-error">加载失败：{error}</div>}
      {volume && <Volume3D volume={volume} />}
    </div>
  )
}
