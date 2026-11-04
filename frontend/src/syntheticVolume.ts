import type { DicomVolumeMeta } from './qwebchannel'

/** Uint8Array → base64（避免中文/大数组时的 stack overflow，分块处理） */
export function bytesToBase64(bytes: Uint8Array): string {
  let bin = ''
  const chunk = 0x8000
  for (let i = 0; i < bytes.length; i += chunk) {
    bin += String.fromCharCode(...bytes.subarray(i, i + chunk))
  }
  return btoa(bin)
}

/**
 * 合成一个类脑部体积（球壳分层 + 脑回噪声），
 * 用于无 Qt 桥接（纯浏览器预览）时演示 DICOM 体渲染。
 * 形状：扁椭球（符合解剖学轮廓） + 皮层高亮 + 中线结构。
 */
export function createSyntheticBrain(dim = 96): { base64: string; meta: DicomVolumeMeta } {
  const data = new Uint8Array(dim * dim * dim)
  const cx = dim / 2
  const cy = dim / 2
  const cz = dim / 2
  const rx = dim * 0.33
  const ry = dim * 0.40
  const rz = dim * 0.33

  for (let z = 0; z < dim; z++) {
    for (let y = 0; y < dim; y++) {
      for (let x = 0; x < dim; x++) {
        const dx = (x - cx) / rx
        const dy = (y - cy) / ry
        const dz = (z - cz) / rz
        const r = Math.sqrt(dx * dx + dy * dy + dz * dz)

        let v = 0
        if (r < 1) {
          // 白质/灰质分层 + 脑回褶皱噪声
          v = 55 + 55 * Math.cos(r * 2.6)
          v += 46 * Math.sin(x * 0.40) * Math.sin(y * 0.36) * Math.sin(z * 0.31)
          v += 26 * Math.sin(dx * 6.0) * Math.cos(dz * 6.0) * (1 - r)
        }
        if (r > 0.82 && r < 1.02) v = 195 // 皮层高亮
        if (Math.abs(dx) < 0.06 && r < 0.76) v = 150 // 大脑纵裂
        if (Math.abs(dy) < 0.045 && r < 0.72) v = 160 // 胼胝体走向
        if (r > 1.08) v = 6 // 颅骨外低值

        data[(z * dim + y) * dim + x] = Math.max(0, Math.min(255, v))
      }
    }
  }

  return {
    base64: bytesToBase64(data),
    meta: {
      dims: [dim, dim, dim],
      spacing: [1, 1, 1],
      min: 0,
      max: 255,
      slices: dim,
      source: 'synthetic',
    },
  }
}