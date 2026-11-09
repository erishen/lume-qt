#!/usr/bin/env python3
"""检查两版 README 的目录树是否还和实际源码文件一致。

规则：
  1. README.md 与 README.zh.md 都必须有且仅有一个以 ``lume-qt/`` 开头的目录树代码块；
  2. src/、frontend/src/、scripts/、tests/ 下每个文件的 stem 都必须出现在两棵目录树里
     （新增模块或脚本忘了写进 README 会直接报错）；
  3. 树里声明的文件数必须与实际文件数一致；
  4. 关键目录不能从树里整块消失。

退出码：0 = 通过，1 = 有违规。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
README_FILES = ("README.md", "README.zh.md")
SOURCE_DIRS = ("src", "frontend/src", "scripts", "tests")
REQUIRED_DIRS = (
    "tests/",
    "scripts/",
    "tools/lume-medmon/",
    "docs/",
    "resources/web/",
    "resources/dicom/",
)
COUNT_RE = re.compile(r"(\d+)\s*(?:个文件|files)")


def extract_tree(text: str) -> str | None:
    lines = text.splitlines()
    starts = [i for i, line in enumerate(lines) if line.strip() == "lume-qt/"]
    if len(starts) != 1:
        return None
    i = starts[0]
    top = i - 1
    while top >= 0 and not lines[top].lstrip().startswith("```"):
        top -= 1
    bottom = i + 1
    while bottom < len(lines) and not lines[bottom].lstrip().startswith("```"):
        bottom += 1
    if top < 0 or bottom >= len(lines):
        return None
    return "\n".join(lines[top + 1:bottom])


def actual_counts() -> dict[str, int]:
    return {
        d: len([p for p in (ROOT / d).iterdir() if p.is_file()])
        for d in SOURCE_DIRS
    }


def main() -> int:
    errors: list[str] = []
    counts = actual_counts()

    for name in README_FILES:
        path = ROOT / name
        if not path.exists():
            errors.append(f"{name}: 文件不存在")
            continue
        tree = extract_tree(path.read_text(encoding="utf-8"))
        if tree is None:
            errors.append(f"{name}: 找不到唯一的 lume-qt/ 目录树代码块")
            continue

        declared = {int(n) for n in COUNT_RE.findall(tree)}
        for d, n in counts.items():
            if n not in declared:
                errors.append(f"{name}: 目录树未声明 {d}/ 的实际文件数 {n}")

        for d in SOURCE_DIRS:
            for p in sorted((ROOT / d).iterdir()):
                if p.is_file() and p.stem not in tree:
                    errors.append(f"{name}: {d}/{p.name} 未登记到目录树")

        for d in REQUIRED_DIRS:
            if d not in tree:
                errors.append(f"{name}: 目录树缺少 {d}")

    if errors:
        for err in errors:
            print(f"FAIL {err}")
        print(f"\n{len(errors)} 处违规")
        return 1

    listed = " / ".join(f"{d} {n}" for d, n in counts.items())
    print(f"OK  README 目录树：{listed} 个文件在两版 README 中均有登记")
    return 0


if __name__ == "__main__":
    sys.exit(main())
