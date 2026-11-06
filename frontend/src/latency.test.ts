import { describe, expect, it, vi } from 'vitest'
import type * as LatencyModule from './latency'

/**
 * latency.ts 的环形缓冲与通道表是模块级单例：每个用例重新导入一份模块，
 * 避免前一个用例记的样本漏到下一个。这里只测纯逻辑，不需要 jsdom。
 */
async function fresh(): Promise<typeof LatencyModule> {
  vi.resetModules()
  return import('./latency')
}

describe('latency 看板的事件型通道', () => {
  it('空快照也包含 alarm，但样本为 0（界面据此不显示该行）', async () => {
    const l = await fresh()
    const row = l.snapshot().find((r) => r.channel === 'alarm')
    expect(row).toBeDefined()
    expect(row?.samples).toBe(0)
  })

  it('报警单次耗时同时记到达与上屏，不再按发布时刻算年龄', async () => {
    const l = await fresh()
    l.recordSpan('alarm', 'receipt', 3.2)
    l.recordSpan('alarm', 'painted', 5.4)

    const row = l.snapshot().find((r) => r.channel === 'alarm')
    expect(row?.samples).toBe(1)
    expect(row?.receiptP50).toBeCloseTo(3.2, 5)
    expect(row?.paintedP50).toBeCloseTo(5.4, 5)
    expect(row?.receiptP99).toBeCloseTo(3.2, 5)
  })

  it('报警不参与中断判定：健康列表仍只有原来的 6 个持续通道', async () => {
    const l = await fresh()
    l.recordSpan('alarm', 'receipt', 3)
    const health = l.channelHealth()

    expect(health.some((h) => h.channel === 'alarm')).toBe(false)
    expect(health.map((h) => h.channel)).toEqual([
      'ecg',
      'eeg',
      'emg',
      'neural',
      'vitals',
      'volume',
    ])
  })

  it('非法耗时（负数 / NaN）不落样本', async () => {
    const l = await fresh()
    l.recordSpan('alarm', 'receipt', -1)
    l.recordSpan('alarm', 'receipt', Number.NaN)
    expect(l.snapshot().find((r) => r.channel === 'alarm')?.samples).toBe(0)
  })

  it('持续通道仍按 100 ms 预算判慢（报警的 200 ms 预算不串到别人）', async () => {
    const l = await fresh()
    l.recordSpan('ecg', 'receipt', 350)
    const ecg = l.channelHealth().find((h) => h.channel === 'ecg')
    expect(ecg?.state).toBe('slow')
    expect(ecg?.receiptP50).toBeCloseTo(350, 5)
  })
})
