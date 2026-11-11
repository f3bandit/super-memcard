#!/bin/sh
# Super MemCard on an SS1: one game a system, Play on card 2 (no save), then Play on card 1 (its save back, byte for
# byte), the core checked each time. Needs the test Main in the menu, vcardd (the simulated card, with an identity) and
# smcd on the real saves folder (ss1test.sh start, then smcd -S /media/fat/saves). Backs the saves up first.
#   sh sweep.sh            log: /media/fat/phosphor/tmp-test/sweep.log
# Copyright (C) 2026 f3bandit. All rights reserved.
T=/media/fat/phosphor/tmp-test
DL=$T/dl
S=/media/fat/saves
BK=$T/saves_backup_sweep
LOG=$T/sweep.log
: > $LOG
say() { echo "$*" | tee -a $LOG; }
ask() { python3 - "$1" <<'EOF'
import socket, sys
s = socket.socket(socket.AF_UNIX); s.connect("/tmp/smcd.sock"); s.settimeout(900)
s.sendall((sys.argv[1] + "\n").encode()); buf = b""
while not buf.endswith(b"END\n"):
    d = s.recv(4096)
    if not d: break
    buf += d
print(buf.decode().split("\n")[0])
EOF
}
cmd() { echo "$1" > /dev/MiSTer_cmd; }
menu() { cmd "load_core /media/fat/menu.rbf"; sleep 9; }
core() { cat /tmp/CORENAME 2>/dev/null; }
# a game's save files in its folder: <game>.<ext>, <game>_N.<ext>
files() { (cd "$S/$1" 2>/dev/null && for f in "$2".* "$2"_[1-4].*; do [ -f "$f" ] && echo "$f"; done); }

# 1. the saves, backed up (the folders this touches)
rm -rf $BK; mkdir -p $BK
for d in NES SNES GAMEBOY GBC GBA MegaDrive N64 TGFX16 NEOGEO; do [ -d $S/$d ] && cp -a $S/$d $BK/; done
(cd $S && find NES SNES GAMEBOY GBC GBA MegaDrive N64 TGFX16 NEOGEO -type f 2>/dev/null | while read f; do md5sum "$f"; done) > $BK/md5.txt
say "backed up: $(wc -l < $BK/md5.txt) save files in $BK"

# 2. the downloaded saves where a game has none (never over a save that's there)
put() {   # <system> <file name> <from>
    mkdir -p "$S/$1"
    if [ -e "$S/$1/$2" ]; then say "  kept $1/$2 (there already)"; else cp "$3" "$S/$1/$2" && say "  put $1/$2" && echo "$S/$1/$2" >> $BK/put.txt; fi
}
put GAMEBOY "Final Fantasy Legend II.sav" $DL/gb_ffl2.sav
put GBC "Legend of Zelda, The - Link's Awakening DX.sav" "$DL/ladx/LinksAwakeningDXSaves/Save 3/Legend of Zelda, The - Links Awakening DX (USA, Europe) (Rev B) (SGB Enhanced).srm"
put GBA "Legend of Zelda, The - The Minish Cap.sav" $DL/gba_minish.sav
put MegaDrive "Phantasy Star IV.sav" $DL/md_ps4.sav
put N64 "Legend of Zelda, The - Ocarina of Time.sra" $DL/n64_oot.sra
put N64 "Legend of Zelda, The - Majora's Mask.fla" $DL/n64_mm.fla
put TGFX16 "Akumajou Dracula X - Chi no Rondo (Japan).sav" $DL/pce_rondo.sav
sync

# 3. the games: system, save name, the core it should be, and an MGL for the ones started only from one
G=/media/usb0/games
mgl_cd() { printf '<mistergamedescription><rbf>_Console/TurboGrafx16</rbf><file delay="1" type="s" index="0" path="%s"/></mistergamedescription>' "$1"; }
mgl_neo() { printf '<mistergamedescription><rbf>_Console/NeoGeo</rbf><file delay="1" type="f" index="1" path="%s"/></mistergamedescription>' "$1"; }
pass=0; fail=0
one() {   # <sys> <game> [<mgl text>]
    sys=$1; game=$2
    say ""
    say "== $sys: $game"
    before=$T/sweep_before.md5
    (cd "$S/$sys" && files "$sys" "$game" | while read f; do md5sum "$f"; done) > $before
    n=$(wc -l < $before)
    [ "$n" -gt 0 ] || { say "  FAIL no save to test with"; fail=$((fail+1)); return; }
    say "  its save: $(cut -c35- $before | tr '\n' ' ')"
    if [ -n "$3" ]; then   # started once from its MGL (as Console Mode would), so Play has it
        printf '%s' "$3" > /tmp/sweep.mgl
        cmd "load_core /tmp/sweep.mgl"; sleep 15
        say "  launched from its MGL: core $(core)"
        menu
    fi
    r=$(ask "PLAY $sys	2	$game"); sleep 15; c2=$(core)
    left=$(files "$sys" "$game" | wc -l)
    say "  Play card 2: $r, core $c2, its save files here now: $left"
    menu
    r1=$(ask "PLAY $sys	1	$game"); sleep 15; c1=$(core)
    if (cd "$S/$sys" && md5sum -c $before >/dev/null 2>&1); then same=yes; else same=no; fi
    say "  Play card 1: $r1, core $c1, its save back byte for byte: $same"
    if [ "$r" = "play ok 2" ] && [ "$r1" = "play ok 1" ] && [ "$left" = 0 ] && [ "$same" = yes ] && [ -n "$c1" ] && [ "$c1" != MENU ]; then
        say "  OK"; pass=$((pass+1))
    else
        say "  FAIL"; fail=$((fail+1))
    fi
    menu
}
one NES "Legend of Zelda, The "
one SNES "Tactics Ogre - Let Us Cling Together"
one GAMEBOY "Final Fantasy Legend II"
one GBC "Legend of Zelda, The - Link's Awakening DX"
one GBA "Legend of Zelda, The - The Minish Cap"
one MegaDrive "Phantasy Star IV"
one N64 "Ogre Battle 64 - Person of Lordly Caliber"
one N64 "Legend of Zelda, The - Majora's Mask"
one TGFX16 "Akumajou Dracula X - Chi no Rondo (Japan)" "$(mgl_cd "$G/TGFX16-CD/Akumajou Dracula X - Chi No Rondo (English v1.03)/Akumajou Dracula X - Chi no Rondo (Japan).cue")"
one NEOGEO "kabukikl" "$(mgl_neo "$G/NEOGEO/kabukikl")"

say ""
say "== done: $pass OK, $fail failed"
# every save as it was before (the games' own and the others)
if (cd $S && md5sum -c $BK/md5.txt >/dev/null 2>&1); then say "every backed-up save as it was: yes"; else say "every backed-up save as it was: NO"; (cd $S && md5sum -c $BK/md5.txt 2>&1 | grep -v ': OK$' | tee -a $LOG); fi
say "smcd errors: $(grep -ciE 'error|failed:' /tmp/smcd.log)"
# the saves put in for the test taken out again: the saves folder as it was
[ -f $BK/put.txt ] && while read f; do rm -f "$f"; done < $BK/put.txt && sync && say "the test's own saves taken out again"
