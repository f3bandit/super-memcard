#!/bin/sh
# smcd and the Main shim end to end on a PC: the frontend's commands and Main's LAUNCH/MENU go to smcd, its card frames
# go through the shim (smc_shim.cpp, as Main runs it) to the virtual card (vcardd). Needs: make -C sim/card SMC=1.
# Copyright (C) 2026 f3bandit. Licensed under the Apache License 2.0 (LICENSE, NOTICE).
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
s = socket.socket(socket.AF_UNIX); s.connect("/tmp/smcd.sock"); s.settimeout(120)
s.sendall((sys.argv[1] + "\n").encode()); buf = b""
while not (buf.endswith(b"END\n") or buf.endswith(b"DONE\n")):
    d = s.recv(4096)
    if not d: break
    buf += d
print(buf.decode(), end="")
EOF
}

mkdir -p "$w/sd" "$w/saves/NES" "$w/games/NES" "$w/state"
export SMC_MISTER_CMD="$w/mister_cmd"   # Main's command pipe, for PLAY: a file here
# a NES game (iNES header + 16 KB) and its save
python3 -c "import os,sys; open(sys.argv[1],'wb').write(b'NES\x1a\x01\x00'+bytes(10)+os.urandom(16384))" "$w/games/NES/Test Game.nes"
python3 -c "import os,sys; open(sys.argv[1],'wb').write(os.urandom(8192))" "$w/saves/NES/Test Game.sav"
printf '<mistergamedescription><rbf>_Console/NES</rbf><file delay="1" type="f" index="0" path="%s/games/NES/Test Game.nes"/></mistergamedescription>' "$w" > "$w/game.mgl"

vcard() { "$B/vcardd" "$w/sd" "$w/vc.sock" > "$w/vcardd.log" 2>&1 & vc=$!; pids="$pids $vc"
    for i in $(seq 50); do [ -S "$w/vc.sock" ] && break; sleep 0.1; done; }
vcard_off() { kill $vc; wait $vc 2>/dev/null || true; rm -f "$w/vc.sock"; }
vcard
# 0. a card without an identity: smcd uses it as a PS1 card only
"$B/smcd" -S "$w/saves" -T "$w/state" -g "$w/games" -d "$w/lists" -v "$w/vc.sock" & sm=$!; pids="$pids $sm"
for i in $(seq 50); do [ -S /tmp/smcd.sock ] && break; sleep 0.1; done
ask HELLO | grep -q "^notgenuine .*no identity" && ok "card without an identity: notgenuine" || bad "card without an identity: $(ask HELLO)"
ask READ | grep -q "^notgenuine" && [ ! -d "$w/sd/SuperMemCard/NES" ] && ok "  and READ leaves it alone" || bad "  READ on a card that isn't genuine"
grep -q "isn't a genuine Retro Remake card" /tmp/smcd.log && ok "  said once in the log" || bad "  the log"
[ "$(grep -c "isn't a genuine" /tmp/smcd.log)" = 1 ] || bad "  said more than once"
# -G (test builds only): the same card without its identity used, for testing a new card before it has one
"$B/smcd" -G -S "$w/saves" -T "$w/stateG" -g "$w/games" -d "$w/lists" -v "$w/vc.sock" -l /tmp/smcd_g.sock > /dev/null 2>&1 & gpid=$!
for i in $(seq 50); do [ -S /tmp/smcd_g.sock ] && break; sleep 0.1; done
r=$(python3 -c "
import socket; s=socket.socket(socket.AF_UNIX); s.connect('/tmp/smcd_g.sock'); s.settimeout(30); s.sendall(b'HELLO\n'); b=b''
while not b.endswith(b'END\n'):
    d=s.recv(4096)
    if not d: break
    b+=d
print(b.decode().split(chr(10))[0])")
kill $gpid 2>/dev/null || true; wait $gpid 2>/dev/null || true; rm -f /tmp/smcd_g.sock
echo "$r" | grep -q "^card .* version 4" && ok "-G (test build): a card without its identity used" || bad "-G: $r"
kill $sm; wait $sm 2>/dev/null || true; rm -f /tmp/smcd.sock
# Retro Remake gives the card its identity (the test key), as at the factory
chip=$("$B/smccli" -s "$w/vc.sock" chip | sed -n 's/^chip //p')
"$B/smccert" issue "$here/../tools/smccert/test-rr.key" "$chip" "$w/sd/smc_key.bin" > /dev/null
vcard_off; vcard
"$B/smccli" -s "$w/vc.sock" chip | grep -qx genuine && [ ! -f "$w/sd/smc_key.bin" ] && ok "identity issued for chip $chip: genuine, key file gone" || bad "identity"
rm -f /tmp/smcd.log
"$B/smcd" -S "$w/saves" -T "$w/state" -g "$w/games" -d "$w/lists" -v "$w/none.sock" & pids="$pids $!"
for i in $(seq 50); do [ -S /tmp/smcd.sock ] && break; sleep 0.1; done

# CORESHELL=<core_shell checkout>: the same tests with Coreshell installed: Main's LAUNCH/MENU/ROM go through it (its
# card_event), and its launch log has them
if [ -n "$CORESHELL" ]; then
    mkdir -p "$w/root/coreshell"; printf '#!/bin/sh\n' > "$w/root/coreshell/coreshell.sh"; chmod +x "$w/root/coreshell/coreshell.sh"
    export SHIM_ROOT="$w/root"; rm -f /tmp/coreshell/launch.log
    (cd "$CORESHELL" && exec python3 -m coreshell serve --root "$w/root" --socket /tmp/coreshell.sock) > "$w/coreshell.log" 2>&1 & pids="$pids $!"
    for i in $(seq 50); do [ -S /tmp/coreshell.sock ] && break; sleep 0.1; done
fi

# 1. no Main passing frames yet: the frontend's command finds no card
[ "$(ask HELLO | head -1)" = nocard ] && ok "no Main shim: no card" || bad "no Main shim: no card"

# Main (the shim) passes frames from here on
"$B/shimtest" "$w/vc.sock" serve 300 > "$w/shim.log" 2>&1 & shim=$!; pids="$pids $shim"
sleep 0.5
ask HELLO | grep -q "^card .* version 4" && ok "HELLO through the shim" || bad "HELLO through the shim"

# PS1 mode's own per-game cards (MemoryCards/PS1/<id>/) as files: the system PS1CARD (firmware SMC_PS1CARD)
p1="$w/sd/MemoryCards/PS1/SLUS-00001"; mkdir -p "$p1"
python3 -c "import os,sys; open(sys.argv[1],'wb').write(b'MC'+os.urandom(128*1024-2))" "$p1/SLUS-00001-1.mcd"
x1=$(mktemp -d)
r=$(ask "EXPORT PS1CARD	1	SLUS-00001	$x1/out" | head -1)
[ "$r" = "export ok 1 1" ] && cmp -s "$x1/out/SLUS-00001/SLUS-00001-1.mcd" "$p1/SLUS-00001-1.mcd" \
    && ok "PS1CARD: a game's own PS1 card read as a file" || bad "PS1CARD export: $r"
mkdir -p "$x1/in/SLUS-00001"; python3 -c "import os,sys; open(sys.argv[1],'wb').write(b'MC'+os.urandom(128*1024-2))" "$x1/in/SLUS-00001/SLUS-00001-1.mcd"
r=$(ask "IMPORT PS1CARD	1	SLUS-00001	$x1/in" | head -1)
case "$r" in "import ok 1 "*) cmp -s "$x1/in/SLUS-00001/SLUS-00001-1.mcd" "$p1/SLUS-00001-1.mcd" \
    && ok "PS1CARD: written back whole (the file PS1 mode opens)" || bad "PS1CARD import: the file differs";; *) bad "PS1CARD import: $r";; esac
[ ! -e "$p1/SLUS-00001-1.mcd.new" ] && ok "  no .new left" || bad "  .new left"
r=$(ask "LIST PS1CARD" | grep -c "SLUS-00001")
[ "$r" -ge 1 ] && ok "PS1CARD: LIST has the game's card folder" || bad "PS1CARD list: $(ask "LIST PS1CARD")"
mounted=$(ls "$w/sd/MemoryCards/PS1" | grep -v SLUS-00001 | head -1)
r=$(ask "EXPORT PS1CARD	1	$mounted	$x1/busy" | head -1)
[ "$r" = "export failed" ] && [ ! -s "$x1/busy/$mounted/$mounted-1.mcd" ] && ok "PS1CARD: the card PS1 mode has open ($mounted) is busy, not read" || bad "PS1CARD busy: $r"
mkdir -p "$x1/w/$mounted"; cp "$x1/in/SLUS-00001/SLUS-00001-1.mcd" "$x1/w/$mounted/$mounted-1.mcd"; before=$(md5sum < "$w/sd/MemoryCards/PS1/$mounted/$mounted-1.mcd")
r=$(ask "IMPORT PS1CARD	1	$mounted	$x1/w" | head -1)
[ "$r" = "import failed" ] && [ "$(md5sum < "$w/sd/MemoryCards/PS1/$mounted/$mounted-1.mcd")" = "$before" ] \
    && ok "PS1CARD: the open card isn't written (its copy in memory is the newer one)" || bad "PS1CARD busy write: $r"
# PSXMERGE: Super MemCard's own PSX copies into each game's own PS1 card (the PSX store), merged, never losing a save
mk="$here/mkps1card.py"
saves_on() { python3 - "$1" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
out = []
for b in range(1, 16):
    e = d[b * 128:(b + 1) * 128]
    if e[0] == 0x51:
        out.append('%s=%02x' % (e[10:30].split(b'\0')[0].decode(), d[b * 8192 + 100]))
print(' '.join(sorted(out)))
PY
}
x2=$(mktemp -d); mkdir -p "$x2/in/SLUS-00002" "$x2/in/SLUS-00003"
python3 "$mk" "$x2/in/SLUS-00002/save.sav" BASLUS-00002AAA:0x11 BASLUS-00002BBB:0x22
python3 "$mk" "$x2/in/SLUS-00003/save.sav" BASLUS-00003CCC:0x33
# (Super MemCard's own PSX folder, as the copies made before version 4 are: IMPORT PSX is the games' own cards now)
for g in SLUS-00002 SLUS-00003; do mkdir -p "$w/sd/SuperMemCard/PSX/Card1/$g"; cp "$x2/in/$g/save.sav" "$w/sd/SuperMemCard/PSX/Card1/$g/"; done
mkdir -p "$w/sd/MemoryCards/PS1/SLUS-00002"; python3 "$mk" "$w/sd/MemoryCards/PS1/SLUS-00002/SLUS-00002-1.mcd" BASLUS-00002AAA:0x99
r=$(ask "PSXMERGE $x2/bk")
echo "$r" | grep -qx "merge SLUS-00002 added 1 kept 1 noroom 0 ok" && echo "$r" | grep -qx "merge SLUS-00003 added 1 kept 0 noroom 0 ok" \
    && echo "$r" | grep -qx "psxmerge ok 2" && ok "PSXMERGE: each copy into its game's own card" || bad "PSXMERGE: $r"
[ "$(saves_on "$w/sd/MemoryCards/PS1/SLUS-00002/SLUS-00002-1.mcd")" = "BASLUS-00002AAA=99 BASLUS-00002BBB=22" ] \
    && ok "  a save both had: the game's card's kept; the one it hadn't: added" || bad "  SLUS-00002's card: $(saves_on "$w/sd/MemoryCards/PS1/SLUS-00002/SLUS-00002-1.mcd")"
[ "$(saves_on "$w/sd/MemoryCards/PS1/SLUS-00003/SLUS-00003-1.mcd")" = "BASLUS-00003CCC=33" ] && ok "  a game with no card yet: a new card with its save" || bad "  SLUS-00003's card"
cmp -s "$x2/bk/SLUS-00002/save.sav" "$x2/in/SLUS-00002/save.sav" && cmp -s "$x2/bk/SLUS-00003/save.sav" "$x2/in/SLUS-00003/save.sav" \
    && ok "  every copy backed up first, as it was" || bad "  the backup: $(ls -R "$x2/bk")"
[ "$(saves_on "$w/state/psxmerge/SLUS-00002-1.mcd")" = "BASLUS-00002AAA=99" ] && ok "  and the game's card as it was, in smcd's state" || bad "  the game's card kept: $(ls "$w/state/psxmerge" 2>&1)"
[ -z "$(find "$w/sd/SuperMemCard/PSX/Card1" -name save.sav -not -path "*/.deleted/*" 2>/dev/null)" ] \
    && ok "  the copies taken off Super MemCard's PSX folder" || bad "  copies left: $(find "$w/sd/SuperMemCard/PSX" -type f)"
r=$(ask "PSXMERGE $x2/bk2"); [ "$(echo "$r" | head -1)" = "psxmerge ok 0" ] && ok "  run again: nothing left to move" || bad "  again: $r"
# the game whose card PS1 mode has open (switched to by SELECT, as at a launch): the card goes back to its default first
mkdir -p "$x2/in2/SLUS-00004"; python3 "$mk" "$x2/in2/SLUS-00004/save.sav" BASLUS-00004DDD:0x44
mkdir -p "$w/sd/SuperMemCard/PSX/Card1/SLUS-00004"; cp "$x2/in2/SLUS-00004/save.sav" "$w/sd/SuperMemCard/PSX/Card1/SLUS-00004/"
ask "SELECT PSX	SLUS-00004" | head -1 | grep -qx "select ok" || bad "PSXMERGE open card: SELECT"
sleep 3
r=$(ask "PSXMERGE $x2/bk3")
echo "$r" | grep -qx "merge SLUS-00004 added 1 kept 0 noroom 0 ok" && [ "$(saves_on "$w/sd/MemoryCards/PS1/SLUS-00004/SLUS-00004-1.mcd")" = "BASLUS-00004DDD=44" ] \
    && ok "  the game's card open in PS1 mode: left for the default card, then merged" || bad "  open card: $r / $(ls "$w/sd/MemoryCards/PS1")"
# the memory manager's PSX: each game's own PS1 card (LIST PSX, IMAGE PSX), PS1 mode's own cards (Card1...) left out
r=$(ask "LIST PSX")
echo "$r" | grep -q "^SLUS-00002	" && echo "$r" | grep -q "^SLUS-00003	" && ! echo "$r" | grep -q "^Card1	" \
    && ok "LIST PSX: the games' own PS1 cards (not PS1 mode's Card1)" || bad "LIST PSX: $r"
# a game's card with no save (PS1 mode makes a blank one when a game starts): deleted, not listed. One made by the card
# itself (SELECT, as a launch: it's open in PS1 mode too) and one with no saves on it
mkdir -p "$w/sd/MemoryCards/PS1/SLUS-00009"; python3 "$mk" "$w/sd/MemoryCards/PS1/SLUS-00009/SLUS-00009-1.mcd"
ask "SELECT PSX	SLUS-00010" | head -1 | grep -qx "select ok" || bad "empty cards: SELECT"
sleep 3
[ -f "$w/sd/MemoryCards/PS1/SLUS-00010/SLUS-00010-1.mcd" ] || bad "empty cards: the card didn't make SLUS-00010's"
r=$(ask "LIST PSX")
! echo "$r" | grep -q "^SLUS-0000[9]	\|^SLUS-00010	" && echo "$r" | grep -q "^SLUS-00002	" && [ ! -e "$w/sd/MemoryCards/PS1/SLUS-00009" ] \
    && [ ! -e "$w/sd/MemoryCards/PS1/SLUS-00010" ] && ok "a game's card with no save (blank, or just made by the card): deleted, not listed" \
    || bad "empty cards: $r / $(ls "$w/sd/MemoryCards/PS1")"
img_of() { ask "IMAGE PSX	1	$1" | tail -n +2 | grep -v '^END' | tr -d '\n' | python3 -c "import sys,binascii; sys.stdout.buffer.write(binascii.unhexlify(sys.stdin.read().strip()))" > "$2"; }
img_of SLUS-00002 "$x2/i1"; n1=$(grep -c "^.*bus: " /tmp/smcd.log)
cmp -s "$x2/i1" "$w/sd/MemoryCards/PS1/SLUS-00002/SLUS-00002-1.mcd" && ok "IMAGE PSX: the game's own card" || bad "IMAGE PSX: $(head -c 80 "$x2/i1" | od -c | head -2)"
img_of SLUS-00002 "$x2/i2"; n2=$(grep -c "^.*bus: " /tmp/smcd.log)
cmp -s "$x2/i2" "$x2/i1" && [ "$n2" = "$n1" ] && ok "  again: from smcd's copy, the card not read (its CRC the same)" || bad "  again: read again ($n1 -> $n2)"
python3 "$mk" "$w/sd/MemoryCards/PS1/SLUS-00002/SLUS-00002-1.mcd" BASLUS-00002ZZZ:0x77
img_of SLUS-00002 "$x2/i3"
cmp -s "$x2/i3" "$w/sd/MemoryCards/PS1/SLUS-00002/SLUS-00002-1.mcd" && ok "  changed by PS1 mode: read again" || bad "  changed: the old copy served"
# every PSX game at once (the memory manager's library backup): the games' own cards only, then back
mkdir -p "$w/sd/MemoryCards/PS1/Card1"
r=$(ask "EXPORT PSX	1	*	$x2/all" | head -1)
case "$r" in "export ok "*) [ -s "$x2/all/SLUS-00002/SLUS-00002-1.mcd" ] && [ ! -e "$x2/all/Card1" ] \
    && ok "EXPORT PSX *: every game's own card, not PS1 mode's Card1" || bad "EXPORT PSX *: $(ls "$x2/all")";; *) bad "EXPORT PSX *: $r";; esac
mkdir -p "$x2/all/Card1"; cp "$x2/all/SLUS-00002/SLUS-00002-1.mcd" "$x2/all/Card1/Card1-1.mcd"
r=$(ask "IMPORT PSX	1	*	$x2/all" | head -1)
case "$r" in "import ok "*) ! cmp -s "$w/sd/MemoryCards/PS1/Card1/Card1-1.mcd" "$x2/all/Card1/Card1-1.mcd" \
    && ok "IMPORT PSX *: back, and never onto PS1 mode's Card1" || bad "IMPORT PSX *: Card1 written";; *) bad "IMPORT PSX *: $r";; esac
[ ! -e "$w/state/ps1cards/SLUS-00009-1.mcd" ] && [ ! -e "$w/state/ps1cards/SLUS-00010-1.mcd" ] && ok "a deleted game card's copy in smcd's state goes too" || bad "ps1cards: $(ls "$w/state/ps1cards")"
rm -rf "$x2"
grep -q "^smc progress 0 Copying SLUS-00001" "$w/vcardd.log" && grep -q "^smc progress -1" "$w/vcardd.log" \
    && ok "the card's progress bar: up while a save is copied, down after" || bad "progress: $(grep progress "$w/vcardd.log" | head -5)"
[ "$(grep -c "^smc progress [0-9]" "$w/vcardd.log")" -ge 4 ] && ok "  and it moved as the 128 KB card went (more than start and end)" || bad "  progress steps: $(grep -c "^smc progress" "$w/vcardd.log")"
rm -rf "$x1"

# the card bus is someone else's (the memory manager reading the PS1 card): smcd waits, then leaves it alone
sleep 30 & holder=$!; pids="$pids $holder"
rm -f /tmp/super_memcard.busy; ln -s "$holder Console Mode memory manager" /tmp/super_memcard.busy
t0=$(date +%s); r=$(ask HELLO); t1=$(date +%s)
[ "$(echo "$r" | head -1)" = nocard ] && [ $((t1 - t0)) -ge 9 ] && grep -q "the card is in use (Console Mode memory manager)" /tmp/smcd.log \
    && ok "card in use by another: smcd waited, then left it alone" || bad "card in use: $r ($((t1 - t0)) s)"
kill $holder; wait $holder 2>/dev/null || true
ask HELLO | grep -q "^card " && ok "  and used it as soon as it was free (the owner gone)" || bad "  after the owner went"
[ ! -e /tmp/super_memcard.busy ] && ok "  smcd gave the card back after its command" || bad "  lock left: $(readlink /tmp/super_memcard.busy)"

# 2. the memory manager's Read card
r=$(ask READ)
echo "$r" | grep -q "^read ok .*pushed=1" && ok "READ: the save to the card" || bad "READ: $r"
ask LIST\ NES | grep -q "Test Game" && ok "LIST NES shows the game" || bad "LIST NES"
sec=$(ask SECTION)
echo "$sec" | grep -q "^system	NES	NES	1	1$" && echo "$sec" | grep -q "^game	NES	Test Game	" && echo "$sec" | grep -q "^section	1	1$" && ok "SECTION: the other systems' saves in one answer" || bad "SECTION: $sec"

# 3. Main loads the game: LAUNCH (the card's copy is the same: nothing to load), then the game saves while it runs
kill $shim; wait $shim 2>/dev/null || true
"$B/shimtest" "$w/vc.sock" launch _Console/NES_20260603.rbf "$w/game.mgl" > "$w/shim2.log" 2>&1 & shim=$!; pids="$pids $shim"
for i in $(seq 100); do grep -q "launch returned" "$w/shim2.log" && break; sleep 0.1; done
grep -q "launch returned" "$w/shim2.log" && ok "LAUNCH: Main waited for smcd, then went on" || bad "LAUNCH"
for i in $(seq 20); do grep -A4 "launch _Console/NES" /tmp/smcd.log | grep -q "game sync done" && break; sleep 0.1; done
grep -q "NES core: its card" /tmp/smcd.log && grep -A4 "launch _Console/NES" /tmp/smcd.log | grep -q "game sync done" && ok "LAUNCH: NES game synced before the core" || bad "LAUNCH: game sync"
sleep 1.2
python3 -c "import os,sys; open(sys.argv[1],'wb').write(os.urandom(8192))" "$w/saves/NES/Test Game.sav"
for i in $(seq 60); do grep -q "to card.*Test Game" /tmp/smcd.log && [ "$(grep -c 'to card.*Test Game' /tmp/smcd.log)" -ge 2 ] && break; sleep 0.25; done
[ "$(grep -c 'to card.*Test Game' /tmp/smcd.log)" -ge 2 ] && ok "in game: the new save to the card as soon as it settled" || bad "in game save"
f=$(find "$w/sd/SuperMemCard/NES" -name 'save.sav' | head -1)
cmp -s "$f" "$w/saves/NES/Test Game.sav" && ok "the card has the game's latest save" || bad "card's save differs"

# 3b. the OSD's Saves page while the game runs: its saves, a new one on another card, then back to the first
cp "$w/saves/NES/Test Game.sav" "$w/first.sav"
sv=$(ask SAVES)
echo "$sv" | grep -q "^save	1	1	[0-9]*	1	" && echo "$sv" | grep -q "^game	NES	Test Game$" && echo "$sv" | grep -q "^new	2$" \
    && ok "SAVES: card 1's save (being played), a new one on card 2" || bad "SAVES: $sv"
[ "$(ask 'USE 2' | head -1)" = "use ok 2" ] && [ ! -e "$w/saves/NES/Test Game.sav" ] && cmp -s "$f" "$w/first.sav" \
    && ok "USE 2 (new): the game starts with no save, card 1 keeps its" || bad "USE 2"
[ "$(ask 'USE 1' | head -1)" = "use ok 1" ] && cmp -s "$w/saves/NES/Test Game.sav" "$w/first.sav" \
    && ok "USE 1: card 1's save back on this SuperStation" || bad "USE 1"
ask SAVES | grep -q "^save	1	1	[0-9]*	1	" && ok "  and card 1 is the one being played" || bad "SAVES after USE 1"
[ "$(ask 'USE 0' | head -1)" = "use failed" ] && ok "USE 0: refused" || bad "USE 0"

# 3c. the same through Main's OSD Saves page (main/smc_osd.cpp): pick, asked again, the game restarts with it
kill $shim; wait $shim 2>/dev/null || true
[ -f /tmp/smc_game ] && grep -qx "$w/game.mgl" /tmp/smc_game && cmp -s /tmp/smc_game.mgl "$w/game.mgl" \
    && ok "LAUNCH said DONE saves: the shim kept the game's MGL (and a copy)" || bad "the shim's game file"
"$B/shimtest" "$w/vc.sock" osd s > "$w/osd1.log" 2>&1
grep -q "^available 1" "$w/osd1.log" && grep -q "^osd 0|  Test Game" "$w/osd1.log" && grep -q "^osd 2|> Card 1   now  [A-Z]" "$w/osd1.log" \
    && grep -q "^osd 3|  New save (card 2)" "$w/osd1.log" && grep -q "^osd 7|> Already running this save" "$w/osd1.log" \
    && ok "OSD: the page (the game, card 1 being played, a new save), the one playing not picked again" || bad "OSD page: $(cat "$w/osd1.log")"
"$B/shimtest" "$w/vc.sock" osd ds > "$w/osd2.log" 2>&1
grep -q "Again: restart" "$w/osd2.log" && ! grep -q "^restart" "$w/osd2.log" && cmp -s "$w/saves/NES/Test Game.sav" "$w/first.sav" \
    && ok "OSD: one select only asks (nothing changed)" || bad "OSD ask first: $(cat "$w/osd2.log")"
"$B/shimtest" "$w/vc.sock" osd dss > "$w/osd3.log" 2>&1
grep -q "^restart $w/game.mgl" "$w/osd3.log" && grep -q "^relaunched" "$w/osd3.log" && [ ! -e "$w/saves/NES/Test Game.sav" ] \
    && ok "OSD: New save picked, the game restarted from its MGL with no save" || bad "OSD new save: $(cat "$w/osd3.log")"
"$B/shimtest" "$w/vc.sock" osd uss > "$w/osd4.log" 2>&1
grep -q "^osd 3|> Card 2   now  empty" "$w/osd4.log" && grep -q "^restart" "$w/osd4.log" && cmp -s "$w/saves/NES/Test Game.sav" "$w/first.sav" \
    && ok "OSD: card 1 picked again, its save back" || bad "OSD card 1: $(cat "$w/osd4.log")"
long=$(sed -n 's/^osd 0|//p' "$w/osd1.log" | head -1); [ ${#long} -le 29 ] && ok "OSD: rows fit the OSD" || bad "OSD row too long"
# leaving a game (smc_shim_leave_game): the OSD up until the core's save has been written a moment, none in the menu
"$B/shimtest" "$w/vc.sock" leave 1200 > "$w/leave.log" 2>&1
at=$(sed -n 's/^osd off at \([0-9]*\) ms/\1/p' "$w/leave.log")
grep -q "^title Saving" "$w/leave.log" && [ "${at:-0}" -ge 1450 ] && [ "${at:-0}" -lt 2500 ] && grep -q "Main polled meanwhile" "$w/leave.log" \
    && [ "$(grep -c '^osd on' "$w/leave.log")" = 1 ] && ok "leaving a game: the OSD up until its save is written" || bad "leaving a game: $(cat "$w/leave.log")"

# 4. back in the menu: MENU (nothing changed since: nothing to do), the card's buttons are the PS1 card's again
kill $shim 2>/dev/null || true; wait $shim 2>/dev/null || true
"$B/shimtest" "$w/vc.sock" menu > "$w/shim3.log" 2>&1 & shim=$!; pids="$pids $shim"
sleep 3
grep -q "menu told" "$w/shim3.log" && ok "MENU told" || bad "MENU"
grep -q "sync stopped with an error" /tmp/smcd.log && bad "sync errors in the log" || ok "no sync errors"

# 4a. a game file picked in the running core's OSD (main/smc_shim.cpp smc_shim_rom_load): the same game, another with
#     the card driver in the core (smcd reaches the card through it), another without (through the menu core and back)
kill $shim 2>/dev/null || true; wait $shim 2>/dev/null || true
rm -f /tmp/smc_core /tmp/smc_bounce /tmp/smc_bounce.mgl
"$B/shimtest" "$w/vc.sock" launch _Console/NES_20260603.rbf "$w/game.mgl" > "$w/shim4.log" 2>&1 & shim=$!; pids="$pids $shim"
for i in $(seq 100); do grep -q "launch returned" "$w/shim4.log" && break; sleep 0.1; done
kill $shim 2>/dev/null || true; wait $shim 2>/dev/null || true
grep -q "^_Console/NES_20260603.rbf$" /tmp/smc_core && ok "ROM: the shim keeps the core smcd heard of" || bad "/tmp/smc_core: $(cat /tmp/smc_core 2>&1)"
"$B/shimtest" "$w/vc.sock" rom "$w/games/NES/Test Game.nes" > "$w/rom1.log" 2>&1
grep -q "^rom returned 1" "$w/rom1.log" && ! grep -q "^load" "$w/rom1.log" && ok "ROM: the same game again: loaded as it is" || bad "ROM same: $(cat "$w/rom1.log")"
cp "$w/games/NES/Test Game.nes" "$w/games/NES/Other Game.nes"
n=$(grep -c "game sync done" /tmp/smcd.log)
"$B/shimtest" "$w/vc.sock" rom "$w/games/NES/Other Game.nes" > "$w/rom2.log" 2>&1
grep -q "^rom returned 1" "$w/rom2.log" && ! grep -q "^load" "$w/rom2.log" && [ "$(grep -c "game sync done" /tmp/smcd.log)" -ge $((n + 1)) ] \
    && grep -q "path=\"$w/games/NES/Other Game.nes\"" /tmp/smc_bounce.mgl && grep -qx /tmp/smc_bounce.mgl /tmp/smc_game \
    && ok "ROM, a core with the driver: the old game's save to the card, the new one's from it, then loaded" || bad "ROM driver: $(cat "$w/rom2.log")"
cp "$w/games/NES/Test Game.nes" "$w/games/NES/Third Game.nes"
SMC_NODRIVER=1 "$B/shimtest" "$w/vc.sock" rom "$w/games/NES/Third Game.nes" > "$w/rom3.log" 2>&1
grep -q "^rom returned 0" "$w/rom3.log" && grep -q "^load menu.rbf" "$w/rom3.log" && grep -q "^load _Console/NES_20260603.rbf /tmp/smc_bounce.mgl" "$w/rom3.log" \
    && grep -q "^bounced" "$w/rom3.log" && grep -q "<rbf>_Console/NES_20260603</rbf>" /tmp/smc_bounce.mgl && grep -q "Third Game.nes" /tmp/smc_bounce.mgl \
    && grep -q "launch _Console/NES_20260603.rbf	/tmp/smc_bounce.mgl" /tmp/smcd.log && [ ! -e /tmp/smc_bounce ] \
    && ok "ROM, a stock core: through the menu core, then the core again with the game (LAUNCH)" || bad "ROM bounce: $(cat "$w/rom3.log")"
SMC_NODRIVER=1 "$B/shimtest" "$w/vc.sock" switch _Console/SNES_20260603.rbf > "$w/sw.log" 2>&1
grep -q "^load menu.rbf" "$w/sw.log" && grep -q "^load _Console/SNES_20260603.rbf" "$w/sw.log" && grep -q "launch _Console/SNES_20260603.rbf" /tmp/smcd.log \
    && ok "another core picked in a stock core's OSD: through the menu core too" || bad "core switch: $(cat "$w/sw.log")"
rm -f "$w/games/NES/Other Game.nes" "$w/games/NES/Third Game.nes" "$w/saves/NES/Other Game.sav" "$w/saves/NES/Third Game.sav"
"$B/shimtest" "$w/vc.sock" menu > "$w/shim5.log" 2>&1 & shim=$!; pids="$pids $shim"   # the menu again, passing frames
sleep 3

# 4b. the memory manager's Play: the save on a card, then Main told to load the game (its MGL from the launch here, else
#     one made from its ROM). Main's pipe is a file here: what smcd wrote to it is checked
cmp -s "$w/state/launch/NES/Test Game.mgl" "$w/game.mgl" && ok "PLAY: the game's MGL kept from its launch" || bad "the kept MGL"
: > "$w/mister_cmd"
r=$(ask "PLAY NES	2	Test Game" | head -1)
[ "$r" = "play ok 2" ] && grep -qx "load_core $w/state/launch/NES/Test Game.mgl" "$w/mister_cmd" && [ ! -e "$w/saves/NES/Test Game.sav" ] \
    && ok "PLAY on card 2 (no save there): Main told to load the game's MGL, no save" || bad "PLAY 2: $r / $(cat "$w/mister_cmd")"
: > "$w/mister_cmd"
r=$(ask "PLAY NES	1	Test Game" | head -1)
[ "$r" = "play ok 1" ] && cmp -s "$w/saves/NES/Test Game.sav" "$w/first.sav" && grep -q "^load_core " "$w/mister_cmd" \
    && ok "PLAY on card 1: its save back, the game loaded" || bad "PLAY 1: $r"
rm -f "$w/state/launch/NES/Test Game.mgl"; : > "$w/mister_cmd"
r=$(ask "PLAY NES	1	Test Game" | head -1)
[ "$r" = "play ok 1" ] && grep -qx "load_core /tmp/smc_play.mgl" "$w/mister_cmd" && grep -q "<rbf>_Console/NES</rbf>" /tmp/smc_play.mgl \
    && grep -q "type=\"f\" index=\"1\" path=\"$w/games/NES/Test Game.nes\"" /tmp/smc_play.mgl \
    && ok "PLAY with no MGL kept: one made from the ROM, as Console Mode's own NES MGLs" || bad "PLAY made MGL: $r / $(cat /tmp/smc_play.mgl 2>&1)"
: > "$w/mister_cmd"
[ "$(ask "PLAY NES	1	No Such Game" | head -1)" = "play notfound" ] && [ ! -s "$w/mister_cmd" ] && ok "PLAY: no ROM: notfound, Main not told" || bad "PLAY notfound"
[ "$(ask "PLAY PSX	1	Some Game" | head -1)" = "play psx" ] && ok "PLAY PSX: refused (its own card)" || bad "PLAY PSX"
[ "$(ask "PLAY NES	0	Test Game" | head -1)" = "play failed" ] && ok "PLAY card 0: refused" || bad "PLAY 0"
# PLAYPREP (Coreshell's): the same card switch, Main not told; the game's MGL in the answer, for Coreshell to start it
: > "$w/mister_cmd"
r=$(ask "PLAYPREP NES	2	Test Game" | head -1)
[ "$r" = "play ok 2	/tmp/smc_play.mgl" ] && [ ! -s "$w/mister_cmd" ] && [ ! -e "$w/saves/NES/Test Game.sav" ] \
    && ok "PLAYPREP on card 2: the switch, the MGL in the answer, Main not told" || bad "PLAYPREP 2: $r / $(cat "$w/mister_cmd")"
r=$(ask "PLAYPREP NES	1	Test Game" | head -1)
[ "$r" = "play ok 1	/tmp/smc_play.mgl" ] && cmp -s "$w/saves/NES/Test Game.sav" "$w/first.sav" && [ ! -s "$w/mister_cmd" ] \
    && ok "PLAYPREP on card 1: its save back" || bad "PLAYPREP 1: $r"
[ "$(ask "PLAYPREP NES	1	No Such Game" | head -1)" = "play notfound" ] && ok "PLAYPREP: no ROM: notfound" || bad "PLAYPREP notfound"
[ "$(ask "PLAYPREP PSX	1	Some Game" | head -1)" = "play psx" ] && ok "PLAYPREP PSX: its own card" || bad "PLAYPREP PSX"

# 4c. the memory manager's Rename (Console Mode's on-screen keyboard): a name for that card's save, on the card
[ "$(ask "NAME NES	1	Test Game	  Main run " | head -1)" = "name ok" ] && ask "LIST NES 1" | grep -q "	Test Game	.*	Main run$" \
    && ok "NAME: card 1's save named (spaces round it dropped), LIST shows it last" || bad "NAME: $(ask "LIST NES 1")"
[ "$(ask "NAME NES	7	Test Game	Nothing" | head -1)" = "name nosave" ] && ok "NAME on a card without the save: nosave" || bad "NAME nosave"
[ "$(ask "NAME NES	1	Test Game	a name far too long for the OSD" | head -1)" = "name failed" ] && ask "LIST NES 1" | grep -q "	Main run$" \
    && ok "NAME too long: refused, the old name kept" || bad "NAME too long"
ask SECTION | grep -q "^game	NES	.*	Main run$" && ok "SECTION shows the name too" || bad "SECTION name: $(ask SECTION)"
[ ! -e "$w/saves/NES/label.txt" ] && ok "the name never in the saves folder" || bad "label.txt in the saves"

# 4d. the memory manager's card picker: port picked, then a system's cards; a blank card, then back
two() { python3 - "$1" "$2" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX); s.connect("/tmp/smcd.sock"); s.settimeout(120)
s.sendall((sys.argv[1] + "\n" + sys.argv[2] + "\n").encode()); buf = b""
while buf.count(b"END\n") < 2:
    d = s.recv(4096)
    if not d: break
    buf += d
print(buf.decode(), end="")
PY
}
r=$(two "PORT 1" "CARDS NES")
echo "$r" | grep -q "^port 1$" && echo "$r" | grep -q "^card	1	1	[0-9]*	1$" && echo "$r" | grep -q "^new	2$" \
    && ok "PORT 1, then CARDS NES: card 1 (1 game, in use), a blank card 2" || bad "CARDS: $r"
[ "$(ask "PORT 3")" = "port any
END" ] && ok "PORT 3: refused, any port" || bad "PORT 3"
r=$(two "PORT 1" "CARD NES	2")
echo "$r" | grep -q "^card ok 2$" && [ ! -e "$w/saves/NES/Test Game.sav" ] && ok "CARD NES 2 (blank): NES games start with no save" || bad "CARD 2: $r"
r=$(ask "CARDS NES")
echo "$r" | grep -q "^card	1	1	[0-9]*	0$" && echo "$r" | grep -q "^card	2	0	0	1$" && echo "$r" | grep -q "^new	3$" \
    && ok "CARDS: card 1 kept its save, card 2 in use, the blank one offered is 3 (never the one in use)" || bad "CARDS after: $r"
[ "$(ask "CARD NES	1" | head -1)" = "card ok 1" ] && cmp -s "$w/saves/NES/Test Game.sav" "$w/first.sav" && ok "CARD NES 1: its save back" || bad "CARD 1"
[ "$(ask "CARD PSX	2" | head -1)" = "card failed" ] && [ "$(ask "CARD NES	0" | head -1)" = "card failed" ] && ok "CARD PSX or card 0: refused" || bad "CARD refusals"
# moving saves around: EXPORT a game, IMPORT it onto another card, DELGAME it there
x="$w/xfer"
gid=$(ask "LIST NES 1" | grep "^Test Game	" | head -1 | cut -f8)
r=$(ask "EXPORT NES	1	$gid	$x/one" | head -1)
[ "${r%% *}" = "export" ] && [ "$(echo "$r" | cut -d' ' -f2)" = ok ] && ls "$x/one/$gid/" | grep -q times.txt \
    && [ "$(ls "$x/one/$gid" | grep -vc times.txt)" -ge 1 ] && ok "EXPORT: a game's files and their times in a folder" || bad "EXPORT: $r $(ls -R "$x" 2>&1)"
r=$(ask "IMPORT NES	5	$gid	$x/one" | head -1)
case "$r" in "import ok 1 "*) ask "LIST NES 5" | grep -q "^Test Game	" && ok "IMPORT: the game on card 5 too" || bad "IMPORT list: $(ask "LIST NES 5")";; *) bad "IMPORT: $r";; esac
r=$(ask "IMPORT NES	5	$gid	$x/one" | head -1)
case "$r" in "import ok 1 "*) ok "IMPORT over the same game already on the card (a copy onto a card that has it)";; *) bad "IMPORT over itself: $r";; esac
r=$(ask "EXPORT NES	5	*	$x/all" | head -1)
case "$r" in "export ok 1 "*) ok "EXPORT *: every game on a card";; *) bad "EXPORT *: $r";; esac
r=$(ask "DELGAME NES	5	$gid" | head -1)
case "$r" in "delgame ok "*) [ -z "$(ask "LIST NES 5" | grep -v END)" ] && ask "LIST NES 1" | grep -q "^Test Game	" && ok "DELGAME: off card 5 only" || bad "DELGAME lists";; *) bad "DELGAME: $r";; esac
r=$(ask "IMPORT NES	6	*	$x/all" | head -1)
case "$r" in "import ok 1 "*) ask "LIST NES 6" | grep -q "^Test Game	" && ok "IMPORT *: a whole card's backup onto card 6" || bad "IMPORT * list";; *) bad "IMPORT *: $r";; esac
[ "$(ask "EXPORT NES	1	../x	$x/bad" | head -1)" = "export failed" ] && [ "$(ask "EXPORT NES	1	No Such Game	$x/bad" | head -1)" = "export failed" ] && [ "$(ask "IMPORT NES	1	*	relative" | head -1)" = "import failed" ] \
    && ok "EXPORT/IMPORT: no .. in an ID, only absolute folders" || bad "EXPORT/IMPORT refusals"

# 5. a long list on the OSD page (a stand-in smcd with 30 saves): it scrolls, no row cut off or past the page
# SELECT: the game being started. PSX: that game's own PS1 card on the bus (set game ID, the firmware's Game ID setting
# on, as it is by default); a game with no card yet gets a new blank one
r=$(ask "SELECT PSX	SLUS-01041" | head -1)
for i in $(seq 30); do [ -d "$w/sd/MemoryCards/PS1/SLUS-01041" ] && break; sleep 0.2; done
[ "$r" = "select ok" ] && [ -d "$w/sd/MemoryCards/PS1/SLUS-01041" ] && grep -q "PSX: game SLUS-01041 selected on the card" /tmp/smcd.log \
    && ok "SELECT PSX: the game's own card on the bus ($(ls "$w/sd/MemoryCards/PS1/SLUS-01041" | head -1))" || bad "SELECT PSX: $r, $(ls "$w/sd/MemoryCards/PS1" 2>&1)"
r=$(ask "SELECT NES	CRC-5B622B11" | head -1)
echo "$r" | grep -q "^select ok [0-9]" && ok "SELECT NES: the card in NES mode ($r)" || bad "SELECT NES: $r"
[ "$(ask "SELECT PSX" | head -1)" = "select failed" ] && ok "SELECT without a game ID: refused" || bad "SELECT without a game ID"

kill $(pgrep -f "$B/smcd") 2>/dev/null || true; sleep 0.5; rm -f /tmp/smcd.sock
python3 - <<'PY' & pids="$pids $!"
import socket, os
s = socket.socket(socket.AF_UNIX); s.bind("/tmp/smcd.sock"); s.listen(4)
for _ in range(4):
    c, _a = s.accept(); f = c.makefile("rw")
    if f.readline().strip() == "SAVES":
        for i in range(1, 31): f.write("save\t%d\t1\t%d\t%d\tPlaythrough number %02d\n" % (i, 1700000000 + i, i == 1, i))
        f.write("game\tNES\tA Game Whose Name Is Much Too Long For The OSD (USA, Europe) (Rev 1)\nnew\t31\nEND\n"); f.flush()
    c.close()
PY
sleep 0.5
"$B/shimtest" "$w/vc.sock" osd $(printf 'd%.0s' $(seq 33)) > "$w/osd5.log" 2>&1
last=$(grep -n "^title" "$w/osd5.log" | tail -1 | cut -d: -f1)
tail -n +$last "$w/osd5.log" | grep "^osd" > "$w/osd5.last"
grep -q "^osd 6|> New save (card 31)" "$w/osd5.last" && grep -q "^osd 2|  Card 27" "$w/osd5.last" \
    && [ "$(grep -c '^osd [2-6]|.*Card\|New save' "$w/osd5.last")" = 5 ] && ok "OSD: 31 rows scroll, the last one shown at the bottom" || bad "OSD long list: $(cat "$w/osd5.last")"
awk -F'|' '/^osd/ { if (length($2) > 30) bad = 1 } END { exit bad }' "$w/osd5.log" && ok "OSD: no row wider than the OSD (a long game name cut)" || bad "OSD long rows"
grep -q "^osd 7|  More: up/down" "$w/osd5.last" && ok "OSD: the hint says there's more" || bad "OSD hint"
grep -q "^osd 2|  Card 27  Playthrough number $" "$w/osd5.last" && ok "OSD: a save's name on its row" || bad "OSD name row"
grep -qE "^scroll [2-6]\| Card [0-9 ]+  Playthrough number [0-9]+  [A-Z][a-z]{2} [0-9]{2} 20[0-9]{2} [0-9:]{5}\|10 " "$w/osd5.log" \
    && ok "OSD: the selected row's name and date scroll (the card number stays)" || bad "OSD scroll: $(grep ^scroll "$w/osd5.log" | head -3)"

if [ -n "$CORESHELL" ]; then
    grep -q "card in .*_Console/NES_20260603.rbf" /tmp/coreshell/launch.log && grep -q "card out " /tmp/coreshell/launch.log \
        && ok "Coreshell: Main's LAUNCH and MENU went through it, in its launch log" || bad "Coreshell launch log: $(cat /tmp/coreshell/launch.log 2>&1)"
fi
# the icons' pictures (smc_sync.c smc_fetch_icons): kept as <base>.cover, for the menus' big picture
rm -rf /tmp/covt; mkdir -p /tmp/covt
python3 -c "
import zlib,struct
w,h=200,280
raw=b''.join(b'\\x00'+b''.join(bytes([(x*255//w),(y*255//h),120]) for x in range(w)) for y in range(h))
c=lambda t,d: struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
open('/tmp/covt/cover.png','wb').write(b'\\x89PNG\\r\\n\\x1a\\n'+c(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+c(b'IDAT',zlib.compress(raw))+c(b'IEND',b''))"
B="$here/../sim/card/build-smc"
if gcc -no-pie -I"$here/../host" "$here/covertest.c" "$B/smc_sync.o" "$B/smc_art_icon.o" "$B/smc_gamedb.o" "$B/smc_ident.o" \
       "$B/smc_gameid.o" "$B/smc_host.o" "$B/smc_icons.o" -o /tmp/covt/t -lz -lm 2>/dev/null; then
    /tmp/covt/t > /tmp/covt/out
    while read -r st rest; do [ "$st" = ok ] && ok "covers: $rest" || bad "covers: $rest"; done < /tmp/covt/out
else
    bad "covers: the test didn't build"
fi
echo "--- smcd.log"; cat /tmp/smcd.log | cut -c1-160
[ $fail = 0 ] && echo "ALL PASSED" || { echo FAILED; exit 1; }
