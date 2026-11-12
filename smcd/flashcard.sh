#!/bin/sh
# flashcard.sh: put a firmware .uf2 on the Super MemCard card plugged into the SS1's USB, with no button: its USB serial
# port at 1200 baud restarts it into the RP2040's bootloader (the Pico SDK's reset), whose RPI-RP2 drive MiSTer mounts
# as /media/usbN; the .uf2 is copied onto it and the card restarts with it. The card's SD card isn't touched.
#   smcd/flashcard.sh <firmware.uf2>
# Copyright (C) 2026 f3bandit.
f=${1:?usage: flashcard.sh <firmware.uf2>}
[ -f "$f" ] || { echo "no $f"; exit 1; }
tty=$(ls /dev/ttyACM* 2>/dev/null | head -1)
[ -n "$tty" ] || { echo "no card on USB (/dev/ttyACM*)"; exit 1; }
python3 - "$tty" <<'PY'
import os, sys, termios
fd = os.open(sys.argv[1], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd); a[4] = a[5] = termios.B1200; termios.tcsetattr(fd, termios.TCSANOW, a); os.close(fd)
PY
m=
for i in $(seq 40); do
    for d in /media/usb*; do [ -f "$d/INFO_UF2.TXT" ] && m=$d; done
    [ -n "$m" ] && break; sleep 0.5
done
[ -n "$m" ] || { echo "the bootloader's drive didn't appear"; exit 1; }
cp "$f" "$m/" && sync || { echo "copy failed"; exit 1; }
for i in $(seq 40); do ls /dev/ttyACM* >/dev/null 2>&1 && break; sleep 0.5; done
ls /dev/ttyACM* >/dev/null 2>&1 && echo "flashed: $(md5sum "$f" | cut -c1-32)" || { echo "the card didn't come back"; exit 1; }
