#!/bin/bash
# realcard.sh: Super MemCard with real cards in the front ports, through the test Main's shim (no simulated card).
# Copy to /media/fat/supermemcard_test (with NES_smc.rbf: the NES core with the front-port driver, smcd and the
# test Main, MiSTer_smctest), as ss1test.sh.
#   start   Console Mode's Main stopped, the test Main started, the NES core with the front-port driver loaded
#   backup  port 1's card read (PS1 sector reads only: 81 52), all 1024 sectors, to backups/port1_<date>.mcd
#   cert    Hello and CERT by hand on port 2 (the chip ID for smccert issue) and port 1, at PS1 speed
#   smcd    smcd (SMCD_OPTS, default -G: a card without its identity used) HELLO on port 2 and port 1, its log
#   stop    restart the SS1 (everything back as it boots)
# Copyright (C) 2026 f3bandit. All rights reserved.
T=/media/fat/supermemcard_test
D=/media/fat/supermemcard_test
SMCD_OPTS=${SMCD_OPTS--G}
cmd() { echo "$1" > /dev/MiSTer_cmd; }
fp() { python3 - "$@" <<'EOF'
import socket, sys, time, functools; print = functools.partial(print, flush=True)
s = socket.socket(socket.AF_UNIX); s.connect("/tmp/smc_frontport.sock"); s.settimeout(15)
f = s.makefile("rw")
def q(l): f.write(l + "\n"); f.flush(); return f.readline().strip()
def frame(port, hexs, ack=100):   # speed 0 (PS1); ack: the ACK wait in ms (Super MemCard frames: smcd's 100)
    r = q("FRAME %d 0 %d %s" % (port, ack, hexs)).split()
    return bytes.fromhex(r[2]) if len(r) == 3 and r[0] == "OK" else None, " ".join(r[:2])
what = sys.argv[1]
print("HOLD 1 ->", q("HOLD 1")); time.sleep(0.1)
if what == "backup":
    out, bad = bytearray(), 0
    for sec in range(1024):
        for tr in range(4):
            b, st = frame(1, "81520000%04x" % sec + "00" * 134, 2)
            if b and len(b) >= 140 and b[139] == 0x47 and b[2:4] == b"\x5a\x5d":
                break
            time.sleep(0.1)
        else:
            print("sector", sec, "FAILED", st); bad += 1; b = bytes(140)
        out += b[10:138]
    open(sys.argv[2], "wb").write(out)
    print("read 1024 sectors,", bad, "failed ->", sys.argv[2])
elif what == "cert":
    # the first frame after the ports are switched on gets no answer (08): a plain read on each first
    for p in (1, 2): frame(p, "81520000%04x" % 0 + "00" * 134, 2); time.sleep(0.1)
    hello = "8160" + "534d027f00" + "00" * 16
    for port in (2, 1):
        b, st = frame(port, hello)
        if not b:
            time.sleep(0.06); b, st = frame(port, hello)
        if b and b[7:9] == b"SM":
            a = b[7:]
            print("port %d Hello: SM, protocol %d, features 0x%02x, card id %s" % (port, a[2], a[3], a[4:12].hex()))
        else:
            print("port %d Hello: no Super MemCard answer (%s)" % (port, st))
            time.sleep(0.1); continue
        time.sleep(0.05)
        b, st = frame(port, "816b" + "00" * 105)
        if not b: print("port %d CERT: no answer (%s)" % (port, st)); continue
        a = b[2:]
        print("port %d CERT: status %02x (47 has an identity, 4f none) chip %s" % (port, a[0], a[1:9].hex()))
print("HOLD 0 ->", q("HOLD 0"))
EOF
}
case "$1" in
start)
    grep -v '^main=ConsoleMode/MiSTer_ConsoleMode' /media/fat/MiSTer.ini > /tmp/MiSTer_smctest.ini
    mountpoint -q /media/fat/MiSTer.ini || mount --bind /tmp/MiSTer_smctest.ini /media/fat/MiSTer.ini
    kill $(pidof MiSTer_ConsoleMode) 2>/dev/null; sleep 1
    cd /media/fat && setsid $T/MiSTer_smctest </dev/null >/tmp/main.log 2>&1 &
    sleep 5
    cmd "load_core $D/NES_smc.rbf"; sleep 7
    echo "main $(pidof MiSTer_smctest), core $(cat /tmp/CORENAME 2>/dev/null), socket $(ls /tmp/smc_frontport.sock 2>&1)"
    ;;
backup)
    mkdir -p $D/backups; o=$D/backups/port1_$(date +%Y%m%d_%H%M).mcd
    fp backup "$o"; md5sum "$o"
    ;;
cert) fp cert ;;
smcd)
    killall smcd 2>/dev/null; sleep 0.3; rm -f /tmp/smcd.log; mkdir -p /tmp/smc_saves /tmp/smc_state /tmp/smc_lists
    setsid $T/smcd $SMCD_OPTS -S /tmp/smc_saves -T /tmp/smc_state -g /media/usb0/games -d /tmp/smc_lists </dev/null >/dev/null 2>&1 &
    for i in $(seq 20); do [ -S /tmp/smcd.sock ] && break; sleep 0.2; done
    for p in 2 1; do echo "== smcd PORT $p, HELLO"; python3 -c "
import socket; s=socket.socket(socket.AF_UNIX); s.connect('/tmp/smcd.sock'); s.settimeout(30)
s.sendall(b'PORT $p\nHELLO\n'); b=b''
while b.count(b'END\n')<2:
    d=s.recv(4096)
    if not d: break
    b+=d
print(b.decode(),end='')"; done
    echo "== smcd.log"; cat /tmp/smcd.log; killall smcd
    ;;
stop)
    killall smcd 2>/dev/null
    mountpoint -q /media/fat/MiSTer.ini && umount /media/fat/MiSTer.ini
    sync; (sleep 2; reboot -f) >/dev/null 2>&1 &
    echo "restarting the SS1"
    ;;
esac
