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
- **桥接**：`bridge.loadDicomVolume(directory)` 触发解析，结果通过 `dicomVolumeReady(meta, base64Pixels)` 信号回传 JS（base64 编码的 `uint8` 体积 + `dims`/`spacing`/`slices` 元数据）；失败发 `dicomVolumeError`。
- **样例数据**：`bridge.dicomSamplePath()` 返回内置脑部 T1 MRI 序列目录 `resources/dicom/brain-t1`（约 384 张切片）。
- **渲染**：前端 `Volume3D.tsx` 用 Three.js 体绘制/切片渲染该体积；脱离 Qt 时由 `syntheticVolume.ts` 程序化合成一个合成脑体，保证纯浏览器预览也能看到体积效果。

> 注意：CMake 配置阶段即 `find_package(DCMTK REQUIRED)`，未安装 DCMTK 会直接配置失败——这是本项目的硬依赖，不同于 Qt 可可选模块。

## 🧪 测试基础设施

### C++ 单元测试

我们集成了 **Google Test** 框架，用于 C++ 核心模块测试：

- **测试范围**：Bridge 类的基本功能、医疗模拟控制、错误处理、类型安全
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

### CI/CD 集成

所有测试通过 **GitHub Actions** 自动集成：

```yaml
name: CI
on: [push, pull_request]
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Setup environment
        run: |
          sudo apt-get update
          sudo apt-get install -y --no-install-recommends \
            cmake ninja-build \
            qt6-base-dev qt6-webengine-dev qt6-webchannel-dev \
            libgl1-mesa-dev libdcmtk-dev libgtest-dev
      - name: Configure CMake
        run: cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
      - name: Build
        run: cmake --build build
      - name: Run C++ tests
        run: cd build && ctest --output-on-failure
      # 前端 Jest/RTL 测试目前尚未接入（见“测试基础设施”说明），
      # 接入后在下方追加 npm ci / npm test 步骤。
```

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

- **自动化测试**：所有测试通过 CI/CD 自动运行
- **质量门**：测试失败会阻止合并
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
