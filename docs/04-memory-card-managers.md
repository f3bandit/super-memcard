# How Console Mode's memory card manager reads and writes cards (and Phosphor's and SS1 Tool's)

The memory card manager was f3bandit's idea and design, first in SS1 Tool; Console Mode 2.0 has Taki's
own version (Media › Memory Cards). This is what was mapped and tested on a SuperStation One running
Console Mode 2.0-RC-1 (Nov 2026), and what SS1 Tool's code (memcard_cmports.go, memcard_cm.go) does.

## Console Mode 2.0

**Cards in the front ports** are reached only through Console Mode's host while **its menu core**
(`menu_ConsoleMode.rbf`) is loaded: the menu core has the front-port card driver, the host drives it.
Commands go to `/dev/MiSTer_RR_cmd`, separated by `|`:

```
@memcard read  <port>|<file>|<frames>              the card into <file>; frames: 0-1023 or a list of ranges
@memcard write <port>|<new file>|<current file>    writes only the frames where the two files differ
```

Progress and the result are in `/tmp/consolemode_memcard.txt`:

```
reading <port> <done> <total>      writing <port> <done> <total>
done read <port> <file>            done write <port> <frames written>
error <port> <why>
```

- Console Mode's own screen reads a card in two passes: the headers first (`0-67,128-131,…`), then the rest.
- A whole card takes about 17 seconds.
- While a game core runs, the commands don't work (the card driver is in the menu core only).
- A write was tested on the SS1: 65 frames written and read back, the card unchanged afterwards (md5 a14f1d4f).

**Card images** the PSX core saves to: `/media/fat/saves/PSX/*.sav|*.mcd|*.mcr` (128 KB each).

**Backups** (its library): `/media/fat/ConsoleMode/memcards/library/<card name>/<date_time>.mcd` + `card.json`.

## SS1 Tool (Files › Memory cards › Console Mode)

Uses the same commands, adding safety around them:
- it checks Console Mode's menu is running;
- before a port write it reads the card again and requires it to match what was read;
- it backs up into Console Mode's library;
- it writes only what differs, then reads it back;
- it never changes a card image while the PSX core runs.

## Phosphor

The same job, done with Phosphor's own open parts (all f3bandit's):

| Part | What | Where |
|---|---|---|
| `phosphor_ext.sv` | in Phosphor's menu core: controllers and cards on the front ports, sharing the bus (controllers aren't read during a card operation); 250 kHz, 30 µs between a card's bytes, ACK timeout 3 ms for slow cards | phosphor-menu rtl/phosphor/ |
| `card_sector.sv` | one 128-byte sector read or write (also in SS1_MemCard_MiSTer) | phosphor-menu rtl/phosphor/ |
| `PH_CARD` (0x83), `PH_CARD_STATUS` (0x84) | Main's commands to it over hps_io's EXT_BUS; 64 data words per sector | phosphor-main phosphor.cpp:143 |
| card jobs | phosphord: backup (each sector read up to 4 times), restore (backup first, write what differs, read back), check | phosphor_os system/phosphord/memcard.c |

Backups: `<fat>/phosphor/memcards/port<N>-YYYYMMDD-HHMMSS.mcd`.

## What this means for Super MemCard

Both frontends reach the card only from their **menu core**. That's enough to copy saves to and from
the card when a game starts and ends (05, "Phase 1"). Reaching it **during** a game needs the same
driver in the game cores (05, "Phase 2"). The driver to use is `phosphor_ext.sv` / `card_sector.sv`,
already tested against software cards like SD2PSX.
