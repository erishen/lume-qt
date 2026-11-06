#!/usr/bin/env bash
# 一键构建：前端构建 + CMake 配置 + 编译
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build"
QT_CMAKE="/opt/homebrew/opt/qt/bin/qt-cmake"

echo "==> [1/3] 构建前端 (Vite + React)"
(cd "${ROOT}/frontend" && npm install && npm run build)

echo "==> [2/3] 配置 CMake"
cmake -S "${ROOT}" -B "${BUILD_DIR}" -DCMAKE_PREFIX_PATH="/opt/homebrew/opt/qt" \
      -G Ninja

echo "==> [3/3] 编译 C++"
cmake --build "${BUILD_DIR}"

echo ""
echo "构建完成。运行: ${BUILD_DIR}/lume-qt"
