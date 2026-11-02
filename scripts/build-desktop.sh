#!/bin/sh
# Linux runner: build/desktop/nbb_desktop. Needs scripts/translate.sh first.
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
cd "$R"
O=build/desktop/obj
mkdir -p "$O"
Y=third_party/ymfm/src
objs=""
for src in host/v33.cpp host/pic.cpp host/board.cpp host/video.cpp host/png.cpp host/rec.cpp host/machine.cpp host/sound.cpp host/nbb_main.cpp gen/v33/*.cpp "$Y/ymfm_opm.cpp"; do
    o=$O/$(basename "$src" .cpp).o
    if [ ! -f "$o" ] || [ "$src" -nt "$o" ] || [ -n "$(find host -newer "$o" -name '*.h' -print -quit)" ]; then
        clang++ -std=c++17 -O2 -Wno-unused -Wno-unused-label -DNBB_YMFM -I host -I "$Y" -c "$src" -o "$o"
    fi
    objs="$objs $o"
done
clang++ $objs -o build/desktop/nbb_desktop
