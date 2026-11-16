#!/bin/bash
# Super MemCard: one save per system round trip on the SS1, card 1 of each system (the BitFunX in port 1).
# Copyright (C) 2026 f3bandit. Licensed under the Apache License 2.0 (LICENSE, NOTICE).
# A game with no save of its own; a test save (random bytes) made for it; launched (to card), then the local save
# moved away and launched again (from card), the bytes compared; then the test save and its card set removed.
# The user's own saves aren't touched (only games that have none). Results: /media/fat/supermemcard_test/systest.txt
OUT=/media/fat/supermemcard_test/systest.txt
BK=/media/fat/supermemcard_test/backups/systest_$(date +%Y%m%d_%H%M%S)
mkdir -p "$BK"
LOG=/tmp/smcd.log
ask() { timeout 120 python3 /tmp/ask.py "$1"; }
say() { echo "$*" | tee -a "$OUT"; }
launch() { cat > /tmp/st.mgl <<M
<mistergamedescription>
<rbf>$1</rbf>
<file delay="2" type="$2" index="$3" path="$4"/>
</mistergamedescription>
M
  echo "load_core /tmp/st.mgl" > /dev/MiSTer_cmd; sleep $5; }
menu() { echo "load_core /media/fat/menu.rbf" > /dev/MiSTer_cmd; sleep 15; }
newlog() { tail -n +$(( $1 + 1 )) $LOG; }

run() {   # name rbf romdir "exts" type index wait
  local name=$1 rbf=$2 dir=/media/usb0/games/$3 exts=$4 typ=$5 idx=$6 w=$7 rom="" base f
  if [ -d "$dir" ] && [ "$typ" = s ]; then rom=$(find "$dir" -maxdepth 1 -type f \( -iname "*.chd" -o -iname "*.cue" \) | sort | head -1)
  elif [ -e "$dir" ]; then rom=$dir; fi
  [ -n "$rom" ] && ls /media/fat/saves/*/"$(basename "${rom%.*}")".* >/dev/null 2>&1 && { say "$name: SKIP ($(basename "$rom") has a save of yours)"; return; }
  [ -z "$rom" ] && { say "$name: SKIP (no game without a save)"; return; }
  base=$(basename "${rom%.*}")
  [ "$name" = Saturn ] && base=$(echo "$base" | sed -E 's/ ?\(?[Dd][Ii][Ss][Cc] ?[0-9A-Fa-f]\)?//')   # (Main's name: every disc shares one save)
  say "== $name: $rom"
  # 1: what save files this core makes for the game
  touch /tmp/st.mark; sleep 1
  launch "$rbf" $typ $idx "$rom" $w; local core=$(cat /tmp/CORENAME 2>/dev/null); menu
  mapfile -t files < <(find /media/fat/saves -type f -newer /tmp/st.mark -name "$base*" 2>/dev/null)
  if [ ${#files[@]} -eq 0 ]; then
    mkdir -p "/media/fat/saves/$core"; files=("/media/fat/saves/$core/$base.sav"); head -c 8192 /dev/zero > "${files[0]}"
    say "  core $core made no save: a test $base.sav (8 KB)"
  fi
  for f in "${files[@]}"; do sz=$(stat -c %s "$f"); head -c $sz /dev/urandom > "$f"; say "  test save: ${f#/media/fat/saves/} ($sz bytes)"; done
  # 2: to card
  local n=$(wc -l < $LOG)
  launch "$rbf" $typ $idx "$rom" $w; menu
  local to=$(newlog $n | grep -c "to card 1")
  local sys=$(newlog $n | grep -m1 "to card 1:" | sed 's/.*to card 1: \([^/]*\)\/.*/\1/')
  declare -A want; for f in "${files[@]}"; do [ -f "$f" ] && want[$f]=$(md5sum < "$f"); mv "$f" "$BK/"; done
  # 3: from card (the local copy gone)
  n=$(wc -l < $LOG)
  launch "$rbf" $typ $idx "$rom" $w
  local from=$(newlog $n | grep -c "from card 1")
  local ok=1 got
  for f in "${files[@]}"; do
    got=$( [ -f "$f" ] && md5sum < "$f" || echo none)
    [ "$got" = "${want[$f]}" ] || { ok=0; say "  DIFFERS: ${f#/media/fat/saves/} ($got)"; }
  done
  menu
  newlog $n | grep -E "failed|error|no answer|didn't" | head -3 | sed 's/^/  log: /' | tee -a "$OUT"
  [ $ok = 1 ] && [ $to -ge 1 ] && [ $from -ge 1 ] && say "  PASS ($name as $sys: $to to card, $from from card, same bytes)" || say "  FAIL ($name as ${sys:-?}: $to to card, $from from card)"
  # 4: clean up: the test save away, its set off card 1
  for f in "${files[@]}"; do [ -f "$f" ] && mv "$f" "$BK/back_$(basename "$f")"; done
  if [ -n "$sys" ]; then
    local id=$(ask "LIST $sys	1" | grep -F "$base" | head -1 | cut -f8)   # (the set's own ID; field 2 is the game's serial)
    [ -n "$id" ] && say "  card: $(ask "DELGAME $sys	1	$id" | head -1)"
  fi
}

say "### $(date)"
case "${1:-all}" in
  one) run "$2" "$3" "$4" "$5" "$6" "$7" "${8:-25}";;
  all)
    run NES      _Console/NES          "NES/Dragon Warrior.nes"                         - f 1 25
    run SNES     _Console/SNES         "SNES/EarthBound.sfc"                            - f 0 25
    run N64      _Console/N64          "N64/Mario Kart 64.n64"                          - f 1 30
    run GB       _Console/Gameboy      "GAMEBOY/The Legend of Zelda - Link's Awakening.gb" - f 1 25
    run GBC      _Console/Gameboy      "GBC/Pokemon - Crystal Version.gbc"              - f 1 25
    run GBA      _Console/GBA          "GBA/Advance Wars.gba"                           - f 1 25
    run Genesis  _Console/MegaDrive    "Genesis/Phantasy Star IV.md"                    - f 1 25
    run TGFX16   _Console/TurboGrafx16 "TGFX16/Neutopia (USA).pce"                      - f 1 25
    run TGFX16CD _Console/TurboGrafx16 "TGFX16-CD/Ys Book I & II (USA)"                 - s 0 40
    run MegaCD   _Console/MegaCD       "MegaCD/Sonic CD (USA)"                          - s 0 40
    run Saturn   _Console/Saturn       "Saturn/Chou Jikuu Yousai Macross - Ai Oboete Imasu ka (Japan) (Disc 1)" - s 0 45
    run PSX      _Console/PSX          "PSX/Alundra 2 - A New Legend Begins (USA)"      - s 1 45
    run NEOGEO   _Console/NeoGeo       "NEOGEO/2020bb"                                  - f 1 35
    ;;
esac
say "### done $(date)"
