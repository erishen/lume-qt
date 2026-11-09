# 隐私与合规审计（2026-11-06）

> 本文是工程侧的风险盘点，不是法律意见，也不构成医疗器械合规结论。
> 适用范围：本仓库源码、当前本机运行数据、桌面壳 → Lume → 模型的数据链路。

## 结论先说

- **仓库里没有发现硬编码密钥或真实病患数据。** `.env`、样例 DICOM、本地 SQLite/skills 缓存都在 `.gitignore` 内，git 历史中没有 `.env` / `.dcm` / 台账文件。
- **但项目不是临床合规产品。** 目前没有医疗器械注册、临床验证、质量体系、电子签名或审计追踪签核；界面和文档必须持续标注“演示/非临床”。
- **真实 PACS 接入后，主要风险是 PHI 出端与本地留存，而不是源码泄露。** 旧链路会把 `studyUid` 和落盘目录原样上行到 Lume；Lume 的读接口也会返回原始 payload。本轮已修复这两条，但部署层仍需做网络、账号、加密和保留期控制。

## 数据清单与流向

| 数据 | 位置 | 是否入库 | 风险 |
|---|---|---|---|
| LLM API Key / 模型地址 | `tools/lume-medmon/.env` | 否（已 ignore） | 本机明文密钥；权限已为 `600`；一旦复制/截图/打包仍可能泄露 |
| 样例 DICOM 序列 | `resources/dicom/` | 否（已 ignore） | 头部可见 `Jane_Doe` 等演示标识；**不是**从临床系统导出的真实病例；换真实数据后即为 PHI |
| 本地审计台账 | `~/Library/Application Support/research/lume-qt/audit/*.jsonl` | 否 | 含 `studyUid`、`dir`、PACS 端点、报警/信号事件；按 PHI 保护；目录 700 / 文件 600，保留期开关默认关闭 |
| PACS 落盘 | `~/Library/Application Support/research/lume-qt/pacs/` | 否 | 完整 DICOM，含患者姓名/ID/检查信息；目录 700 / 文件 600；保留期开关默认关闭 |
| Lume 镜像库 | `tools/lume-medmon/.data/medmon.db` | 否 | 旧数据可能含原始 `studyUid`/`dir`；新数据落库前已白名单化 |
| 会话快照 | `~/Library/Application Support/research/lume-qt/state/` | 否 | 含上次体数据目录；不含姓名，但可关联检查 |

### 外发链路

1. 桌面壳 `AuditSink` 写本地 JSONL（完整字段，作为唯一权威）。
2. `LumeSink::post()` 在发端做最小化：字段走白名单（未知字段直接丢弃）；`studyUid` / `dir` / `path` 等换成稳定 16 位摘要；`patient*` / `accessionNumber` / `medicalRecordNumber` 等患者键直接移除；消息里的绝对路径和 DICOM UID 替换为 `<path>` / `<uid>`。
3. Lume `POST /api/medmon/audit` 落库前再按标量白名单裁剪一次，并对 `message` / `source` 等字符串做独立的路径/UID 清洗；`GET /api/medmon/events` 和 `medmon_summary` 只返回白名单字段。
4. Agent 工具 `medmon_summary` / `medmon_events` / `medmon_review` 只读这些白名单字段；模型看不到原始 UID、落盘路径和患者键。

> 注意：本地 JSONL 仍保留完整字段，这是为了本地复核和故障定位；它不能直接上传、外发或提交到 git。

## 本轮已落地的控制

- 源端脱敏：`src/lumesink.cpp` 的 `LumeSink::sanitizedForLume()`；单测覆盖顶层与嵌套对象、消息路径/UID、患者键。
- Lume 侧收口：`tools/lume-medmon/app.lume` 把 `redact_event()` 前移到路由之前；写库和两个读接口都走白名单。
- 界面标识：顶栏增加“演示 · 非临床”胶囊；Lume AI 面板仍标注“只读台账 · 非诊疗”。
- 密钥与数据边界：`.env` 权限 `600`；`.env.example` 只留空模板；样例 DICOM 与 `.data` 不入库。

## 残余风险（按优先级）

1. **Lume HTTP 无鉴权、无 TLS。** 默认只绑 `127.0.0.1`，但 `LUME_BIND` 可改成 `0.0.0.0`；一旦暴露到科室网络，任何能访问 8090 的人都能读事件/改报警阈值。生产必须加反向代理鉴权、mTLS 或至少网络 ACL，并保持 loopback。
2. **外部模型数据处理。** `LLM_API_URL` 指向第三方模型时，脱敏后的事件摘要仍会离开本机；需要确认供应商的数据处理条款、地域、留存与训练使用政策，必要时换院内模型或关闭 `QTRS_LUME_URL`。
3. **本地 PHI 留存。** PACS 落盘和 JSONL 台账已加属主-only 权限与显式保留期开关，但默认不自动清理；磁盘未加密时，拿到机器仍可读。需要按院方制度做全盘加密、明确的天数配置、删除审批和备份策略。
4. **旧镜像数据。** 升级前已写入 `.data/medmon.db` 的历史 payload 可能仍有原始字段；如确认只是演示数据，可在停服后删除该库重建；否则应按 PHI 数据做清理/迁移。
5. **临床宣称。** “围术期影像工作台”“监护”“AI 复盘”容易被理解为临床用途；当前只在界面/文档标注非临床，正式对外发布前需要法务/注册路径确认，不能靠一句免责声明替代注册。

## 2026-11-07 复审后的代码加固

- **发端白名单**：`src/lumesink.cpp` 不再只靠 `patient*` 黑名单；未知字段默认丢弃，`personName` / `institutionName` / `referringPhysician` / `studyDescription` / `accession` / `operatorName` 等常见 DICOM 元数据即使新增也不会出端。`studyUid` / `dir` 仍哈希后保留关联性。
- **收端清洗**：`tools/lume-medmon/app.lume` 的 `redact_event()` 收窄为标量白名单，去掉 `studies` / `event` / `dir` / `studyUid`；`message` / `source` / `providerDescription` 等字符串统一把绝对路径替换为 `<path>`、DICOM UID 替换为 `<uid>`。已用临时实例 POST 嵌套患者字段验证：库中只剩清洗后的 `source/message`，嵌套字段全丢。
- **本地权限**：台账目录 `700`、`*.jsonl` 及轮转文件 `600`；PACS 检查目录 `700`、DICOM 文件 `600`。两者都加了显式保留期开关 `QTRS_AUDIT_RETAIN_DAYS` / `QTRS_PACS_RETAIN_DAYS`（默认不删，只有设了天数才清理）。
- **开发 URL**：`QTRS_DEV_URL` 只接受 `http(s)` + loopback（`localhost` / `127.0.0.1` / `::1`），远程页面拿不到 QWebChannel 桥。
- **鉴权头**：`QTRS_LUME_TOKEN` 让审计/配置/AI 请求带 `Authorization: Bearer …`；日志只打印去掉 userinfo/query/fragment 的 URL。注意：Lume 参考服务本身仍不校验这个头，真实远程部署必须由反代/mTLS 收口。
- **路径校验**：`src/appschemehandler.cpp` 的 `app://` 根目录前缀补了 `/`，避免同前缀兄弟目录绕过。

## 2026-11-09 复审后的清洗补强

- **发端与收端同步**：`src/lumesink.cpp` 的 `scrubText()` 与 `tools/lume-medmon/app.lume` 的 `scrub_text()` 原先都要求 DICOM UID 至少 5 段、只认 5 种 Unix 路径前缀且遇空格即停。实测 `2.25.*`（UUID 派生 UID）、`1.2.840.10008`、`…/Application Support/…`、`~/Library/...`、`/opt/homebrew/...`、`C:\Users\...` 会原样或残留半截出端，已修复。
- **新边界与代价**：UID 从 3 段起折叠；路径覆盖 `~/`、Windows 盘符与常见 macOS/Linux 根，允许空格。代价是 IPv4 形状的数字串会被一并折叠成 `<uid>`、路径后的尾随描述可能被一起折成 `<path>`——均为「宁可多删」的过度脱敏，本地 JSONL 保留完整原文。
- **回归证据**：`tests/bridge_test.cpp` 新增 2 条 C++ 用例（先跑红确认缺口，修复后 5/5 绿）；`tools/lume-medmon/test-post.sh` 扩展为第二个 POST，故意直投原始 PHI 并核对 SQLite `payload` 不含任何标识；把 `app.lume` 换回旧正则时该测试确实变红，排除假绿。
- **仍存在的边界**：自由文本里的患者姓名、机构/术式描述无法靠正则消除，仍依赖字段白名单与「不用真实数据演示」；`LumeSink::ask()` 的用户输入会发给模型（界面目前只发固定 `AI_PROMPT`，不含台账内容）。

## 2026-11-09 PACS TLS 复审

- `QTRS_PACS_TLS_VERIFY=check` 已禁用。DCMTK 头文件明确该级别是「对端有证书就验、**没有证书也放行**」，配上 CA 也不等于验过服务端身份；原先把它和 `require` 并列为合法取值，等于给 PHI 链路留了一个看起来验了、实际可被冒充的开关。现在只保留 `require`（默认）与 `ignore`，后者运行时打 warning 并在状态摘要标注「不验服务端证书」。
- C-MOVE 的 TLS 判定原先在 `openListenPort()` **之后**：TLS 模式下会先开放一个明文 Storage SCP 端口再报错退出。现改为函数入口 `pacsTlsMoveBlockReason()` 前置判定，且 TLS 配置不合法时同样拒绝，避免「证书配错就退回明文 C-MOVE」。
- 证书验证失败（`certificate verify failed` / `TLS error`）原先因消息含「关联协商失败」被判为瞬时故障、重试三次，把 CA 配错伪装成 PACS 不稳定；现归入确定性失败。
- 回归证据：`PacsTlsConfigTest.*` 新增 3 条（先跑红、修复后 10/10 绿），`PacsClientTest.ClassifiesRetryableErrors` 扩展 3 个断言；`scripts/check-pacs-tls.py` 静态门禁做了四负例一正例自检。`QRS-I-007` 的验证证据已补。
- **仍未解决**：`ignore` 模式本身不验服务端证书；C-MOVE 接收端没有 TLS（只能强制禁用）；`DcmTLSSCU::enableAuthentication()` 返回 `void`，证书文件存在但内容损坏时无法在建连前从返回码判定，只能靠握手失败暴露。

仍**未**解决、也改代码解决不了的：医疗器械注册/临床验证/QMS/电子签名/操作者身份；Lume 直接暴露时的写接口鉴权；PACS C-FIND 全库列举的最小必要访问；真实 PACS 只支持 C-MOVE 时的 TLS 取回。演示阶段可以接受，进临床前必须单独处理。

## 上线前最低检查表

- [ ] `git grep -nE 'sk-|LLM_API_KEY=|BEGIN .*PRIVATE KEY'` 无命中；`.env` 仍被 ignore。
- [ ] `LUME_BIND=127.0.0.1`；8090 不暴露到公网/科室广播地址；需要远程时前置鉴权代理。
- [ ] 真实 PACS 场景使用测试患者/去标识数据做联调；生产数据按院方 PHI 制度处理。
- [ ] 明确台账/PACS 的保留期、删除审批、备份加密与访问账号。
- [ ] 与模型供应商确认 DPA/数据处理协议；关闭不必要的会话留存。
- [ ] 对外页面、演示 PPT、截图统一标注“演示/非临床”，不展示真实患者信息。

## 复现审计的命令

```bash
git grep -n -I -E 'sk-[A-Za-z0-9]{16,}|LLM_API_KEY[[:space:]]*=|BEGIN .*PRIVATE KEY' -- .
git rev-list --objects --all | rg -i '\.env|\.dcm|\.dicom|audit.*\.jsonl|\.plist|\.key|\.pem'
git check-ignore -v tools/lume-medmon/.env resources/dicom/ tools/lume-medmon/.data/
```
