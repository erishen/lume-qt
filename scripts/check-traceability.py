#!/usr/bin/env python3
"""检查 docs/traceability.md 的结构与引用是否还成立。

规则：
  1. 每个 QRS-* 需求行 ID 唯一；
  2. 状态只能是 已验证 / 部分验证 / 缺口；
  3. 「实现」列里引用的文件必须真实存在；
  4. 「缺口」行不允许引用实现文件（缺口就是没做，写了实现自相矛盾）。

退出码：0 = 通过，1 = 有违规。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOC = ROOT / "docs" / "traceability.md"
STATUSES = {"已验证", "部分验证", "缺口"}
ID_RE = re.compile(r"^\|\s*(QRS-[A-Z]-\d{3})\s*\|")
PATH_RE = re.compile(r"`([^`]+)`")


def main() -> int:
    if not DOC.exists():
        print(f"FAIL 找不到 {DOC.relative_to(ROOT)}")
        return 1

    errors: list[str] = []
    seen: dict[str, int] = {}
    counts = {status: 0 for status in STATUSES}
    rows = 0

    for lineno, line in enumerate(DOC.read_text(encoding="utf-8").splitlines(), 1):
        match = ID_RE.match(line)
        if not match:
            continue
        rows += 1
        req_id = match.group(1)
        if req_id in seen:
            errors.append(f"{lineno}: ID 重复 {req_id}（首次出现在第 {seen[req_id]} 行）")
        seen[req_id] = lineno

        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) != 5:
            errors.append(f"{lineno}: {req_id} 列数应为 5，实际 {len(cells)}")
            continue

        _, _, impl, _, status = cells
        if status not in STATUSES:
            errors.append(f"{lineno}: {req_id} 状态取值非法：{status!r}")
            continue
        counts[status] += 1

        paths = [token for token in PATH_RE.findall(impl) if "/" in token or "." in token]
        if status == "缺口" and paths:
            errors.append(f"{lineno}: {req_id} 标为缺口却引用了实现文件：{', '.join(paths)}")
        for path in paths:
            if not (ROOT / path).exists():
                errors.append(f"{lineno}: {req_id} 引用的文件不存在：{path}")

    if rows == 0:
        errors.append("没有解析到任何 QRS-* 需求行")

    if errors:
        for err in errors:
            print(f"FAIL {err}")
        print(f"\n{rows} 行需求，{len(errors)} 处违规")
        return 1

    print(f"OK  {rows} 行需求：已验证 {counts['已验证']} / "
          f"部分验证 {counts['部分验证']} / 缺口 {counts['缺口']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
