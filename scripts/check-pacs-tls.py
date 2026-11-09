#!/usr/bin/env python3
"""PACS TLS fail-closed 静态门禁。

配置解析有单测，但单测绿不代表传输层没有悄悄退回明文：
  - DCMTK 的 DCV_checkCertificate 在「对端不给证书」时直接放行；
  - C-MOVE 的接收 SCP 是明文端口，若 TLS 判定晚于 openListenPort()，
    就等于先开一个明文入口再报错退出；
  - TLS 配置可用性检查必须早于 negotiateAssociation()。

这三条是结构约束，不随实现细节变化，所以放在 make trace 里静态锁住。
配套的行为判定（pacsTlsMoveBlockReason / check 级别 / 错误分类）由
tests/bridge_test.cpp 覆盖；本脚本只负责「顺序与禁用符号」。
"""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "src" / "pacsclient.cpp"


def slice_function(text: str, signature: str, terminator: str) -> str | None:
    """截取单个函数体文本（按下一个顶层定义或命名空间结束符切）。"""
    start = text.find(signature)
    if start < 0:
        return None
    end = text.find(terminator, start + len(signature))
    return text[start:] if end < 0 else text[start:end]


def check_source(text: str) -> list[str]:
    errors: list[str] = []

    if "DCV_checkCertificate" in text:
        errors.append(
            "pacsclient.cpp 仍引用 DCV_checkCertificate："
            "DCMTK 该级别在对端不出示证书时直接放行（fail-open）"
        )

    assoc = slice_function(text, "bool openAssociation(", "\n} // namespace")
    if assoc is None:
        errors.append("找不到 openAssociation()")
    else:
        usable = assoc.find("cfg.usable()")
        negotiate = assoc.find("negotiateAssociation(")
        if usable < 0 or negotiate < 0 or usable > negotiate:
            errors.append(
                "openAssociation()：cfg.usable() 必须早于 negotiateAssociation()，"
                "否则无效 TLS 配置会走到建连阶段"
            )

    move = slice_function(text, "bool PacsClient::moveStudy(", "\nbool PacsClient::")
    if move is None:
        errors.append("找不到 PacsClient::moveStudy()")
    else:
        block = move.find("pacsTlsMoveBlockReason(")
        listen = move.find("openListenPort(")
        mkdir = move.find("mkpath(")
        if block < 0:
            errors.append("moveStudy() 缺少 pacsTlsMoveBlockReason() 前置判定")
        elif listen >= 0 and block > listen:
            errors.append(
                "moveStudy()：TLS 判定晚于 openListenPort()，"
                "TLS 模式下会先开放明文接收端口再拒绝"
            )
        if mkdir >= 0 and block > mkdir:
            errors.append("moveStudy()：TLS 判定应早于创建取回目录（fail-fast）")

    return errors


def main() -> int:
    if not SOURCE.exists():
        print(f"ERROR  缺少源码：{SOURCE}", file=sys.stderr)
        return 1
    errors = check_source(SOURCE.read_text(encoding="utf-8"))
    if errors:
        for e in errors:
            print(f"ERROR  {e}", file=sys.stderr)
        print(f"FAIL  PACS TLS 门禁：{len(errors)} 项不通过", file=sys.stderr)
        return 1
    print("OK  PACS TLS 门禁：无 fail-open 验签级别，TLS 判定早于建连与明文监听")
    return 0


if __name__ == "__main__":
    sys.exit(main())
