#!/bin/sh
# Copyright (C) 2026 f3bandit. All rights reserved.
# Super MemCard on the SS1 (copy to /media/fat/phosphor/tmp-test with smcd, vcardd and a test-bench Main with the shim): the test-bench Main with the shim (main/smc_shim.cpp) and smcd.
#   sh ss1test.sh start   stop Console Mode's Main, start vcardd, smcd (simulated card) and the test Main
#   sh ss1test.sh b       B: smcd with the simulated card: READ, then a NES game launched by Main (LAUNCH), then menu (MENU)
#   sh ss1test.sh a       A: the shim's frames to the real front port (NES core with the card driver)
#   sh ss1test.sh stop    everything off, Console Mode's Main back as it ran
# Copies of the saves in /tmp: the real ones aren't touched.
T=/media/fat/phosphor/tmp-test
ask() { python3 - "$1" <<'EOF'
import socket, sys
s = socket.socket(socket.AF_UNIX); s.connect("/tmp/smcd.sock"); s.settimeout(120)
s.sendall((sys.argv[1] + "\n").encode()); buf = b""
while not (buf.endswith(b"END\n") or buf.endswith(b"DONE\n")):
    d = s.recv(4096)
    if not d: break
    buf += d
print(buf.decode(), end="")
EOF
}
main_pid() { pidof MiSTer_smctest; }
cmd() { echo "$1" > /dev/MiSTer_cmd; }
smcd_start() {   # $1: the simulated card's socket (none: the front ports through the shim)
    killall smcd 2>/dev/null; sleep 0.3
    rm -f /tmp/smcd.log
    setsid $T/smcd -S /tmp/smc_saves -T /tmp/smc_state -g /media/usb0/games -d /tmp/smc_lists -v "$1" </dev/null >/dev/null 2>&1 &
    for i in 1 2 3 4 5 6 7 8 9 10; do [ -S /tmp/smcd.sock ] && break; sleep 0.2; done
}

case "$1" in
start)
    grep -v '^main=ConsoleMode/MiSTer_ConsoleMode' /media/fat/MiSTer.ini > /tmp/MiSTer_smctest.ini
    mountpoint -q /media/fat/MiSTer.ini || mount --bind /tmp/MiSTer_smctest.ini /media/fat/MiSTer.ini
    rm -rf /tmp/smc_saves /tmp/smc_state /tmp/smc_lists /tmp/smc_sd; mkdir -p /tmp/smc_saves /tmp/smc_state /tmp/smc_sd
    cp -r /media/fat/saves/NES /media/fat/saves/SNES /tmp/smc_saves/
    setsid $T/vcardd /tmp/smc_sd /tmp/smc_vc.sock </dev/null >/tmp/vcardd.log 2>&1 &
    sleep 0.5
    smcd_start /tmp/smc_vc.sock
    kill $(pidof MiSTer_ConsoleMode) 2>/dev/null; sleep 1
    cd /media/fat && setsid $T/MiSTer_smctest </dev/null >/tmp/main.log 2>&1 &
    sleep 4
    echo "main $(main_pid), smcd $(pidof smcd), vcardd $(pidof vcardd)"
    ;;
b)
    echo "== HELLO";  ask HELLO
    echo "== READ";   ask READ | tail -3
    echo "== LIST NES"; ask "LIST NES" | head -6
    printf '<mistergamedescription><rbf>_Console/NES</rbf><file delay="1" type="f" index="0" path="/media/usb0/games/NES/Final Fantasy.nes"/></mistergamedescription>' > /tmp/smc_ff.mgl
    echo "== launch Final Fantasy (MGL)"; t0=$(date +%s)
    cmd "load_core /tmp/smc_ff.mgl"
    sleep 8
    echo "core now: $(cat /tmp/CORENAME 2>/dev/null)"
    echo "== the game saves (the copy's save changed)"
    dd if=/dev/urandom of="/tmp/smc_saves/NES/Final Fantasy.sav" bs=8192 count=1 2>/dev/null
    sleep 5
    echo "== back to the menu"
    cmd "load_core /media/fat/menu.rbf"
    sleep 6
    echo "main now $(main_pid)"
    ok=; for d in $(find /tmp/smc_sd/SuperMemCard/NES -name save.sav); do cmp -s "$d" "/tmp/smc_saves/NES/Final Fantasy.sav" && ok=$d; done
    [ -n "$ok" ] && echo "OK the card has the game's latest save ($ok)" || echo "FAIL the card hasn't the game's latest save"
    echo "== smcd.log"; cat /tmp/smcd.log
    ;;
a)
    smcd_start /tmp/none.sock
    echo "== menu core (no driver): HELLO"; ask HELLO
    cmd "load_core /media/fat/supermemcard_test/NES_smc.rbf"
    sleep 6
    echo "core now: $(cat /tmp/CORENAME 2>/dev/null)"
    echo "== NES core with the driver: HELLO through the shim to the front port"; ask HELLO
    killall smcd; sleep 0.5
    echo "== a PS1 card read of sector 0, by hand through the shim's socket"
    python3 - <<'EOF'
import socket
s = socket.socket(socket.AF_UNIX); s.connect("/tmp/smc_frontport.sock"); s.settimeout(10)
f = s.makefile("rw")
def q(l): f.write(l + "\n"); f.flush(); return f.readline().strip()
print("HOLD 1 ->", q("HOLD 1"))
for port in (1, 2):
    r = q("FRAME %d 0 2 81520000" % port + "00" * 136)
    print("port", port, "->", r[:60], "...")
    p = r.split()
    if len(p) == 3 and p[0] == "OK":
        b = bytes.fromhex(p[2])
        print("   bytes answered", p[1], "| card ID", b[2:4].hex(), "(5a5d: a PS1 card)", "| end byte", b[139:140].hex(), "(47: good)")
print("HOLD 0 ->", q("HOLD 0"))
EOF
    cmd "load_core /media/fat/menu.rbf"; sleep 5
    echo "== smcd.log"; cat /tmp/smcd.log
    ;;
stop)
    killall smcd vcardd 2>/dev/null
    kill $(main_pid) 2>/dev/null; sleep 1
    mountpoint -q /media/fat/MiSTer.ini && umount /media/fat/MiSTer.ini
    rm -f /tmp/smcd.sock /tmp/smc_frontport.sock /tmp/smc_vc.sock
    sync
    cd / && setsid /media/fat/ConsoleMode/MiSTer_ConsoleMode ConsoleMode/menu_ConsoleMode.rbf </dev/null >/dev/null 2>&1 &
    sleep 3; ps -o pid,args | grep -E 'MiSTer|smcd|vcardd' | grep -v grep
    ;;
esac
