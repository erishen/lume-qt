# qt-react-shell

Qt WebEngine · C++ · React（Vite + TypeScript）桌面前后端一体壳项目。

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
┌─────────────────────────── qt-react-shell ───────────────────────────┐
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
qt-react-shell/
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
./build/qt-react-shell
```

### 分步构建

```bash
# 1. 前端
cd frontend && npm install && npm run build && cd ..

# 2. C++
cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt -G Ninja
cmake --build build

# 3. 运行
./build/qt-react-shell
```

> CMake 的 `frontend-build` 目标带有 staleness 检查：仅当前端源码比 `resources/web/` 新时才重新跑 `npm run build`，所以改 C++ 一行不会触发整次前端构建。它会同时把 `qwebchannel.js`（Qt 安装的 `share/qt6/webchannel/` 下）拷贝到 `resources/web/`。

### 前端开发模式（热重载）

```bash
make dev        # 一条命令：清理残留 → 启动 Vite dev server（5173）+ Qt 应用，Ctrl-C 一并退出
make dev-hot    # 只起 Vite（同样先清残留），另开终端执行：QTRS_DEV_URL=http://localhost:5173 ./build/qt-react-shell
make dev-stop   # 单独清理残留；也可用于排查端口状态
DEV_KILL_STALE=0 make dev   # 关闭自动清理：端口被占用时改为报错退出
```

`make dev` 通过环境变量 `QTRS_DEV_URL` 让应用加载 `http://localhost:5173` 而非 `app://app/index.html`；前端改动经 Vite HMR 即时生效，无需重编 C++ 或重新打包。

- **QWebChannel 在 HTTP 模式下同样可用**：transport 由 `QWebEnginePage::setWebChannel` 注入，与页面 scheme 无关。`qwebchannel.js` 由 `frontend/vite.config.ts` 在启动时同步到 `frontend/public/`（Vite 静态根），与生产构建同源；该文件是生成物，已 gitignored。
- **dev server 固定 5173 端口**（`strictPort: true`）：`make dev` / `make dev-hot` 启动前会**自动清理残留**——按监听端口找到旧进程、打印 `lsof` 明细（进程名 / PID / 监听地址）后结束，并等待端口真正释放（最多 5 秒）再启动新服务，避免应用静默连到过期页面。退出时按监听端口清理，不会残留孤儿 Vite 进程。若不想自动结束进程，用 `DEV_KILL_STALE=0 make dev` 退回严格报错模式。
- `make dev-stop` 是同一套清理逻辑的独立入口：空闲时直接返回，遇到结束不掉（如权限不足）的进程会报错并以非零码退出，方便脚本判断。
- **Qt 侧必须用命名（持久化）`QWebEngineProfile`**：实测 Qt 6.12 下 off-the-record profile（无 name 构造）不会把 `app://` 请求投递给自定义 scheme handler，页面会直接空白。代价是状态写在 `~/Library/Application Support/research/qt-react-shell/`，因此不要同时运行多个实例；`make dev` 已做启动前自动清理与退出清理，避免残留实例。
- `make dev-rebuild` 保留旧的「清空后全量重建再运行」行为，不依赖 dev server，适合排查构建产物相关问题。

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

## 术中化架构（时间戳 / 二进制体积 / 原生波形条）

把 demo 往手术室方向挪，做了三件事；每件都对应一条「原理性限制」的解法，而不是 UI 装修。

### 1. 全链路时间戳与延迟看板

- **同一时基**：C++ `Bridge::nowMicros()` 取 `system_clock` epoch 微秒；前端 `nowMicros()` 用
  `performance.timeOrigin + performance.now()` 换算成同一 epoch 微秒。差值即链路年龄。
- **载荷带戳**：`ecgSample/eegSample/emgSample/neuralSpike` 改为 `(value, tsUs)` 双参；
  `vitalsReady` / `interventionReady` / DICOM meta 的 map 内加 `tsUs`。
- **看板**：`latency.ts` 每通道保留 512 样本环形缓冲，`MedicalMonitor` 里 `LatencyBoard` 按 1Hz
  刷新，展示「到达 P50/P99」（采集 → JS 回调）与「上屏 P50/P99」（采集 → 真正绘制），
  P99 > 100ms 标红。当前只有 ECG 与 DICOM 体积采「上屏」样本。

实测（Apple M3，窗口前台单实例）：到达中位数 **0.3–1.0ms**（偶发 3ms）；
窗口底部的原生波形条走 Qt 信号直连 + `QPainter`，采集→绘制 P50 **0.2ms** / P99 **≈28ms**
（日志 `[VitalStrip]`），作为「不经 WebView」的同机对照。

> 测量陷阱：macOS 上窗口被遮挡/失焦时，Chromium 会把渲染与定时器节流到秒级，积压样本会让
> 看板读数涨到几百毫秒甚至数秒（且各通道 P50 会几乎相同）。这类数字是环境产物，不是链路性能。
> 测的时候把窗口置前后再读；调试可用 `QTRS_TOPMOST=1 ./build/qt-react-shell` 让窗口置顶。

### 2. DICOM 体积改二进制直传（去掉 base64）

结论先行：**2MB 级二进制不能走 QWebChannel**。实测把 `QByteArray` 放进信号第二参，前端收到的是
`String` 而不是 `ArrayBuffer`，且 2,097,152 字节到达后只剩 **2,097,145 个字符**——文本序列化
按 UTF-8 处理，非法字节序列被吃掉，数据已损坏。这条退路（省掉 base64）做不通。

最终方案是**虚拟二进制资源**：

- `Bridge` 解析成功后把体积存进 `m_volumeBytes`，信号 `dicomVolumeReady(meta, volumeUrl)` 只传
  URL：`app://app/__volume/current.bin`。
- `AppSchemeHandler::serveVolume()` 用 `QBuffer`（parent = job）以 `application/octet-stream`
  回给 Chromium；`MainWindow` 通过 `setVolumeProvider()` 注册取数回调（延迟求值，`m_bridge` 创建晚于注册，安全）。
- 前端 `fetch(volumeUrl).arrayBuffer()` 直接拿到 2,097,152 字节的 `ArrayBuffer`（实测日志
  `体数据已取回 2097152 bytes`），**零编码放大、零逐字节解码**；`Volume3D` 的 `VolumeInput`
  接受 `Uint8Array | ArrayBuffer`，`base64ToBytes` / `bytesToBase64` 已删除。
- 硬前提：`app://` 的 scheme 注册必须带 `FetchApiAllowed`（否则 `fetch` 直接 `Failed to fetch`）；
  本地资源同时关掉磁盘缓存（`NoCache`），避免「改了代码仍加载旧 bundle」。
- `Vite` 产物目录仍由同一 handler 服务，`job->reply` 的 MIME 与虚拟资源分支互不影响。

### 3. 原生关键波形条（ECG 不过 WebView）

`src/vitalstrip.cpp` 的 `NativeVitalStrip : QWidget`（固定 112px）挂在 `QWebEngineView` 下方：

- `bridge->ecgSample → pushEcgSample(value, tsUs)`、`bridge->vitalsReady → setVitals(map)` 直连，
  数据不出 Qt 事件循环，`paintEvent` 用 `QPainter` 画网格 + 600 点波形 + 右侧 HR/SpO₂/NIBP。
- 条体右侧画出「采集→绘制 P50/P99」，并每约 2 秒落一条 `[VitalStrip]` 日志；含演示级阈值报警（SpO₂ < 90 或 HR < 50 时红点 + 文案）。
- 意义：这是「关键波形到底该不该走 WebView」的最小可对照实现——同机同源数据，一条走 QWebChannel
  + Canvas，一条走 Qt 信号 + QPainter，延迟差一个数量级的量级可直接读出来。

### 4. 确定性：关闭渲染节流 + 信号看门狗 + 帧预算

术中与演示最本质的差别不是「快」，而是**失败必须可见**：信号停了不能继续画旧波形。
这一批做三件事。

**a. 关闭 Chromium 的遮挡/后台节流**（`src/main.cpp` 的 `disableRendererThrottling()`，在
`QApplication` 构造前 `qputenv`，若用户已自行配置则跳过）：

```
--disable-features=CalculateNativeWinOcclusion
--disable-background-timer-throttling
--disable-backgrounding-occluded-windows
--disable-renderer-backgrounding
```

启动日志会打印 `[Chromium] flags: ...`，`ps -Ao command | grep QtWebEngineProcess` 可核对
这些开关确实进了渲染进程（实测已生效，`CalculateNativeWinOcclusion` 在 `--disable-features` 列表里）。

**b. 信号看门狗**（`latency.ts::channelHealth()` + `MedicalMonitor` 的 `WatchdogStrip`，500ms 轮询）：

| 通道 | 判「中断」阈值 |
| --- | --- |
| ecg | 250ms |
| eeg | 400ms |
| emg | 700ms |
| neural | 600ms |
| vitals | 4s |
| volume | 30s（一次性加载） |

- 「中断」= 距最近一次收到样本超过阈值 → 红条 + 闪烁点 + `（画面为冻结帧）`；
- 「延迟超预算」= 最近 30 个样本的**到达中位数** > 100ms（用中位数而不是 P99：一次卡顿不该
  把状态钉在告警里，那是历史噪声不是当前链路状态）；
- 从未收到判 `pending`，不计入告警（启动本来就该等）；
- 状态变化落 `[Watchdog] ...`（`console.warn`）/ `[Watchdog] 信号恢复正常`（`console.log`），
  告警可回溯；「慢」连续 2 次采样才上报，避免抖动刷日志。

**c. 帧预算**：`recordFrame()` 只统计真正可见的 rAF 间隔，`frameStats()` 给出 P50/P99 与
`overBudgetRatio`（>33ms 即掉帧），看门狗条正常态显示「信号正常 · n/N 通道有数据 · 帧 P50/P99」。

验证记录（Apple M3，单实例）：

- 告警路径：把 `ecg` 阈值临时改成 5ms → `[Watchdog] ecg=stale(...)` 立即出现；还原 250ms 后消失。
- 真实告警：窗口未被激活的一次运行中出现
  `[Watchdog] ecg=stale(age=284ms, p50=301ms, p99=1191ms) eeg=slow(...)`，
  同期各通道到达中位数落在 **250–325ms**，与前台时的 **0.3–1.0ms** 相差两个量级。

> **本节局限（实测，别误解成已解决）**：上面四个开关只关掉了 Chromium 的**定时器/后台节流**，
> 并不能消除 macOS「窗口不在前台」时 QWebChannel 投递的批量迟发：实测遮挡状态下到达中位数
> 仍是 250–325ms，个别样本迟到 10s 以上（积压后一次性投递），恢复前台后要过一段时间才排空。
> 结论：**实时通路不能建立在这套 WebView 投递上**——这正是路线图第 3 步「关键波形原生化」的依据，
> 看门狗的作用是让这种劣化不可能被静默忽略。

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
    EXPECT_EQ(bridge->appName(), "qt-react-shell");
}
```

### 前端单元测试

> **状态：规划中，尚未接入。** 当前 `frontend/package.json` 未包含 `test` 脚本，也未引入 `jest` / `react-testing-library` 依赖，因此 `npm test` 与 `make frontend-tests` 均不会实际运行前端单测（`make frontend-tests` 会明确提示跳过而非静默失败）。以下为预期设计，待后续补齐依赖与测试用例。

计划使用 **Jest + React Testing Library** 进行前端组件测试：

- **测试范围**：MedicalMonitor 组件、数据流处理、用户交互
- **测试目标**：`npm test` → 运行所有前端单元测试
- **测试框架**：支持组件测试、数据断言和用户事件模拟

#### 预期示例测试

```typescript
test('renders vitals data', () => {
    render(<MedicalMonitor bridge={mockBridge} />);
    expect(screen.getByText('心率')).toBeInTheDocument();
});

test('handles ecg updates', () => {
    render(<MedicalMonitor bridge={mockBridge} />);
    act(() => mockBridge.ecgSample.emit(0.5));
    expect(screen.getByText('0.5')).toBeInTheDocument();
});
```

### CI

**不接入 CI。** 这个项目的质量门就是本地两条命令：

```bash
make run-tests                      # C++ 单测（Google Test，经 ctest）
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
```

### 测试质量保证

- **自动化测试**：本地 `make run-tests`（C++ Google Test）为质量门；本项目不接入 CI（见上节）
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
