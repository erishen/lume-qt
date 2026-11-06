import { useEffect, useState } from 'react'
import { connectBridge, type Bridge } from './qwebchannel'
import AuditStream from './AuditStream'
import MedicalMonitor from './MedicalMonitor'

/**
 * App — 演示 React 前端与 C++ 桥接的双向通信。
 *
 * 1. 页面加载后自动连接 QWebChannel，读取平台信息
 * 2. 按钮触发 JS → C++ 调用（showMessage / add）
 * 3. 订阅 C++ → JS 主动推送（dataFromCpp 信号）
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

      // 订阅 C++ 主动推送
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
      <header className="header">
        <h1>qt-react-shell</h1>
        <p className="subtitle">Qt WebEngine · C++ · React (Vite + TypeScript)</p>
      </header>

      <main className="content">
        <MedicalMonitor bridge={bridge} />

        <AuditStream bridge={bridge} />

        <section className="card">
          <h2>桥接状态</h2>
          <dl>
            <dt>平台信息</dt>
            <dd>{platformInfo}</dd>
            <dt>App 名称</dt>
            <dd>{bridge?.appName ?? '—'}</dd>
          </dl>
        </section>

        <section className="card">
          <h2>JS → C++ 调用</h2>
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

        <section className="card">
          <h2>C++ → JS 推送</h2>
          <button onClick={handlePushDemo} disabled={!bridge}>
            触发 dataFromCpp
          </button>
          {pushed && <p className="result">{pushed}</p>}
        </section>
      </main>

      <footer className="footer">
        {bridge ? '已连接 Qt C++ 桥接' : '未连接桥接'}
      </footer>
    </div>
  )
}
