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

# 收端独立清洗回归：故意让「生产者」直接 POST 原始 PHI（旧镜像、脚本调用、
# 未来新客户端都可能这样），验证落库前仍会被白名单 + 路径/UID 折叠拦住。
PHI_PAYLOAD='{"app":"lume-qt","events":[{"ts":"2026-11-09T15:00:00.000","tsUs":2,"session":"'"${SESSION}"'","type":"test.redaction","message":"取回 2.25.31873619609263979417185351299617829387 于 /Users/<user>/Library/Application Support/research/lume-qt/pacs/A.dcm","source":"~/Library/Application Support/research/lume-qt/audit/a.jsonl","reason":"C:\\Users\\<user>\\Documents\\ct 与 /opt/homebrew/opt/qt","patientName":"Jane_Doe","studyUid":"1.2.840.10008","nested":{"patientId":"P123"}}]}'
# fixture 自检：占位符必须真的出现在原始 payload 里，否则下面的 needle 断言会空转。
printf '%s' "${PHI_PAYLOAD}" | grep -Fq '<user>' || {
  echo "==> [lume-test] 脱敏回归 fixture 缺少 <user> 占位符，断言会假绿" >&2
  exit 1
}
status=""
for attempt in 1 2; do
  if status="$(curl -sS --noproxy '*' --max-time 5 -o "${TMP}/phi-resp.json" -w '%{http_code}' \
    -X POST "http://127.0.0.1:${PORT}/api/medmon/audit" \
    -H 'Content-Type: application/json' \
    --data "${PHI_PAYLOAD}")"; then
    break
  fi
  status=""
  sleep 1
done
if [ "${status}" != "200" ]; then
  echo "==> [lume-test] 脱敏回归 POST 返回 ${status}" >&2
  cat "${TMP}/phi-resp.json" >&2 || true
  exit 1
fi

stored="$(sqlite3 "${TMP}/.data/medmon.db" \
  "SELECT payload FROM events WHERE type='test.redaction' AND session='${SESSION}'")"
if [ -z "${stored}" ]; then
  echo "==> [lume-test] 脱敏回归事件未落库" >&2
  exit 1
fi
for needle in "2.25." "3187361960" "1.2.840.10008" "/Users/" "Application Support" \
              "~/Library" "opt/homebrew" "<user>" "Jane_Doe" "P123"; do
  if printf '%s' "${stored}" | grep -Fq "${needle}"; then
    echo "==> [lume-test] 落库仍含未脱敏片段：${needle}" >&2
    printf '%s\n' "${stored}" >&2
    exit 1
  fi
done
printf '%s' "${stored}" | grep -Fq '<path>' || {
  echo "==> [lume-test] 落库里没有 <path>，路径清洗可能未生效" >&2; exit 1; }
printf '%s' "${stored}" | grep -Fq '<uid>' || {
  echo "==> [lume-test] 落库里没有 <uid>，UID 清洗可能未生效" >&2; exit 1; }
echo "==> [lume-test] 收端脱敏回归：短 UID / 带空格路径 / 家目录 / 患者字段均已清除"

echo "==> [lume-test] POST 200 accepted=1，SQLite 落库 1 行；通过"
