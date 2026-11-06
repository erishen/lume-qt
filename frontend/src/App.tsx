import { useEffect, useState } from 'react'
import { connectBridge, type Bridge } from './qwebchannel'
import AuditStream from './AuditStream'
import MedicalMonitor from './MedicalMonitor'

/**
 * App — 演示 React 前端与 C++ 桥接的双向通信。
 *
 * 页面结构按术中信息优先级排：生命体征与波形在前，链路诊断与审计在后，
 * 桥接调试收进「开发者工具」，避免把工程细节摊在首屏。
 */
export default function App() {
  const [bridge, setBridge] = useState<Bridge | null>(null)
  const [platformInfo, setPlatformInfo] = useState<string>('连接中…')
  const [reply, setReply] = useState<string>('')
  const [pushed, setPushed] = useState<string>('')
  const [sum, setSum] = useState<number | null>(null)

  useEffect(() => {
    let disposed = false

    connectBridge().then(async (b) => {
      if (disposed) return
      if (!b) {
        setPlatformInfo('未运行于 Qt WebEngine（纯浏览器预览）')
        return
      }
      setBridge(b)
      setPlatformInfo(await b.getPlatformInfo())

      b.dataFromCpp.connect((payload) => {
        setPushed(`收到 C++ 推送 #${String(payload.seq)} @ ${String(payload.time)}: ${JSON.stringify(payload.echo)}`)
      })
    })

    return () => {
      disposed = true
    }
  }, [])

  async function handleShowMessage() {
    if (!bridge) return
    const text = `来自 React 的消息 @ ${new Date().toLocaleTimeString()}`
    const result = await bridge.showMessage(text)
    setReply(result)
  }

  async function handleAdd() {
    if (!bridge) return
    const value = await bridge.add(3, 4)
    setSum(value)
  }

  function handlePushDemo() {
    bridge?.setDataFromCpp({ source: 'react', value: 42 })
  }

  return (
    <div className="app">
      <header className="topbar">
        <div className="brand">
          <span className="brand-mark" aria-hidden="true">Q</span>
          <div className="brand-text">
            <h1>术中影像工作台</h1>
            <p>qt-react-shell · Qt WebEngine / C++ / React</p>
          </div>
        </div>
        <div className="topbar-right">
          <span className={`conn-pill ${bridge ? 'ok' : 'idle'}`}>
            <i aria-hidden="true" />
            {bridge ? '桥接在线' : '桥接离线'}
          </span>
          <span className="app-name">{bridge?.appName ?? '纯浏览器预览'}</span>
        </div>
      </header>

      <main className="content">
        <MedicalMonitor bridge={bridge} />

        <AuditStream bridge={bridge} />

        <section className="devtools">
          <div className="section-head compact">
            <div>
              <span className="eyebrow">DEVELOPER</span>
              <h2>开发者工具</h2>
            </div>
            <span className="section-meta">桥接与推送验证</span>
          </div>
          <div className="dev-grid">
            <section className="card dev-card">
              <h3>桥接状态</h3>
              <dl>
                <dt>平台信息</dt>
                <dd>{platformInfo}</dd>
                <dt>App 名称</dt>
                <dd>{bridge?.appName ?? '—'}</dd>
              </dl>
            </section>

            <section className="card dev-card">
              <h3>JS → C++ 调用</h3>
              <div className="row">
                <button onClick={handleShowMessage} disabled={!bridge}>
                  showMessage()
                </button>
                <button onClick={handleAdd} disabled={!bridge}>
                  add(3, 4)
                </button>
              </div>
              {reply && <p className="result">{reply}</p>}
              {sum !== null && <p className="result">3 + 4 = {sum}</p>}
            </section>

            <section className="card dev-card">
              <h3>C++ → JS 推送</h3>
              <button onClick={handlePushDemo} disabled={!bridge}>
                触发 dataFromCpp
              </button>
              {pushed && <p className="result">{pushed}</p>}
            </section>
          </div>
        </section>
      </main>

      <footer className="statusbar">
        <span>{bridge ? '已连接 Qt C++ 桥接' : '未连接桥接'}</span>
        <span className="statusbar-meta">本地审计为权威 · Lume 为跨端事件镜像</span>
      </footer>
    </div>
  )
}
