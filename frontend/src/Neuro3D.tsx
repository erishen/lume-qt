import { useEffect, useRef, useState } from 'react'
import * as THREE from 'three'
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js'
import type { Bridge, Intervention } from './qwebchannel'
import { observeInViewport } from './framePacing'

/**
 * Neuro3D — 三维神经脉络 + 实时神经介入手术可视化（Three.js / WebGL）。
 *
 * 基础层：程序化神经网络（神经元点云 + 突触线段），脉冲沿突触传导，与 EEG 棘波联动。
 * 手术层：脑区靶点（STN/GPi/M1/病灶）+ 介入工具（DBS 电极 / TMS 线圈 / ECT 贴片 / NF），
 *         随 interventionReady 事件切换显示，强度/频率驱动刺激动画。
 */

/** 当前激活的介入状态（驱动工具动画 + UI 覆盖层） */
interface ActiveIntervention {
  type: string      // 小写工具名：dbs/tms/ect/nf
  label: string     // 显示名：DBS/TMS/ECT/NF
  intensity: number // 30-100
  frequency: number // 1-130
  until: number     // performance.now() 毫秒，到期隐藏
  enterAt: number   // 入场时间（scale 弹入）
  phase: 'active' | 'done' // 刺激中 / 已完成
}

interface ToolState {
  group: THREE.Group
  update: (dt: number, iv: ActiveIntervention) => void
}

/** 可清理资源集合 */
interface Disposable { dispose(): void }

export default function Neuro3D({ bridge }: { bridge: Bridge | null }) {
  const mountRef = useRef<HTMLDivElement | null>(null)
  const [status, setStatus] = useState<ActiveIntervention | null>(null)

  useEffect(() => {
    const mount = mountRef.current
    if (!mount) return

    // ------------------------------------------------------------------
    // 场景 / 相机 / 渲染器
    // ------------------------------------------------------------------
    const scene = new THREE.Scene()
    scene.background = new THREE.Color(0x060a12)
    scene.fog = new THREE.Fog(0x060a12, 12, 28)

    const camera = new THREE.PerspectiveCamera(55, mount.clientWidth / mount.clientHeight, 0.1, 100)
    camera.position.set(6, 5, 9)

    const renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true })
    renderer.setSize(mount.clientWidth, mount.clientHeight)
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, 1.5))
    mount.appendChild(renderer.domElement)

    const controls = new OrbitControls(camera, renderer.domElement)
    controls.enableDamping = true
    controls.dampingFactor = 0.08
    controls.autoRotate = true
    controls.autoRotateSpeed = 1.2

    // 清理资源收集
    const disposables: Disposable[] = []
    const track = <T extends Disposable>(d: T): T => { disposables.push(d); return d }

    // ------------------------------------------------------------------
    // 程序化生成神经网络（神经元 + 突触）
    // ------------------------------------------------------------------
    // 压力模式：?stress=1 增加神经元/脉冲密度（模拟大数据量渲染）
    const stress = new URLSearchParams(window.location.search).get('stress') === '1'
    const neuronCount = stress ? 200 : 90
    const positions = new Float32Array(neuronCount * 3)
    const neuronData: THREE.Vector3[] = []

    // 布局：大脑（椭球）+ 脑干 + 脊髓（向下延伸），符合解剖学轮廓
    for (let i = 0; i < neuronCount; i++) {
      let x: number, y: number, z: number
      const r = Math.random()
      if (r < 0.72) {
        // 大脑：扁椭球
        const a = 2.6, b = 2.0, c = 2.2
        const u = Math.random() * Math.PI * 2
        const v = Math.acos(2 * Math.random() - 1)
        x = a * Math.sin(v) * Math.cos(u)
        y = b * Math.cos(v) * 0.7 + 1.4 // 上移，脑位于上方
        z = c * Math.sin(v) * Math.sin(u)
      } else if (r < 0.86) {
        // 脑干：细圆柱
        const u = Math.random() * Math.PI * 2
        const t = Math.random()
        x = Math.cos(u) * 0.5
        y = -0.2 - t * 1.6
        z = Math.sin(u) * 0.5
      } else {
        // 脊髓：向下延伸的细柱
        const u = Math.random() * Math.PI * 2
        const t = Math.random()
        x = Math.cos(u) * 0.35
        y = -1.8 - t * 3.2
        z = Math.sin(u) * 0.35
      }
      positions[i * 3] = x
      positions[i * 3 + 1] = y
      positions[i * 3 + 2] = z
      neuronData.push(new THREE.Vector3(x, y, z))
    }

    // 神经元点云
    const neuronGeom = track(new THREE.BufferGeometry())
    neuronGeom.setAttribute('position', new THREE.BufferAttribute(positions, 3))
    const neuronMat = track(new THREE.PointsMaterial({
      color: 0x5ee7df,
      size: 0.24,
      transparent: true,
      opacity: 1.0,
    }))
    scene.add(new THREE.Points(neuronGeom, neuronMat))

    // 突触连接：每个神经元连到最近 2 个邻居
    const edgePositions: number[] = []
    for (let i = 0; i < neuronCount; i++) {
      const dists = neuronData
        .map((p, j) => ({ j, d: p.distanceToSquared(neuronData[i]) }))
        .filter((o) => o.j !== i)
        .sort((a, b) => a.d - b.d)
      for (const near of dists.slice(0, 2)) {
        const j = near.j
        if (j < i) continue
        if (near.d > 6.5) continue
        edgePositions.push(
          neuronData[i].x, neuronData[i].y, neuronData[i].z,
          neuronData[j].x, neuronData[j].y, neuronData[j].z,
        )
      }
    }
    const edgeGeom = track(new THREE.BufferGeometry())
    edgeGeom.setAttribute('position', new THREE.Float32BufferAttribute(edgePositions, 3))
    const edgeMat = track(new THREE.LineBasicMaterial({
      color: 0x4fa8d8,
      transparent: true,
      opacity: 0.85,
    }))
    scene.add(new THREE.LineSegments(edgeGeom, edgeMat))

    // ------------------------------------------------------------------
    // 神经脉冲系统（沿突触移动的发光点）
    // ------------------------------------------------------------------
    const pulseCount = stress ? 150 : 60
    const pulsePos = new Float32Array(pulseCount * 3)
    const pulseAlive = new Float32Array(pulseCount).fill(0)
    const pulsePath: number[][] = Array.from({ length: pulseCount }, () => [])
    const pulseSeg = new Float32Array(pulseCount).fill(0)
    const pulseT = new Float32Array(pulseCount).fill(0)
    let pulsesAlive = 0

    const pulseGeom = track(new THREE.BufferGeometry())
    pulseGeom.setAttribute('position', new THREE.BufferAttribute(pulsePos, 3))
    const pulseMat = track(new THREE.PointsMaterial({
      color: 0xffd166,
      size: 0.32,
      transparent: true,
      opacity: 0,
      blending: THREE.AdditiveBlending,
      depthWrite: false,
    }))
    scene.add(new THREE.Points(pulseGeom, pulseMat))

    // 邻接表（i → [j...]）
    const adj: number[][] = Array.from({ length: neuronCount }, () => [])
    for (let i = 0; i < neuronCount; i++) {
      const dists = neuronData
        .map((p, j) => ({ j, d: p.distanceToSquared(neuronData[i]) }))
        .filter((o) => o.j !== i)
        .sort((a, b) => a.d - b.d)
      for (const near of dists.slice(0, 2)) {
        if (near.d > 6.5) continue
        adj[i].push(near.j)
      }
    }

    /** 从给定神经元发射一个脉冲 */
    const firePulse = (from: number) => {
      for (let p = 0; p < pulseCount; p++) {
        if (!pulseAlive[p]) {
          pulseAlive[p] = 1
          pulsesAlive += 1
          pulsePath[p] = [from]
          const nbrs = adj[from]
          if (nbrs.length > 0) {
            pulsePath[p].push(nbrs[Math.floor(Math.random() * nbrs.length)])
          }
          pulseSeg[p] = 0
          pulseT[p] = 0
          break
        }
      }
    }

    // 初始随机发射几个脉冲
    for (let k = 0; k < 8; k++) {
      firePulse(Math.floor(Math.random() * neuronCount))
    }

    // ------------------------------------------------------------------
    // 脑区靶点标记（STN/GPi/M1/病灶）
    // ------------------------------------------------------------------
    const regions = [
      { pos: new THREE.Vector3(0.7, 1.0, 0.4), color: 0x8be9fd, label: 'STN 丘脑底核', r: 0.35 },
      { pos: new THREE.Vector3(-0.9, 1.1, 0.2), color: 0x8be9fd, label: 'GPi 苍白球', r: 0.3 },
      { pos: new THREE.Vector3(1.2, 2.9, -1.2), color: 0xb794f6, label: 'M1 运动皮层', r: 0.3 },
      { pos: new THREE.Vector3(0, 1.6, 0.8), color: 0xff6b9d, label: '病灶区', r: 0.5 },
    ]
    const makeLabel = (text: string, hex: string) => {
      const c = document.createElement('canvas')
      c.width = 256; c.height = 64
      const ctx = c.getContext('2d')
      if (ctx) {
        ctx.font = 'bold 26px system-ui, -apple-system, sans-serif'
        ctx.fillStyle = hex
        ctx.textAlign = 'center'
        ctx.textBaseline = 'middle'
        ctx.fillText(text, 128, 32)
      }
      const tex = track(new THREE.CanvasTexture(c))
      const spriteMat = track(new THREE.SpriteMaterial({ map: tex, transparent: true, opacity: 0.9, depthTest: false }))
      const sprite = new THREE.Sprite(spriteMat)
      sprite.scale.set(2.0, 0.5, 1)
      return sprite
    }
    for (const rg of regions) {
      const ball = new THREE.Mesh(
        track(new THREE.SphereGeometry(rg.r, 20, 20)),
        track(new THREE.MeshBasicMaterial({ color: rg.color, transparent: true, opacity: 0.16, depthWrite: false })),
      )
      ball.position.copy(rg.pos)
      scene.add(ball)
      const sprite = makeLabel(rg.label, '#' + rg.color.toString(16).padStart(6, '0'))
      sprite.position.copy(rg.pos)
      sprite.position.y += rg.r + 0.28
      scene.add(sprite)
    }

    // ------------------------------------------------------------------
    // 介入工具：DBS 电极
    // ------------------------------------------------------------------
    const dbsTop = new THREE.Vector3(0.7, 4.6, 0.4)   // 颅顶入口
    const dbsTip = new THREE.Vector3(0.7, 1.0, 0.4)   // STN 靶点
    const createDbsTool = (): ToolState => {
      const group = new THREE.Group()
      const dir = new THREE.Vector3().subVectors(dbsTip, dbsTop)
      const len = dir.length()
      const yAxis = new THREE.Vector3(0, 1, 0)
      const wireQuat = new THREE.Quaternion().setFromUnitVectors(yAxis, dir.clone().normalize())

      const wire = new THREE.Mesh(
        track(new THREE.CylinderGeometry(0.035, 0.035, len, 8)),
        track(new THREE.MeshBasicMaterial({ color: 0xc8ccd8 })),
      )
      wire.position.copy(dbsTop).add(dbsTip).multiplyScalar(0.5)
      wire.quaternion.copy(wireQuat)
      group.add(wire)

      // 4 个电极触点环
      const contacts: THREE.Mesh[] = []
      for (let i = 0; i < 4; i++) {
        const t = 1 - i * 0.09
        const p = new THREE.Vector3().lerpVectors(dbsTop, dbsTip, t)
        const ring = new THREE.Mesh(
          track(new THREE.TorusGeometry(0.09, 0.02, 8, 20)),
          track(new THREE.MeshBasicMaterial({ color: 0x8be9fd, transparent: true, opacity: 0.9 })),
        )
        ring.position.copy(p)
        ring.quaternion.copy(wireQuat)
        contacts.push(ring)
        group.add(ring)
      }

      // 电流粒子（沿导线下行到靶点）
      const curCount = 20
      const curPos = new Float32Array(curCount * 3)
      const curGeom = track(new THREE.BufferGeometry())
      curGeom.setAttribute('position', new THREE.BufferAttribute(curPos, 3))
      const curMat = track(new THREE.PointsMaterial({
        color: 0xffd166, size: 0.16, transparent: true, opacity: 0,
        blending: THREE.AdditiveBlending, depthWrite: false,
      }))
      group.add(new THREE.Points(curGeom, curMat))
      const curT = new Float32Array(curCount)
      for (let i = 0; i < curCount; i++) curT[i] = Math.random()
      const tmp = new THREE.Vector3()

      group.visible = false
      scene.add(group)
      return {
        group,
        update(dt, iv) {
          const speed = 0.6 + (iv.frequency / 130) * 2.4
          for (let i = 0; i < curCount; i++) {
            curT[i] += dt * speed * (0.85 + (i % 5) * 0.09)
            if (curT[i] >= 1) curT[i] -= 1
            tmp.lerpVectors(dbsTop, dbsTip, curT[i])
            curPos[i * 3] = tmp.x
            curPos[i * 3 + 1] = tmp.y
            curPos[i * 3 + 2] = tmp.z
          }
          curGeom.attributes.position.needsUpdate = true
          curMat.opacity = 0.35 + (iv.intensity / 100) * 0.55
          // 触点脉冲发光
          const glow = (Math.sin(performance.now() * 0.012 * iv.frequency) * 0.5 + 0.5) * (iv.intensity / 100)
          for (const c of contacts) {
            ;(c.material as THREE.MeshBasicMaterial).opacity = 0.35 + glow * 0.65
          }
        },
      }
    }

    // ------------------------------------------------------------------
    // 介入工具：TMS 8字线圈
    // ------------------------------------------------------------------
    const tmsCenter = new THREE.Vector3(1.2, 3.9, -1.2) // M1 皮层上方
    const createTmsTool = (): ToolState => {
      const group = new THREE.Group()
      const coilMat = track(new THREE.MeshBasicMaterial({ color: 0xd0a9e8, transparent: true, opacity: 0.95 }))
      const ring1 = new THREE.Mesh(track(new THREE.TorusGeometry(0.4, 0.07, 12, 30)), coilMat)
      ring1.position.set(-0.38, 0, 0)
      ring1.rotation.x = Math.PI / 2
      const ring2 = new THREE.Mesh(track(new THREE.TorusGeometry(0.4, 0.07, 12, 30)), coilMat)
      ring2.position.set(0.38, 0, 0)
      ring2.rotation.x = Math.PI / 2
      group.add(ring1, ring2)

      // 磁脉冲粒子（从线圈向下扩散到皮层）
      const magCount = 30
      const magPos = new Float32Array(magCount * 3)
      const magGeom = track(new THREE.BufferGeometry())
      magGeom.setAttribute('position', new THREE.BufferAttribute(magPos, 3))
      const magMat = track(new THREE.PointsMaterial({
        color: 0xb794f6, size: 0.18, transparent: true, opacity: 0,
        blending: THREE.AdditiveBlending, depthWrite: false,
      }))
      group.add(new THREE.Points(magGeom, magMat))
      const magT = new Float32Array(magCount)
      const magAng = new Float32Array(magCount)
      for (let i = 0; i < magCount; i++) {
        magT[i] = Math.random()
        magAng[i] = Math.random() * Math.PI * 2
      }

      group.position.copy(tmsCenter)
      group.visible = false
      scene.add(group)
      return {
        group,
        update(dt, iv) {
          const speed = 0.7 + (iv.frequency / 130) * 2.2
          for (let i = 0; i < magCount; i++) {
            magT[i] += dt * speed * (0.9 + (i % 4) * 0.08)
            if (magT[i] >= 1) magT[i] -= 1
            const r = magT[i] * 1.7
            magPos[i * 3] = Math.cos(magAng[i]) * r * 0.85
            magPos[i * 3 + 1] = -magT[i] * 0.7
            magPos[i * 3 + 2] = Math.sin(magAng[i]) * r * 0.85
          }
          magGeom.attributes.position.needsUpdate = true
          magMat.opacity = 0.3 + (iv.intensity / 100) * 0.6
          coilMat.opacity = 0.5 + (Math.sin(performance.now() * 0.01 * iv.frequency) * 0.5 + 0.5) * 0.5
        },
      }
    }

    // ------------------------------------------------------------------
    // 介入工具：ECT 双侧颞部贴片
    // ------------------------------------------------------------------
    const createEctTool = (): ToolState => {
      const group = new THREE.Group()
      const padMat = track(new THREE.MeshBasicMaterial({ color: 0xff8a65, transparent: true, opacity: 0.95 }))
      const padGeo = track(new THREE.SphereGeometry(0.35, 16, 16, 0, Math.PI * 2, 0, Math.PI / 2))
      const pad1 = new THREE.Mesh(padGeo, padMat)
      pad1.position.set(-2.3, 2.4, 0)
      pad1.rotation.z = Math.PI / 2
      const pad2 = new THREE.Mesh(padGeo, padMat)
      pad2.position.set(2.3, 2.4, 0)
      pad2.rotation.z = -Math.PI / 2
      group.add(pad1, pad2)

      // 电弧粒子（两贴片间闪烁）
      const arcCount = 24
      const arcPos = new Float32Array(arcCount * 3)
      const arcGeom = track(new THREE.BufferGeometry())
      arcGeom.setAttribute('position', new THREE.BufferAttribute(arcPos, 3))
      const arcMat = track(new THREE.PointsMaterial({
        color: 0xffe066, size: 0.18, transparent: true, opacity: 0,
        blending: THREE.AdditiveBlending, depthWrite: false,
      }))
      group.add(new THREE.Points(arcGeom, arcMat))
      const arcT = new Float32Array(arcCount)
      for (let i = 0; i < arcCount; i++) arcT[i] = Math.random()

      group.visible = false
      scene.add(group)
      return {
        group,
        update(dt, iv) {
          const speed = 0.5 + (iv.frequency / 130) * 2.5
          for (let i = 0; i < arcCount; i++) {
            arcT[i] += dt * speed
            if (arcT[i] >= 1) arcT[i] -= 1
            const arc = Math.sin(arcT[i] * Math.PI)
            arcPos[i * 3] = -2.3 + arcT[i] * 4.6
            arcPos[i * 3 + 1] = 2.4 + arc * 0.35 + (Math.random() - 0.5) * 0.3
            arcPos[i * 3 + 2] = (Math.random() - 0.5) * 0.9
          }
          arcGeom.attributes.position.needsUpdate = true
          arcMat.opacity = 0.3 + (iv.intensity / 100) * 0.6
          padMat.opacity = 0.5 + (Math.sin(performance.now() * 0.012 * iv.frequency) * 0.5 + 0.5) * 0.5
        },
      }
    }

    // ------------------------------------------------------------------
    // 介入工具：NF 神经反馈（病灶呼吸高亮 + 汇聚脉冲）
    // ------------------------------------------------------------------
    const createNfTool = (): ToolState => {
      const group = new THREE.Group()
      const target = new THREE.Vector3(0, 1.6, 0.8) // 病灶区
      const ballMat = track(new THREE.MeshBasicMaterial({
        color: 0xff6b9d, transparent: true, opacity: 0.3,
        depthWrite: false, blending: THREE.AdditiveBlending,
      }))
      const ball = new THREE.Mesh(track(new THREE.SphereGeometry(0.55, 24, 24)), ballMat)
      ball.position.copy(target)
      group.add(ball)

      const convCount = 24
      const convPos = new Float32Array(convCount * 3)
      const convGeom = track(new THREE.BufferGeometry())
      convGeom.setAttribute('position', new THREE.BufferAttribute(convPos, 3))
      const convMat = track(new THREE.PointsMaterial({
        color: 0xff9ecd, size: 0.18, transparent: true, opacity: 0,
        blending: THREE.AdditiveBlending, depthWrite: false,
      }))
      group.add(new THREE.Points(convGeom, convMat))
      const convT = new Float32Array(convCount)
      const convAng = new Float32Array(convCount)
      for (let i = 0; i < convCount; i++) {
        convT[i] = Math.random()
        convAng[i] = Math.random() * Math.PI * 2
      }

      group.visible = false
      scene.add(group)
      return {
        group,
        update(dt, iv) {
          const speed = 0.5 + (iv.frequency / 130) * 1.8
          for (let i = 0; i < convCount; i++) {
            convT[i] += dt * speed
            if (convT[i] >= 1) convT[i] -= 1
            const r = 2.4 * (1 - convT[i])
            convPos[i * 3] = target.x + Math.cos(convAng[i]) * r
            convPos[i * 3 + 1] = target.y + Math.sin(convAng[i] * 1.7) * r * 0.75
            convPos[i * 3 + 2] = target.z + Math.sin(convAng[i]) * r
          }
          convGeom.attributes.position.needsUpdate = true
          convMat.opacity = 0.3 + (iv.intensity / 100) * 0.5
          ballMat.opacity = 0.2 +
            (Math.sin(performance.now() * 0.004 * iv.frequency) * 0.5 + 0.5) * 0.3 * (iv.intensity / 100)
        },
      }
    }

    // ------------------------------------------------------------------
    // 工具注册 + 介入事件处理
    // ------------------------------------------------------------------
    const tools: Record<string, ToolState> = {
      dbs: createDbsTool(),
      tms: createTmsTool(),
      ect: createEctTool(),
      nf: createNfTool(),
    }

    const toolEntries = Object.entries(tools)

    let activeTool: ActiveIntervention | null = null

    const onIv = (iv: Intervention) => {
      const type = iv.type.toLowerCase()
      if (!tools[type]) return
      activeTool = {
        type,
        label: iv.label,
        intensity: iv.intensity,
        frequency: iv.frequency,
        until: performance.now() + (iv.status === 'active' ? 4500 : 1800),
        enterAt: performance.now(),
        phase: iv.status === 'active' ? 'active' : 'done',
      }
      setStatus({ ...activeTool })
    }

    // 订阅 C++ 推送；无桥接时 JS 定时演示
    let unsubEeg: (() => void) | null = null
    let unsubIv: (() => void) | null = null
    let jsIvTimer: number | undefined

    const onEeg = (v: number) => {
      if (Math.abs(v) > 0.6) {
        const n = 1 + Math.floor(Math.random() * 3)
        for (let k = 0; k < n; k++) firePulse(Math.floor(Math.random() * neuronCount))
      }
    }
    if (bridge) {
      bridge.eegSample.connect(onEeg)
      unsubEeg = () => bridge.eegSample.disconnect(onEeg)
      bridge.interventionReady.connect(onIv)
      unsubIv = () => bridge.interventionReady.disconnect(onIv)
    } else {
      // 纯浏览器演示：随机介入事件 + 模拟 EEG 棘波
      jsIvTimer = window.setInterval(() => {
        const labels = ['TMS', 'DBS', 'ECT', 'NF']
        const label = labels[Math.floor(Math.random() * labels.length)]
        onIv({
          type: label,
          label,
          intensity: Math.round(30 + Math.random() * 70),
          frequency: Math.round(1 + Math.random() * 129),
          duration: Math.round(5 + Math.random() * 55),
          status: Math.random() < 0.75 ? 'active' : 'completed',
          time: new Date().toLocaleTimeString('zh-CN', { hour12: false }),
        })
      }, stress ? 300 : 2600)
    }

    // ------------------------------------------------------------------
    // 动画循环
    // ------------------------------------------------------------------
    let raf = 0
    const timer = new THREE.Timer()
    const pulseTmp = new THREE.Vector3()
    // 3D 面板在页面下方，滚出视口或窗口在后台时停止整棵场景的更新与渲染
    let inView = false
    const stopObserve = observeInViewport(mount, (v) => { inView = v })

    const animate = () => {
      raf = requestAnimationFrame(animate)
      if (!inView || document.hidden) return
      timer.update()
      const dt = Math.min(timer.getDelta(), 0.1)
      const now = performance.now()

      // 更新神经脉冲
      for (let p = 0; p < pulseCount; p++) {
        if (!pulseAlive[p]) {
          pulsePos[p * 3 + 1] = -999
          continue
        }
        const path = pulsePath[p]
        const cur = path[pulseSeg[p]]
        if (adj[cur].length > 0) {
          pulseT[p] += dt * 2.2
          if (pulseT[p] >= 1) {
            pulseT[p] = 0
            pulseSeg[p]++
            const next = adj[cur][Math.floor(Math.random() * adj[cur].length)]
            path.push(next)
          }
          const a = neuronData[path[pulseSeg[p]]]
          const b = neuronData[path[Math.min(pulseSeg[p] + 1, path.length - 1)]]
          pulseTmp.lerpVectors(a, b, pulseT[p])
          pulsePos[p * 3] = pulseTmp.x
          pulsePos[p * 3 + 1] = pulseTmp.y
          pulsePos[p * 3 + 2] = pulseTmp.z
        } else {
          pulseAlive[p] = 0
          pulsesAlive -= 1
          pulsePos[p * 3 + 1] = -999
        }
      }
      pulseGeom.attributes.position.needsUpdate = true
      const active = pulsesAlive > 0
      pulseMat.opacity += ((active ? 0.9 : 0) - pulseMat.opacity) * 0.2

      // 更新介入工具
      for (const [key, tool] of toolEntries) {
        const isActive = activeTool?.type === key && now < activeTool.until
        if (isActive) {
          tool.group.visible = true
          // 入场 scale 弹入
          const enter = Math.min(1, (now - activeTool!.enterAt) / 280)
          tool.group.scale.setScalar(0.6 + 0.4 * (1 - Math.pow(1 - enter, 3)))
          tool.update(dt, activeTool!)
        } else {
          tool.group.visible = false
        }
      }
      // 介入事件超时后清空 UI 状态
      if (activeTool && now >= activeTool.until) {
        activeTool = null
        setStatus(null)
      }

      controls.update()
      renderer.render(scene, camera)
    }
    animate()

    // ------------------------------------------------------------------
    // 窗口尺寸自适应
    // ------------------------------------------------------------------
    const onResize = () => {
      const w = mount.clientWidth
      const h = mount.clientHeight
      camera.aspect = w / h
      camera.updateProjectionMatrix()
      renderer.setSize(w, h)
    }
    const ro = new ResizeObserver(onResize)
    ro.observe(mount)

    // ------------------------------------------------------------------
    // 清理
    // ------------------------------------------------------------------
    return () => {
      cancelAnimationFrame(raf)
      stopObserve()
      ro.disconnect()
      if (unsubEeg) unsubEeg()
      if (unsubIv) unsubIv()
      if (jsIvTimer !== undefined) clearInterval(jsIvTimer)
      controls.dispose()
      for (const d of disposables) d.dispose()
      renderer.dispose()
      mount.removeChild(renderer.domElement)
    }
  }, [bridge])

  return (
    <div className="neuro-3d-stage">
      <div ref={mountRef} className="neuro-3d" />
      {status && (
        <div className={`neuro-3d-status ns-${status.type}`}>
          <span className="ns-badge">{status.label}</span>
          <span className="ns-meta">强度 {status.intensity}% · {status.frequency}Hz</span>
          <span className="ns-state">
            {status.phase === 'active' ? '● 刺激中' : '○ 完成'}
          </span>
        </div>
      )}
    </div>
  )
}
