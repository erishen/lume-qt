#!/usr/bin/env bash
# Lume medmon supervisor: start / stop / status / watch.
#
# 为什么健康检查要打 /api/medmon/health 而不是 /health：
# 现场遇到过「GET 读接口 200，但 POST 写接口一直挂住」的实例状态，
# /health 只能证明进程活着，写探针才能真正发现写链路卡死。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${LUME_MEDMON_PORT:-8090}"
BASE="http://127.0.0.1:${PORT}"
export LUME_MEDMON_PORT="${PORT}"
LUME_BIN="${LUME_BIN:-${HOME}/.local/bin/lume}"
APP="${LUME_APP:-app.lume}"
LOG="${LUME_LOG:-${ROOT}/.data/lume-supervisor.log}"
PIDFILE="${LUME_PIDFILE:-${ROOT}/.data/lume.pid}"

mkdir -p "${ROOT}/.data"

listener_pids() {
  lsof -nP -tiTCP:"${PORT}" -sTCP:LISTEN 2>/dev/null || true
}

listener_pid() {
  listener_pids | head -n 1 || true
}

is_lume_pid() {
  local pid="$1"
  ps -o command= -p "${pid}" 2>/dev/null | grep -q 'app.lume'
}

health() {
  curl -fsS --noproxy '*' --max-time 3 "${BASE}/api/medmon/health" >/dev/null 2>&1
}

stop() {
  local pids pid child
  pids="$(listener_pids)"
  if [ -z "${pids}" ]; then
    echo "==> [lume] ${PORT} 端口空闲"
    return 0
  fi
  # 先确认所有监听 PID 都属于 app.lume，避免误杀别的服务。
  for pid in ${pids}; do
    if ! is_lume_pid "${pid}"; then
      echo "==> [lume] ${PORT} 被非 Lume 进程占用（pid=${pid}），拒绝误杀" >&2
      return 1
    fi
  done
  echo "==> [lume] 停止 pid=$(echo ${pids})（端口 ${PORT}）"
  # Lume 会 fork 多个 worker；lsof 返回的监听 PID 可能不止一个，必须全部收掉，
  # 只杀首个会留下 worker 继续占端口。
  for pid in ${pids}; do
    while IFS= read -r child; do
      [ -n "${child}" ] && kill -9 "${child}" 2>/dev/null || true
    done < <(pgrep -P "${pid}" 2>/dev/null || true)
    kill -9 "${pid}" 2>/dev/null || true
  done
  rm -f "${PIDFILE}"
  for _ in $(seq 1 25); do
    [ -z "$(listener_pid)" ] && return 0
    sleep 0.2
  done
  echo "==> [lume] 端口 ${PORT} 仍未释放" >&2
  return 1
}

start() {
  local pid child_pid
  pid="$(listener_pid)"
  if [ -n "${pid}" ]; then
    if health; then
      echo "==> [lume] 已在运行且写探针正常（pid=${pid}，${BASE}）"
      return 0
    fi
    echo "==> [lume] 实例存在但写探针失败，重启" >&2
    stop || return 1
  fi

  [ -x "${LUME_BIN}" ] || { echo "==> [lume] 找不到 lume：${LUME_BIN}" >&2; return 1; }
  echo "==> [lume] 启动 ${APP}（端口 ${PORT}，日志 ${LOG}）"
  (
    cd "${ROOT}"
    nohup "${LUME_BIN}" "${APP}" >>"${LOG}" 2>&1 &
    echo $! >"${PIDFILE}"
  )
  child_pid="$(cat "${PIDFILE}" 2>/dev/null || true)"

  # Lume 实测约 18s 才 listen；给 45s 余量。
  for _ in $(seq 1 90); do
    if health; then
      echo "==> [lume] 就绪（写探针 200，pid=${child_pid}）"
      return 0
    fi
    if [ -n "${child_pid}" ] && ! kill -0 "${child_pid}" 2>/dev/null; then
      echo "==> [lume] 启动进程已退出" >&2
      break
    fi
    sleep 0.5
  done
  echo "==> [lume] 启动失败，最近日志：" >&2
  tail -n 20 "${LOG}" 2>/dev/null | sed 's/^/    /' >&2 || true
  return 1
}

status() {
  local pid
  pid="$(listener_pid)"
  if [ -z "${pid}" ]; then
    echo "==> [lume] ${PORT} 未监听"
    return 1
  fi
  if health; then
    echo "==> [lume] 正常：pid=${pid}，${BASE}（写探针 200）"
    return 0
  fi
  echo "==> [lume] 异常：pid=${pid} 在监听，但写探针失败；请执行 start 重启" >&2
  return 1
}

watch() {
  local failures=0
  echo "==> [lume] 监督启动（端口 ${PORT}，每 5s 检查写探针）"
  trap 'echo; echo "==> [lume] 监督退出"; stop || true; exit 0' INT TERM
  while true; do
    if health; then
      if [ "${failures}" -gt 0 ]; then
        echo "==> [lume] 写探针恢复"
      fi
      failures=0
    else
      failures=$((failures + 1))
      echo "==> [lume] 写探针失败（第 ${failures} 次）" >&2
      if [ "${failures}" -ge 2 ]; then
        echo "==> [lume] 连续失败，重启服务" >&2
        stop || true
        start || true
        failures=0
      fi
    fi
    sleep 5
  done
}

case "${1:-status}" in
  start) start ;;
  stop) stop ;;
  status) status ;;
  watch) watch ;;
  *)
    echo "用法: $0 {start|stop|status|watch}" >&2
    exit 2
    ;;
esac
