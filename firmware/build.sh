#!/bin/sh
# Build the real card firmware (sd2psXtd for the RP2040) with Super MemCard, to check the patch compiles and
# links on the hardware's toolchain, GUI included (sim/card builds neither the GUI nor the RP2040 parts).
#   firmware/build.sh [variant]      variant: SD2PSX (default, has the screen), SD2PSXlite, PMC+, PMCZero, PSXMemCard
# Needs cmake and gcc-arm-none-eabi (+ libnewlib-arm-none-eabi, libstdc++-arm-none-eabi-newlib). Output:
# firmware/build/<variant>/<name>.uf2, and with Super MemCard's updater (firmware/flashloader):
#   <name>-full.uf2               the updater and the firmware: a card's first flash over USB
#   <name>.bin                    the firmware for an update through the front port (smcd FWUPDATE)
#   supermemcard.bin/.version     the same .bin and its version, as Phosphor ships them (phosphor/firmware/)
# The version (what the card reports, FWINFO) is the build's date and time and this repository's commit.
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
SMC_BUILD=$(date -u +%Y%m%d.%H%M)-$(git -C "$here" rev-parse --short HEAD 2>/dev/null || echo local)
export SMC_BUILD
cmake -S "$src" -B "$here/build/$variant" -DCMAKE_BUILD_TYPE=Release -DVARIANT="$variant" -G "Unix Makefiles" >/dev/null
cmake --build "$here/build/$variant" -j"$(nproc)"
name=$(echo "$variant" | tr 'A-Z' 'a-z')
if [ -f "$here/build/$variant/$name.bin" ] && [ -f "$here/build/$variant/$name-full.uf2" ]; then
	cp "$here/build/$variant/$name.bin" "$here/build/$variant/supermemcard.bin"
	echo "$SMC_BUILD" > "$here/build/$variant/supermemcard.version"
fi
ls "$here/build/$variant"/*.uf2
echo "version $SMC_BUILD"
