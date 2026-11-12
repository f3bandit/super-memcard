# smcview

Console Mode's Memory Cards screen (the ribbon theme, rebuilt from snapshots the way fpga_offload_module's
cmscreens does) with the Super MemCard section under it: each system with saves on the card, its current virtual
card, and its games with the icons the card has (cover art made by smcd, or the system's placeholder), their names
and when they were saved. It shows on the TV what Console Mode's memory manager would show once it has the section.

```
smcview [-T <smcd state dir>]... [-f <file>] [-s <file>] [seconds]
```

- `smcview.sh [seconds]` runs it over Console Mode: the frontend paused meanwhile, the controllers its own (up and
  down move, any other button leaves, left alone it moves on every 3 s), the screen put back as it was after;
- live: reads `SECTION` from smcd (`/tmp/smcd.sock`), so the card must be reachable (the test Main and a menu core
  with the front-port driver);
- `-s file 0`: only saves the section to a file; `-f file`: shows a saved section, for a Main that can't reach the card
  (Console Mode's own, until it has the hooks). Icons come from smcd's state dirs (`-T`, several allowed);
- needs `Outfit-Medium.ttf` and `cmrec/btn_a.raw`, `cmrec/btn_b.raw` (cmprep.py's) in the current folder; draws on
  Console Mode's 1280x720 framebuffer, which only Console Mode's Main shows.

Build (static, for the SS1):

```
arm-linux-gnueabihf-gcc -O2 -static -std=gnu11 -Ihost -Itools/smcview tools/smcview/smcview.c host/smc_icons.c -lm -o smcview
```

Drawing code from fpga_offload_module's cmscreens. Copyright (C) 2026 f3bandit.
