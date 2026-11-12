# card_serial.py: the Super MemCard card's own log (its USB serial port, smc: lines) with times, to /tmp/card_serial.log;
# opened again when the card restarts or moves to another /dev/ttyACM*. Run in the background on the SS1 while testing:
#   setsid python3 card_serial.py &
# Copyright (C) 2026 f3bandit.
import glob, os, time, termios, tty
while True:
    try:
        fd = os.open(sorted(glob.glob('/dev/ttyACM*'))[0], os.O_RDWR | os.O_NOCTTY)
        tty.setraw(fd); a = termios.tcgetattr(fd); a[4] = a[5] = termios.B115200; termios.tcsetattr(fd, termios.TCSANOW, a)
        buf = b''
        with open('/tmp/card_serial.log', 'a') as log:
            while True:
                d = os.read(fd, 4096)
                if not d:
                    break
                buf += d
                while b'\n' in buf:
                    line, buf = buf.split(b'\n', 1)
                    log.write(time.strftime('%H:%M:%S ') + line.decode(errors='replace').rstrip('\r') + '\n')
                    log.flush()
    except (OSError, IndexError):
        time.sleep(1)
