# Findings log

What we've learned along the way: bugs found (and fixed), surprises, facts about the hardware and tools that
the design depends on. Newest first. Each entry says where it came from so it can be checked again.

## 2026-11-09

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
