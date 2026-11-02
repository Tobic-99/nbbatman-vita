#!/bin/sh
# Translate the game's main CPU code: rom/ -> gen/v33/. Needs the ROM files in rom/.
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
cd "$R"
mkdir -p gen
python3 tools/make_main_bin.py
python3 tools/v33rec.py
