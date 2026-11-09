# Findings log

What we've learned along the way: bugs found (and fixed), surprises, facts about the hardware and tools that
the design depends on. Newest first. Each entry says where it came from so it can be checked again.

## 2026-11-09

### Raw card frames on the SS1's front ports; a real Sony card ignores Super MemCard

phosphor-menu (branch `supermemcard`) has raw frames in its front-port extension (version 2: `PH_RAW` 87,
`PH_RAW_STATUS` 88): up to 255 bytes in one select through `card_sector`'s own byte engine, each reply kept,
stopping after the first byte without an ACK, with a per-frame ACK wait up to ~168 ms. phosphor-main has
`card_raw P MS HEX` and `phosphor_card_raw()`; the sync uses the card in front port 1 or 2 that answers Hello.
Simulated (iverilog, also in CI) and run on the SS1 with the original Sony card in front port 1 (backed up first,
all 1024 sectors identical to the backup afterwards):

- **A Sony card ignores Super MemCard's commands**: for `81 60` (Hello) and every `81 61`-`68` it stops after the
  command byte (FLAG 08 back, no ACK), and reads normally afterwards. With it in the port, the sync finds no
  Super MemCard card and changes nothing. (Closes the open test in 05.)
- A sector read as a raw frame: 140 bytes, data equal to the backup, end byte 47.
- This Sony card doesn't answer `81 53` (get ID) either: it predates that command. A host must not use get-ID to
  find a card.
- **The first frame after the ports are switched on gets no answer**; Main now waits 50 ms after switching them on,
  and holds them on for a whole sync instead of switching per frame.
- **After the Sony card refuses a command, the next frame sometimes gets no answer at all** (seen with 0-20 ms
  between them, never with 50-100 ms; plain reads back to back are fine). A real PS1 never sends a refused
  command, so it has never mattered before. Hello now waits 60 ms before its second try, and the sync leaves a
  plain card alone for 100 ms after.
- The first build of the core missed HDMI timing by 42 ps (the `phosphor` branch has +0.49 ns); the only sizeable
  new logic, a multiplier for the ACK wait, became a shift (the wait is sent in units of 1024 clocks).

### Phase 1 works end to end on the SS1

With the test Main and the virtual card, on the real SS1 (all reached only through Main's command interface):

- **Leaving a game:** Chrono Trigger loaded by MGL on `/dev/MiSTer_cmd`; its save changed during the game; leaving
  the game (`load_core menu.rbf`) restarts Main in the menu core and the sync sent it to the card (md5 equal).
- **A reinstall:** test saves folder and sync state wiped, Main restarted: all 5 saves (SNES x2, N64 EEPROM +
  Controller Pak, Genesis) came back from the card byte for byte in ~7 s.
- Main creates a game's save file only when the game first saves, so a test without a player has to put the
  save there itself; Main's own writing of saves is MiSTer's and untouched.
- Phosphor's watchdog isn't consistent about restarting Main: once within seconds, once not within 15 s. The test
  script starts Main itself if nothing has after 8 s.
- Afterwards: the installed Main back, `saves/` and `config/` checked against their backups (unchanged).

#### Before that: the test Main and the first run

Phosphor's Main with `supermemcard.cpp` (phosphor-main branch `supermemcard`) syncs at every start in the menu
core. On the SS1, with the virtual card (`vcardd`) and test saves in `supermemcard_test/`: SNES 8 KB, N64 EEPROM +
Controller Pak, Genesis 16 KB all reached the card byte for byte; a second run leaves unchanged saves alone.
The test Main runs bind-mounted over the installed binary's path (`supermemcard_test/main_test.sh`): Phosphor's
watchdog restarts Main within seconds of it stopping, so swapping the process loses that race; the bind mount
makes every restart run the test build, and `umount -l` (lazy: the running Main holds the file) puts it back.

### Bug found on the SS1: Create filled the new file before answering

MiSTer mounts `/media/fat` **sync**: every small write waits for the SD card, ~2.7 ms per 128 bytes (8 KB: 177 ms,
128 KB: 3.5 s). The card firmware's Create zero-filled the new file before answering, so anything over ~4 KB took
longer than the host's 100 ms wait: the SNES 8 KB save and the 32 KB Controller Pak failed, the 512-byte EEPROM
got through. The PC tests passed because a PC's disk is fast. Fixed: Create only opens an empty file (the blocks
fill it), and Commit refuses a file that doesn't have every byte. Rule: **no command does work in proportion to a
save's size before answering.** The virtual card can now act like a slow SD card (`VCARD_SD_WRITE_US`), and a test
writes a 128 KB save at 2.7 ms a write (it fails on the old firmware, passes now). Also fixed on the way: Commit's
`.new` path buffer (128 bytes) could cut long names; a test now uses the longest names (32 + 64 + 16).
- The sync now logs and counts a game the card doesn't answer for (the Genesis save was skipped silently).

### Super MemCard commands work in the virtual card

`firmware/` adds 81 60-68 to sd2psXtd (`ps1_supermemcard.c`, `ps1_supermemcard_bus.inc`, a 4-file patch applied by
`firmware/apply.sh`); `host/smc.c` is the host side. `make SMC=1` builds the virtual card with it: all 15 PS1 tests
still pass, plus 9 Super MemCard tests (Hello and card ID, off until Hello and after power-off, SNES round trip,
N64 eep + Controller Pak + 128 KB FlashRAM, power cut mid-save keeps the old save, unsafe names, list, PS1 card
unaffected). ASan: nothing in the new code.

- Bug fixed on the way: `git apply` run inside another git repo (the build copy lives inside super-memcard) skips
  every path silently and exits 0. `apply.sh` uses `patch` and checks the result.
- Game IDs are Main's save names, so they have spaces and brackets ("Super Metroid (USA)"); the card accepts what
  FAT accepts and refuses path separators, `..` and trailing dots or spaces (FAT drops those, so two names could
  land in one folder).
- Core 1 never touches the SD card: it collects a request, core 0 does the file work, core 1 sends the answer. The
  ACK of a request's last byte is therefore late by the SD access time; only Super MemCard hosts send these
  commands, and the host library waits up to 100 ms.

### The memory card manager doesn't write to cards (a marker is planned)

SS1 Tool, Phosphor and ps1_memcard_lib recognise a card by the saves on it (common save names / all, 0.5 or more =
the same card) and don't keep a version identical to the card's latest; nothing is written to the card
(`ps1_memcard_lib/src/mclib.c`, SS1 Tool `memcard.go`). Block 0 frames 56-62 are unused by the PS1 format
(`mcformat.go`), but they're not used for a Super MemCard marker: they travel with the image and can be wiped.
Decided: a Super MemCard card gives its own card ID in Hello's answer (05 §2).

Matching by save names goes wrong on nearly empty cards (1 save + 2 new = 33%, a new card), empty cards (always a
new card), reorganised cards, and different cards holding the same games (merged). So the manager gets a marker
for card identity (block 0 frame 56, written once, save names as the fallback): on the SS1 Tool / Phosphor todo
list, to be tested first for invisibility on the PSX core, Console Mode and a real PS1/PS2.

### Decided: the core sends Hello on its first card poll

Hello carries who the host is (Phosphor Main, Console Mode, a core's `sys/` driver) and what it can do, so the
card knows it's on an approved core and turns on that host's features; stock cores, the stock PSX core and a
real PS1 never send it, so the card stays a plain PS1 card for them. Sent twice if the first gets no reply
(finding 4 below). Details in 05 §2.

### The virtual card runs on the SS1

- `make ss1` builds `vcardd` and `test_card` for the SS1 (armv7, **static**, so MiSTer Linux's glibc 2.31 doesn't
  matter). All 15 card tests pass on the SS1 in ~20 s, run from `/tmp` (nothing written to the SD card).
- `vcardd <folder>` keeps a virtual card powered and serves the bus on `/tmp/supermemcard.sock`. Checked on the
  SS1: card ID `08 5A 5D 5C 5D 04 00 00 80`, `Card1-1.mcd` created, clean power-off on SIGTERM. Until Taki's card
  arrives, Phosphor's Main talks to this instead of the front port; only that link changes when the card comes.
- SS1 used: MiSTer Linux 260912, kernel 6.18.38-MiSTer, armv7l, Phosphor Main at
  `/media/fat/phosphor/bin/MiSTer_phosphor`, `/tmp` is a 246 MB tmpfs.

### Save folders don't always match core names

The TurboGrafx16 core saves to `saves/TGFX16/`, not `saves/TurboGrafx16/` (seen on the SS1). The host has to map
a core to its save folder from Main's own rules (the core's `CONF_STR` name), never guess it from the core's
file name. Check every core in `cores/` for this before phase 1 ships.

### sd2psXtd firmware bugs (found with the virtual card, sd2psXtd 8a20bd9)

1. **A write to a sector past 3FF overwrites RAM** on boards without PSRAM (PMC+, PSXMemCard, PMCZero,
   SD2PSXlite): `ps1_mc_data_interface_write_byte()` writes `cache[address]` with no bounds check, up to 8 MB past
   the 128 KB image, and the card answers 47 (good). A Sony card answers FF. The SD2PSX board has PSRAM and takes
   another path. Games never send this; a buggy host could. Our host must never send a sector ≥ 0x400, and a fix
   (answer FF, change nothing) is a candidate patch for upstream. Test: *write past sector 3FF* (known bug).
2. **`game_db.c:189`**: `strlcpy(idString, split, 11)` into `char idString[10]`. Overflows only for game IDs with
   10+ digits; glibc's `_FORTIFY_SOURCE` aborts on it, so the virtual card builds without it. One-character fix.
3. **A bad-checksum write still lands in the card's RAM image**: the card answers 4E and doesn't write the SD card,
   but reads return the rejected data until the next good write of that sector or a power-off. Harmless for
   games (they rewrite on 4E). Our host must also always rewrite after a 4E, never read back to "check".
4. **The first command after power-up or a card switch can go unanswered.** The card's loop waits for SEL to
   rise once; after a switch (game ID, combo, menu) whether it's already risen depends on timing. A PS1 never
   notices (it polls the pad on the same select). **Host rule: one missing reply after power-up or a switch
   means "busy", not "no card": deselect and ask again.**

### Which boards sd2psXtd builds for

SD2PSX (with PSRAM), PMC+ (PicoMemcard+), PSXMemCard, PMCZero, SD2PSXlite (no PSRAM) — `misc/variants.cmake`.
Retro Remake's card is sd2psXtd-based; which variant it is (PSRAM or not) is still unknown. The virtual card
currently builds the no-PSRAM path; add the PSRAM path if Taki's card turns out to have it.

### Console cores and sys/ versions

15 console cores are pinned in `cores/` (Nintendo: NES, SNES, N64, Gameboy, GBA; Sega: SMS, Genesis, MegaDrive,
MegaCD, S32X, Saturn; TurboGrafx16, Jaguar, NeoGeo, PSX). At their pinned commits they carry **7 different `sys/`
versions**, so the phase 2 `sys/` patch has to apply to all 7 (`tools/cores.sh sys`). SG-1000 has no core of its
own (it's in ColecoVision_MiSTer), not included.

### Tooling notes

- The virtual card's first harness rebooted the firmware inside one process: wrong, because the firmware's static
  variables survive (a real power-off clears them). Every power-up is now a fresh process.
- From this session the GitHub API can't change repo settings (403); they were changed in the browser
  (Actions may create PRs; description).
- PRs opened by Actions' own token don't start workflows, so the upstream watcher starts the build itself
  (`gh workflow run`) on the new branch.

### Open

- One full test run in ten failed once on the PC before the post-switch fix; which test wasn't caught. Watch for
  it in CI.
