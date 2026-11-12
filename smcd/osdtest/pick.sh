#!/bin/sh
# pick.sh <downs>: in the running core's OSD, its first row (the game loader), then the file <downs> rows down, picked
T=${T:-/media/fat/supermemcard_test}
downs=""; i=0; while [ $i -lt ${1:-1} ]; do downs="$downs down"; i=$((i+1)); done
python3 $T/keys.py wait2500 f12 wait1500 enter wait3000 $downs wait600
$T/osd.sh | grep '^>'
python3 $T/keys.py wait2500 enter
