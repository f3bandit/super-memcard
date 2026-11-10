#!/bin/sh
# Each core with the Super MemCard driver in its sys_top, on an SS1: loaded by the test Main (with the shim), the PS1 card
# in front port 1 read through that core's driver (sector 0, as a PS1 does it). Needs a PS1 card in port 1.
#   sh coretest.sh <folder of .rbf>      then the SS1 restarts (Console Mode back as it boots)
# Copyright (C) 2026 f3bandit. All rights reserved.
T=/media/fat/phosphor/tmp-test
dir=${1:?usage: coretest.sh <folder of .rbf>}
out=/media/fat/phosphor/tmp-test/smc_cores.txt   # on the SD card: the restart at the end clears /tmp
: > $out

grep -v '^main=ConsoleMode/MiSTer_ConsoleMode' /media/fat/MiSTer.ini > /tmp/MiSTer_smctest.ini
mountpoint -q /media/fat/MiSTer.ini || mount --bind /tmp/MiSTer_smctest.ini /media/fat/MiSTer.ini
kill $(pidof MiSTer_ConsoleMode) 2>/dev/null; sleep 1
cd /media/fat && setsid $T/MiSTer_smctest </dev/null >/tmp/main.log 2>&1 &
sleep 5

for rbf in "$dir"/*.rbf; do
    name=$(basename "$rbf")
    echo "load_core $rbf" > /dev/MiSTer_cmd
    sleep 8
    core=$(cat /tmp/CORENAME 2>/dev/null)
    r=$(python3 - <<'EOF'
import socket
try:
    s = socket.socket(socket.AF_UNIX); s.connect("/tmp/smc_frontport.sock"); s.settimeout(10)
    f = s.makefile("rw")
    def q(l): f.write(l + "\n"); f.flush(); return f.readline().strip()
    q("HOLD 1")
    r = q("FRAME 1 0 2 81520000" + "00" * 136)
    q("HOLD 0")
    p = r.split()
    if len(p) == 3 and p[0] == "OK":
        b = bytes.fromhex(p[2])
        print("OK" if p[1] == "140" and b[2:4] == b"\x5a\x5d" else "BAD", p[1], b[2:4].hex(), b[6:8].hex())
    else:
        print("BAD", r[:40])
except Exception as e:
    print("BAD", e)
EOF
)
    echo "$name	core $core	$r" >> $out
done

echo "done" >> $out
mountpoint -q /media/fat/MiSTer.ini && umount /media/fat/MiSTer.ini
sync
(sleep 2; reboot -f) >/dev/null 2>&1 &
