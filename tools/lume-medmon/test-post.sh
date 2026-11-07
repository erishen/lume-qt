#!/usr/bin/env bash
# 真实 POST 集成测试：在临时端口 + 临时库上启动 Lume，打 /api/medmon/audit，
# 既检查 HTTP 200，也检查 SQLite 真的落了一行。不会碰正式 .data/medmon.db。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${LUME_TEST_PORT:-8092}"
LUME_BIN="${LUME_BIN:-${HOME}/.local/bin/lume}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/lume-medmon-post.XXXXXX")"
LOG="${TMP}/lume-test.log"
SESSION="test-post-$$"

cleanup() {
  local pids pid child
  pids="$(lsof -nP -tiTCP:"${PORT}" -sTCP:LISTEN 2>/dev/null || true)"
  for pid in ${pids}; do
    while IFS= read -r child; do
      [ -n "${child}" ] && kill -9 "${child}" 2>/dev/null || true
    done < <(pgrep -P "${pid}" 2>/dev/null || true)
    kill -9 "${pid}" 2>/dev/null || true
  done
  if [ "${LUME_TEST_KEEP:-0}" = "1" ]; then
    echo "==> [lume-test] 保留临时目录：${TMP}" >&2
  else
    rm -rf "${TMP}"
  fi
}
trap cleanup EXIT

if lsof -nP -tiTCP:"${PORT}" -sTCP:LISTEN >/dev/null 2>&1; then
  echo "==> [lume-test] 端口 ${PORT} 已被占用，拒绝覆盖" >&2
  exit 1
fi
[ -x "${LUME_BIN}" ] || { echo "==> [lume-test] 找不到 lume：${LUME_BIN}" >&2; exit 1; }

mkdir -p "${TMP}/.data"
cp "${ROOT}/app.lume" "${TMP}/app.lume"

echo "==> [lume-test] 启动临时实例 :${PORT}"
(
  cd "${TMP}"
  LUME_MEDMON_PORT="${PORT}" nohup "${LUME_BIN}" app.lume >"${LOG}" 2>&1 &
  echo $! >"${TMP}/lume.pid"
)

ready=0
for _ in $(seq 1 90); do
  if curl -fsS --noproxy '*' --max-time 2 "http://127.0.0.1:${PORT}/api/medmon/health" >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 0.5
done
if [ "${ready}" != "1" ]; then
  echo "==> [lume-test] 临时实例未就绪，日志：" >&2
  tail -n 30 "${LOG}" >&2 || true
  exit 1
fi

# 健康探针只说明至少一个 worker 已就绪；给第二个 worker 两秒完成初始化，
# 避免集成测试偶发踩到「健康 200、首个 POST 空回复」的冷启动窗口。
sleep 2

PAYLOAD="{\"app\":\"lume-qt\",\"events\":[{\"ts\":\"2026-11-07T17:20:00.000\",\"tsUs\":1,\"session\":\"${SESSION}\",\"type\":\"test.post\"}]}"
# 冷启动时偶尔会踩到 Lume worker 尚未接住第一个 POST（curl 52）；
# 重试一次，仍失败就视为真实故障，不掩盖路由/落库问题。
status=""
for attempt in 1 2; do
  if status="$(curl -sS --noproxy '*' --max-time 5 -o "${TMP}/resp.json" -w '%{http_code}' \
    -X POST "http://127.0.0.1:${PORT}/api/medmon/audit" \
    -H 'Content-Type: application/json' \
    --data "${PAYLOAD}")"; then
    break
  fi
  status=""
  sleep 1
done

if [ "${status}" != "200" ]; then
  echo "==> [lume-test] POST 返回 ${status}" >&2
  cat "${TMP}/resp.json" >&2 || true
  tail -n 30 "${LOG}" >&2 || true
  exit 1
fi

python3 - "${TMP}/resp.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as f:
    body = json.load(f)
assert body.get("ok") is True, body
assert body.get("accepted") == 1, body
PY

count="$(sqlite3 "${TMP}/.data/medmon.db" \
  "SELECT COUNT(*) FROM events WHERE type='test.post' AND session='${SESSION}'")"
if [ "${count}" != "1" ]; then
  echo "==> [lume-test] 落库行数不是 1：${count}" >&2
  exit 1
fi

echo "==> [lume-test] POST 200 accepted=1，SQLite 落库 1 行；通过"
