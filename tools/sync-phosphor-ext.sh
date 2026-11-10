#!/bin/sh
# Copies phosphor-menu's front-port logic into core/sys (port, don't rewrite: change it in phosphor-menu, then run this
# and move sim/bus/run.sh's PIN to that commit; the phase 2 bus simulation checks the two match).
#   tools/sync-phosphor-ext.sh <phosphor-menu checkout>
# Copyright (C) 2026 f3bandit.
set -eu
src=${1:?usage: sync-phosphor-ext.sh <phosphor-menu checkout>}
dst=$(cd "$(dirname "$0")/.." && pwd)/core/sys
for f in phosphor_ext card_sector sio_byte; do
	{ echo "// Copied from phosphor-menu rtl/phosphor/$f.sv (port, do not edit here: change it there and copy, tools/sync-phosphor-ext.sh)."
	  cat "$src/rtl/phosphor/$f.sv"; } > "$dst/$f.sv"
done
echo "core/sys: phosphor-menu $(git -C "$src" rev-parse --short HEAD)"
