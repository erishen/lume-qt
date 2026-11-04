#!/usr/bin/env bash
#
# frontend-build.sh — 带 staleness 检查的增量前端构建。
#
# 仅当前端源码（src/、index.html、vite.config.ts、package.json、lockfile）
# 比已产出 resources/web 更新时才运行 `npm run build`，避免每次 C++ 构建
# 都强制重跑整次前端构建（其中包含全量 `tsc` 类型检查）。
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FRONTEND_DIR="$ROOT/frontend"
WEB_ROOT="$ROOT/resources/web"
STAMP="$WEB_ROOT/.frontend-build-stamp"

# 收集需要参与 staleness 判断的源
SOURCES=()
while IFS= read -r -d '' f; do SOURCES+=("$f"); done < <(find "$FRONTEND_DIR/src" -type f -print0 2>/dev/null)
for f in "$FRONTEND_DIR/index.html" "$FRONTEND_DIR/vite.config.ts" \
         "$FRONTEND_DIR/package.json"; do
  [ -f "$f" ] && SOURCES+=("$f")
done
# 注意：package-lock.json 故意不纳入 staleness 判断——`npm install` 会改写
# 它的 mtime，而 make 的 deps 目标每次都会跑 npm install，若纳入则会误判
# “前端已变更”而每次都重跑整次 npm run build。真正的源码信号是 package.json
# （新增依赖时它会变）与 src/、配置文件。

mtime() {
  local f="$1"
  if [ -f "$f" ]; then
    stat -f %m "$f" 2>/dev/null || stat -c %Y "$f" 2>/dev/null || echo 0
  else
    echo 0
  fi
}

newest=0
for f in "${SOURCES[@]}"; do
  mt="$(mtime "$f")"
  [ "$mt" -gt "$newest" ] && newest="$mt"
done

stamp_mt="$(mtime "$STAMP")"

if [ -f "$WEB_ROOT/index.html" ] && [ "$newest" -le "$stamp_mt" ]; then
  echo "[frontend-build] 产物已是最新，跳过 npm run build"
  exit 0
fi

echo "[frontend-build] 检测到源码更新，运行 npm run build"
cd "$FRONTEND_DIR"
npm run build
touch "$STAMP"
