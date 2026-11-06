# 术中化需求追溯矩阵

这份表回答一个问题：**我们说「已经术中化」的每一条，是靠什么代码实现的、又靠什么证据证明的。**

写法上刻意保持「一行一条、可被脚本检查」：`scripts/check-traceability.py` 会校验
每个 ID 唯一、状态只有三种取值、以及「实现」列里引用的文件路径真实存在。
文档一旦腐化（删了文件、改了状态），`make trace` 就会失败，而不是等到评审时才发现。

状态定义：

- **已验证** — 有可复现的实测或自动化用例，证据可追到提交与日志。
- **部分验证** — 实现存在且能跑，但覆盖不全（只测了一种服务端、只测了单实例等）。
- **缺口** — 还没有实现，或实现不足以支撑术中使用的结论。

## A. 时基与确定性

| ID | 需求 | 实现 | 验证方式 | 状态 |
| --- | --- | --- | --- | --- |
| QRS-T-001 | 每一帧样本携带采集时刻（epoch 微秒），前端与原生侧换算到同一时间轴 | `src/simulatorprovider.cpp` `frontend/src/latency.ts` | 看板「C++ 打时间戳 · 宿主同源」；单测 `Latency` 用例 | 已验证 |
| QRS-T-002 | 设备时钟与宿主时钟存在未知常量偏移时，延迟不得出现负值或数千毫秒的假值 | `frontend/src/latency.ts` `src/vitalstrip.cpp` | `QTRS_CLOCK_OFFSET_MS=5000`：修复前 `-4999.81ms`，修复后 ECG 229ms | 已验证 |
| QRS-T-003 | MB 级 DICOM 体积不得经 QWebChannel 传输（会破坏字节） | `src/appschemehandler.cpp` `frontend/src/Volume3D.tsx` | `app://app/__volume/current.bin` 取回 2,097,152 字节，与源文件逐字节一致 | 已验证 |
| QRS-T-004 | 渲染进程不得因窗口遮挡/后台而被 Chromium 节流 | `src/main.cpp` | `ps` 核对四个开关进入渲染进程；遮挡下 WebView 到达中位数仍 250–325ms | 部分验证 |
| QRS-T-006 | 三维体积绘制必须能在不依赖 WebView 的情况下独立出图 | `src/volumeview.cpp` `src/mainwindow.cpp` | 离屏 OpenGL 3.3：首帧 241–261ms（含上下文与着色器），稳态 60 帧均值 2.9–7.3ms（404×558）；`dicom.volumeNativeFrame` 入账 | 已验证 |
| QRS-T-007 | 体积显示必须保住 HU、按临床窗位/窗宽预设呈现，默认灰阶 | `src/dicomreader.cpp` `src/volumeview.cpp` `src/mainwindow.cpp` | 合成 CT（HU −1000…1200）：软组织窗 W80/L40 出颅骨与脑实质、骨窗 W1500/L300 只剩骨；MR 四个档位仍可切换（按组织强度近似映射）并使用 a.u. 文案；CT 默认灰阶、非 CT 默认伪彩且均标注非诊断 | 部分验证 |
| QRS-T-008 | 事件型通道（体积）的延迟必须按单次耗时计量，且原生接管时不得重复拉取体数据 | `src/bridge.cpp` `src/mainwindow.cpp` `frontend/src/latency.ts` `frontend/src/MedicalMonitor.tsx` | 看板体积行由 `530282.0 ms` 假值变为 到达 1849 ms / 上屏 273 ms；RSS 519→408 MB；`QTRS_NATIVE_VOLUME=0` 时解析只发生 1 次、原生首帧为 0 条 | 已验证 |
| QRS-T-009 | 体积必须支持术中的手动窗位/窗宽调整，且预设与手动值不得互相错位 | `src/volumeview.cpp` `src/mainwindow.cpp` | Alt+左键拖动改窗；W/L 输入框单位随模态切 HU/a.u.；手动后下拉切「自定义」、选回预设数值同步；`QTRS_VOLUME_WL=800,150` 实测 W800/L150，预设 1 实测 W1500/L300 且输入框同步；体积不再误触 100 ms 链路预算 | 已验证 |
| QRS-T-010 | 体积视图必须支持去骨与正交切面，且不能把整条光线误裁 | `src/volumeview.cpp` `src/mainwindow.cpp` | 骨窗 + 去骨实测画面全黑（证明 HU≥300 被剔除）；矢状面 50% 实测半剖可见内部；`QTRS_VOLUME_BONE=1`、`QTRS_VOLUME_CLIP=0,0.5` 均入日志 | 已验证 |
| QRS-T-011 | 窗位/窗宽、自适应分位与手动调窗边界必须有不依赖 GL 的纯函数单测 | `src/volumestats.cpp` `tests/volume_stats_test.cpp` | `ctest` 2/2：CT 四档窗值、非 CT 映射、clamp 边界、CT 剔空气 p50、MR 剔背景 p5、全空气回退；重构后分位日志与改造前逐字一致 | 已验证 |
| QRS-T-012 | 审计台账必须按大小/日期轮转，旧证据不得被覆盖或丢弃 | `src/auditsink.cpp` `tests/bridge_test.cpp` | 单测预置 >1 MB 台账后 `open()`，断言旧文件改名为 `.001`、新文件从空开始且写入计数为 1；跨天切文件逻辑在 `record()` 内 | 已验证 |
| QRS-T-005 | 关键通路的设计必须在「WebView 投递不可靠」这一实测结论上成立 | `src/vitalstrip.cpp` | 原生 P50 0.24ms vs 前端 291–367ms（约 1000 倍量级差） | 已验证 |

> QRS-T-004 标为部分验证：开关确实生效，但挡不住 macOS 窗口非前台时 QWebChannel 批量迟发。
> 结论是「实时通路不建立在这套 WebView 投递上」，而不是「开关解决了问题」——这条结论直接支撑 QRS-N-001。

## B. 关键波形与报警原生化

| ID | 需求 | 实现 | 验证方式 | 状态 |
| --- | --- | --- | --- | --- |
| QRS-N-001 | ECG 与 SpO₂ 的显示不经过 WebView，信号直连原生绘制 | `src/vitalstrip.cpp` `src/mainwindow.cpp` | 截图 + `[VitalStrip]` 日志 P50 0.24ms；`QTRS_FAULT` 注入下仍出图 | 已验证 |
| QRS-N-002 | 每个通道自行判定断流，并在画面上明确标注冻结帧与已断时长 | `src/vitalstrip.cpp` | `QTRS_FAULT=ecg,spo2`：两条 lane 各盖「信号中断 2507 ms · 冻结帧」 | 已验证 |
| QRS-N-003 | 报警判定在原生侧完成，不依赖前端是否渲染成功 | `src/vitalstrip.cpp` | 台账 `alarm.on` 在 `QTRS_SIM_SPO2=86` 下产生，与前端状态无关 | 已验证 |
| QRS-N-004 | 报警提示可静音，但判定不变、数值不隐藏、静音有时限且可追溯 | `src/vitalstrip.cpp` `src/mainwindow.cpp` | `QTRS_ALARM_MUTE_SEC=6`：琥珀边框 → 到期回红，台账 `alarm.mute` / `alarm.unmute(expired)` | 已验证 |
| QRS-N-005 | 报警应有分级与声音，且优先级符合 IEC 60601-1-8 的思路 | `src/vitalstrip.cpp` | 高/中两级（红/黄）；高 1000 Hz 三连 3 s 复鸣、中 620 Hz 两声 8 s 复鸣，`QSoundEffect` 播自合成 WAV；台账带 `priority` | 已验证 |
| QRS-N-006 | 静音期间不得再发声，但不得改变判定或隐藏数值 | `src/vitalstrip.cpp` | 静音期间无 `[Alarm] tone` 行；SpO₂ 仍显示 84、判定仍为高优先级 | 已验证 |

## C. 设备接入

| ID | 需求 | 实现 | 验证方式 | 状态 |
| --- | --- | --- | --- | --- |
| QRS-D-001 | 采集源与传输层解耦：换设备不改前端与原生条 | `src/deviceprovider.h` `src/simulatorprovider.cpp` `src/bridge.cpp` | 拆分后看板与原生条读数与拆分前一致（ECG 到达 347ms / 原生 0.23ms） | 已验证 |
| QRS-D-002 | 至少接入一类真实设备协议（监护仪/麻醉机），而非仅模拟器 | — | — | 缺口 |

## D. 影像与 PACS

| ID | 需求 | 实现 | 验证方式 | 状态 |
| --- | --- | --- | --- | --- |
| QRS-I-001 | 能探测 PACS 连通性并列出检查，状态既推也拉 | `src/pacsclient.cpp` `src/bridge.cpp` | `PACS 已连通 13 ms · 1 例`；断连态红字；`pacsState()` 快照解决启动早于订阅 | 已验证 |
| QRS-I-002 | 能把整例取回、落盘并重建体积显示到屏幕上 | `src/pacsclient.cpp` `src/bridge.cpp` `src/dicomreader.cpp` | C-GET 60 对象 97–354ms → 60 张切片重采样 128³ → `dicom.volumeReady` | 已验证 |
| QRS-I-003 | 只支持 C-MOVE 的 PACS 也有可取路径（本机接收 SCP） | `src/pacsclient.cpp` `src/bridge.cpp` | C-MOVE 60 对象 3137ms；`--disable-get` 下 C-GET → C-MOVE 回落 3226ms | 已验证 |
| QRS-I-004 | 瞬时故障自动重试（有界退避），状态翻转时记录在线/离线 | `src/pacsclient.cpp` `src/bridge.cpp` | 断网三次失败后放弃；重试途中恢复第 2 次成功；心跳恢复 4ms | 已验证 |
| QRS-I-005 | 多例选择 / 按病人过滤，而不是永远取第一例 | `frontend/src/MedicalMonitor.tsx` `src/bridge.cpp` | 库里两例时下拉可切换；选第二例后取回 8 个对象、体积来自该例目录 | 已验证 |
| QRS-I-006 | 长时间取回期间的掉线要能及时暴露，而不是等取回失败 | `src/bridge.cpp` | 心跳独立线程/关联：C-MOVE 进行中杀掉 PACS，同一秒记 `pacs.offline`、取回随后才失败；日志无「跳过本次探测」 | 已验证 |

## E. 审计与可追溯

| ID | 需求 | 实现 | 验证方式 | 状态 |
| --- | --- | --- | --- | --- |
| QRS-A-001 | 关键事件以结构化 JSON Lines 落盘，固定字段不可被业务字段覆盖 | `src/auditsink.cpp` | 抽查 `audit-YYYYMMDD.jsonl`：`ts`/`tsUs`/`session`/`type` 恒在 | 已验证 |
| QRS-A-002 | 断流、恢复、报警只在边沿记录，稳态不刷屏 | `src/vitalstrip.cpp` `src/bridge.cpp` | 长时间运行下 `signal.stale` 不重复；`alarm.on` 一次告警一条 | 已验证 |
| QRS-A-003 | 多个实例同时运行不得互相污染台账（会话隔离或文件锁） | `src/main.cpp` | `state/instance.lock` 按 PID 判活：第二个实例打印持有者 pid 并 exit 1；死进程遗留锁在下次启动被接管；正常退出由 atexit 清锁 | 已验证 |

## F. 故障安全

| ID | 需求 | 实现 | 验证方式 | 状态 |
| --- | --- | --- | --- | --- |
| QRS-F-001 | 前端崩溃或卡死时，关键波形与报警仍可用 | `src/vitalstrip.cpp` | 原生条独立于 WebView；`QTRS_FAULT` 下仍显示冻结帧与时长 | 已验证 |
| QRS-F-002 | 静音有时限（≤5 分钟），到期自动恢复提示并留痕 | `src/vitalstrip.cpp` | 6 秒静音到期后边框回红，`alarm.unmute(reason=expired)` | 已验证 |
| QRS-F-003 | 主进程异常退出后，重启能恢复到「有明确状态」而不是静默空白 | `src/bridge.cpp` `frontend/src/MedicalMonitor.tsx` | `kill -9` 后重启：`session.recovered(gapSeconds=4, events=8)` + 看板琥珀提示 + 上次体数据自动重建 60 层 | 已验证 |
| QRS-F-004 | 手术室内无外网时，功能不得依赖任何在线服务 | `src/pacsclient.cpp` `src/bridge.cpp` | PACS 全部为本机/院内地址；断网时进入未连通态而非崩溃 | 部分验证 |

## G. 法规与流程

| ID | 需求 | 实现 | 验证方式 | 状态 |
| --- | --- | --- | --- | --- |
| QRS-R-001 | 软件按 IEC 62304 做安全分类并形成需求—实现—验证追溯 | `docs/traceability.md` `scripts/check-traceability.py` | `make trace` 校验本表结构完整、引用文件存在 | 部分验证 |
| QRS-R-002 | 按 ISO 14971 形成风险分析（危害、原因、控制措施、剩余风险） | `docs/risk-analysis.md` | 14 条危害，每条指回控制代码与 QRS 需求；缺概率量化与临床输入 | 部分验证 |
| QRS-R-003 | 报警系统满足 IEC 60601-1-8（优先级、声音、视觉编码、延迟） | `src/vitalstrip.cpp` | 两级优先级 + 可区分音调（三连/两声）；缺声压级与脉冲群验证、视觉编码规范、报警延迟量化 | 部分验证 |
| QRS-R-004 | 临床场景验证（真实术式、真实设备、真实 PACS） | — | — | 缺口 |

## 证据索引

| 提交 | 覆盖的条目 |
| --- | --- |
| `1b3da06` | QRS-T-001、QRS-T-003、QRS-T-005 |
| `78bae2e` | QRS-T-004 |
| `6e7077c` | QRS-N-001、QRS-N-002、QRS-N-003 |
| `6da4b92` | QRS-D-001 |
| `1a81932` | QRS-T-002 |
| `eab4848` | QRS-A-001、QRS-A-002 |
| `1adf384` | QRS-I-001 |
| `9ad0613` | QRS-I-002 |
| `9c1efae` | QRS-I-004 |
| `af1db94` | QRS-I-003 |
| `20e7385` | QRS-N-004、QRS-F-002 |
| 本批（报警分级） | QRS-N-005、QRS-N-006 |
| 本批（多例选择） | QRS-I-005 |
| 本批（重启恢复） | QRS-F-003 |
| 本批（风险分析） | QRS-R-002 |
| 本批（取回中心跳） | QRS-I-006 |
| 本批（单实例） | QRS-A-003 |
| 本批（HU 窗预设） | QRS-T-007 |
| 本批（体积耗时口径与原生接管） | QRS-T-008 |
| 本批（手动调窗） | QRS-T-009 |
| 本批（去骨与切面） | QRS-T-010 |
| 本批（窗计算单测） | QRS-T-011 |
| 本批（台账轮转） | QRS-T-012 |

统计不手写：`make trace` 每次重新数一遍，缺口或引用失效会直接报错。

## 怎么用这张表

- 加功能时先加一行需求，再写实现，最后补验证；没有验证方式的行不允许标「已验证」。
- 评审时只看「缺口」与「部分验证」两列，它们就是下一批工作的候选清单。
