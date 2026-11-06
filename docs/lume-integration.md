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
  "app": "qt-react-shell",
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
QTRS_LUME_URL=http://127.0.0.1:8090 QTRS_SIM_SPO2=86 ./build/qt-react-shell
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

## 下一步（按需推进，不是本轮范围）
2. **SSE**：Lume 推 `alarm.mute` / `case.select` 指令，`QNetworkReply::readyRead` 逐行解析。
3. **A2A / Agent**：把 `summary` 注册成 Lume agent 的只读工具，术后问答「这台手术断了几次」。
4. **鉴权**：`htpasswd` 或 `X-*` 头门禁；`QTRS_LUME_URL` 目前明文，仅限本机/内网。

## 安全与部署

- `server{}` 默认 `bind 127.0.0.1`；容器化要显式 `LUME_BIND=0.0.0.0`，并配反代鉴权。
- `env()` 对含 `TOKEN`/`SECRET`/`KEY`/`PASSWORD` 的变量名返回 null（防 DSL 泄露凭据），
  不能用它判断「密钥是否配好」——那个判断恒为 false。
- 上行内容含患者相关字段时，先做去标识化：`session` 是本地会话号，不含姓名/住院号；
  后续接真实 PACS 时，`StudyInstanceUID` 等应改为哈希或干脆不上行。
