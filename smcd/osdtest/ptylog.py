#!/usr/bin/env python3
# ptylog.py <log> <program> [args]: run a program on a pty (so its stdout is line-buffered: Main's log as it goes)
import os, pty, sys
log = open(sys.argv[1], "ab", buffering=0)
pid, fd = pty.fork()
if pid == 0:
    os.execv(sys.argv[2], sys.argv[2:])
while True:
    try: d = os.read(fd, 4096)
    except OSError: break
    if not d: break
    log.write(d)
