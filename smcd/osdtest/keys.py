#!/usr/bin/env python3
# keys.py: key presses to MiSTer Main through a virtual keyboard (/dev/uinput), for testing the OSD on the SS1.
#   keys.py f12 down enter ...   (also up left right esc, letters a-z; wait<ms>). Main takes a second or two to find
#   the new keyboard: start with wait2500.
import fcntl, os, struct, sys, time
L = dict(zip("qwertyuiop", range(16, 26))); L.update(zip("asdfghjkl", range(30, 39))); L.update(zip("zxcvbnm", range(44, 51)))
K = {"f12": 88, "up": 103, "down": 108, "left": 105, "right": 106, "enter": 28, "esc": 1, **L}
fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
fcntl.ioctl(fd, 0x40045564, 1)            # UI_SET_EVBIT EV_KEY
for c in K.values(): fcntl.ioctl(fd, 0x40045565, c)   # UI_SET_KEYBIT
dev = b"smc-test-keys".ljust(80, b"\0") + struct.pack("HHHHi", 3, 1, 1, 1, 0) + bytes(4 * 64 * 4)
os.write(fd, dev); fcntl.ioctl(fd, 0x5501)   # UI_DEV_CREATE
time.sleep(1.5)                                # Main finds the new keyboard
def ev(t, c, v): os.write(fd, struct.pack("llHHi", 0, 0, t, c, v))
for a in sys.argv[1:]:
    if a.startswith("wait"): time.sleep(int(a[4:]) / 1000); continue
    ev(1, K[a], 1); ev(0, 0, 0); time.sleep(0.08); ev(1, K[a], 0); ev(0, 0, 0); time.sleep(0.35)
time.sleep(0.3); fcntl.ioctl(fd, 0x5502); os.close(fd)
