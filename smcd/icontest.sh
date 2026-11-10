#!/bin/sh
# Save icons end to end on a PC, with real downloads (needs the internet): smcd and the simulated card.
#   1. a game the game lists know: libretro's cover made into its icon
#   2. a homebrew game with Console Mode's art beside it (media/<game>.png): that picture
#   3. a game with no picture anywhere: its system's placeholder stays
#   4. a manual read again: nothing downloaded twice; a PSX card image keeps its own icon
# Copyright (C) 2026 f3bandit. All rights reserved.
set -e
here=$(cd "$(dirname "$0")" && pwd)
B=${B:-$here/../sim/card/build-smc}
w=$(mktemp -d)
pids=
trap 'for p in $pids; do kill $p 2>/dev/null || true; done; wait 2>/dev/null; [ -n "$KEEP" ] && echo "kept $w" || rm -rf "$w"' EXIT
fail=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fail=1; }
ask() { python3 - "$1" <<'EOF'
import socket, sys
s = socket.socket(socket.AF_UNIX); s.connect("/tmp/smcd.sock"); s.settimeout(300)
s.sendall((sys.argv[1] + "\n").encode()); buf = b""
while not buf.endswith(b"END\n"):
    d = s.recv(65536)
    if not d: break
    buf += d
print(buf.decode(), end="")
EOF
}
# a game's folder on the card is its game ID: found by its name.txt
folder() { for d in "$w/sd/SuperMemCard/$1"/Card1/*/; do d=${d%/}; n=$(cat "$d/name.txt" 2>/dev/null || basename "$d"); [ "$n" = "$2" ] && echo "$d"; done; true; }
card_icon() { f=$(folder "$1" "$2")/icon.bin; [ -f "$f" ] && echo "$f"; true; }

mkdir -p "$w/sd" "$w/saves/NES" "$w/saves/PSX" "$w/games/NES/media" "$w/games/NES/Homebrew/media" "$w/state" "$w/lists"
# 1: a ROM the game list will name "Final Fantasy (USA)" (its CRC is put in the list below)
python3 -c "import os,sys; open(sys.argv[1],'wb').write(b'NES\x1a\x02\x01'+bytes(10)+os.urandom(32768))" "$w/games/NES/Final Fantasy.nes"
head -c 8192 /dev/urandom > "$w/saves/NES/Final Fantasy.sav"
# 2: homebrew with its own picture (a 40x56 picture: red top, blue bottom)
python3 -c "import os,sys; open(sys.argv[1],'wb').write(b'NES\x1a\x01\x00'+bytes(10)+os.urandom(16384))" "$w/games/NES/Homebrew/My Homebrew.nes"
python3 -c "
import sys; w,h=40,56
px=b''.join((b'\xd0\x20\x20' if y<h//2 else b'\x20\x40\xd0')*w for y in range(h))
open(sys.argv[1],'wb').write(b'P6\n%d %d\n255\n'%(w,h)+px)" "$w/games/NES/Homebrew/media/My Homebrew.ppm"
mv "$w/games/NES/Homebrew/media/My Homebrew.ppm" "$w/games/NES/Homebrew/media/My Homebrew.png"
head -c 8192 /dev/urandom > "$w/saves/NES/My Homebrew.sav"
# 3: no ROM, no picture
head -c 8192 /dev/urandom > "$w/saves/NES/Nothing Known.sav"

"$B/vcardd" "$w/sd" "$w/vc.sock" > /dev/null 2>&1 & vc=$!; pids="$pids $vc"
for i in $(seq 50); do [ -S "$w/vc.sock" ] && break; sleep 0.1; done
chip=$("$B/smccli" -s "$w/vc.sock" chip | sed -n 's/^chip //p')
"$B/smccert" issue "${KEY:-$here/../tools/smccert/test-rr.key}" "$chip" "$w/sd/smc_key.bin" > /dev/null
kill $vc; wait $vc 2>/dev/null || true
"$B/vcardd" "$w/sd" "$w/vc.sock" > /dev/null 2>&1 & vc=$!; pids="$pids $vc"
for i in $(seq 50); do [ -S "$w/vc.sock" ] && break; sleep 0.1; done

smcd_start() {
    rm -f /tmp/smcd.log
    "$B/smcd" -S "$w/saves" -T "$w/state" -g "$w/games" -d "$w/lists" -v "$w/vc.sock" & sm=$!; pids="$pids $sm"
    for i in $(seq 50); do [ -S /tmp/smcd.sock ] && break; sleep 0.1; done
}
# first pass: finds the ROM's game ID (CRC), so the game list can be written for it
smcd_start
ask READ > /dev/null
kill $sm; wait $sm 2>/dev/null || true; rm -f /tmp/smcd.sock
id=$(basename "$(folder NES "Final Fantasy")")
crc=${id#CRC-}
printf 'clrmamepro (\n\tname "Nintendo - Nintendo Entertainment System"\n)\n\ngame (\n\tname "Final Fantasy (USA)"\n\trom ( name "Final Fantasy (USA).nes" size 262144 crc %s )\n)\n' "$crc" \
    > "$w/lists/Nintendo - Nintendo Entertainment System.dat"
rm -rf "$w/state/icons"
# a manual read: icons looked for, made and put on the card in the same read
smcd_start
r=$(ask READ)
echo "$r" | grep -q "^read ok" && ok "manual read" || bad "read: $r"
f=$(card_icon NES "Final Fantasy")
[ -n "$f" ] && [ "$(head -c 4 "$f")" = SMIC ] && [ "$(od -An -tu1 -j6 -N2 "$f" | tr -s ' ')" = " 32 32" ] \
    && ok "1. Final Fantasy: libretro's cover, a 32x32 icon on the card" || bad "1. Final Fantasy's icon ($f)"
[ -f "$w/state/icons/NES/$id.bin" ] && cmp -s "$w/state/icons/NES/$id.bin" "$f" && grep -q "icons: 1 made from pictures, 1 put on the card" /tmp/smcd.log \
    && ok "   downloaded, made and put on the card by the read itself" || bad "   log: $(grep icons /tmp/smcd.log)"
f2=$(card_icon NES "My Homebrew")
[ -n "$f2" ] && [ "$(od -An -tu1 -j6 -N2 "$f2" | tr -s ' ')" = " 32 32" ] && ok "2. homebrew: its own picture" || bad "2. homebrew icon"
f3=$(card_icon NES "Nothing Known")
[ -n "$f3" ] && [ "$(wc -c < "$f3")" = 168 ] && ok "3. nothing known: the system's placeholder" || bad "3. placeholder"
[ -f "$w/state/icons/NES/$(basename "$(dirname "$f3")").none" ] || [ -f "$w/state/icons/NES/Nothing Known.none" ] && ok "   marked: no picture anywhere" || bad "   .none mark"
# 4. again: nothing to download, nothing changes
before=$(cat "$f" | md5sum)
made_before=$(grep -c "icons: [1-9][0-9]* made" /tmp/smcd.log || true)
ask READ > /dev/null
made_after=$(grep -c "icons: [1-9][0-9]* made" /tmp/smcd.log || true)
[ "$(cat "$f" | md5sum)" = "$before" ] && [ "$made_after" = "$made_before" ] \
    && ok "4. read again: nothing downloaded twice" || bad "4. second read ($made_before -> $made_after)"
# 5. a game launched (not a read): its icon made in the background, on the card the next time smcd opens it
python3 -c "import os,sys; open(sys.argv[1],'wb').write(b'NES\x1a\x01\x00'+bytes(10)+os.urandom(16384))" "$w/games/NES/Homebrew/Later Game.nes"
cp "$w/games/NES/Homebrew/media/My Homebrew.png" "$w/games/NES/Homebrew/media/Later Game.png"
head -c 8192 /dev/urandom > "$w/saves/NES/Later Game.sav"
printf '<mistergamedescription><rbf>_Console/NES</rbf><file delay="1" type="f" index="0" path="%s"/></mistergamedescription>' "$w/games/NES/Homebrew/Later Game.nes" > "$w/later.mgl"
python3 - "$w/later.mgl" <<'EOF2'
import socket, sys
s = socket.socket(socket.AF_UNIX); s.connect("/tmp/smcd.sock"); s.settimeout(120)
s.sendall(("LAUNCH _Console/NES_20260603.rbf\t%s\n" % sys.argv[1]).encode()); b = b""
while not b.endswith(b"DONE\n"):
    d = s.recv(4096)
    if not d: break
    b += d
EOF2
fl=$(card_icon NES "Later Game")
[ -n "$fl" ] && [ "$(wc -c < "$fl")" = 168 ] && ok "5. launched: the placeholder first (the sync doesn't wait)" || bad "5. icon at launch ($fl)"
for i in $(seq 50); do grep -q "icons: 1 made" <(tail -2 /tmp/smcd.log) && break; sleep 0.2; done
ask HELLO > /dev/null
fl=$(card_icon NES "Later Game")
[ -n "$fl" ] && [ "$(wc -c < "$fl")" = 552 ] && ok "   made in the background, on the card at its next opening" || bad "   background icon ($(wc -c < "$fl"))"
ls -la "$w/state/icons/NES" | sed 1d | awk '{print "     " $5, $9}'
kill $sm
[ $fail = 0 ] && echo "ALL PASSED" || { echo FAILED; exit 1; }
