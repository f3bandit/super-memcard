# How Super MemCard works with Console Mode

Console Mode is the target (rule 7). What it already does with cards (04) has to keep working
unchanged, and what Super MemCard needs from it is small.

## Nothing changes for what Console Mode does today

- **Media › Memory Cards** reads and writes front-port cards with `@memcard read/write`: those are
  PS1 commands, which always reach the PS1 card (05 §1). Same results as today.
- **The PSX core** keeps using its card images in `saves/PSX` (01).
- **A real PS1/PS2** with the card: unchanged, round trip included (rule 1).

## What Console Mode would add (Retro Remake's side)

1. **A setting**: *Settings › Saves on memory card: Off / Front port 1 / Front port 2* (off by default).
2. **Two moments around each game**, in Console Mode's host (`MiSTer_ConsoleMode`), as a PS1 uses its card (05 §3):
   - just before the core loads: Core (the system) and that game's save from the card if the card's copy is newer
     (`smc_core()`, `smc_sync_game()`; the game is the file the launch's MGL names);
   - back in the menu core after the game: that game's save to the card if it changed (`smc_sync_game()`), then
     Core `.`.
   Nothing at menu start. Console Mode already does work at the exit moment: the backup module's
   `snap-…-exit-<core>` snapshot. Super MemCard runs after it, so the snapshot is always taken first.
3. **Game IDs**: worked out by the library from the game's own file (09), nothing needed from Console Mode.
4. **Media › Memory Cards**: its existing *Read card* step (it must read the card before the other options) is where
   the full sync runs (`smc_sync()`); then each game the card has, by system: `smc_list_card()` and
   `smc_game_info()` give the game's name, its ID, the save's date and time and its icon (`icon.bin`, the PS1
   save-icon layout the screen already draws). Delete (`smc_delete_save()`), and "keep which?" for a save changed on
   both sides (`smc_conflicts()`, `smc_resolve()`). A progress bar while saves move (≈11 KB/s).
5. **One user of the bus at a time**: card jobs (Memory Cards screen, SS1 Tool, Super MemCard sync)
   take a lock, like `/tmp/ss1_bt.busy` for Bluetooth, so a sync and a backup never overlap.
   Controller reads pause during a card operation, as `phosphor_ext.sv` already does.

## What Retro Remake gets

- the protocol spec (05 §2);
- the card firmware changes (on sd2psXtd, GPL-3.0);
- the Main changes, built and tested in phosphor-main (Main_MiSTer-derived, so they apply to
  `MiSTer_ConsoleMode` nearly as they are);
- the `sys/` driver for phase 2.

Plus test results from an SS1 running Phosphor with a Retro Remake card.

## Console Mode 2.0 RC1 (Nov 7 2026), what it tells us

From its release notes and what the frontend shows (`ConsoleMode_Distribution`, tag `2.0-RC-1`; nothing taken
apart):

- The menu core reads **memory cards in the front ports** now ("menu core with front-port memory cards"), and
  *Media › Memory Cards* browses PS1 card images and saves, copies, imports and exports (f3bandit's idea and
  design, ps1_memcard_lib). The frontend asks the host with `@memcard read` / `@memcard write` (`@memcard
  cancel`) on `/dev/MiSTer_RR_cmd` and reads the result from `/tmp/consolemode_memcard.txt`; card images go to
  `ConsoleMode/memcards` (and `/media/fat/memcards`).
- So the pieces Super MemCard needs are where we put them on Phosphor: the bus in the menu core, the card jobs in
  the host, the screens in the frontend. Super MemCard adds to each:
  - **menu core**: `81 6x` frames of any length (Phosphor's `PH_RAW`, phosphor-menu); its PS1 sector reads stay;
  - **host**: the library (`host/`), called at game start and exit, and new `@memcard` commands for the screens
    (read card = sync, list games, delete, keep which);
  - **frontend**: the Super MemCard view in Memory Cards (name, ID, date and time, icon per game).
- `/tmp/FULLPATH` and `/tmp/ACTIVEGAME` are written at launch (1.1.4, 1.2.5): the game a launch starts is known.
- The source of the RC1 menu core and host isn't published yet (`menu_ConsoleMode` is at 1.2): their card
  timing is still to compare with phosphor-menu's when it is.

### On an SS1 with Console Mode 2.0-RC-1 (Nov 9)

Looked at with backups first (saves, config, `ConsoleMode/memcards`, binaries' checksums), all unchanged after.

- Console Mode boots **its own menu core**, `ConsoleMode/menu_ConsoleMode.rbf` (`MiSTer.ini`: `main=ConsoleMode/MiSTer_ConsoleMode`);
  `/media/fat/menu.rbf` is a different file. Loading that one (as on Phosphor) leaves Main in the menu without
  Console Mode's frontend: a reboot brings it back. The Super MemCard hooks have to come back to Console Mode's menu
  core, the way its own launches do.
- Save folders: as Main's, plus **`GBC`** (Game Boy Color saves apart from `GAMEBOY`) and `SS1_MemCard` (the SS1
  memory card core's images). GBC added to the systems.
- A game started from an MGL by `/dev/MiSTer_cmd` writes only `/tmp/CORENAME` and `/tmp/RBFNAME`; the game is in
  the MGL (as Phosphor's hooks read it). `FULLPATH`/`ACTIVEGAME`/`last_launch` are written by the frontend's own
  launches.
- `@memcard read 1` written to `/dev/MiSTer_RR_cmd` by hand got no answer (no `/tmp/consolemode_memcard.txt`):
  the screen's own request isn't just that line. Not looked into further (nothing taken apart); the Memory Cards
  screen itself is the baseline to use.

## Open points for Taki

- Is the shipping card's firmware sd2psXtd-based, and can we have its source? It's GPL-3.0.
- Does Console Mode's menu core card driver match `phosphor_ext.sv`'s timing for software cards?
  Its card reads work today, so probably.
- Where in the launch path Console Mode wants the two hooks.
- Raw frames in the RC1 menu core: the same command as phosphor-menu's `PH_RAW`, or its own.
