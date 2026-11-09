# 第三方组件与许可说明（THIRD_PARTY_NOTICES）

> 本文是工程侧的依赖盘点，**不是法律意见**。各组件的确切许可文本、版本与附加条款以**上游随发行版提供的 LICENSE / COPYRIGHT / NOTICE 文件为准**；若本文与上游不一致，以上游为准。

根目录 [`LICENSE`](LICENSE) 中的 MIT 许可**只覆盖本仓库的原创源码与文档**，不覆盖下列第三方组件，也不覆盖 `resources/dicom/` 下的样例数据。本仓库**不内嵌**任何第三方二进制、字体或数据集；Qt、DCMTK、Google Test 由构建机通过系统包/Homebrew 提供，npm 依赖通过 `npm install` 从 registry 获取。

## C++ / 桌面宿主侧

| 组件 | 用途 | 许可（概述） | 上游 |
|---|---|---|---|
| Qt 6（`Widgets`、`WebEngineWidgets`、`WebChannel`、`Network`、`Multimedia`、`OpenGLWidgets`、`Test`、`Core`） | 桌面壳、WebEngine 页面、WebChannel 桥接、OpenGL 体渲染、测试 | 多许可：商业许可，或开源许可下的 **LGPLv3 / GPLv2 / GPLv3**（按模块与使用方式不同，可用集合不同）。**Qt WebEngine 模块的可选开源许可集与其它 Qt 模块不完全相同**，通常按 LGPLv3 / GPLv3 提供，且内含 Chromium 及大量第三方组件（BSD/MIT/LGPL 等），各自许可另计 | <https://www.qt.io/licensing/> · Qt 安装目录内的 LICENSE / `qtwebengine` 第三方许可文档 |
| DCMTK 3.7（`dcmdata`、`dcmnet`、`dcmtls`、`oflog`） | DICOM 解析、C-FIND/C-GET/C-MOVE、DICOM TLS | DCMTK 自定义的宽松许可（OFFIS 等贡献者），允许使用与再分发，但要求**保留版权声明、许可文本与免责声明**，并有署名/背书方面的限制 | <https://github.com/DCMTK/dcmtk> · 安装目录 `COPYRIGHT` 文件 |
| OpenSSL（经 `dcmtls` 间接依赖） | TLS 握手与证书校验 | OpenSSL 3.x 使用 **Apache-2.0**；OpenSSL 1.1.1 使用 OpenSSL + SSLeay 双许可。以实际链接版本为准 | <https://www.openssl.org/source/license.html> |
| Google Test（仅测试目标） | `bridge_test` / `volume_stats_test` | BSD 3-Clause | <https://github.com/google/googletest> |
| `qwebchannel.js` | 前端与 C++ 通信 | 随 Qt 发行，按 Qt 对应模块许可；由 `vite.config.ts` 从本机 Qt 安装同步，属生成物、不入库 | 随 Qt 安装 |

## 前端侧

版本与完整传递依赖见 [`frontend/package.json`](frontend/package.json) 与 [`frontend/package-lock.json`](frontend/package-lock.json)。

| 组件 | 许可（概述） |
|---|---|
| React 19 / React DOM 19 | MIT |
| Three.js | MIT |
| Vite 6 / `@vitejs/plugin-react` / Vitest 2 | MIT |
| TypeScript 5 | Apache-2.0 |
| `@types/react` / `@types/react-dom` / `@types/three` | MIT |

## 使用 LGPL 组件时需要留意的条件

本项目通过 `find_package(Qt6 ...)` + `target_link_libraries()` 链接构建机上的 Qt，默认是**动态链接**，且没有在源码中静态编入或改写 Qt 库。若日后分发二进制，通常需要同时满足（以所选用 LGPL 版本原文为准）：

1. 随分发提供本文件与 [`LICENSE`](LICENSE)，并声明使用了 Qt（含版本、获取途径、是否做过修改）。
2. 提供 Qt 库的对应源码或明确的获取方式，允许用户**替换/重新链接**修改后的库；静态链接时还需提供可重链的目标文件。
3. 不得限制用户为调试「对库的修改」而进行的逆向工程。
4. Qt WebEngine 内含 Chromium 及第三方组件，需一并提供其许可与版权声明（可直接引用 Qt 发行版自带的 third-party 文档）。

如果改用 Qt 的商业许可，或改为只分发源码，适用条件不同，请单独评估。

## 样例数据

`resources/dicom/` 下的 DICOM 序列**不在本仓库、也不在对外分享的干净导出包中**；其来源与再分发许可尚未确认。详见 [`docs/sample-data-attribution.md`](docs/sample-data-attribution.md)。

## 分发检查单

- [ ] 附上 [`LICENSE`](LICENSE) 与本文。
- [ ] 附上 Qt / DCMTK / OpenSSL / Google Test 的许可与版权文本，或给出可核验的获取路径。
- [ ] 若分发二进制：满足上节 LGPL 重链与声明条件，并保留非临床用途标注。
- [ ] 不包含 `resources/dicom/`、`tools/lume-medmon/.env`、`.data/`、`build/` 等本机私有内容。
- [ ] 由法务/合规角色确认最终许可组合，尤其是 GPL 选项与 Qt WebEngine 的附加组件声明。
