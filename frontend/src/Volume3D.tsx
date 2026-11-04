import { useEffect, useRef, useState } from 'react'
import * as THREE from 'three'
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js'
import type { DicomVolumeMeta } from './qwebchannel'

/**
 * Volume3D — DICOM 体积体渲染（Three.js / WebGL2）。
 *
 * 输入：uint8 体积（dims³）+ 元数据。用 THREE.Data3DTexture + ray marching
 * 渲染真实解剖结构（脑部 MR/CT），支持窗宽窗位调节与 OrbitControls 交互。
 */

interface VolumeInput {
  base64: string
  meta: DicomVolumeMeta
}

const VERT = /* glsl */ `precision highp float;
in vec3 position;
uniform mat4 projectionMatrix;
uniform mat4 modelViewMatrix;
uniform mat4 modelMatrix;
out vec3 vWorldPos;
void main() {
  vWorldPos = (modelMatrix * vec4(position, 1.0)).xyz;
  gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
}
`

const FRAG = /* glsl */ `precision highp float;
precision highp sampler3D;
in vec3 vWorldPos;
out vec4 fragColor;

uniform sampler3D uVolume;
uniform vec3 uCameraPos;
uniform vec3 uBoxCenter;
uniform float uSampleCount;
uniform float uAlphaScale;
uniform vec2 uWindow;     // x=lo, y=hi（归一化灰度窗）

// 注意：参数名不可用 half —— 它是 GLSL ES 的保留字，
// 会导致片元着色器编译失败（表现为 WebGL 面板整块空白）。
vec2 hitBox(vec3 orig, vec3 dir, vec3 center, float halfSize) {
  vec3 inv = 1.0 / dir;
  vec3 t0 = (center - halfSize - orig) * inv;
  vec3 t1 = (center + halfSize - orig) * inv;
  vec3 tn = min(t0, t1);
  vec3 tx = max(t0, t1);
  float tN = max(max(tn.x, tn.y), tn.z);
  float tF = min(min(tx.x, tx.y), tx.z);
  return vec2(max(tN, 0.0), tF);
}

// 返回标量（漫反射强度），不可声明为 vec3——与 return 表达式类型不匹配
// 同样会导致着色器编译失败。
float shade(vec3 p, vec3 rd) {
  // 简单有限差分梯度光照
  float e = 0.006;
  float vx = texture(uVolume, p + vec3(e,0,0)).r - texture(uVolume, p - vec3(e,0,0)).r;
  float vy = texture(uVolume, p + vec3(0,e,0)).r - texture(uVolume, p - vec3(0,e,0)).r;
  float vz = texture(uVolume, p + vec3(0,0,e)).r - texture(uVolume, p - vec3(0,0,e)).r;
  vec3 grad = normalize(vec3(vx, vy, vz) + 1e-5);
  return max(dot(grad, -rd), 0.0);
}

void main() {
  vec3 rd = normalize(vWorldPos - uCameraPos);
  vec2 box = hitBox(uCameraPos, rd, uBoxCenter, 0.5);
  vec3 bg = mix(vec3(0.015, 0.03, 0.05), vec3(0.05, 0.09, 0.15), smoothstep(0.0, 1.8, length(vWorldPos)));
  if (box.y <= box.x) { fragColor = vec4(bg, 1.0); return; }

  float dist = box.x;
  float tStep = (box.y - box.x) / max(uSampleCount, 1.0);
  vec3 acc = vec3(0.0);
  float accA = 0.0;
  vec3 lo = uBoxCenter - vec3(0.5);

  for (int i = 0; i < 400; i++) {
    if (float(i) >= uSampleCount) break;
    vec3 p = uCameraPos + rd * dist;
    vec3 uvw = p - lo;
    if (uvw.x >= 0.0 && uvw.y >= 0.0 && uvw.z >= 0.0 &&
        uvw.x <= 1.0 && uvw.y <= 1.0 && uvw.z <= 1.0) {
      float v = texture(uVolume, uvw).r;
      float nv = clamp((v - uWindow.x) / max(uWindow.y - uWindow.x, 1e-4), 0.0, 1.0);
      if (nv > 0.02) {
        float diff = shade(uvw, rd);
        vec3 c = mix(vec3(0.30, 0.34, 0.42), vec3(0.88, 0.90, 0.95), nv);
        c *= 0.55 + 0.66 * diff;
        float alpha = nv * uAlphaScale;
        acc += (1.0 - accA) * alpha * c;
        accA += (1.0 - accA) * alpha;
        if (accA > 0.98) break;
      }
    }
    dist += tStep;
  }
  fragColor = vec4(acc + bg * (1.0 - accA), 1.0);
}
`

function base64ToBytes(b64: string): Uint8Array {
  const bin = atob(b64)
  const bytes = new Uint8Array(bin.length)
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i)
  return bytes
}

export default function Volume3D({ volume }: { volume: VolumeInput | null }) {
  const mountRef = useRef<HTMLDivElement | null>(null)
  // 跨 effect 的场景/相机/材质/渲染器状态
  const sceneRef = useRef<THREE.Scene | null>(null)
  const cameraRef = useRef<THREE.PerspectiveCamera | null>(null)
  const rendererRef = useRef<THREE.WebGLRenderer | null>(null)
  const boxMeshRef = useRef<THREE.Mesh | null>(null)
  const texRef = useRef<THREE.Data3DTexture | null>(null)
  const winUniformRef = useRef<{ lo: number; hi: number }>({ lo: 0.05, hi: 0.95 })
  const [wc, setWc] = useState(50)
  const [ww, setWw] = useState(100)

  // 场景初始化（一次）
  useEffect(() => {
    const mount = mountRef.current
    if (!mount) return

    // DICOM 体渲染依赖 WebGL2 的 sampler3D / GLSL3；缺 WebGL2 时明确提示，
    // 而非静默黑屏（否则用户只会看到“三维医学影像”标题下一片空白）。
    const probe = document.createElement('canvas')
    if (!probe.getContext('webgl2')) {
      mount.innerHTML =
        '<div class="vol-error">当前环境不支持 WebGL2，DICOM 三维体渲染需要 WebGL2 + 3D 纹理支持</div>'
      return
    }

    const scene = new THREE.Scene()
    scene.background = new THREE.Color(0x05080d)
    sceneRef.current = scene

    const camera = new THREE.PerspectiveCamera(50, mount.clientWidth / mount.clientHeight, 0.05, 100)
    camera.position.set(0, 0, 2.4)
    cameraRef.current = camera

    const canvas = document.createElement('canvas')
    canvas.style.width = '100%'
    canvas.style.height = '100%'
    canvas.style.display = 'block'
    canvas.style.background = '#02040a'
    canvas.style.borderRadius = '8px'
    mount.appendChild(canvas)

    // 显式请求 WebGL2 以使用 sampler3D / in-out 语法
    const glContext = canvas.getContext('webgl2', { antialias: true }) as WebGL2RenderingContext
    // Three.js WebGLRenderer 依据 context.version >= 2 判定是否 WebGL2，
    // 故需在创建 renderer 前标记 version，否则会按 GLSL ES 1.00（WebGL1）编译着色器
    if (glContext) {
      ;(glContext as any).version = 2
      ;(glContext as any).isWebGL2 = true
    }
    const renderer = new THREE.WebGLRenderer({ canvas, context: glContext, antialias: true })
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2))
    rendererRef.current = renderer

    const controls = new OrbitControls(camera, renderer.domElement)
    controls.enableDamping = true
    controls.dampingFactor = 0.1
    controls.autoRotate = true
    controls.autoRotateSpeed = 1.0
    controls.target.set(0, 0, 0)

    // 占位网格（体积加载前保持场景有内容）
    const boxGeo = new THREE.BoxGeometry(1, 1, 1)
    const placeholder = new THREE.Mesh(boxGeo, new THREE.MeshBasicMaterial({ color: 0x02040a }))
    boxMeshRef.current = placeholder
    scene.add(placeholder)

    let raf = 0
    const animate = () => {
      raf = requestAnimationFrame(animate)
      controls.update()
      // ray-marching 的射线原点必须跟随真实相机：OrbitControls（含
      // autoRotate）会持续移动相机，若 uCameraPos 不更新，体将被错误投影
      // 到视野外，表现为“标题在但三维不显示”。
      const mat = boxMeshRef.current?.material as THREE.RawShaderMaterial | undefined
      if (mat?.uniforms?.uCameraPos) {
        ;(mat.uniforms.uCameraPos.value as THREE.Vector3).copy(camera.position)
      }
      renderer.render(scene, camera)
    }
    animate()

    const onResize = () => {
      const w = mount.clientWidth
      const h = mount.clientHeight
      camera.aspect = w / h
      camera.updateProjectionMatrix()
      renderer.setSize(w, h)
    }
    const ro = new ResizeObserver(onResize)
    ro.observe(mount)
    onResize()

    return () => {
      cancelAnimationFrame(raf)
      ro.disconnect()
      controls.dispose()
      boxGeo.dispose()
      placeholder.material.dispose()
      renderer.dispose()
      if (canvas.parentElement === mount) mount.removeChild(canvas)
    }
  }, [])

  // 体积数据到达 → 构建 Data3DTexture + 替换为体渲染材质
  useEffect(() => {
    if (!volume) return
    const scene = sceneRef.current
    if (!scene) return

    const bytes = base64ToBytes(volume.base64)
    const d = volume.meta.dims[0]

    const tex = new THREE.Data3DTexture(bytes, d, d, d)
    tex.format = THREE.RedFormat
    tex.type = THREE.UnsignedByteType
    tex.minFilter = THREE.LinearFilter
    tex.magFilter = THREE.LinearFilter
    tex.needsUpdate = true
    texRef.current = tex

    const mat = new THREE.RawShaderMaterial({
      glslVersion: THREE.GLSL3,
      vertexShader: VERT,
      fragmentShader: FRAG,
      uniforms: {
        uVolume: { value: tex },
        uCameraPos: { value: cameraRef.current!.position.clone() },
        uBoxCenter: { value: new THREE.Vector3(0, 0, 0) },
        uSampleCount: { value: 140 },
        uAlphaScale: { value: 0.42 },
        uWindow: { value: new THREE.Vector2(winUniformRef.current.lo, winUniformRef.current.hi) },
      },
      side: THREE.DoubleSide,
      depthWrite: true,
    })

    const geo = new THREE.BoxGeometry(1, 1, 1)
    const mesh = new THREE.Mesh(geo, mat)

    // 移除占位网格
    if (boxMeshRef.current) {
      scene.remove(boxMeshRef.current)
      boxMeshRef.current = null
    }
    boxMeshRef.current = mesh
    scene.add(mesh)

    return () => {
      scene.remove(mesh)
      geo.dispose()
      mat.dispose()
      tex.dispose()
      texRef.current = null
    }
  }, [volume])

  // 窗宽窗位 → 更新材质 uniform
  useEffect(() => {
    if (!volume) return
    const lo = Math.max(0, (wc - ww / 2) / 100)
    const hi = Math.min(1, (wc + ww / 2) / 100)
    winUniformRef.current = { lo, hi }
    const mesh = boxMeshRef.current
    const mat = mesh?.material as THREE.RawShaderMaterial | undefined
    if (mat?.uniforms) {
      mat.uniforms.uWindow.value.set(lo, hi)
    }
  }, [wc, ww, volume])

  return (
    <div className="vol3d">
      <div ref={mountRef} className="vol3d-canvas" />
      {volume && (
        <div className="vol3d-controls">
          <label>
            窗位
            <input type="range" min={0} max={100} value={wc}
              onChange={(e) => setWc(Number(e.target.value))} />
            <em>{wc}%</em>
          </label>
          <label>
            窗宽
            <input type="range" min={1} max={200} value={ww}
              onChange={(e) => setWw(Number(e.target.value))} />
            <em>{ww}%</em>
          </label>
          <span className="vol3d-dims">
            {volume.meta.dims[0]}³ · {volume.meta.slices} 层 · {Math.round(volume.base64.length / 1024)}KB
          </span>
        </div>
      )}
    </div>
  )
}