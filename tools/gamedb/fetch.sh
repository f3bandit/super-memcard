#!/bin/sh
# Fetch libretro-database's game lists for the Super MemCard systems into a folder (for the sync's official names,
# smc_sync_cfg.gamedb_dir). The same lists Phosphor fetches; not part of this repository, used under their terms.
#   tools/gamedb/fetch.sh <folder>
# Copyright (C) 2026 f3bandit.
set -e
dir=${1:?usage: fetch.sh <folder>}
LR=https://raw.githubusercontent.com/libretro/libretro-database/master/metadat
mkdir -p "$dir"
# MiSTer Linux's curl doesn't find its certificates by itself (as Phosphor does, give them)
CA=
[ -f /etc/ssl/certs/cacert.pem ] && CA="--cacert /etc/ssl/certs/cacert.pem"
get() { # <no-intro|redump> <name>
	url="$LR/$1/$(printf '%s' "$2" | sed 's/ /%20/g').dat"
	if curl $CA -fsSL "$url" -o "$dir/$2.dat.part"; then mv "$dir/$2.dat.part" "$dir/$2.dat"; echo "ok   $2"
	else rm -f "$dir/$2.dat.part"; echo "FAIL $2"; fi
}
for n in "Nintendo - Nintendo Entertainment System" "Nintendo - Family Computer Disk System" \
	"Nintendo - Super Nintendo Entertainment System" "Nintendo - Satellaview" "Nintendo - Nintendo 64" \
	"Nintendo - Game Boy" "Nintendo - Game Boy Color" "Nintendo - Game Boy Advance" \
	"Sega - Master System - Mark III" "Sega - Game Gear" "Sega - SG-1000" "Sega - Mega Drive - Genesis" "Sega - 32X" \
	"NEC - PC Engine - TurboGrafx 16" "NEC - PC Engine SuperGrafx" "Atari - Jaguar"; do get no-intro "$n"; done
for n in "Sony - PlayStation" "Sega - Saturn" "Sega - Mega-CD - Sega CD"; do get redump "$n"; done
