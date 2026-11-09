# Lume 围术期影像工作台 — 演示版交付说明

- 收尾日期：2026-11-07
- 收尾基线：`main` 分支，tag `demo-2026-11-07`
- 定位：科研/工程演示，**不是医疗器械，不能用于诊断、治疗或临床决策**

## 演示范围

| 模块 | 演示内容 |
|---|---|
| Web 监护台 | 六通道波形、生命体征、报警、延迟看板、审计事件流 |
| 原生影像页 | OpenGL 体渲染、窗位/窗宽、切面、CT 阈值去骨 |
| DICOM/PACS | 本地样例加载、C-ECHO、C-FIND、C-GET、C-MOVE 接收与回落 |
| Lume 集成 | 审计事件上行、配置下发、只读事件查询、AI 复盘（需本地模型配置） |
| 隐私加固 | 发端白名单、收端路径/UID 清洗、台账与 PACS 属主权限、可选保留期、开发 URL loopback 限制 |

## 启动

```bash
cd work/lume/lume-qt
export PATH=/opt/homebrew/bin:/opt/homebrew/sbin:$PATH

# 编译并启动
make cpp
./build/lume-qt
```

可选环境变量：

```bash
QTRS_LUME_URL=http://127.0.0.1:8090 \
QTRS_PACS=127.0.0.1:11113:QTRSQR \
./build/lume-qt
```

- `QTRS_LUME_URL` 未设置时，不建立 Lume 网络栈，AI 复盘按钮禁用。
- `QTRS_PACS` 未设置时，不做 PACS 探测与取回。
- 真实 PACS 联调前先确认目标端口有 DICOM 服务监听；本机 `11113` 未监听时 C-ECHO 会失败。

## 最终验证

| 项目 | 结果 |
|---|---|
| `make cpp` | 通过 |
| `ctest --test-dir build --output-on-failure` | 2/2 通过 |
| `make frontend-tests` | 8/8 通过 |
| `npm --prefix frontend run typecheck` | 通过 |
| `make trace` | 48 条需求：已验证 39 / 部分验证 7 / 缺口 2 |
| `git diff --check` | 无空白错误 |
| 台账权限 | 目录 `700`、`*.jsonl` `600` |
| PACS 权限 | 目录 `700`、DICOM 文件 `600` |

## 数据与隐私

- 本机演示数据位于 `~/Library/Application Support/research/lume-qt/`，不进入版本库。
- 台账与 PACS 落盘按 PHI 对待，已收紧为属主可读写；默认不自动删除，需显式设置 `QTRS_AUDIT_RETAIN_DAYS` / `QTRS_PACS_RETAIN_DAYS`。
- `QTRS_LUME_TOKEN` 只负责让客户端带 `Authorization: Bearer …`；Lume 参考服务本身不校验该头，远程部署必须由反向代理/mTLS 收口。
- 演示截图和对外材料不得展示真实患者信息；样例 DICOM 不随仓库分发，对外材料只使用许可明确的公开/合成数据（本机样例的来源与许可尚未确认，见 `docs/sample-data-attribution.md`）。

## 已知边界（不影响演示收尾）

- 追溯表仍有 2 个缺口：真实设备协议接入（`QRS-D-002`）、真实临床场景验证（`QRS-R-004`）。
- 未完成 IEC 62304 软件安全分类、ISO 14971 正式风险可接受性结论、IEC 60601-1-8 报警合规验证。
- Lume 直接暴露时写接口无鉴权；PACS C-MOVE 接收端为明文；真实 PACS 仅支持 C-MOVE 时 TLS 取回受限。
- 没有 React 组件测试、没有 CI、没有 macOS app bundle/签名。交付快照时也没有 LICENSE；2026-11-09 已补齐 MIT 许可与第三方声明，见根目录 `LICENSE` / `THIRD_PARTY_NOTICES.md`。

## 收尾定义

`demo-2026-11-07` 是演示快照：功能可运行、文档与验证记录齐全、隐私加固已落地。它不是临床发布版本，后续如进入临床，应按 `docs/privacy-compliance.md` 与 `docs/traceability.md` 的缺口清单单独立项。
