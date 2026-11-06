# 接入 Lume：术中桌面壳 ↔ 事件总线

## 为什么是「事件上行」而不是「把界面搬进 Lume」

Lume（`work/lume`）是自研 `.lume` 语言 + 单二进制 HTTP 服务器（静态站 / JSON API / SSE / Agent），
它的强项是**服务端聚合与跨端可见**，不是 WebGL 实时渲染。桌面壳的实时波形、原生 OpenGL 体渲染
留在本机；Lume 只收「发生了什么」，供术后复核、值班大屏、Agent 问答使用。

职责边界：

| 角色 | 职责 | 故障后果 |
|---|---|---|
| `AuditSink`（本地） | 逐条落盘 JSON Lines，唯一权威 | 磁盘写失败只降级为 stderr，业务继续 |
| `LumeSink`（本地） | 批量 HTTP 上行镜像 | 断网/超时只丢镜像，本地证据链完整 |
| `tools/lume-medmon`（Lume 侧） | 接收、落 SQLite、汇总查询 | 重启后从库恢复；只影响跨端可见性 |

## 协议

```
POST <QTRS_LUME_URL>/api/medmon/audit
Content-Type: application/json

{
  "app": "lume-qt",
  "sentAt": "2026-11-06T11:22:49.025",
  "events": [
    { "ts": "2026-11-06T11:22:49.020", "tsUs": 1793935369020038,
      "session": "20261106T112246-72270", "type": "provider.start",
      "group": "neurology", "provider": "simulator" }
  ]
}
```

响应：`{"ok": true, "accepted": N}`；`400` 表示 body 非法或缺少 `events`；`500` 表示落库失败。

客户端行为：队列上限 500、单批 50、`transferTimeout=3000ms`、失败 5s 重试（放回队首）、
溢出丢最旧并计数（`sent()/failed()/dropped()`）。未设置 `QTRS_LUME_URL` 时对象存在但不建
`QNetworkAccessManager`，ctest 与无网环境行为不变。

## Lume 侧参考实现

`tools/lume-medmon/app.lume`：

- `GET /health`
- `POST /api/medmon/audit` → `events` 表（SQLite，`.data/medmon.db`）
- `GET /api/medmon/summary` → `total/alarm/session/pacs/watchdog/dicom/lume/last`
- `GET /api/medmon/events?limit=` → 按 id 倒序

启动：

```bash
cd tools/lume-medmon
lume app.lume          # http://127.0.0.1:8090（LUME_BIND 可覆盖 bind）
```

表结构：

```sql
CREATE TABLE events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  ts TEXT, ts_us INTEGER, session TEXT, type TEXT, level TEXT,
  payload TEXT,   -- 原始事件的 JSON 文本，保真
  created INTEGER
);
```

**坑：不要用进程内全局数组。** `run()` 每个请求 fork，POST 写入的是子进程副本，
后续 GET 读不到（实测 `accepted=2` 而 `summary.total=0`）。要么落库，要么写文件。

## 验证记录（2026-11-06）

```bash
cd tools/lume-medmon && lume app.lume &
QTRS_LUME_URL=http://127.0.0.1:8090 QTRS_SIM_SPO2=86 ./build/lume-qt
curl -s http://127.0.0.1:8090/api/medmon/summary
```

结果：`total=9, session=2, watchdog=1, dicom=2, alarm=1`，`last.type=alarm.on`
（`channel=SpO2`, `priority=high`, `spo2=86`）。桌面壳日志 `[Lume] 首次上行成功，共 1 条`；
界面三面板与原生 ECG/SpO₂ 条照常，血氧 86% 红框报警可见。

单测（不依赖网络）：`AuditSinkObserver.ReceivesEveryWrittenEventAfterPersist`、
`LumeSinkTest.DisabledWithoutUrlAndNormalizesEndpoint`。

## 配置下行（已落地，2026-11-06 第二批）

`GET/POST /api/medmon/config`（默认值 `spo2High=90, spo2Mid=92, hrHigh=50, hrMid=55`）：

- 桌面壳启动时 GET 一次，覆盖 env 默认；服务不可用则保持本地值（2 s 超时）。
- POST 侧做与客户端相同的范围校验，非法值 400 且不落库；`alarm.config` 台账记录来源与生效值。
- 实测：`spo2Mid=95` + `QTRS_SIM_SPO2=93` → 中优先级「血氧偏低 93%」；改回 92 → 同值不报警。

## Agent 复盘（已落地，2026-11-06 第三批）

Lume 框架自带 Agent（`.lume` 注册 `tool`，框架驱动 ReAct 多轮，浏览器侧走
`POST /react/api/chat` 的 SSE）。这一批把「术后问答」接进了桌面壳：

- Lume 侧注册三个只读工具：`medmon_summary` / `medmon_events` / `medmon_review`；
  `medmon_review` 一次返回汇总 + 最近 12 条事件，复盘只让它调用一次，避免多轮查同一批数据。
- `GET /api/agent/ready` 返回已注册工具列表，启动/排查时先探这个端点。
- 桌面壳 `Bridge::askLume()` → `LumeSink::ask()`：POST `<base>/react/api/chat`，
  body 带 `message`、`sessionId`、`tools:["medmon_review"]`；解析
  `{"t":"delta","d":"…"}` / `{"t":"note","d":"…"}` / `{"t":"error"}` / `{"t":"done"}`。
- 前端「事件流 · 台账检索」标题右侧新增 `Lume AI 复盘` 按钮，结果面板显示回答与
  「工具轨迹：medmon_review · N 轮」；Lume 未启用（没设 `QTRS_LUME_URL`）时按钮禁用并显示
  「Lume 未连接」。所有输出只读，不影响本地判定与报警。

启用模型（`.env` 放在 `tools/lume-medmon/` 下；**不要提交**）：

```bash
cp /path/to/your/lume/.env tools/lume-medmon/.env   # 本机已有配置可直接复制
cd tools/lume-medmon
lume app.lume
curl -s http://127.0.0.1:8090/api/agent/ready
# {"ok":true,"tools":["medmon_summary","medmon_events","medmon_review"],"chat":"/react/api/chat"}
```

没有 `tools/lume-medmon/.env`（或 `LLM_API_KEY` 为空）时，Lume 走内置离线演示引擎：
接口仍能返回 SSE，但不会调用工具；桌面壳会显示「Lume 未配置大模型」而不是假装成功。
配置模板见 `tools/lume-medmon/.env.example`。

实测（`QTRS_LUME_URL=http://127.0.0.1:8090`，真实模型）：

```
[Lume] AI 复盘请求： http://127.0.0.1:8090/react/api/chat
[Lume] AI 复盘完成：162 字，工具轨迹 1 条，模式 model
```

界面显示一段 200 字内的复盘（报警 / 信号 / PACS / 体数据 / 看门狗计数），
单工具版本端到端约 35–90 s（受模型侧波动）；多工具版本约 90 s，所以生产提示词固定只开 `medmon_review`。

**隐私边界**：桌面壳 `LumeSink::sanitizedForLume()` 在发端把 `studyUid` / `dir` 换成
不可逆摘要、移除患者相关键，并把消息里的绝对路径与 DICOM UID 替换掉；Lume 落库前
`redact_event()` 再做一次白名单，`/api/medmon/events` 与 Agent 工具都只返回白名单字段。
本地 JSONL 台账仍保留完整字段，按 PHI 保护。`session` 是本地会话号，不含姓名。
模型回答只作为工程复盘，不是诊断/治疗建议；异常仍以本地台账与报警为准。

## 下一步（按需推进，不是本轮范围）

1. **SSE 指令下行**：Lume 推 `alarm.mute` / `case.select`，`QNetworkReply::readyRead` 逐行解析。
2. **A2A 跨节点**：把 `medmon_review` 暴露给 A2A 对端，支持值班大屏/多机协同问答。
3. **鉴权**：`htpasswd` 或 `X-*` 头门禁；`QTRS_LUME_URL` 目前明文，仅限本机/内网。
4. **流式界面**：当前等 SSE 全部收完再显示；后续可把 `delta` 逐块推到面板。

## 安全与部署

- `server{}` 默认 `bind 127.0.0.1`；容器化要显式 `LUME_BIND=0.0.0.0`，并配反代鉴权。
- `env()` 对含 `TOKEN`/`SECRET`/`KEY`/`PASSWORD` 的变量名返回 null（防 DSL 泄露凭据），
  不能用它判断「密钥是否配好」——那个判断恒为 false。
- 上行内容含患者相关字段时，先做去标识化：`session` 是本地会话号，不含姓名/住院号；
  后续接真实 PACS 时，`StudyInstanceUID` 等应改为哈希或干脆不上行。
