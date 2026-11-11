#!/bin/sh
# The memory manager's card picker on an SS1 (smcd on the real saves folder, the simulated card): SNES to a blank card,
# a game started there (no save), then back to card 1 (every SNES save back, byte for byte). Log: picker.log
T=/media/fat/phosphor/tmp-test; S=/media/fat/saves; LOG=$T/picker.log; : > $LOG
say() { echo "$*" | tee -a $LOG; }
two() { python3 - "$1" "$2" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX); s.connect("/tmp/smcd.sock"); s.settimeout(600)
s.sendall((sys.argv[1] + "\n" + sys.argv[2] + "\n").encode()); buf = b""
while buf.count(b"END\n") < 2:
    d = s.recv(4096)
    if not d: break
    buf += d
print(buf.decode().replace("END\n", ""), end="")
PY
}
(cd $S && md5sum SNES/* ) > /tmp/picker_snes.md5
say "SNES saves here: $(ls $S/SNES | wc -l)"
say "== PORT 1, CARDS SNES"; two "PORT 1" "CARDS SNES" | tee -a $LOG
say "== PORT 1, CARD SNES 2 (the blank one)"; two "PORT 1" "CARD SNES	2" | tee -a $LOG
say "SNES saves here now: $(ls $S/SNES 2>/dev/null | wc -l)"
printf '<mistergamedescription><rbf>_Console/SNES</rbf><file delay="2" type="f" index="0" path="/media/usb0/games/SNES/Final Fantasy VI (TWUE 3.06).sfc"/></mistergamedescription>' > /tmp/picker.mgl
echo "load_core /tmp/picker.mgl" > /dev/MiSTer_cmd; sleep 15
say "FF6 started on card 2: core $(cat /tmp/CORENAME), its save here: $(ls "$S/SNES/Final Fantasy VI (TWUE 3.06).sav" 2>/dev/null || echo none)"
echo "load_core /media/fat/menu.rbf" > /dev/MiSTer_cmd; sleep 9
say "== CARDS SNES"; two "PORT 1" "CARDS SNES" | tee -a $LOG
say "== CARD SNES 1"; two "PORT 1" "CARD SNES	1" | tee -a $LOG
(cd $S && md5sum -c /tmp/picker_snes.md5 >/dev/null 2>&1) && say "every SNES save back byte for byte: yes" || say "every SNES save back byte for byte: NO"
say "smcd errors: $(grep -ciE 'error|failed:' /tmp/smcd.log)"
say "done"
