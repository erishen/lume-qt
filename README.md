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

### 5. 关键通路原生化（ECG + SpO₂ 双通道）

上一节实测已经说明：WebView 那条路在窗口失焦/被遮挡时中位数 250–325ms、偶发十几秒，
**不能作为术中关键波形的显示与报警通路**。这一步把两路关键波形搬到原生条上：

- **新增通道**：`Bridge::spo2Sample(double value, double tsUs)`，40ms 一个样本（stress 时 5ms），
  波形是带重搏切迹的体积描记波；与 ECG 一样遵循「每样本带 epoch 微秒时间戳」的契约
  （`tests/bridge_test.cpp::Spo2WaveSharesTimestampContract` 守着这条）。
- **只连原生**：`MainWindow::setupNativeStrip()` 里把 `ecgSample` / `spo2Sample` 直连
  `NativeVitalStrip`，**WebView 侧不订阅 SpO₂**——关键通路的显示与判定不经过 IPC。
  WebView 仍保留同一路 ECG 画布，作为「原生 vs WebView」的同机对照。
- **原生条结构**：上下两条 lane（ECG 绿 / SpO₂ 蓝），右侧列 HR/SpO₂/NIBP、两条 lane 各自的
  采集→绘制 P50/P99、报警判定结果。
- **原生看门狗**：每条 lane 自己判中断（ECG 250ms / SpO₂ 400ms），超时就在波形上盖
  `信号中断 XXXX ms · 冻结帧` 的红条，并落 `[VitalStrip] <通道> 信号中断` / `信号恢复` 日志。
  与前端看门狗是**两套独立判定**，任一侧失效另一侧仍能报警。
- **报警判定原生化**：SpO₂ < 90 或 HR < 50 在 `paintEvent` 里直接判、直接画，不依赖前端渲染成功。

实测（Apple M3，单实例）：

- 稳态：`[VitalStrip] ECG P50 0.24 P99 21.34 ms · SpO2 P50 0.13 P99 17.91 ms（n=239）`
- 同一时刻前端看板：到达 P50 291–367ms（窗口未激活时），量级差约 1000 倍
- 中断验证（`QTRS_FAULT=spo2,ecg`，启动 3 秒后停掉这两个定时器）：
  - 原生：`[VitalStrip] ECG 信号中断：699 ms 无样本（画面为冻结帧）`、`SpO2 信号中断：699 ms`
  - 前端：`[Watchdog] ecg=stale(age=619ms, ...)`
  - 画面：两条 lane 同时盖红条 `信号中断 2507 ms · 冻结帧`

故障注入开关（演示/回归都用得上）：

```bash
QTRS_FAULT=spo2,ecg ./build/qt-react-shell   # 启动 3 秒后停掉指定通道
```

### 6. 设备接入抽象层（DeviceProvider）

前面几步把「显示与判定」从 WebView 里搬了出来，但采样与传输仍然堆在 `Bridge` 里：
`Bridge` 既是设备、又是传输层，换真实设备（监护仪串口/网口、导联盒、导航系统、
DICOM 网络 C-FIND/C-GET/STOW）时没有落点。这一步把它们分开。

- **`src/deviceprovider.h`**：设备抽象接口。波形通道 `ecgSample` / `spo2Sample` /
  `eegSample` / `emgSample` / `neuralSpike` 都是 `(value, tsUs)`，快照通道
  `vitalsReady` / `interventionReady` 传 map。启停分医疗与神经两组，要求幂等。
  另有 `kind()` / `description()`，以及 `hasHardwareClock()`——时间戳是否来自设备侧。
- **`src/simulatorprovider.h/.cpp`**：内置模拟器，`DeviceProvider` 的一种实现。
  原来 `Bridge` 里的定时器、波形合成（ECG/Pleth/EEG/EMG/neural）、生命体征随机游走、
  神经干预事件、`QTRS_FAULT` 故障注入都搬到这里。
- **`Bridge` 退化为传输层**：构造时创建 provider，把它的 7 条信号原样 `connect` 到自己的
  信号上（前端契约完全不动）；`startMedicalSimulation()` 等仍可被 JS 调用，内部只是委派。
  转发不重新打时间戳——采集时刻由 provider 决定。

启动日志会明确当前用的是哪种设备：

```
[Bridge] provider: simulator · 内置生理信号模拟器（ECG 40ms / SpO₂ 40ms / vitals 600ms，宿主墙钟时基） · 硬件时基: 否（宿主墙钟）
```

**接入真实设备时要做的事**（下一步）：

1. 写一个 `XxxProvider : DeviceProvider`，在 `startMedical()` 里打开串口/建 TCP 连接/
   发 C-FIND，收到数据就 `emit ecgSample(value, tsUs)`。
2. 时间戳用**设备侧**的；如果设备不给时间戳，就只有宿主墙钟可用，此时
   `hasHardwareClock()` 必须返回 `false`——上层据此知道「采集→上屏」这段延迟
   只能做相对测量，不能当成临床延迟指标。
3. 在 `Bridge` 构造函数里把 `new SimulatorProvider(this)` 换成新 provider
   （或按 `QTRS_PROVIDER` 环境变量选择），前端与原生条不用改。

### 7. 时基与延迟口径（设备时钟 vs 宿主时钟）

接上真实设备后，`tsUs` 来自**设备时钟**，与宿主墙钟之间存在一个**未知常量偏移**。
如果前端还按「同一时基」直接相减，看到的就不是延迟，而是「时钟差」——
偏移 5 秒的设备会让看板稳定显示 5000ms，甚至负数。

单侧测量在原理上拿不到偏移与延迟各自的值，只能拿到二者之和：

```
raw = 宿主now − tsUs = 时钟偏移 + 真实延迟
```

偏移短期恒定、真实延迟有下界，所以**最近窗口内 raw 的最小值就是偏移的下界估计**；
扣掉它，剩下的残差才是链路延迟（比真实延迟略小，不会虚高）。断流期间没有新样本进窗口，
偏移估计不会漂移，因此中断判定仍然成立。

落地：

- **`DeviceProvider::hasHardwareClock()`** 如实声明时基来源；`Bridge::providerInfo()`
  把它和 `kind` / `description` 一起交给前端。
- **`frontend/src/latency.ts`**：`setClockSource()` 切口径，`residualUs()` 做偏移估计。
  宿主同源时偏移≈0，退回直接相减，与之前的行为完全一致。
- **DICOM 体积例外**：它的 `tsUs` 是解析完成时刻（`Bridge::nowMicros`，宿主墙钟），
  不随设备时基变化，因此列在 `HOST_STAMPED_CHANNELS` 里，不参与偏移扣除。
- **原生条同步修正**：`NativeVitalStrip` 原来直接 `now - tsUs`，设备时钟下会算出
  `-4999ms` 甚至让看门狗误判中断。现在每个 lane 在样本到达时算残差、记下接收时刻，
  年龄 = `(now − 接收时刻) + 到达时残差`，与时间戳来自哪个时钟无关。
- **看板文案**：设备时钟时显示 `设备时钟快/慢 X.X s · 偏移已扣除`，并在说明行里写清
  「这是扣偏移后的残差，不是绝对延迟」。

验证开关（模拟设备时基与宿主不同源）：

```bash
QTRS_CLOCK_OFFSET_MS=5000 ./build/qt-react-shell
# 日志： [Bridge] provider: ... 设备时钟时基，偏移 5000ms ... · 硬件时基: 是
```

实测（偏移 +5 s，即设备时钟比宿主快 5 秒）：

| | 修复前 | 修复后 |
| --- | --- | --- |
| 看板头部 | `C++ 打时间戳 · 同一时基`（假值） | `设备时钟快 5.0 s · 偏移已扣除` |
| ECG 到达 P50 | 未扣偏移，会多出 5000ms | 229 ms（真实残差） |
| 原生条 ECG 采集→绘制 | `P50 -4999.81 ms`（负值，看门狗误判） | `P50 0.29 ms / P99 40.88 ms` |
| DICOM 体积 | 5008123 ms（宿主时间戳被按设备时钟扣） | 26105 ms（按宿主口径，不再被污染） |

回归保护：`tests/bridge_test.cpp::SimulatorClockTest.DeviceClockOffsetIsAppliedToSamples`
断言注入偏移后样本时间戳确实带偏移、且 `hasHardwareClock()` 返回 true。

### 8. 故障安全与可追溯：结构化事件台账

只往 stderr 打日志不够——那是给人看的，不可解析、不可统计、不可归档。术中系统里
「刚才到底发生了什么」必须能事后复核：某通道什么时候断的、断了多久、是谁报的警。

- **`src/auditsink.h/.cpp`**：JSON Lines 台账，一行一条。固定字段
  `ts`（ISO 带毫秒）/ `tsUs`（epoch 微秒）/ `session`（会话 id）/ `type`，
  业务字段原样并入且**不能覆盖固定字段**。按天切分：
  `~/Library/Application Support/research/qt-react-shell/audit/audit-YYYYMMDD.jsonl`
  （Linux 上是 `~/.local/share/research/qt-react-shell/audit/`）。
- **两个独立来源都写进同一份台账**，用 `source` 区分：
  - `native-strip`：原生条的中断 / 恢复 / 报警边沿（`signal.stale` / `signal.recover` /
    `alarm.on` / `alarm.off`，带 `channel`、`ageMs`、`thresholdMs`）；
  - `web-frontend`：前端看门狗的 `watchdog.bad` / `watchdog.ok`（带 `states`、`ageMs`、`p50Ms`）。
    前端通过 `bridge.logAuditEvent(...)` 提交，C++ 侧只认 `type`，其余字段照收。
- **C++ 侧事件**：`session.start`（provider 与时基）、`provider.start`（医疗/神经分组）、
  `dicom.volumeReady` / `dicom.volumeError`。
- 报警只在**边沿**记一次（`m_alarmActive` 比对），不会每帧刷台账。

一次故障注入（`QTRS_FAULT=ecg,spo2`）产生的台账（节选）：

```json
{"ts":"2026-11-05T15:08:06.821","tsUs":...,"session":"20261105T150806-55042","type":"session.start","provider":"simulator"}
{"ts":"2026-11-05T15:08:08.560","tsUs":...,"type":"dicom.volumeReady","bytes":2097152,"dims":[128,128,128]}
{"ts":"2026-11-05T15:08:09.934","tsUs":...,"type":"watchdog.bad","source":"web-frontend","states":"ecg:stale,eeg:slow,..."}
{"ts":"2026-11-05T15:08:10.391","tsUs":...,"type":"signal.stale","source":"native-strip","channel":"ECG","ageMs":739.556,"thresholdMs":250}
{"ts":"2026-11-05T15:08:10.394","tsUs":...,"type":"signal.stale","source":"native-strip","channel":"SpO2","ageMs":739.502,"thresholdMs":400}
```

排查与统计：

```bash
f=~/Library/Application\ Support/research/qt-react-shell/audit/audit-$(date +%Y%m%d).jsonl
python3 -c "import json,sys;[print(json.loads(l)['type']) for l in open(sys.argv[1])]" "$f" | sort | uniq -c
```

边界说明：这是**工程级事件时间线**，不是合规证明。IEC 62304 的证据链还要版本冻结、
验证记录、变更控制与签核；本节的产出是那条链里「发生了什么」这一层。

### 9. 真实设备协议：DICOM 网络（C-ECHO / C-FIND）

前面把设备抽象成 `DeviceProvider`，但那只覆盖波形源。术中的影像与病人信息来自 PACS，
接入真实环境的第一个问题永远是两句话：「连得上吗？」「这例检查在库里吗？」。
本批把这两件事做成可验证的一小块——不是又加一层抽象，而是真的走 DICOM 网络。

- **`src/pacsclient.h/.cpp`**：`DcmSCU` 之上封装两个动作。
  - `echo()`：C-ECHO 连通性验证，返回往返毫秒；
  - `findStudies()`：STUDY 级 C-FIND，返回 `patientName / patientId / studyDate /
    studyInstanceUid / modality / series`。
  - 每次都开新关联再释放，不留长连接——术中网络抖动时长连接会留下半死状态，
    而下一次探测的开销只有几毫秒。
- **配置**：`QTRS_PACS=host:port:calledAE`（例 `QTRS_PACS=192.0.2.12:104:ORPACS`）。
  启动后 1.2 s 自动 C-ECHO，成功则立刻接着 C-FIND——接入现场的真实顺序就是
  「先验证连通、再找病人」，分两步手动触发只会让人忘了第二步。
- **线程模型**：网络调用在工作线程里跑，回主线程后才发信号。`PacsThreadHolder`
  自己负责 join：`std::thread` 析构时若仍 joinable 会直接 `std::terminate()`，
  这个坑在第一次实测就踩到（第二次请求替换 holder 时进程 abort）。
- **看板**：延迟看板头部会带一枚状态 chip——
  `PACS 已连通 13 ms · 1 例`（绿）/ `PACS 未连通 无法建立 TCP 连接…`（红）/ `PACS 未配置`（灰）。
  状态既能推（`pacsStatus` / `pacsStudies` 信号）也能拉（`pacsState()` 快照），
  因为启动即探测时信号往往早于 WebView 订阅，只靠信号会永远停在「未配置」。
- **台账**：`pacs.echo` / `pacs.echoFailed` / `pacs.find` / `pacs.findFailed`，
  带 host / calledAe / roundTripMs / studies / 原始错误 detail。界面显示人话文案，
  台账留 DCMTK 原文——排查时要的是原始码，看板上要的是一眼判读。

实测（本机 `dcmqrscp` 起在 127.0.0.1:11112，AE `QTRSQR`，用 `resources/dicom/brain-t1`
的切片经 `storescu` 入库）：

```
[PACS] C-ECHO → 127.0.0.1:11112 AE=QTRSQR
[PACS] C-ECHO 成功：13 ms
[PACS] C-FIND(STUDY) → 127.0.0.1:11112 AE=QTRSQR
[PACS] C-FIND 返回 1 例
```

停掉服务端后同一路径：

```
[PACS] C-ECHO 失败： 无法建立 TCP 连接（目标未响应或被网络策略拦截）
```

本地复现一套 PACS（仅验证用，不随仓库分发）：

```bash
mkdir -p /tmp/qtrs-pacs/db
cat > /tmp/qtrs-pacs/dcmqrscp.cfg <<'CFG'
NetworkTCPPort  = 11112
MaxPDUSize      = 16384
MaxAssociations = 16
AETable BEGIN
QTRSQR   /tmp/qtrs-pacs/db   RW (200, 1024mb)   ANY
AETable END
CFG
export DCMDICTPATH=/opt/homebrew/Cellar/dcmtk/3.7.1/share/dcmtk-3.7.1/dicom.dic
dcmqrscp -c /tmp/qtrs-pacs/dcmqrscp.cfg &
storescu -aet QTRS -aec QTRSQR 127.0.0.1 11112 resources/dicom/brain-t1/N2D_0001.dcm
```

已知边界（后续批次）：没有实现 C-GET / C-MOVE（真正把影像拉回本地并重建体积），
没有 TLS 与 AE 白名单，没有断线重连与退避；C-FIND 目前是全量 STUDY，未按病人/日期过滤。
这几项都属于接入真实 PACS 时必须补的，但现在这条链已经能回答「通不通」与「有没有」。

### 10. 把「库里有」变成「屏幕上能看到」：C-GET 取回整例并重建体积

C-ECHO / C-FIND 只解决「通不通、有没有」。术中真正需要的是：选中的这例检查，
影像要在几秒内出现在体渲染面板上。本批实现了 C-GET 取回整例，
并直接复用本地样例那一条解析链路（含重采样与 `app://` 二进制供数），不另起一套。

- **`PacsClient::retrieveStudy()`**：同一个关联上发 C-GET、收对方推来的 C-STORE，
  存储模式 `DCMSCU_STORAGE_DISK` + `setStorageDir(destDir)`；
  落盘目录 `~/Library/Application Support/research/qt-react-shell/pacs/<StudyInstanceUID>/`。
- **`Bridge::pacsRetrieveStudy()`**：C-GET 完成后自动 `loadDicomVolume(destDir)`，
  后台解析、不阻塞 UI；单次 DIMSE 超时 15 s。
- **前端**：看板头部 PACS chip 旁一枚「取回该例」按钮（取回按钮用的 StudyInstanceUID 来自
  `pacsState()` 快照）；`QTRS_PACS_AUTO_GET=1` 让 C-FIND 命中第一例后自动取回，
  现场联调时「连上就看到」，不用先点按钮。
- **台账**：`pacs.retrieve`（files / ms / studyUid / dir）+ `pacs.retrieveFailed`，
  与随后那条 `dicom.volumeReady`（bytes / dims / source）按时间对齐，
  事后可以回答「这台机器的这一屏，影像是从哪来的」。

三个串联的坑，都是「协议对了、文件名对了、逻辑却还在丢数据」这一类：

1. **存储类必须申请 SCP 角色。** C-GET 里同一关联上角色是反的——我们发检索请求是 SCU，
   同时接收对方推来的 C-STORE 是 SCP。只申请默认角色时，服务端每个子操作都报
   `No presentation context with requestor SCP role`，60 个子操作全失败。
   用 DCMTK 自带的 `getscu` 复现同样的错误，才确认这是协议协商问题、不是代码 bug：
   `addPresentationContext(uid, syntaxes, ASC_SC_ROLE_SCP)` 加上后立刻通。
2. **落盘文件名没有扩展名。** `DcmSCU` 写成 `MR.<SOPInstanceUID>`，而下游解析器
   只认 `*.dcm / *.dicom / *.DCM`——取回成功、文件都在、解析器却一张都不认。
   在取回这一层统一补 `.dcm` 后缀，不让调用方去猜。
3. **体积解析重入被丢弃，样例又反过来覆盖。** `loadDicomVolume` 早期遇到在途解析直接
   `return`（取回了但画面还是样例）；改为记 `m_pendingDicomDir` 排队，当前解析收尾后接手。
   接着暴露第二个问题：前端启动会自动加载 `DICOM_SAMPLE_PATH`，取回成功后几秒被演示数据
   打回。加 `m_authoritativeVolumeDir`——一旦 PACS 取回被解析，样例自动加载直接忽略。
   术中屏幕上必须是这个病人的影像，不是 demo。

实测（本机 `dcmqrscp`，60 层 MR 入库）：

```
[PACS] C-ECHO 成功：43 ms
[PACS] C-FIND 返回 1 例
[PACS] C-GET 完成：60 个对象，354 ms → …/pacs/1.2.826.0.1.3680043.2.11…
[Bridge] 开始后台解析 DICOM： …/pacs/1.2.826.0.1.3680043.2.11…
[DicomReader] 解析完成： 60 张切片 274 × 384 → 重采样到 128 ³
[Bridge] 当前体数据来自 PACS 取回，忽略样例自动加载： …/resources/dicom/brain-t1
```

台账两条对齐（同一 session）：

```json
{"type":"pacs.retrieve","files":60,"ms":354,"studyUid":"1.2.826…","dir":"…/pacs/1.2.826…"}
{"type":"dicom.volumeReady","bytes":2097152,"dims":[128,128,128],"source":"…/pacs/1.2.826…"}
```

已知边界：只做 C-GET，未做 C-MOVE（跨网段/需第三方存储时 C-MOVE 更常见）；
没有 TLS 与 AE 白名单，没有断线重连与退避；C-FIND 仍是全量 STUDY、取回只取第一例，
未按病人/日期过滤；体积固定重采样到 128³，层数更多的检查会丢细节。

### 11. 网络抖动下的韧性：取回重试 + 在线心跳

取回能跑通一次不等于能用。术中真正会遇到的是：C-FIND 刚返回、按下去取回的那一刻
网络抖了一下；或者手术做到一半 PACS 掉线，而没人有空去点一下探测。
这一批把这两件事都交给程序自己判断。

- **取回重试**：失败后最多重试 3 次，退避 0.6 s / 1.8 s。
  只有瞬时故障才重试——`PacsClient::isRetryableError()` 把「连不上 / 超时 / 关联被打断」
  与「库里没有这例（0xA900）」分开，后者重试三次只会让看板多转十几秒还给不出答案。
  这条判据是纯函数，直接进单测（`PacsClientTest.ClassifiesRetryableErrors`）。
- **重试可见**：按钮上直接写「重试中…」并在 `title` 里带原因，状态经 `pacsRetrieve`
  带回 `attempt / maxAttempts / retrying`；台账每次失败记一行 `pacs.retrieveRetry`。
- **在线心跳**：`QTRS_PACS_HEARTBEAT_MS`（默认 30000，0 关闭）只做 C-ECHO、不重新枚举。
  发现掉线才是目的，所以**只在状态翻转时**记台账（`pacs.offline` / `pacs.online`）
  与推信号，稳态不写——否则每 30 s 一行心跳会很快把台账淹掉。
  心跳撞上正在进行的取回时直接跳过：网络卡住时堆探测只会让两边都超时。
- **可复现的验证手段**：`QTRS_PACS_AUTO_GET` 现在接受毫秒数（`1` 仍表示命中即取），
  把取回推迟到 C-FIND 之后若干秒，就能在「C-FIND 成功、C-GET 之前」精确制造一次断网。

实测三种情形（同一台 dcmqrscp，手动 kill / 重启）：

```
# 1) 断网 → 三次重试后放弃
C-GET（第 1/3 次）→ 第 1 次失败（1 ms），600 ms 后重试：TCP Initialization Error
C-GET（第 2/3 次）→ 第 2 次失败（1 ms），1800 ms 后重试：TCP Initialization Error
C-GET（第 3/3 次）→ 失败：TCP Initialization Error: Operation now in progress

# 2) 重试途中服务端恢复 → 第 2 次成功
C-GET（第 1/3 次）→ 第 1 次失败（0 ms），600 ms 后重试
C-GET（第 2/3 次）→ C-GET 完成：60 个对象，97 ms
[DicomReader] 解析完成：60 张切片 274 × 384 → 重采样到 128 ³

# 3) 心跳（3 s 间隔，便于观察）
心跳失败： 关联协商失败：TCP Initialization Error
心跳恢复： 4 ms
```

台账对应：

```json
{"type":"pacs.retrieveRetry","attempt":1,"backoffMs":600,"message":"…TCP Initialization Error…"}
{"type":"pacs.retrieve","attempt":2,"files":60,"ms":97,"studyUid":"1.2.826…"}
{"type":"pacs.offline","roundTripMs":-1,"message":"关联协商失败：TCP Initialization Error…"}
{"type":"pacs.online","roundTripMs":4,"message":""}
```

已知边界：重试没有抖动（jitter），多实例同时重试仍可能同步撞车；心跳与取回互斥，
所以长取回期间的掉线只能在取回失败时暴露。C-MOVE 回退见下一节。

### 12. 遇上只支持 C-MOVE 的 PACS：本机起接收 SCP 接图

C-GET 是「我去你那儿取」，C-MOVE 是「你往我这儿推」——后者要求我们自己在手术室这台机器上
开一个存储 SCP，并把 AE / 端口登记到 PACS 的 AE 表里。真实医院里两种都有，
所以这一批把 C-MOVE 做成 C-GET 的退路而不是替代品。

- **方式选择**：`QTRS_PACS_MODE=auto|get|move`（默认 auto）。auto 先走 C-GET——
  它不需要 PACS 侧登记任何 AE，能用就是最省事的一条；失败且判据命中
  「服务端根本不会 C-GET」（`scp role` / `presentation context` / `部分失败` / `0x0122`）
  才回落 C-MOVE，并记一行 `pacs.retrieveFallback`。这类拒绝重试三次也没用，只会多等十几秒。
- **接收端身份**：`QTRS_PACS_STORE_AE`（默认 `QTRSSTORE`）与 `QTRS_PACS_STORE_PORT`（默认 11113）。
  端口要避开 PACS 和本机其他 DICOM 服务；AE 表里写错一个字母，PACS 就会把图推去别处或直接报错。
- **状态与台账**：`pacsRetrieve` 带回 `method`，按钮 `title` 写明这次是 C-GET 还是 C-MOVE；
  台账 `pacs.retrieve` 多一个 `method` 字段，事后能回答「这例是怎么进来的」。

实测（同一台 dcmqrscp，`QTRS_PACS_MODE=move`）：

```
C-ECHO 成功：20 ms
C-MOVE 1.2.826.0.1.3680043.2.11… → 127.0.0.1:11112 AE=QTRSQR（第 1/3 次）
C-MOVE 完成：60 个对象，3137 ms
[DicomReader] 解析完成：60 张切片 274 × 384 → 重采样到 128 ³
```

台账：`{"type":"pacs.retrieve","method":"move","files":60,"ms":3137}`。

这一批踩到的坑都出在「接收端怎么被 PACS 认出来」这一侧，记下来省下一次重走：

- **PACS 侧要能解析我们的 AE**：dcmqrscp 的 `AETable` 里 peers 写具体符号名会把 C-ECHO
  一起挡掉（它同时是「允许谁来查」的白名单），实测报 `DUL Association Rejected`；
  peers 保持 `ANY`，另在 `HostTable` 里登记 `qtrsstore = (QTRSSTORE, localhost, 11113)`
  供 C-MOVE 目标解析。
- **`DcmStorageSCP` 不加载协商档就只认 C-ECHO**：头文件写明「未加载关联协商档时仅支持
  Verification」，`openListenPort()` 直接返回 `Invalid or non-existing SCP Association Profile`。
  正确做法是运行时生成一份 `[[PresentationContexts]]` 协商档（Verification + MR/CT/
  EnhancedMR/SecondaryCapture，显式与隐式小端），再 `loadAssociationConfiguration(path, profile)`；
  手动 `addPresentationContext()` 不够，profile 仍然「不存在」。
- **收尾时间是连接超时，不是传输时间**：监听循环没有新关联时会一直等到
  `setConnectionTimeout()` 才退出，最初设 12 s，60 张图明明 1 s 内推完却报 12207 ms。
  改成 3 s 后同一批 3137 ms；这 3 s 就是「收完到收工」的固定尾账。
- **完成数以最终响应为准**：中间响应给的是「还剩几个」，只有最后一个响应才是结论；
  再用落地文件数交叉校验，`landed < completed` 直接报「部分失败」，不把半例当成功。
- **接口名与访问级别**：`DcmSCP` 只有虚函数 `stopAfterConnectionTimeout()`（需 override 返回
  `OFTrue` 让超时后退出），没有 `setStopAfterConnectionTimeout()`；`stopAfterCurrentAssociation()`
  是 protected，要在自己类里包一层 `requestStop()`；C-MOVE 的成功常量是 `STATUS_MOVE_Success`。

回落路径用 `dcmqrscp --disable-get` 造了一次「服务端不会 C-GET」：

```
C-GET（第 1/3 次）→ C-GET 不被支持，回落 C-MOVE（AE=QTRSSTORE:11113）
                 C-GET 失败：DIMSE No valid Presentation Context ID
C-MOVE（第 1/3 次）→ C-MOVE 完成：60 个对象，3226 ms
```

台账先记 `pacs.retrieveFallback`（from=get, to=move, storeAe/storePort），
再记 `pacs.retrieve(method=move)`，两条能对上。

已知边界：接收端目前只登记四个存储类，遇到超声、内镜等其它 SOP 类需要补配置；
C-MOVE 同样只取第一例，没有多例选择；回落只在「明确不支持」时触发，
若服务端是「能协商但一张也取不回」的软失败，仍按普通重试处理。

### 13. 报警的「提示」与「判定」分开：静音、到期恢复与注入验证

上一批把报警判定搬到了原生侧，但只有判定还不够——术中会有人按掉提示，
报警系统必须记住这件事。这一批做的是提示生命周期，判定本身不动：

- **静音不改判定**：`muteAlarms(seconds)` 只把边框和文字降为琥珀色并显示倒计时，
  「低血氧」判定、冻结帧看门狗、台账里的 `alarm.on/off` 全部照旧。
  上限 5 分钟——静音是暂时压掉提示，不是关掉报警系统。
- **两种入口**：窗口级快捷键 `M` 切换（焦点在 WebView 里也生效），
  或直接点原生条任意位置。术中手套在手，精确点小按钮不现实。
  `QTRS_ALARM_MUTE_SEC` 可把默认时长改短（限 5–300 s），便于验证与演示。
- **到期自动恢复且留痕**：静音到期后边框自动回红，同时写一条
  `alarm.unmute(reason=expired)`；手动再按一次则是 `reason=manual`。
  事后能回答「谁在几点几分把提示压掉、压了多久、什么时候放回来的」。
- **整条边框随报警变色**：红=报警，琥珀=报警但已静音。它不依赖文字被看见，
  也不依赖前端是否渲染成功——这是原生通道存在的意义。

为了能反复验证，模拟器新增两个固定值注入：`QTRS_SIM_SPO2=86` / `QTRS_SIM_HR=45`。
默认的生命体征是 93–100 的随机游走，永远不会穿过报警阈值，不注入就没法复现报警态。

实测（`QTRS_SIM_SPO2=86 QTRS_ALARM_MUTE_SEC=6`）：

```
16:47:56  {"type":"alarm.mute","seconds":6,"alarmActive":true,"spo2":86,"hr":72}
16:48:02  {"type":"alarm.unmute","reason":"expired","seconds":6}
```

静音期间截图：琥珀边框 + 「低血氧报警 · 已静音 2 s」；到期后截图：红边框 +
「低血氧报警」恢复，SpO₂ 仍显示 86（数值从不因静音而隐藏）。

已知边界：只有提示层，没有声音；报警优先级目前只有一档（低血氧 / 心动过缓都算高）；
静音状态不跨进程恢复，重启后回到「未静音」；这仍是演示级实现，
不构成对 IEC 60601-1-8 报警系统的合规声明。

### 14. 需求—实现—验证的追溯矩阵：让「术中化」可被核查

前面 13 节讲的是做了什么、怎么验证的，但都散在叙事里；评审时真正会被问的是
「这条需求靠哪段代码实现、证据在哪、哪些还没做」。这一批把这件事落成一张表：

- `docs/traceability.md` 一行一条需求（`QRS-*`），五列：需求 / 实现（文件路径）/
  验证方式 / 状态（已验证 / 部分验证 / 缺口）。共 30 条，当前 20 已验证、6 部分验证、4 缺口。
- `scripts/check-traceability.py` + `make trace` 做机器检查：ID 不重复、状态只有三种、
  「实现」列引用的文件必须真实存在、「缺口」行不允许引用实现文件。
  文档腐化（删了文件、状态乱写）会直接失败，而不是等评审时才发现。
- 缺口清单就是下一批工作的候选：真实设备接入、取回期间掉线暴露、
  进程重启恢复、ISO 14971 风险分析、临床验证。

实测：

```
$ make trace
OK  30 行需求：已验证 20 / 部分验证 6 / 缺口 4
```

反向验证（故意写错路径、写非法状态）两次都以非零码退出，确认检查器不是空跑。

### 15. 报警分级与声音：高/中两级 + 复鸣节流 + 静音即静音

有了静音还不够——所有报警一个颜色、一个节奏，等于没有优先级。这一批把提示从「有/无」
扩到「高/中两级」，并把声音接上：

- **两级判定**（原生侧，阈值演示级）：高 = SpO₂<90 或 HR<50，红；中 = SpO₂<92 或 HR<55，黄。
  级别变化才记台账，`alarm.on` 多一个 `priority` 字段（`high` / `medium`）。
- **声音节流**：高优先级每 3 s 复鸣、中优先级每 8 s，靠 `QElapsedTimer` 限流，
  不是每帧都响。默认开启，`QTRS_ALARM_SOUND=0` 关闭（开发与演示用）。
- **静音即静音**：静音期间不响，边框变灰虚线、文字变灰并带倒计时；
  判定、数值、台账照旧。这一点单独验过——静音前后对比 `[Alarm] beep` 行数不再增长。
- **不依赖前端**：颜色、文字、声音全在原生条里，WebView 崩了也照常响。

实测（`QTRS_SIM_SPO2=84`）：

```
[Alarm] beep level=2 hr=72 spo2=84
[Alarm] beep level=2 hr=70 spo2=84        # 3 s 后复鸣，不是每帧
{"type":"alarm.on","priority":"high","channel":"SpO2","spo2":84,"hr":72}
{"type":"alarm.mute","alarmLevel":2,"seconds":20}
```

静音后 7 秒内 beep 行数保持 3 条不变，截图里边框转灰虚线、文字为
「高优先级 · 血氧偏低 84% · 已静音 13 s」。

已知边界：`QApplication::beep()` 用的是系统提示音，音色与节奏不满足 IEC 60601-1-8
对报警音的规定（需要专门设计的音库与调制），且不同 macOS 版本音色可能不同；
两级之外没有「技术报警 / 生理报警」的区分；报警延迟未做量化验证。

### 16. 多例选择：取哪一例由人决定，而不是永远第一例

C-FIND 命中多例时，之前的实现只认 `studies.first()`——库里只有一个样例时看不出问题，
接上真实 PACS 就会变成「点取回，取回来的不是屏幕上正在讨论的那一例」。这一批把选择权交回给人：

- C-FIND 的整份列表进 `pacsState()` 快照，前端刷新或晚订阅也能恢复列表，不只是拿到一个 UID。
- 看板头部多一个下拉框（`Jane_Doe · 20130717` / `Li_Lei · 20250101`），显示患者名、检查日期、
  模态与序列数；已选那一例如果还在列表里就保留，不会被下一次查询顶回第一例。
- 「取回该例」跟着下拉走；切换后取回的是所选 StudyInstanceUID。

实测（本地 `dcmqrscp` 里灌了两例）：

```
[PACS] C-FIND 返回 2 例
[PACS] C-GET 完成：8 个对象，16 ms → …/pacs/1.2.826.0.1.3680043.2.1143.99112233445566778899
[DicomReader] 解析完成：8 张切片 274 × 384 → 重采样到 128 ³
{"type":"pacs.retrieve","studyUid":"…99112233445566778899","files":8,"method":"get"}
{"type":"dicom.volumeReady","source":"…/pacs/1.2.826.0.1.3680043.2.1143.99112233445566778899"}
```

第二例是 `dcmodify` 改标签后 `storescu` 灌进去的（患者 `Li_Lei`、8 张切片），
所以「选第二例 → 取回 8 个对象 → 体积来自第二例目录」是一条完整可核对的链路。

已知边界：下拉只按 C-FIND 返回顺序展示，没有排序、搜索与按患者过滤；列表是一次查询的快照，
PACS 侧新增检查需要重新查询；体积通道的「到达」在事件型通道上会显示为距上次到达的时间，
长时间无新体积时会累积成很大的数字（`volume` 通道的既有口径问题，未在本批处理）。

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

# 检查需求追溯矩阵（ID / 状态 / 引用文件）
make trace
```

### 测试质量保证

- **自动化测试**：本地 `make run-tests`（C++ Google Test）为质量门；本项目不接入 CI（见上节）
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
