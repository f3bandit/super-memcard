#!/bin/sh
# smcview.sh: the Super MemCard section on the TV over Console Mode, Console Mode's frontend paused meanwhile (so the
# two don't draw over each other) and carried on after, the screen as it was.
#   smcview.sh [seconds]       (0 or none: until a button other than up/down)
# Copyright (C) 2026 f3bandit.
here=$(cd "$(dirname "$0")" && pwd)
T=$(cd "$here/.." && pwd)
fe=$(pidof ConsoleMode_arm)
[ -n "$fe" ] && kill -STOP $fe
cd "$here" && ./smcview -T "$T/seed/state" -T "$T/state" -T /media/fat/ConsoleMode/supermemcard \
    $( [ -S /tmp/smcd.sock ] || echo "-f $here/section.txt" ) ${1:-0}
[ -n "$fe" ] && kill -CONT $fe
