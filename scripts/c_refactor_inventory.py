#!/usr/bin/env python3
"""
Generate C/C++ inventory for refactor tracking.
"""

from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN_DIR = ROOT / "main"
BOARDS_DIR = MAIN_DIR / "boards"
OUTPUT_FILE = ROOT / "c_refactor_inventory.json"


def list_files(base: Path, suffixes: tuple[str, ...]) -> list[Path]:
    files: list[Path] = []
    for path in base.rglob("*"):
        if path.is_file() and path.suffix in suffixes:
            files.append(path)
    return sorted(files)


def rel(path: Path) -> str:
    return str(path.relative_to(ROOT))


def main() -> None:
    cc_files = list_files(MAIN_DIR, (".cc", ".cpp", ".cxx"))
    c_files = list_files(MAIN_DIR, (".c",))
    h_files = list_files(MAIN_DIR, (".h", ".hpp"))

    board_dirs = sorted(
        p for p in BOARDS_DIR.iterdir() if p.is_dir() and p.name != "common"
    )
    board_summary = []
    for board in board_dirs:
        board_cc = sorted(board.rglob("*.cc"))
        board_c = sorted(board.rglob("*.c"))
        board_h = sorted(board.rglob("*.h"))
        board_summary.append(
            {
                "board": board.name,
                "cc_count": len(board_cc),
                "c_count": len(board_c),
                "h_count": len(board_h),
                "cc_files": [rel(p) for p in board_cc],
                "c_files": [rel(p) for p in board_c],
                "h_files": [rel(p) for p in board_h],
            }
        )

    inventory = {
        "root": str(ROOT),
        "main_summary": {
            "cc_like_count": len(cc_files),
            "c_count": len(c_files),
            "header_count": len(h_files),
            "cc_like_files": [rel(p) for p in cc_files],
            "c_files": [rel(p) for p in c_files],
            "header_files": [rel(p) for p in h_files],
        },
        "boards": {
            "count": len(board_dirs),
            "items": board_summary,
        },
    }

    OUTPUT_FILE.write_text(json.dumps(inventory, indent=2, ensure_ascii=False) + "\n")
    print(f"Wrote {OUTPUT_FILE}")


if __name__ == "__main__":
    main()
