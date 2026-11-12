# Findings log

What we've learned along the way: bugs found (and fixed), surprises, facts about the hardware and tools that
the design depends on. Newest first. Each entry says where it came from so it can be checked again.

## 2026-11-10

### Phase 2: the card while a game runs, from sys_top

- Retro Remake's documentation: the front ports are SNAC on the FPGA's user port, used in a game only by the PSX core
  (other cores take USB/Bluetooth controllers). So the bus is free during any other core's game.
- Every pinned core's `sys_top.v` decodes Main's UIO commands itself and owns the user port, at four lines all 15
  share (4 different `sys_top.v` versions among them): the driver goes there, the cores' own code is untouched.
  hps_io uses UIO commands up to `3E`; the cores' `EXT_BUS` users (SNES, Mega Drive MSU/CD, PSX, Saturn, Mega-CD,
  TurboGrafx-16 CD) answer `34`/`35`. Super MemCard's `80`-`88` are free in every core.
- `sys_top`'s `clk_sys` is the core's clock (21-100+ MHz). smc_port's timings were constants for the menu core's
  100 MHz: now they're worked out from the clock (cycles in 64 us), which `smc_sys` measures against the 50 MHz board
  clock. The menu core's results are identical (the test menu core's simulation, before and after); the bit clock on the
  wire measured in the bus simulation: 2.0 us half-bits for PS1 frames, 0.5 us at 1 MHz, at 33, 42.95, 50, 85.9 and
  100 MHz.
- **On the SS1 (Console Mode's SD card):** the NES core built with the driver (`NES_49a0a66`, Quartus 17.0.2 in
  Actions, timing met, 79% of the logic) loaded under the test Main; Main found the front ports in the game core
  (SMC_ID) and read sector 0 of the Sony PS1 card in port 1 through it: 140 bytes, `MC`, checksum, `47`; port 2
  (empty) answered nothing. The driver works in a game core on the real hardware.
- **Every core built with the driver** (Actions, Quartus 17.0.2): NES, Gameboy, Genesis, MegaDrive, N64, MegaCD,
  S32X, SMS, SNES, Jaguar, TurboGrafx16 and NeoGeo fit and meet timing. GBA, Saturn and PSX didn't fit (Saturn
  needed 4296 LABs of 4191, PSX 4265): the driver had smc_port's controller reads, sector commands and controller
  commands, which a game core never uses (~1000 LUTs, ~700 registers by Yosys' count, ~1400 ALMs in Quartus on NES).
  Now `RAW_ONLY` keeps only the raw frames (~640 LUTs, ~530 registers); the menu core is unchanged (the test menu core's
  simulation identical). PSX doesn't get it: it uses the front ports itself. With `RAW_ONLY` GBA fits; Saturn needed
  4200 LABs, then 4199 with the faster speeds worked out by shifts (~450 registers): Saturn fills the FPGA without
  the driver, so it stays on phase 1 (its saves go before and after the game). 13 of the 15 cores carry it.
- The same core built with and without the driver, each loaded by Console Mode's own Main and played: both look and
  play the same. (A bad picture seen first came from starting a core by hand with the test Main on Console
  Mode's SD card, not from the core.)

### Console Mode 2.0-RC-1's SD card: every save onto the card and back

On the SS1 with Console Mode's SD card, with `tools/smccli` (each frontend call from a shell) and the card
simulated by `vcardd`. A copy of `saves/` went through Read card (the real folder untouched: contents and file times
checked before and after); the games came from its USB drive and SD card, names from libretro's lists.

- **32 saves in 5 systems onto the card, then all 32 back onto an empty saves folder, identical, with their file
  times** (NES 4, SNES 3, N64 14 in 4 games with Controller Pak files, NEOGEO 1, PSX 10). `SS1_MemCard/Test.mcd` (the
  SS1 memory card core's images) isn't a console save folder: not taken.
- Game IDs and names as the memory manager shows them: `NTEA` *1080 Snowboarding (Japan, USA) (En,Ja)*, `ND4E`
  *Castlevania - Legacy of Darkness (USA)*, `NHTE` *Hydro Thunder (USA)*, `NOBE` *Ogre Battle 64*, `SCUS-94604`
  *Carnage Heart (USA)*, NES by CRC (*Final Fantasy (USA)*, *Legend of Zelda, The (USA)*; the translations aren't in
  the lists: their save names), `SNES-AO7J` *Tactics Ogre* (a translation: header title), a FFT hack's own disc code
  `TLW-294221`. PSX saves with no matching disc found keep their save names.
- **Found: two saves of one game on one SuperStation.** `Final Fantasy III (TWUE)(3.06).sfc` and `Final Fantasy VI
  (TWUE 3.06).sfc` are one ROM (same CRC) with separate saves, and two FFT hack saves share the hack's disc code. The
  sync filed both under the one game ID and **skipped the second save without saying so** (not lost here, but not on
  the card either). Fixed: the second save gets its own set, `<id>~<CRC of its save name>` (`card_id_of_save()`);
  the card's set keeps belonging to the save named in its `name.txt`. Both come back on a reinstall under their own
  names. A memory manager shows the save's name next to the game's (two entries of one game otherwise look the same).
  Test: *two saves here for one game*.
- **MGL `<setname>` is the save folder** (Main's `CoreName2`, unless `same_dir="1"`). Console Mode's
  `GameboyColor.mgl` starts the Gameboy core with saves in `GBC`, `Game Gear.mgl` the SMS core with `GameGear`. The
  hooks used the `.rbf` only, so a Game Boy Color game's save would have been looked for in `GAMEBOY`. Fixed:
  `smc_mgl_read()` and `smc_launch_system()` in the library (the test Main uses them now). `GameGear` added as a
  system (game ID `GG-<product code>`, its icon, the Game Gear list). Mega Duck, Atari 2600, WonderSwan Color and
  Pocket Challenge V2 MGLs aren't systems the card keeps.
- A game's load and save around a launch from an MGL written the way Console Mode's are: N64 (`Hydro Thunder.n64`:
  load unchanged, played, back: 1 to the card; the other SuperStation's launch: 1 from the card, identical), Game Boy
  Color (`1942.gbc` through `GameboyColor.mgl`: filed as `GB-AQ4E` *1942 (USA, Europe)*, back into `saves/GBC` on the
  other). No Game Gear games on this SD card to try.
- Time: 5 minutes each way for 2.6 MB through the simulated card on the SS1 (its socket wire, not the bus); the ten
  128 KB PSX card images are half of it. On the real card at a PS1's speed (~11 KB/s) about 4 minutes, less at the
  faster frame speeds: a first Read card of a full saves folder wants a progress bar.
- **PSX like every other core.** Loading the PSX core didn't talk to the card (it was left out so the card's buttons
  would stay the PS1 card's), so PSX saves only moved at Read card. Now the PSX core is a system like the rest: Core
  PSX at its start, that game's save in, and out after it. The buttons still go back to PS1 cards as soon as anything
  reads or writes the PS1 card (system mode already ends there).
- Console Mode's games folders include `cifs/games` (a network share): added to the games folders searched.

### Faster Super MemCard frames (SMC_RAW speed)

Where a 140-byte frame's 10.9 ms went on the SS1: ~4.5 ms of bits at a PS1's 250 kHz, ~4.2 ms of 30 us gaps after
each byte, the rest ACK waits. The card ACKs every byte before the next one goes, so it sets the pace itself: a faster
clock and shorter gaps are safe to offer to a card that said it can (Hello). the test menu core's `SMC_RAW` takes a speed
now (extension version 3): 250 kHz/30 us, 500 kHz/10 us, 1 MHz/4 us, 2 MHz/2 us. the test Main sends Super MemCard
frames at 1 MHz and Hello and PS1 frames at 250 kHz, and drops to 250 kHz for the session if a fast frame isn't
answered whole.

- the test menu core's simulation: 8.8 ms at 250 kHz, 4.0 ms at 500 kHz, 2.0 ms at 1 MHz, 1.2 ms at 2 MHz (the bus
  alone; a real card's answer time adds to it).
- Bus simulation with the card firmware at 1 MHz: the whole round trip passes, stock firmware too.
- **The SS1's front ports at those clocks** (Sony card in port 1, PS1 sector reads as raw frames, compared with its
  backup image; reads only): 250 kHz and 500 kHz 96 of 96 right, 1 MHz 93 of 96 (3 cut short), 2 MHz none (all cut
  short). Never wrong data: the wiring carries the faster clocks cleanly, the Sony card's timing is the limit. A
  Super MemCard card never gets fast frames unless it answered Hello, so this only says the ports can do it.
- the test Main starts Super MemCard frames at 1 MHz and steps down one speed (500 kHz, then 250 kHz) for the
  session when a frame isn't answered whole. The real card's answer time decides where it settles.
- The menu core with speeds builds with timing met (worst setup slack +0.270 ns).

### The card only when a save is loaded or saved (like a PS1)

The first phase-1 version synced every save at every menu start; that read the card and the whole saves folder each
time, and a first run took over a minute identifying games. Now (as a PS1 uses its card): nothing at menu start, a
game's own save before it starts and after it, everything only from the memory manager ("read card").

On the SS1 (test Main, vcardd): menu start left the card alone; an N64 game started through an MGL (as the test Main and
Console Mode start games) checked only its save (under a second, unchanged); back in the menu only that game's saves
went to the card, including the 32 KB `.sra` the real N64 core wrote while it ran. Restored after as before.

### Game IDs, names and times on the SS1, with its own games

Test Main (bind-mounted) + vcardd + libretro's lists fetched onto the SS1 (`tools/gamedb/fetch.sh`, 19 lists, 14 MB),
8 test saves named after games on the SS1's USB drive (saves in `supermemcard_test/`, the real ones untouched).

- Filed as designed: `N64/Card1/NTEA-08FE81C7` "1080 Snowboarding (Japan, USA) (En,Ja)", `GBA/Card1/BPEE-1F1C08FB`
  "Pokemon - Emerald Version (USA, Europe)", the Castlevania hack apart from the original (`A2CE-C647E8A8`, no
  official name, header name `CASTLEVANIA2`), `NES/Card1/CRC-171251E3` "1942 (Japan, USA) (En)", TurboGrafx
  `CRC-B03E0B32` "Aero Blasters (USA)"; a save with no ROM kept its name. Each save's time on the card.
- A reinstall (saves and state gone): all 8 back byte for byte with their original time. One set whose `name.txt`
  named no ROM here was found by game ID and came back under that ROM's name.
- **Bugs found and fixed:**
  - `.md` was skipped as a text file, but it's a Mega Drive ROM: that Genesis game had no ID.
  - A save with no ROM made the sync hash every ROM of its system (549 SNES ROMs, ~100 s over USB) looking for
    its ID; it now only looks for sets filed under a game ID.
  - MiSTer's curl needs `--cacert /etc/ssl/certs/cacert.pem` to fetch over HTTPS (as the test frontend does).
- The SS1's clock: a DS1307 clock chip and ntpd, right to the second, so save times are right. Times are kept in UTC;
  a frontend shows them in local time.
- Left as is: two copies of one dump under different names (`Pokemon - Emerald` and `Pokemon - Emerald Version`) share a
  save set; restored on a fresh SuperStation it comes back under one of the two names only.

Restored after: installed Main and menu core (running the installed Main), `saves/` and `config/` unchanged, vcardd off,
no bind mounts.

### Saves filed by game ID, with their date and time

Every save set on the card is now filed under its game's ID (the code in the game's own header; the disc product
code; the ROM's CRC32 where neither exists) with `game.ini` (ID, header name) and the save's time, so Console Mode's
memory manager can show game name, game ID, date and time (`smc_game_info()`). Tested in the virtual card with
made-up ROM headers (N64 in two byte orders, Genesis, GBA, Master System, NES): one game under two file names on two
SuperStations is one save set, and each gets the save under its own ROM's name with the original time.

- **Save times:** console saves don't hold a time; the save file's time is the SuperStation's clock when Main wrote
  it, and the card has no clock at all. The time travels with the save (Commit `time[4]`, Info) and is put back on
  the file when it's copied to another SuperStation. Whether the SS1's clock is right (network time, a clock chip)
  is still to check: a wrong clock gives wrong dates.
- Checked on the SS1's own games: the codes read right (N64 `NO7E` TWINE, GBA `AW2E`, Genesis `GM T-95106-00`,
  PSX `SLUS-00633`). Two things changed from it: hacks keep the original's code (*Aria of Sorrow* and its
  *Persephone* hack are both `A2CE`), so a cartridge's save set is filed as code-CRC (`A2CE-35536183`), the code
  alone shown; and homebrew/boot ROMs carry `GM 00000000-00`, which now falls back to the CRC.
- The identification is the game library's (`host/smc_ident.c`, copied); the header codes are new (`host/smc_gameid.c`).

### Deleting saves, and settling "both changed"

Built and tested in the virtual card: Delete (`81 6A`) leaves a tombstone on the card; the sync deletes (moves
aside) other SuperStations' copies, or brings the save back when one was played since; nothing is erased anywhere.
`smc_conflicts()` / `smc_resolve()` let a frontend settle a both-changed save either way. Console Mode needs screens
for both (a delete in its save manager, a "which one?" question); the test Main is only the test bench.

### The card's SuperStation menu, free space, save icons

- **Menu** (`src/gui.c` patch, built for SD2PSX and PMC+, not run on a card yet): Saves Yes/No, Free (count),
  Safe to remove. Saves No makes it a stock card on the bus: the Hello request isn't collected, so `81 60` isn't
  ACKed (tested, also after a power-off). Until the card has read `settings.ini` after power-up it answers no
  Hello either; hosts ask twice anyway.
- **Free space**: sd2psXtd's SD wrapper had no call for it; the patch adds `sd_free_kb()` (SdFat
  `freeClusterCount()`). That scans the whole FAT, seconds on a big card, far over the bus's ACK wait, so Hello never
  counts: it reports what the menu counted, less what was written since (tested), and FFFFFFFF before.
- **Save icons**: 15 original system icons (`icons/`, `tools/icons/make_icons.py`), PS1 save-icon layout, put on
  the card as `icon.bin` the first time a game goes there (tested). Per-game icons later.

### Virtual cards per system, and Core (protocol version 2)

Built and tested in the virtual card. A core (Main, for now) names the running system with **Core** (`81 69`); while
it's named, the card's buttons step that system's virtual card (1-99) instead of the PS1 card, and the choice is
saved in `SuperMemCard/<SYSTEM>/card.ini`. Saves moved to `SuperMemCard/<SYSTEM>/Card<N>/<game>/` (version 1 had no
`Card<N>` level; nothing outside the test Main used it). Select and List take a card number (0: current).

- The PS1 card is untouched: without a Core the buttons do what they always did (tested), and the first PS1 read or
  write, or a Core `.`, gives them back to the PS1 card (tested).
- The card's screen and buttons are `src/gui.c` (`evt_scr_main`, `gui_task`): patched, but sim/card doesn't build the
  GUI, so the virtual card presses the buttons through the same three lines (`host/vcard.c`, new wire op `OP_KEY`).
  The GUI patch is compile-checked on real firmware (next entry), not run.
- Sync: the state now has a card column (old 5-column lines read as card 1) and `cards.txt` (which virtual card each
  system's local saves belong to). Switching cards puts the local saves on the old card first and moves them aside,
  never deletes; with a conflict or a failure on the old card it waits (tested).
- Like after a PS1 card switch, the first command after one can go unanswered; tests ask again.
- In CI one run lost a whole system in the sync (`game names over 64 characters`: nothing pushed); not seen in 15
  local runs. The likeliest cause is a missed Core frame (the sync then skips that system), so `smc_core()` now asks
  twice, as Hello does (Core changes nothing when repeated). CI now prints the sync log to confirm next time.
- The next CI run failed other tests the same way (a Create or Commit frame unanswered). Version 1 does the same on
  this PC with two CPU hogs running, so it's the virtual card's harness, not version 2: it gave up on an ACK after
  100 ms of wall time even when the card's threads hadn't run at all in that time. It now also waits until the
  card's main loop has gone round 20 times (2 for a frame's last byte). Each test may take 3 minutes (was 1).
  With two hogs spinning on both of this PC's cores the big-save tests still run past that: the card's threads get
  almost no CPU there, which CI's runners don't do.

### Virtual cards on the SS1: picked during a game, switched at the next menu

On the SS1 with the test Main (bind-mounted) and vcardd version 2, saves in
`supermemcard_test/`: Main sends Core just before it loads a console core (`fpga_load_rbf`, the system from the
.rbf's name; not PSX), and Core `.` after the menu's sync.

1. Sync at menu start: 5 test saves to card 1.
2. SNES core loaded: "SNES core: its card 1". A press of the card's button (vcardd's `OP_KEY`): `SNES/card.ini`
   says `card=2` 1.5 s later.
3. Back to the menu: "SNES: card 1 -> card 2"; both SNES saves moved aside into the state folder, the other systems
   untouched. In the menu the buttons moved the PS1 card again (Core `.` worked), and back.
4. SNES again, card 1 picked, menu: "SNES: card 2 -> card 1", both saves back from card 1: every test save byte for
   byte as before.

Restored after: installed Main and menu core (md5 checked, running the installed Main), `saves/` and `config/`
unchanged, vcardd off, no bind mounts left. The earlier test card and state (version 1 layout) are kept as
`supermemcard_test/card_v1_*`, `state_v1_*`.

### The real card firmware builds with Super MemCard (and a bug that only the real build showed)

`firmware/build.sh` builds sd2psXtd (pinned commit, submodules) with the patch for the RP2040, as sd2psXtd's own CI
does; in CI for SD2PSX (with the screen) and PMC+ (without). Both link: `sd2psx.uf2`, `pmc+.uf2`.

- **Bug, fixed:** `ps1_supermemcard.c` uses `get_rand_64()` (the card ID) from `pico/rand.h`, and the `ps1_card`
  library didn't link `pico_rand`: the real build stopped with "pico/rand.h: No such file". The virtual card has its
  own stub of it, so only the real build could find this. The patch now adds `pico_rand` to `src/ps1/CMakeLists.txt`.
- Locally, CMake's own fetch of picotool failed ("not our ref" in one of its submodules); building picotool from
  the fetched source and passing `-Dpicotool_DIR` worked. CI fetches it normally.

### The front-port logic and the card firmware work together, bit by bit (layer 2)

`sim/bus` runs the front-port driver (`core/sys`: `smc_port`, `smc_card_sector`, `smc_sio_byte`, pinned at the
supermemcard commit) in Icarus Verilog, with sd2psXtd's own PS1 code (with Super MemCard, or stock) behind a
bit-level card model (`vcard_bus.sv`, DAT on the falling clock, ACK after the byte) through a VPI bridge to vcardd
(new wire op `OP_PEEK`: the byte the card queued for the next clock-out). Main's side is driven the way the test Main
drives EXT_BUS.

- With Super MemCard: PS1 sector read (SMC_CARD and as a raw frame), Hello, Select, Create, two Writes, Commit, two
  Reads, then the PS1 sector again: all right; `save.sav` lands on the SD card byte for byte. Port 2 empty answers
  nothing. 1033 bytes on the bus, 1015 ACKed.
- Stock firmware: Hello and Select stop at byte 2 (no ACK of the command byte), PS1 reads before and after work.
- Runs in CI (`sim.yml`); `sh sim/bus/run.sh`, `STOCK=1 sh sim/bus/run.sh`. About a minute each: every byte the
  card doesn't ACK (the controller polls on port 1) costs the bridge's real 100 ms wait.
- Tooling: in Icarus, an automatic task's `output` argument written into an array element (`w(0, data[k])`) is
  lost; go through a temporary. A string literal assigned to a wider reg is right-aligned.
- Tooling: sim/card's Makefile didn't track headers, so a stale `build/vcardd` (from before `OP_PEEK`) failed every
  stock read in the bus simulation. Now built with `-MMD -MP`.

### The flaky PC test, found: the first write after a card switch

Two tests (`game ID switches to the game's card`, `PS1 session leaves SuperMemCard/ byte for byte`) failed about
1 run in 6 to 20: the first write after a game ID card switch got no reply even after one wake. The card restarts its
loop after a switch (already noted in the test); a PS1 never notices because it polls the pad on the same select.
The tests now ask again (up to 3 wakes, 20 ms apart), as a host must. Not a firmware change.

With the PC busy (other simulations running) a third one showed: `controller combo L1+L2+R1+R2+Up: next card`
sent the combo in one poll and the release in the next, and the firmware sometimes missed one. The test now holds
the buttons for 4 polls and lets go for 4, 16 ms apart, as a PS1 polls. After both fixes: 0 failures in 40 stock
runs and 10 Super MemCard runs. The simulations wait in real time (the bridge, the harness), so running several at
once on a 2-core machine can still fail them; CI runs them one after another.

### How fast the SS1's front ports move a card's data

Timed on the SS1 with the Sony card (read only): a 140-byte frame (one 128-byte block plus the command bytes)
takes **~10.9 ms** on the bus, so Super MemCard moves **~11 KB/s** of save data: 8 KB ~0.7 s, 32 KB ~3 s,
128 KB ~11 s. The design had assumed 20-25 KB/s (README and 05 corrected). Most of a frame is the 30 us gap left
after each byte for software cards; a shorter gap for Super MemCard frames is the first thing to try if it's too
slow (sd2psXtd answers from PIO, it may not need it all). Through Main's socket (`card_read` from a card job or a
tool) it's much slower, ~51 ms a sector: Main handles its socket once per pass of its UI loop (~25 ms), and a
`card_read` needs two passes. Super MemCard's sync calls the bus from inside Main, so it isn't affected.

### Raw card frames on the SS1's front ports; a real Sony card ignores Super MemCard

the test menu core has raw frames in its front-port extension (version 2: `SMC_RAW` 87,
`PH_RAW_STATUS` 88): up to 255 bytes in one select through `card_sector`'s own byte engine, each reply kept,
stopping after the first byte without an ACK, with a per-frame ACK wait up to ~168 ms. the test Main has
`card_raw P MS HEX` and a raw-frame call (now `main/smc_frontport.cpp`); the sync uses the card in front port 1 or 2 that answers Hello.
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
- The first build of the core missed HDMI timing by 42 ps (the test Main's branch has +0.49 ns); the only sizeable
  new logic, a multiplier for the ACK wait, became a shift (the wait is sent in units of 1024 clocks).

### Phase 1 works end to end on the SS1

With the test Main and the virtual card, on the real SS1 (all reached only through Main's command interface):

- **Leaving a game:** Chrono Trigger loaded by MGL on `/dev/MiSTer_cmd`; its save changed during the game; leaving
  the game (`load_core menu.rbf`) restarts Main in the menu core and the sync sent it to the card (md5 equal).
- **A reinstall:** test saves folder and sync state wiped, Main restarted: all 5 saves (SNES x2, N64 EEPROM +
  Controller Pak, Genesis) came back from the card byte for byte in ~7 s.
- Main creates a game's save file only when the game first saves, so a test without a player has to put the
  save there itself; Main's own writing of saves is MiSTer's and untouched.
- the test frontend's watchdog isn't consistent about restarting Main: once within seconds, once not within 15 s. The test
  script starts Main itself if nothing has after 8 s.
- Afterwards: the installed Main back, `saves/` and `config/` checked against their backups (unchanged).

#### Before that: the test Main and the first run

the test Main with `supermemcard.cpp` (the test Main branch `supermemcard`) syncs at every start in the menu
core. On the SS1, with the virtual card (`vcardd`) and test saves in `supermemcard_test/`: SNES 8 KB, N64 EEPROM +
Controller Pak, Genesis 16 KB all reached the card byte for byte; a second run leaves unchanged saves alone.
The test Main runs bind-mounted over the installed binary's path (`supermemcard_test/main_test.sh`): the test frontend's
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

SS1 Tool, f3bandit's card tools and ps1_memcard_lib recognise a card by the saves on it (common save names / all, 0.5 or more =
the same card) and don't keep a version identical to the card's latest; nothing is written to the card
(`ps1_memcard_lib/src/mclib.c`, SS1 Tool `memcard.go`). Block 0 frames 56-62 are unused by the PS1 format
(`mcformat.go`), but they're not used for a Super MemCard marker: they travel with the image and can be wiped.
Decided: a Super MemCard card gives its own card ID in Hello's answer (05 §2).

Matching by save names goes wrong on nearly empty cards (1 save + 2 new = 33%, a new card), empty cards (always a
new card), reorganised cards, and different cards holding the same games (merged). So the manager gets a marker
for card identity (block 0 frame 56, written once, save names as the fallback): on the SS1 Tool todo
list, to be tested first for invisibility on the PSX core, Console Mode and a real PS1/PS2.

### Decided: the core sends Hello on its first card poll

Hello carries who the host is (the test Main, Console Mode, a core's `sys/` driver) and what it can do, so the
card knows it's on an approved core and turns on that host's features; stock cores, the stock PSX core and a
real PS1 never send it, so the card stays a plain PS1 card for them. Sent twice if the first gets no reply
(finding 4 below). Details in 05 §2.

### The virtual card runs on the SS1

- `make ss1` builds `vcardd` and `test_card` for the SS1 (armv7, **static**, so MiSTer Linux's glibc 2.31 doesn't
  matter). All 15 card tests pass on the SS1 in ~20 s, run from `/tmp` (nothing written to the SD card).
- `vcardd <folder>` keeps a virtual card powered and serves the bus on `/tmp/supermemcard.sock`. Checked on the
  SS1: card ID `08 5A 5D 5C 5D 04 00 00 80`, `Card1-1.mcd` created, clean power-off on SIGTERM. Until Taki's card
  arrives, the test Main talks to this instead of the front port; only that link changes when the card comes.
- SS1 used: MiSTer Linux 260912, kernel 6.18.38-MiSTer, armv7l, the test Main installed, `/tmp` is a 246 MB tmpfs.

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

### The first real Super MemCard card: a BitFunX PSxMemCard Gen2 (Nov 11 2026)

On the SS1, through the test Main's shim with the NES core's front-port driver (`supermemcard_test/realcard.sh`), the
BitFunX Gen2 with this firmware in front port 2 and the original Sony card (with the user's saves) in port 1:

- The Sony card was read whole first (PS1 sector reads only) and was identical to its Nov 7 backup; it refused
  every Super MemCard frame (2 bytes, as before) and smcd said `nocard` for its port.
- The BitFunX answered Hello (protocol 2, features 1F) and CERT: chip `e46560f6cf512326`, no identity (4F).
  `smcd -G` used it. `smccert issue` (the test key) made its `smc_key.bin`; on the card's SD card it was taken at
  the next start (CERT 47, the certificate equal to the file's signature).
- **A fast bus corrupts bytes on this card without losing any.** smcd then said it "isn't signed by Retro Remake":
  at speed 1 (500 kHz) one CERT in three came back whole with one wrong byte; speeds 2 and 3 got no answer at all;
  speed 0 (PS1 speed) was always right. smcd steps down only when a frame isn't answered whole, so it stayed at
  speed 1 and checked a damaged certificate. With every frame at speed 0 the same card was genuine (no `-G`).
  Fixed: the identity's frames (CERT, AUTH, AUTHSIG: no checksum) always go at PS1 speed, like Hello, in smcd and in
  Main; and a save frame that fails its checksum (a read's, or the card's 4E for a write) steps the speed down too
  (`smc_bus.bad_data`), as a short frame does. (The Sony card had never shown wrong data at speed: it isn't a
  software card.)
- The first frame after the ports are switched on still gets no answer (as before): a plain read first.
- A frame needs smcd's ACK wait (100 ms): with 2 ms, Super MemCard frames to this card weren't answered.

Then a real save, end to end (Crystalis on `NES_smc.rbf` under the test Main, the BitFunX in port 2, smcd without -G):

- **Speed per card, measured.** At 500 kHz the card flipped bits in status bytes too (Create's 47 came back 07), so
  saves "failed" though the card had them. smcd now measures each card once (`front_calibrate`): CERT, whose 105-byte
  answer never changes, 10 times at each faster speed against PS1 speed; four short Hellos weren't enough (they passed
  at 500 kHz). This card: speed 0.
- **ACK wait 167 ms** (`SMC_ACK_TIMEOUT_MS`, the front ports' most): the card took up to ~90 ms to ACK a write while
  its SD card wrote; with smcd's frames back to back a commit answered past 100 ms. The card had the save (read back:
  equal), smcd reported it failed and its state went stale.
- **A failed save is sent again** (every 30 s while the game runs); before, a failure counted as synced until the game
  saved again. A launch with no card reachable no longer keeps the previous game's "synced" time.
- **Core during the game.** With Main's stock menu core the card isn't reachable at launch; smcd now tells the card
  the system (Core) the first time it reaches it in the game: the card's screen shows "SuperStation", the card number
  and "NES saves" (seen on the card).
- **Never replace a save file Main has open.** A sync that brought a save in while the game ran (here: LAUNCH sent by
  hand to a restarted smcd) wrote a new file and renamed it over `Crystalis.sav`; Main went on writing the game's
  saves to the old, deleted file (`/proc/<Main>/fd`: "(deleted)"), so they were lost when the game ended (rescued
  from the open file). `write_file` now writes an existing save in place.
- `failed:` lines say the step and why (select, write name.txt, write, read the card's copy: no answer, refused, ...).
- The game IDs: smcd files a save under its ROM's ID (`CRC-1335CB05` for this Crystalis), its name in `name.txt`.
- MiSTer writes a game's save to the SD card when the OSD opens (Autosave) only if the game's save RAM changed;
  otherwise OSD > Save Backup RAM.
- Console Mode itself has none of the hooks yet (its Main's shim calls, its menu core's raw frames): this was the test
  Main with Main's stock menu core, and leaving a game lands in the stock menu, not Console Mode.

### The card's screen for the other systems (Nov 11 2026, BitFunX Gen2, SS1)

- Core now carries the whole screen (`SMC_CORE_SCREEN`, firmware version 3): the system, the virtual card, when the
  save was made, the game, the save's state and size. smcd sends it after the launch sync, the first time the card is
  reached during the game, and after each save that reaches the card. Shown on the BitFunX with NES/Crystalis on our
  NES core.
- **A scroll bar down the screen's right side** covered the card number and channel: two new rows ran past the
  64-pixel screen, and LVGL adds a scroll bar when a screen's content is taller than it. The bottom 12 pixels are the
  button bar (`< Menu >`) too. Fixed: in system mode the rows move up into the space above the button bar (card,
  saved, game, save line), the save line scrolls like the game's name, and the main screen doesn't scroll.
- **smcd gave up on the card for the whole game** when its first in-game try came while the core was still starting
  (1 s after launch): `no_card` was never cleared. Now it tries again every 10 s (30 s after a minute).
- **`load_core` of an MGL while a core runs** restarts Main straight into the new core: `smc_shim_core_start()` isn't
  called and smcd never hears of the game. Going to the menu first worked. Needs the core-start call on Main's
  start-up path (10, "Every way a game starts").
- Flashing the card from the SS1 needs no button: the firmware's USB serial port (`/dev/ttyACM0`) takes the Pico SDK's
  1200-baud reset into the bootloader; the `RPI-RP2` drive mounts as `/media/usbN` and the `.uf2` is copied onto it.

### The card's menu, its log, and the launch paths with a menu core that reaches the card (Nov 12 2026)

- One settings menu: the card's own, a `--SuperStation--` divider, then Saves on card, Saving, Clock, Free, Safe to
  remove (03). Saves on card goes systems > games > saves with their date and time, names only.
- Games on the card showed as `CRC-1335CB05`: the card took a game's name from `game.ini` only, which needs the game
  lists (not on this SS1). Now it falls back to the save's own name (`name.txt`). Two Crystalis sets on card 1 were
  the same game filed two ways: one by its ROM's ID, one by name from before game IDs (an old test).
- The card's USB serial log (03) showed every command with its time: transfers ran at 3.5-7 KB/s, a Commit 50-130
  ms, Select ~9 ms.
- **A user's card switch on the card loads that card's saves.** Switching NES from card 3 to 1 on the card put the
  SS1's NES saves on card 3 first, then card 1's on the SS1: as designed (a different card is a different set of
  saves). Two sets of one game on a card (the old name-filed one beside the game-ID one) were both written into the
  same save file, the newer last: to fix (one set a save file, the newest).
- **Launch paths on the BitFunX** (test Main, our NES core): an MGL from the menu, the memory manager's Play, leaving a
  game and a save during the game all worked, the card's screen right each time, once the menu core could reach the
  card. With Main's stock `menu.rbf` the menu side got `nocard`. An MGL started while a core runs restarts Main
  straight into the core and isn't seen (outside Console Mode: not needed). A game picked in the OSD needs the
  ROM-load hook (10), not built yet.
- Flashing from the SS1: `smcd/flashcard.sh`, about 15 s, no button.

### Open

- (Closed: the flaky PC test was the first write after a card switch, see above.)
