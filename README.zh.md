# lume-qt

> [English](README.md) | **简体中文**

Lume 的 Qt 桌面工作台（医疗监护演示）· Qt WebEngine / C++ / React（Vite + TypeScript）。

> **非临床用途**：本项目是科研/工程演示，不是医疗器械，不能用于诊断、治疗或临床决策。
> 隐私与合规审计、数据流向和上线前检查表见 [`docs/privacy-compliance.md`](docs/privacy-compliance.md)。
> 系统架构、线程模型、原生/Web 分页与 DICOM/PACS、审计-Lume 链路见 [`docs/architecture.md`](docs/architecture.md)。

## 技术栈

| 层 | 技术 |
|---|---|
| 桌面宿主 | Qt 6 WebEngine (`QWebEngineView`)，C++17 |
| C++ ↔ JS 通信 | Qt WebChannel（`QWebChannel` + `qwebchannel.js`） |
| 医学影像 | DCMTK 3.7 解析 DICOM 序列，后台线程重建三维 uint8 体积 |
| 前端 | React 19 + Vite 6 + TypeScript |
| 构建 | CMake + Ninja（C++），npm（前端） |

## 架构

```
┌──────────────────────────── lume-qt ────────────────────────────┐
│  C++ (Qt)                                    │  React (Vite)         │
│                                             │                       │
│  MainWindow ── QWebEngineView                │  App.tsx              │
│       │                                      │    │                  │
│       │  QWebChannel (bridge) ───────────────┼──→ qwebchannel.ts     │
│       │  app:// scheme (AppSchemeHandler) ───┼──→ index.html         │
│                                             │                       │
│  Bridge (QObject)                            │  resources/web/*.js   │
└──────────────────────────────────────────────────────────────────────┘
```

- **`app://app/index.html`**：前端构建产物经自定义 URL scheme（`AppSchemeHandler`）从 `resources/web/` 提供，规避 `file://` 下 SPA 的限制。
- **`bridge`**：C++ 侧 `Bridge` 对象经 `QWebChannel` 暴露给 JS，支持方法调用（`Q_INVOKABLE`）与信号推送（双向通信）。

## 目录结构

```
lume-qt/
├── CMakeLists.txt              # 构建配置：Qt6 + 前端构建集成
├── src/
│   ├── main.cpp                # 入口：注册 app:// scheme
│   ├── mainwindow.h/.cpp       # 主窗口 + QWebEngineView + WebChannel
│   ├── bridge.h/.cpp           # C++↔JS 桥接对象
│   └── appschemehandler.h/.cpp # app:// 静态资源处理器
├── frontend/                   # React 前端 (Vite + TS)
│   ├── package.json
│   ├── vite.config.ts          # 输出到 ../resources/web
│   ├── index.html
│   └── src/
│       ├── main.tsx
│       ├── App.tsx             # 桥接演示界面
│       └── qwebchannel.ts      # QWebChannel 类型安全封装
├── resources/web/              # 前端构建产物（生成，gitignored）
└── scripts/build.sh            # 一键构建
```

## 环境要求

- **macOS / Linux**（Windows 亦支持，路径需调整）
- Qt 6.5+，含 `WebEngineWidgets`、`WebChannel` 模块
  - macOS Homebrew：`brew install qt`（含 qtwebengine、qtwebchannel）
- **DCMTK** 3.6+（DICOM 体积解析，CMakeLists 通过 `find_package(DCMTK REQUIRED)` 强依赖）
  - macOS Homebrew：`brew install dcmtk`
- **Google Test**（仅 C++ 单元测试需要）：`brew install googletest`
- CMake ≥ 3.21，Ninja（或 make）
- Node.js ≥ 18，npm ≥ 9

## 构建与运行

### 推荐：Makefile

```bash
make            # 完整构建（前端 + C++）
make run        # 构建并运行（加载 app:// 构建产物，默认模式）
make dev        # 开发模式：Vite 热重载 + 应用（自动清理 5173 残留后启动）
make dev-hot    # 只启动 Vite dev server，另开终端手动运行应用
make dev-stop   # 清理残留的 Vite dev server（打印 lsof 明细并等待端口释放）
make dev-rebuild # 全量重建后运行（旧的 make dev 行为）
make frontend   # 仅构建前端
make clean      # 清理 build 与前端产物
make rebuild    # clean + 完整重建
make run-tests  # 运行单元测试
make perf-bench # 运行性能基准测试
make help       # 查看所有目标
```

`make run` 会依次完成 npm install、CMake 配置、C++ 编译，然后启动应用。

### 脚本方式

```bash
./scripts/build.sh
./build/lume-qt
```

### 分步构建

```bash
# 1. 前端
cd frontend && npm install && npm run build && cd ..

# 2. C++
cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt -G Ninja
cmake --build build

# 3. 运行
./build/lume-qt
```

> CMake 的 `frontend-build` 目标带有 staleness 检查：仅当前端源码比 `resources/web/` 新时才重新跑 `npm run build`，所以改 C++ 一行不会触发整次前端构建。它会同时把 `qwebchannel.js`（Qt 安装的 `share/qt6/webchannel/` 下）拷贝到 `resources/web/`。

### 前端开发模式（热重载）

```bash
make dev        # 一条命令：清理残留 → 启动 Vite dev server（5173）+ Qt 应用，Ctrl-C 一并退出
make dev-hot    # 只起 Vite（同样先清残留），另开终端执行：QTRS_DEV_URL=http://localhost:5173 ./build/lume-qt
make dev-stop   # 单独清理残留；也可用于排查端口状态
DEV_KILL_STALE=0 make dev   # 关闭自动清理：端口被占用时改为报错退出
```

`make dev` 通过环境变量 `QTRS_DEV_URL` 让应用加载 `http://localhost:5173` 而非 `app://app/index.html`；前端改动经 Vite HMR 即时生效，无需重编 C++ 或重新打包。

- **QWebChannel 在 HTTP 模式下同样可用**：transport 由 `QWebEnginePage::setWebChannel` 注入，与页面 scheme 无关。`qwebchannel.js` 由 `frontend/vite.config.ts` 在启动时同步到 `frontend/public/`（Vite 静态根），与生产构建同源；该文件是生成物，已 gitignored。
- **dev server 固定 5173 端口**（`strictPort: true`）：`make dev` / `make dev-hot` 启动前会**自动清理残留**——按监听端口找到旧进程、打印 `lsof` 明细（进程名 / PID / 监听地址）后结束，并等待端口真正释放（最多 5 秒）再启动新服务，避免应用静默连到过期页面。退出时按监听端口清理，不会残留孤儿 Vite 进程。若不想自动结束进程，用 `DEV_KILL_STALE=0 make dev` 退回严格报错模式。
- `make dev-stop` 是同一套清理逻辑的独立入口：空闲时直接返回，遇到结束不掉（如权限不足）的进程会报错并以非零码退出，方便脚本判断。
- **Qt 侧必须用命名（持久化）`QWebEngineProfile`**：实测 Qt 6.12 下 off-the-record profile（无 name 构造）不会把 `app://` 请求投递给自定义 scheme handler，页面会直接空白。代价是状态写在 `~/Library/Application Support/research/lume-qt/`，因此不要同时运行多个实例；`make dev` 已做启动前自动清理与退出清理，避免残留实例。
- `make dev-rebuild` 保留旧的「清空后全量重建再运行」行为，不依赖 dev server，适合排查构建产物相关问题。

### 运行参数一览（环境变量）

所有开关都只在启动时读一次；除特别注明外，`0`/未设都表示「关闭」。

**通用与诊断**

| 变量 | 默认 | 作用 |
| --- | --- | --- |
| `QTRS_DEV_URL` | 未设（`app://app/index.html`） | 改从 Vite dev server 加载；只接受 http(s) + loopback（`localhost` / `127.0.0.1` / `::1`），远程值会被忽略 |
| `QTRS_TOPMOST` | 未设 | 存在即窗口置顶（调试截图用） |
| `QTRS_RELOAD_MIN` | `30` | 每 N 分钟换 profile+page 重建渲染进程；`0` 关闭（见 H-21） |
| `QTRS_MUTE_SIGNALS` | 未设 | `1` = 不把波形样本转发给网页（原生条照常画），用于内存归因 |
| `QTRS_SAMPLE_DIV` | `1` | 每 N 个波形样本只转发 1 个给网页（降频诊断） |
| `QTRS_CLOCK_OFFSET_MS` | `0` | 给模拟设备时钟加偏移（毫秒），验证「设备时基」下的延迟扣除 |
| `QTRS_FAULT` | 未设 | 逗号分隔通道名（`ecg,spo2,eeg,emg`），启动 3 s 后停掉对应通道，注入中断 |
| `QTRS_AUDIT_MAX_MB` | `8` | 单份台账文件上限（MB），超过即轮转 |
| `QTRS_AUDIT_RETAIN_DAYS` | 未设（不清理） | 按文件名日期清理旧台账；显式设为天数才启用 |

**模拟设备与报警**

| 变量 | 默认 | 作用 |
| --- | --- | --- |
| `QTRS_SIM_HR` | 未设（模拟波动） | 固定心率数值 |
| `QTRS_SIM_SPO2` | 未设（模拟波动） | 固定血氧数值（如 `86` 触发高优先级报警） |
| `QTRS_ALARM_SPO2_HIGH` / `QTRS_ALARM_SPO2_MID` | `90` / `92` | 高/中优先级 SpO₂ 阈值；限 `[50,100]`，中<高时自动对齐 |
| `QTRS_ALARM_HR_HIGH` / `QTRS_ALARM_HR_MID` | `50` / `55` | 高/中优先级心率阈值；限 `[20,200]` |
| `QTRS_ALARM_MUTE_SEC` | `120` | 静音默认时长，夹到 `[5,300]` s |
| `QTRS_ALARM_SOUND` | 开 | `0` 关闭报警音（判定与红框照常） |
| `QTRS_LUME_URL` | 未设 | Lume 基地址，如 `http://127.0.0.1:8090`；未设则完全不建网络栈 |
| `QTRS_LUME_TOKEN` | 未设 | 上行/配置/AI 请求统一带 `Authorization: Bearer …`；用于前置反代鉴权，日志不打印值 |

**体数据与窗值**

| 变量 | 默认 | 作用 |
| --- | --- | --- |
| `QTRS_NATIVE_VOLUME` | 开 | `0` = 关闭原生 OpenGL 体渲染，退回网页侧 Volume3D |
| `QTRS_VOLUME_DIR` | 未设 | 启动 6 s 后加载指定 DICOM 目录（覆盖演示样例） |
| `QTRS_NO_RESTORE` | 未设 | 存在即不恢复上次体数据来源（冷启动对照） |
| `QTRS_KEEP_VOLUME_BYTES` | 未设 | 存在即保留 8-bit/HU 两份 CPU 副本（默认交接完释放，见[实现记录 23.9](docs/implementation-notes.md)） |
| `QTRS_VOLUME_UPLOAD_MB` | `16` | 纹理分帧上传的块大小；`0` 回到「一次传完」旧行为 |
| `QTRS_VOLUME_PRESET` | 读用户偏好 | `0`–`3` 预设；`4` = 自定义（配 `QTRS_VOLUME_WL`） |
| `QTRS_VOLUME_WL` | 读用户偏好 | 手动窗 `窗宽,窗位`（如 `800,150`） |
| `QTRS_VOLUME_COLOR` | 读用户偏好 | `1` 伪彩 / `0` 灰阶；非空即覆盖偏好 |
| `QTRS_VOLUME_BONE` | 读用户偏好 | `1` 去骨 / `0` 保留；仅 CT 生效 |
| `QTRS_VOLUME_CLIP` | 读用户偏好 | 切面 `轴,位置`，轴 `0`–`2`，位置 `0`–`1`（如 `1,0.6`） |

**PACS**

| 变量 | 默认 | 作用 |
| --- | --- | --- |
| `QTRS_PACS` | 未设 | `host:port:calledAE`；未设则不探测 |
| `QTRS_PACS_MODE` | `auto` | `auto`（先 C-GET，不支持则回落 C-MOVE）/ `get` / `move` |
| `QTRS_PACS_STORE_AE` | `QTRSSTORE` | C-MOVE 接收端 AE Title |
| `QTRS_PACS_STORE_PORT` | `11113` | C-MOVE 接收端端口 |
| `QTRS_PACS_HEARTBEAT_MS` | `30000` | C-ECHO 心跳间隔；`0` 关闭 |
| `QTRS_PACS_AUTO_GET` | 未设 | `1` = C-FIND 命中第一例即取回；数字 = 延时毫秒（`<=1` 视为 0） |
| `QTRS_PACS_TLS` | 未设（明文） | `1` 启用 DICOM TLS；`0`/`false` 关闭 |
| `QTRS_PACS_TLS_CA` | 未设 | CA 证书路径；启用 TLS 且 verify≠ignore 时必填 |
| `QTRS_PACS_TLS_CERT` / `QTRS_PACS_TLS_KEY` | 未设 | 客户端证书/私钥 PEM；启用 TLS 时必填 |
| `QTRS_PACS_TLS_VERIFY` | `require` | `require` / `check` / `ignore` |
| `QTRS_PACS_RETAIN_DAYS` | 未设（不清理） | 启动时按目录 mtime 清理旧检查；显式设为天数才启用 |

## C++ ↔ JS 通信示例

**JS → C++（调用方法）：**

```ts
import { connectBridge } from './qwebchannel'

const bridge = await connectBridge()
const info = await bridge.getPlatformInfo()   // 返回 Promise
const sum = await bridge.add(3, 4)
```

**C++ → JS（信号推送）：**

```cpp
// bridge.h
signals:
    void dataFromCpp(const QVariant &data);
```

```ts
bridge.dataFromCpp.connect((payload) => {
  console.log('收到 C++ 推送', payload)
})
```

## 医疗影像实时数据模拟

应用内置一个医疗监护面板（`MedicalMonitor.tsx`），演示 Qt WebEngine 的实时渲染能力：

- **数据源**：C++ 侧 `Bridge` 用 `QTimer` 实时合成——
  - `ecgSample(double)` 每 40ms 推送一个心电图波形样本（P-QRS-T 高斯合成 + 噪声）
  - `vitalsReady(map)` 每 600ms 推送生命体征快照（心率/血氧/呼吸/血压/体温，随机游走波动）
- **渲染**：前端 Canvas 绘制——
  - 滚动 ECG 波形（含网格、扫描头）
  - 模拟超声实时扫描影像（扇区视野 + 组织噪点漂移 + 多普勒血流 + 往复扫描线）
- **控制**：`bridge.startMedicalSimulation()` / `stopMedicalSimulation()`，面板挂载时自动启动

`make run` 启动后，窗口顶部即显示医疗监护面板，实时刷新。

> **纯浏览器演示**：无 Qt 桥接（如 `npm run dev` 直接打开）时，前端自动切换为 JS 侧模拟数据源（与 C++ 逻辑一致），便于脱离桌面壳预览实时效果。

### 神经内科介入模拟

医疗面板下方另有一块 **神经内科介入监测** 面板，模拟神经干预场景：

- **数据源**：C++ 侧 `Bridge` 用 `QTimer` 实时合成——
  - `eegSample(double)` 每 50ms 推送脑电图样本（α/β/γ/δ 波段叠加 + 偶发棘波）
  - `emgSample(double)` 每 100ms 推送肌电图样本（基线噪声 + 肌肉收缩爆发）
  - `neuralSpike(double)` 每 75ms 推送神经元活动样本（背景振荡 + 随机动作电位）
  - `interventionReady(map)` 每 500ms 推送神经介入事件（TMS/DBS/ECT/NF）
- **渲染**：三条 Canvas 波形（EEG/EMG/神经元）+ 介入事件实时列表
- **控制**：`bridge.startNeurologySimulation()` / `stopNeurologySimulation()`，面板挂载时自动启动

> **介入类型**：TMS 经颅磁刺激、DBS 深部脑刺激、ECT 电抽搐治疗、NF 神经反馈。

### 三维神经脉络可视化

神经学面板下方内置 **Three.js（WebGL）3D 神经脉络**，与 EEG 棘波联动：

- **程序化生成**（无外部模型文件）：90 个神经元按解剖学轮廓布局（大脑扁椭球 + 脑干 + 脊髓），每个神经元连接最近 2 个邻居形成突触网络
- **神经脉冲**：60 个脉冲粒子沿突触路径实时传导（金色加性发光），EEG 棘波（|值| > 0.6）时触发 1-3 个脉冲发射
- **交互**：`OrbitControls` 拖拽旋转 / 滚轮缩放 / 平移，自动缓慢旋转
- **构建**：`three` 经 `manualChunks` 独立分包（主 bundle ~260KB + three ~520KB）

## DICOM 三维体积可视化

除实时监护数据外，C++ 侧还集成了 DICOM 解析能力，用于加载真实医学影像序列并在前端做三维体积渲染：

- **解析**：`DicomReader`（基于 DCMTK）扫描目录下所有 `.dcm`，按空间 Z（无 Z 时按 `InstanceNumber`）排序，最近邻重采样到 `128³`，灰度归一化为 `uint8`，经 `std::thread` 后台线程解析避免阻塞 UI。
- **健壮性**（均由 `tests/bridge_test.cpp` 的 `DicomReaderRobustness.*` 覆盖）：
  - 矩阵尺寸不一致的切片（如序列里混入不同 Rows/Columns）按出现次数最多的尺寸为基准剔除，计入 `meta.skipped`；不再按首张切片的步长索引全部切片（那会越界读）；
  - Z 位置缺失用独立标记判断，`z == 0` 不再被当作"没有位置"（否则居中采集的中间层会排到最前，串层）；
  - 缺少 `RescaleSlope` / `RescaleIntercept` 的序列按 1/0 处理。DCMTK 的 `findAndGetFloat64` 在标签缺失时会把出参写成 0，曾导致 MR 序列整卷全零、前端黑屏（样例脑部 T1 恰好显式写了 1/0，掩盖了这个坑）；
  - 像素以 `uint16` 原始样本保存，符号与 rescale 在采样时应用：384 张 384×274 的序列常驻约 77MB，此前用 `double` 存是约 308MB；
  - `meta.spacing[2]` 取相邻层间距的中位数，缺失 Z 时退回 `SliceThickness`（层厚 ≠ 层间距，有 gap 时差异明显）。
- **取消**：解析支持协作式取消（每个文件 / 每个 Z 层检查一次）。`~Bridge` 先置 `alive=false` 再置 `cancel=true` 后 join，因此关窗不会被整卷解析拖住数秒。
- **桥接**：`bridge.loadDicomVolume(directory)` 触发解析，结果通过 `dicomVolumeReady(meta, base64Pixels)` 信号回传 JS（base64 编码的 `uint8` 体积 + `dims`/`spacing`/`slices`/`skipped` 元数据）；失败发 `dicomVolumeError`。
- **样例数据（本地自备，不入库）**：`bridge.dicomSamplePath()` 返回 `resources/dicom/brain-t1`（约 384 张脑部 T1 切片）。该序列约 78MB，已从版本库移出并加入 `.gitignore`，需要自己放一份到该路径，否则 DICOM 演示面板会报“目录不存在”，相关单测会自动 skip。
- **渲染**：前端 `Volume3D.tsx` 用 Three.js 体绘制/切片渲染该体积；脱离 Qt 时由 `syntheticVolume.ts` 程序化合成一个合成脑体，保证纯浏览器预览也能看到体积效果。

> 注意：CMake 配置阶段即 `find_package(DCMTK REQUIRED)`，未安装 DCMTK 会直接配置失败——这是本项目的硬依赖，不同于 Qt 可可选模块。

## 渲染性能

长时间浸泡（10 分钟稳态）的记录见 [`docs/soak-2026-11-06.md`](docs/soak-2026-11-06.md)：
主进程 RSS/线程稳定、看门狗 0 误报；但 Web 渲染进程 RSS 线性增长（JS 堆恒为 31 MB，
属 Chromium 原生侧）。已做运行期兜底：默认每 30 分钟换 profile+page 重建一次渲染进程，
内存回到基线（实测 3 轮均 93 MB），`QTRS_RELOAD_MIN` 可调间隔、0 关闭。

页面上同时跑着两个 WebGL 场景（神经脉络、DICOM 体渲染）加 5 个 2D canvas。原先它们各自按
`requestAnimationFrame` 全速重绘，即使整块面板滚出视口也不停——这是"整页卡顿"的主因。现在的约定：

- **离屏暂停**：每个渲染循环用 `IntersectionObserver` 判断宿主元素是否在视口内，配合
  `document.hidden`，滚出视口或窗口切后台时整条循环直接跳过更新与绘制（`framePacing.ts`）。
- **帧率分级**：DICOM 体渲染按 30fps 刷新（`autoRotate` 观感几乎无差别，GPU 开销减半）；
  超声扫描 30fps；ECG / EEG / EMG 波形改为"有新样本才重绘"（样本本身只有 10–25Hz）。
- **体绘制成本控制**：像素比限制到 1.5（Retina 下按 2 倍渲染等于 4 倍片元），关闭 MSAA
  （全屏 ray-marching 没有几何边缘，MSAA 纯烧带宽），采样数 140 → 96。
- **2D 绘制去冗余**：背景网格改成离屏层缓存后 `drawImage` 贴上；超声的径向渐变按画布尺寸
  缓存；粒子不再每帧拼 `rgba(...)` 字符串，改用 `globalAlpha`。

实测（Apple M3 / Qt WebEngine ANGLE Metal，单实例）：优化前首屏约 52fps、最差帧 100ms+；
优化后首屏 60fps、滚到体渲染面板 55–60fps。注意同时开多个实例会争抢 GPU，帧率会明显下降，
测的时候只留一个。


## 文档索引

- [docs/demo-handoff.md](docs/demo-handoff.md)：演示版交付说明、启动方式、最终验证与已知边界
- [docs/architecture.md](docs/architecture.md)：系统架构、线程模型、原生/Web 分页、DICOM/PACS 与审计-Lume 链路
- [docs/implementation-notes.md](docs/implementation-notes.md)：从本 README 拆出的逐条实现与验证记录（时间戳/延迟、原生波形条、PACS、去骨、Tabs、Lume AI 等）
- [docs/privacy-compliance.md](docs/privacy-compliance.md)：数据清单、出站路径、已实施控制、残余风险与上线前检查表
- [docs/lume-integration.md](docs/lume-integration.md)：事件契约、参考服务、配置下发与 Agent 复盘
- [docs/traceability.md](docs/traceability.md)：需求 → 实现 → 证据追溯矩阵
- [docs/risk-analysis.md](docs/risk-analysis.md)：风险/控制分析
- [docs/soak-2026-11-06.md](docs/soak-2026-11-06.md)：长时间运行浸泡记录

## 🧪 测试基础设施

### C++ 单元测试

我们集成了 **Google Test** 框架，用于 C++ 核心模块测试：

- **测试范围**：Bridge 类的基本功能、医疗模拟控制、错误处理、类型安全；DICOM 后台加载的竞态 / 析构安全 / 载荷可渲染性，以及 `DicomReaderRobustness.*`（z=0 排序、混合矩阵剔除、协作式取消）
- **测试目标**：`make run-tests` → 运行所有 C++ 单元测试
- **测试框架**：使用 Google Test，提供丰富的断言和测试生命周期管理

#### 示例测试

```cpp
TEST_F(BridgeTest, BasicOperations) {
    EXPECT_EQ(bridge->add(2, 3), 5);
    EXPECT_EQ(bridge->add(-1, 1), 0);
    EXPECT_EQ(bridge->add(0, 0), 0);
}

TEST_F(BridgeTest, PlatformInfo) {
    EXPECT_FALSE(bridge->getPlatformInfo().isEmpty());
    EXPECT_EQ(bridge->appName(), "lume-qt");
}
```

### 前端单元测试

**已接入（Vitest 2，node 环境，不需要 jsdom）。** `frontend/package.json` 有 `test` 脚本，
`make frontend-tests` 不再走「跳过」分支，而是真的跑用例：

```bash
cd frontend && npm test          # vitest run
# 或
make frontend-tests              # 与 C++ 测试一起：make test-all
```

当前覆盖的是延迟看板的纯逻辑（`src/latency.test.ts`，8 条）：

- 空快照里 `alarm` 行存在但 `samples=0`（界面据此不显示该行）
- 报警的 `recordSpan('alarm','receipt'|'painted')` 两个值都落到同一条样本上
- `channelHealth()` 不包含报警：没有报警不是「中断」，也不拉低 `n/6` 的分母
- 负数 / `NaN` 耗时被丢弃，不污染 P50/P99
- 持续通道仍按 100 ms 预算判慢，报警的 200 ms 预算不会串到 ECG
- 同一任务内的多次到达合并为一批，并记录批间隔
- 切回 Web 页时重置基线，隐藏期样本不进入 P50
- 窗口不在前台时状态为 `paused`，恢复后丢弃隐藏期积压，不把节流当慢

还没做的是 **React 组件测试**（挂载 `MedicalMonitor`、模拟 `QWebChannel` 信号、断言界面
文案）。那需要 `jsdom` + `@testing-library/react`，等组件边界稳定后再补；现在不引入，
避免为了「有组件测试」而把大量时间花在 mock 整个桥接层上。

### CI

**不接入 CI。** 这个项目的质量门就是本地两条命令：

```bash
make run-tests                      # C++ 单测（Google Test，经 ctest）
make frontend-tests                 # 前端单测（Vitest）
cd frontend && npm run typecheck    # 前端类型检查
```

Qt WebEngine + DCMTK 在 CI 镜像里装包成本高（部分 Ubuntu 仓库缺 `qt6-webengine-dev`），
为这点体量的研究项目维护一套远端流水线不划算。如果以后要上 CI，先在目标镜像里确认
`qt6-webengine-dev` / `libdcmtk-dev` 可安装，再补 workflow。

### 构建目标

```bash
# 运行所有单元测试
make run-tests

# 运行 C++ 单元测试
make bridge_test

# 运行前端单元测试
make frontend-tests

# 运行性能基准测试
make perf-bench

# 运行完整测试套件（C++ + 前端）
make test-all

# 检查需求追溯矩阵（ID / 状态 / 引用文件）
make trace
```

### 测试质量保证

- **自动化测试**：本地 `make test-all`（C++ Google Test + 前端 Vitest）为质量门；本项目不接入 CI（见上节）
- **追溯矩阵**：`make trace` 校验 `docs/traceability.md` 的结构与引用，缺口不会被悄悄改成「已验证」
- **质量门**：主目标与 `bridge_test` 共用同一套 `-Wall -Wextra -Werror -Wconversion` 编译选项，测试代码不比产品代码宽松；测试失败即以非零码退出
- **测试数据**：为每个测试提供可重现的数据
- **错误处理测试**：验证错误场景的处理
- **性能测试**：验证关键路径的性能指标

### 优势

- **代码质量**：通过测试确保代码正确性和健壮性
- **开发效率**：早期发现错误，减少回归风险
- **文档生成**：测试用例可作为文档
- **持续集成**：确保每次提交都经过质量验证

## 已知事项

- 首次构建 Qt 时若 Homebrew 为 `qt` 公式，`qwebchannel.js` 位于 `/opt/homebrew/opt/qtwebchannel/share/qt6/webchannel/`，CMake 会自动查找并拷贝；若未找到会在构建时输出 WARNING，需手动复制。
- 若需打包分发（`.app` / 安装包），可在 `CMakeLists.txt` 中启用 `MACOSX_BUNDLE`，并将 `resources/web/` 拷贝进包内资源目录。
- 启动后若日志出现 `[WebEngine] 前端加载失败`，现在会同时打印 `加载错误详情`（Chromium 的 errorDomain / errorCode / errorString），据此区分：前端产物缺失、自定义 scheme 未投递、或进程在 WebEngine 就绪前被终止。先确认 `resources/web/index.html` 存在，再检查是否有残留实例；`make dev` 会在启动前自动清理 5173 残留，也可随时单独执行 `make dev-stop`。
- 在**隔离沙箱 / 无 GUI 会话**的环境里启动 Qt 应用，WebEngine 渲染进程可能起不来并报 `loadFinished(false)`；此类失败属于运行环境限制，请在本机 Terminal（或 `make run` / `make dev`）正常桌面会话中运行。
