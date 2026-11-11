#!/bin/sh
# smcd and the Main shim end to end on a PC: the frontend's commands and Main's LAUNCH/MENU go to smcd, its card frames
# go through the shim (smc_shim.cpp, as Main runs it) to the virtual card (vcardd). Needs: make -C sim/card SMC=1.
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
kill $sm; wait $sm 2>/dev/null || true; rm -f /tmp/smcd.sock
# Retro Remake gives the card its identity (the test key), as at the factory
chip=$("$B/smccli" -s "$w/vc.sock" chip | sed -n 's/^chip //p')
"$B/smccert" issue "$here/../tools/smccert/test-rr.key" "$chip" "$w/sd/smc_key.bin" > /dev/null
vcard_off; vcard
"$B/smccli" -s "$w/vc.sock" chip | grep -qx genuine && [ ! -f "$w/sd/smc_key.bin" ] && ok "identity issued for chip $chip: genuine, key file gone" || bad "identity"
rm -f /tmp/smcd.log
"$B/smcd" -S "$w/saves" -T "$w/state" -g "$w/games" -d "$w/lists" -v "$w/none.sock" & pids="$pids $!"
for i in $(seq 50); do [ -S /tmp/smcd.sock ] && break; sleep 0.1; done

# 1. no Main passing frames yet: the frontend's command finds no card
[ "$(ask HELLO | head -1)" = nocard ] && ok "no Main shim: no card" || bad "no Main shim: no card"

# Main (the shim) passes frames from here on
"$B/shimtest" "$w/vc.sock" serve 300 > "$w/shim.log" 2>&1 & shim=$!; pids="$pids $shim"
sleep 0.5
ask HELLO | grep -q "^card .* version 2" && ok "HELLO through the shim" || bad "HELLO through the shim"

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

# 4. back in the menu: MENU (nothing changed since: nothing to do), the card's buttons are the PS1 card's again
kill $shim 2>/dev/null || true; wait $shim 2>/dev/null || true
"$B/shimtest" "$w/vc.sock" menu > "$w/shim3.log" 2>&1 & shim=$!; pids="$pids $shim"
sleep 3
grep -q "menu told" "$w/shim3.log" && ok "MENU told" || bad "MENU"
grep -q "sync stopped with an error" /tmp/smcd.log && bad "sync errors in the log" || ok "no sync errors"

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

# 4c. the memory manager's Rename (Console Mode's on-screen keyboard): a name for that card's save, on the card
[ "$(ask "NAME NES	1	Test Game	  Main run " | head -1)" = "name ok" ] && ask "LIST NES 1" | grep -q "	Test Game	.*	Main run$" \
    && ok "NAME: card 1's save named (spaces round it dropped), LIST shows it last" || bad "NAME: $(ask "LIST NES 1")"
[ "$(ask "NAME NES	7	Test Game	Nothing" | head -1)" = "name nosave" ] && ok "NAME on a card without the save: nosave" || bad "NAME nosave"
[ "$(ask "NAME NES	1	Test Game	a name far too long for the OSD" | head -1)" = "name failed" ] && ask "LIST NES 1" | grep -q "	Main run$" \
    && ok "NAME too long: refused, the old name kept" || bad "NAME too long"
ask SECTION | grep -q "^game	NES	.*	Main run$" && ok "SECTION shows the name too" || bad "SECTION name: $(ask SECTION)"
[ ! -e "$w/saves/NES/label.txt" ] && ok "the name never in the saves folder" || bad "label.txt in the saves"

# 5. a long list on the OSD page (a stand-in smcd with 30 saves): it scrolls, no row cut off or past the page
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

echo "--- smcd.log"; cat /tmp/smcd.log | cut -c1-160
[ $fail = 0 ] && echo "ALL PASSED" || { echo FAILED; exit 1; }
