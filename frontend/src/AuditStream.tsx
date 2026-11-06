import { useEffect, useRef, useState } from 'react'
import type { Bridge } from './qwebchannel'

/**
 * AuditStream — 台账事件流（本地 JSONL 是唯一权威，这里只是它的检索视图）。
 *
 * 解决的问题：术中复盘问的是「那次血氧报警之前信号断过几次」，
 * 而不是把几千行 JSONL 摊开。所以提供类型过滤 + 全文搜索（类型、通道、数值都参与），
 * 结果固定按「新→旧」显示。
 *
 * 注意：列表不设内层 max-height/overflow —— 页面本身已经有滚动条，
 * 再套一层会出现第二个竖向滚动条、滚轮落在列表上时不带动页面。
 */

const TYPE_FILTERS = [
  { id: '', label: '全部' },
  { id: 'alarm.', label: '报警' },
  { id: 'signal.', label: '信号' },
  { id: 'pacs.', label: 'PACS' },
  { id: 'volume.', label: '体数据' },
  { id: 'session.', label: '会话' },
] as const

const ROW_LIMIT = 8

interface AuditEvent {
  type?: string
  ts?: string
  session?: string
  priority?: string
  level?: number
  channel?: string
  spo2?: number
  hr?: number
  ms?: number
  dir?: string
  host?: string
  port?: number
  studies?: number
  layers?: number
  parseMs?: number
  source?: string
  spo2High?: number
  spo2Mid?: number
  [key: string]: unknown
}

interface AuditQueryResult {
  events: AuditEvent[]
  scanned: number
  returned: number
  truncated: boolean
}

/** 一行摘要：只挑对判读有用的字段，缺就退回紧凑 JSON（不隐藏信息）。 */
function describe(ev: AuditEvent): string {
  const type = ev.type ?? '?'
  switch (type) {
    case 'alarm.on':
    case 'alarm.off':
      return [
        ev.channel,
        ev.priority ? `优先级 ${ev.priority}` : null,
        typeof ev.spo2 === 'number' ? `SpO₂ ${Math.round(ev.spo2)}%` : null,
        typeof ev.hr === 'number' ? `HR ${Math.round(ev.hr)}` : null,
      ].filter(Boolean).join(' · ')
    case 'alarm.config':
      return [
        `来源 ${ev.source ?? '?'}`,
        `SpO₂<${ev.spo2High ?? '?'}/${ev.spo2Mid ?? '?'}`,
      ].join(' · ')
    case 'signal.stall':
    case 'signal.resume':
      return [
        ev.channel,
        typeof ev.ms === 'number' ? `${Math.round(ev.ms)} ms` : null,
      ].filter(Boolean).join(' · ')
    case 'pacs.status':
      return [ev.dir ?? ev.host, typeof ev.port === 'number' ? `:${ev.port}` : null,
        typeof ev.ms === 'number' ? `${Math.round(ev.ms)} ms` : null]
        .filter(Boolean).join(' · ')
    case 'volume.load':
    case 'volume.restore': {
      const dir = typeof ev.dir === 'string' ? ev.dir.split('/').filter(Boolean).pop() : undefined
      return [
        dir,
        typeof ev.layers === 'number' ? `${ev.layers} 层` : null,
        typeof ev.parseMs === 'number' ? `解析 ${Math.round(ev.parseMs)} ms` : null,
      ].filter(Boolean).join(' · ') || '—'
    }
    default: {
      const { type: _t, ts: _ts, session: _s, tsUs: _us, ...rest } = ev
      const keys = Object.keys(rest)
      if (keys.length === 0) return '—'
      return keys.slice(0, 4).map((k) => `${k}=${String(rest[k])}`).join(' · ')
    }
  }
}

function timeOf(ev: AuditEvent): string {
  const ts = ev.ts ?? ''
  // "2026-11-06T12:17:34.584" → 12:17:34
  return ts.length >= 19 ? ts.slice(11, 19) : ts
}

function toneOf(ev: AuditEvent): string {
  const type = ev.type ?? ''
  if (type.startsWith('alarm.')) {
    if (ev.priority === 'high' || ev.level === 2) return 'high'
    if (ev.priority === 'medium' || ev.level === 1) return 'mid'
  }
  if (type.startsWith('signal.')) return 'signal'
  return 'plain'
}

export default function AuditStream({ bridge }: { bridge: Bridge | null }) {
  const [typeFilter, setTypeFilter] = useState<string>('')
  const [text, setText] = useState<string>('')
  const [result, setResult] = useState<AuditQueryResult | null>(null)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string>('')
  const seq = useRef(0)

  useEffect(() => {
    if (!bridge) return
    const id = window.setTimeout(async () => {
      const mine = ++seq.current
      setBusy(true)
      try {
        const res = (await bridge.queryAuditEvents({
          type: typeFilter,
          text,
          limit: ROW_LIMIT,
        })) as AuditQueryResult
        // 搜索是异步的：晚到的旧结果不能覆盖新结果，否则输入「93」时先闪出全量再跳。
        if (mine === seq.current) {
          setResult(res)
          setError('')
        }
      } catch (err) {
        if (mine === seq.current) setError(String(err))
      } finally {
        if (mine === seq.current) setBusy(false)
      }
    }, 300)
    return () => window.clearTimeout(id)
  }, [bridge, typeFilter, text])

  const events = result?.events ?? []

  return (
    <section className="card audit-stream">
      <div className="audit-head">
        <h2>事件流 · 台账检索</h2>
        <span className="audit-meta">
          {result ? `命中 ${result.returned} / 扫描 ${result.scanned}${result.truncated ? '（近 5000 条）' : ''}` : '读取中…'}
        </span>
      </div>
      <p className="monitor-note">
        本地 JSONL 台账是唯一权威 · 最新在前 · 类型、通道、数值都参与搜索
      </p>

      <div className="audit-controls">
        <div className="audit-chips">
          {TYPE_FILTERS.map((f) => (
            <button
              key={f.id || 'all'}
              type="button"
              className={`audit-chip ${typeFilter === f.id ? 'on' : ''}`}
              onClick={() => setTypeFilter(f.id)}
            >
              {f.label}
            </button>
          ))}
        </div>
        <input
          id="audit-search"
          data-testid="audit-search"
          className="audit-search"
          type="search"
          value={text}
          placeholder="搜索：alarm / SpO2 / 93 / signal.stall"
          onChange={(e) => setText(e.target.value)}
        />
      </div>

      {error && <p className="audit-empty">查询失败：{error}</p>}
      {!error && events.length === 0 && (
        <p className="audit-empty">{busy ? '检索中…' : '没有匹配的事件'}</p>
      )}

      {events.map((ev, i) => (
        <div key={`${ev.ts ?? i}-${ev.type ?? ''}-${i}`} className={`audit-row ${toneOf(ev)}`}>
          <span className="audit-time">{timeOf(ev)}</span>
          <span className="audit-type">{(ev.type ?? '?').replace(/\./g, ' · ')}</span>
          <span className="audit-desc">{describe(ev)}</span>
        </div>
      ))}
    </section>
  )
}
