#!/bin/sh
# Layer 2 of the virtual SS1 (docs/07): Phosphor's front-port RTL (phosphor-menu) in Icarus Verilog, bit by bit
# against the virtual card (sd2psXtd's own PS1 code with Super MemCard, in vcardd) through a VPI bridge.
#   sh sim/bus/run.sh            (needs iverilog, and sim/card built: make -C sim/card SMC=1)
#   STOCK=1                      against stock sd2psXtd instead (sim/card built with make -C sim/card)
#   PHOSPHOR_MENU=<checkout>     use a local phosphor-menu instead of fetching the pinned commit
# Copyright (C) 2026 f3bandit.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
PIN=5b7b01e540253a42476eae6d2ce91685cdf62720   # phosphor-menu, branch supermemcard (extension version 2)
work=$(mktemp -d)
trap 'kill $vpid 2>/dev/null || true; wait $vpid 2>/dev/null || true; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT

rtl=$PHOSPHOR_MENU
if [ -z "$rtl" ]; then
	rtl=$work/phosphor-menu
	git init -q "$rtl"
	git -C "$rtl" fetch -q --depth 1 https://github.com/f3bandit/phosphor-menu "$PIN"
	git -C "$rtl" checkout -q FETCH_HEAD
fi

if [ -n "$STOCK" ]; then build=build; args=+stock; else build=build-smc; args=; fi
vcardd=${VCARDD:-$root/sim/card/$build/vcardd}
[ -x "$vcardd" ] || { echo "no $vcardd: build sim/card first"; exit 1; }

cd "$here"
iverilog-vpi vcard_vpi.c >/dev/null 2>&1
iverilog -g2012 -o "$work/tb_bus.vvp" tb_bus.sv vcard_bus.sv \
	"$rtl/rtl/phosphor/sio_byte.sv" "$rtl/rtl/phosphor/card_sector.sv" "$rtl/rtl/phosphor/phosphor_ext.sv"

mkdir "$work/sd"
export VCARD_SOCKET=$work/vcard.sock
"$vcardd" "$work/sd" "$VCARD_SOCKET" > "$work/vcardd.log" 2>&1 &
vpid=$!
for i in $(seq 100); do [ -S "$VCARD_SOCKET" ] && break; sleep 0.1; done

vvp -n -M. -mvcard_vpi "$work/tb_bus.vvp" $args | tee "$work/out.txt"

# what reached the SD card
kill -TERM $vpid; wait $vpid || true
if [ -z "$STOCK" ]; then
f=$(find "$work/sd/SuperMemCard" -path '*SNES/Card1/Bus Test/save.sav' 2>/dev/null | head -1)
python3 - "$f" <<'EOF' | tee -a "$work/out.txt"
import sys
p = sys.argv[1]
want = bytes(((i * 7 + b * 31) & 0xFF) for b in range(2) for i in range(128))
try:
    got = open(p, 'rb').read()
except Exception:
    print('FAIL save.sav not on the SD card'); sys.exit()
print('ok   save.sav on the SD card, as written' if got == want else 'FAIL save.sav on the SD card differs')
EOF
fi
grep -q FAIL "$work/out.txt" && { echo "--- vcardd"; tail -20 "$work/vcardd.log"; exit 1; }
grep -q "ALL PASSED" "$work/out.txt"
