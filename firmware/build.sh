#!/bin/sh
# Build the real card firmware (sd2psXtd for the RP2040) with Super MemCard, to check the patch compiles and
# links on the hardware's toolchain, GUI included (sim/card builds neither the GUI nor the RP2040 parts).
#   firmware/build.sh [variant]      variant: SD2PSX (default, has the screen), SD2PSXlite, PMC+, PMCZero, PSXMemCard
# Needs cmake and gcc-arm-none-eabi (+ libnewlib-arm-none-eabi, libstdc++-arm-none-eabi-newlib). Output:
# firmware/build/<variant>/<name>.uf2
# Copyright (C) 2026 f3bandit.
set -e
here=$(cd "$(dirname "$0")" && pwd)
variant=${1:-SD2PSX}
REV=${SD2PSXTD_REV:-8a20bd991b43ac731882f5f85030b19e94c23a3b}   # as sim/card/get-firmware.sh
src=$here/build/sd2psXtd
if [ ! -d "$src/.git" ]; then
	mkdir -p "$here/build"
	git clone --quiet https://github.com/sd2psXtd/firmware.git "$src"
fi
git -C "$src" checkout --quiet -- . && git -C "$src" clean -qfd src
git -C "$src" fetch --quiet origin "$REV" 2>/dev/null || true
git -C "$src" checkout --quiet "$REV"
git -C "$src" submodule update --quiet --init --recursive
sh "$here/apply.sh" "$src"
# (the PS2 game list's github.com/.../raw/ address is refused from some build machines: the same file, by raw.githubusercontent.com)
sed -i 's#https://github.com/israpps/HDL-Batch-installer/raw/main/#https://raw.githubusercontent.com/israpps/HDL-Batch-installer/main/#' "$src/database/CMakeLists.txt"
cmake -S "$src" -B "$here/build/$variant" -DCMAKE_BUILD_TYPE=Release -DVARIANT="$variant" -G "Unix Makefiles" >/dev/null
cmake --build "$here/build/$variant" -j"$(nproc)"
ls "$here/build/$variant"/*.uf2
