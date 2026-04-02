#!/usr/bin/env bash
# PaToH 小例子冒烟测试（ConsMLP/other_tools）
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
PATOH_DIR="$ROOT/PaToH/Linux-x86_64"
EX="$ROOT/PaToH/examples/tiny_test.u"
"$PATOH_DIR/patoh" "$EX" 2 WI=1
