# Super MemCard in Console Mode: the guide for Retro Remake

Everything Console Mode needs to add, in one place, in the order it would be built. The details are in 05 (design and
protocol), 06 (Console Mode as it is), 09 (game IDs and names) and 08 (what was found testing). All of it is built
and tested on an SS1 with a test Main (a fork of Main_MiSTer), and against Console Mode 2.0-RC-1's SD card (section 7).

What it does: every console core's saves also go to the Retro Remake memory card, by game, so they survive a
reinstall and follow the card to another SuperStation. Like a PS1, the card is only used when a save is loaded or
saved. The card stays a plain PS1 card for everything else (a real PS1/PS2, the PSX core, Memory Cards' card reads).

## 1. The pieces

| where | what | from |
|---|---|---|
| the card (RP2040 firmware) | `81 60`-`6D` commands, the saves on its SD card, virtual cards per system, its own screen, its identity | `firmware/` (a patch to sd2psXtd, GPL-3.0) |
| menu core | send a frame of any length (up to 255 bytes) to a front port and give back the card's bytes | `core/sys/smc_port.sv` (`SMC_RAW`, version 3) |
| `smcd` | all of Super MemCard: which save, the sync, game IDs and names, the genuine-card check, the memory manager's commands | `smcd/` with `host/` (closed, Retro Remake's licence), one program beside Main |
| `MiSTer_ConsoleMode` | the shim: passes `smcd`'s card frames to the front ports, tells it when a game starts and when the menu is back | `main/smc_shim.*`, `main/smc_frontport.*` (GPL-3.0, as Main) |
| frontend | a Super MemCard view in Media › Memory Cards, through `smcd`'s socket | (Console Mode's own screens) |

`smcd` is its own program and talks to Main over a local socket, the way Console Mode's frontend already talks to its
Main, so nothing of it is in a GPL program (11). With no `smcd` running, Main works exactly as without Super MemCard.

## 2. Menu core: raw frames

The library needs one thing from the bus: send `out[0..len-1]` in one card select and give back the card's byte
for each, stopping after the first byte the card doesn't ACK (`struct smc_bus.frame`, `host/smc.h`). The ACK after
a Super MemCard request can be up to 100 ms late (the card reads its SD card): the wait has to be that long.

Super MemCard's front-port driver (`core/sys/smc_port.sv`, the same in a menu core and, raw frames only, in every
game core's `sys_top`), as Main drives it over `EXT_BUS` / UIO (`main/smc_frontport.cpp`):

```
80 SMC_ID          read  534D ('SM'), version, mode
81 SMC_MODE        write bit 0: the front ports are the driver's (only while frames run)
87 SMC_RAW         write {port, len}, {speed[15:14], ack wait[13:0] in 1024-clock units}, then the bytes as words
88 SMC_RAW_STATUS  read  {busy, done, count}, then the card's bytes as words
speed: 0 = 250 kHz as a PS1 (Hello and every PS1 command), 1 = 500 kHz, 2 = 1 MHz, 3 = 2 MHz
```

`smcd` sends Hello at speed 0 and Super MemCard frames at 2, and steps down by one when a frame comes back
short (`front_frame()` in `smcd/smcd.c`). The speed the real card handles is still to measure. PS1
reads in Console Mode 2.0-RC-1 take ~17 ms a sector (06): if its menu core has its own raw command, only the frame
length (≤255) and the ACK wait matter.

Controller reads pause while a frame runs (they share the bus), as `smc_port.sv` does.

## 3. smcd and the shim

### Checklist: everything Console Mode adds

A working example of all of Main's part: the private `test-main` branch of this repository (a Main_MiSTer fork with
the shim, the Saves page and their hooks; `git diff` it against its stock base to see each change). Tested with it on an
SS1 (Nov 10 2026).

**In `MiSTer_ConsoleMode` (GPL, copied from `main/`):**

1. Copy `smc_shim.cpp/.h`, `smc_frontport.cpp/.h`, `smc_osd.cpp/.h` into the source (built by the Makefile's `*.cpp`).
2. `fpga_load_rbf()`: `smc_shim_core_start(name, xml)` for every core that isn't `menu.rbf`, before it loads.
3. `main()`, after `user_io_init()`: `smc_shim_menu_start()` (it does nothing unless Main started with the menu core).
4. Main's loop: `smc_shim_poll()`.
5. `menu.cpp`, the OSD's Saves page (the table in "The OSD's Saves page" below): two menu states, the " Saves" row on
   the core's first page, its select, and the two states' code (with `smc_osd_scroll()`).

**On the SuperStation:** `smcd` started at boot (it needs nothing else; with no card it waits), the cores with the card
driver (public builds: f3bandit/super-memcard-cores), the gamedb module (cm_modules) for game names and icons.

**In the frontend's memory manager (the commands below, on `/tmp/smcd.sock`):**

| button / screen | command |
|---|---|
| Read card | `READ` |
| the other systems' saves (a section under PSX, or a tab) | `SECTION` (or `SYSTEMS` + `LIST`) |
| Play (a save on a card) | `PLAY` |
| Rename (Console Mode's on-screen keyboard) | `NAME` |
| Delete | `DELETE` |
| both changed: keep which | `CONFLICTS`, `KEEP` |
| the card bus: wait while someone has it | `/tmp/super_memcard.busy` (Shared bus, below) |


### In `MiSTer_ConsoleMode` (the shim)

Copy `main/smc_shim.cpp/.h` and `main/smc_frontport.cpp/.h` into its source (GPL, as Main) and add three calls:

| call | where |
|---|---|
| `smc_shim_core_start(rbf, mgl)` | in `fpga_load_rbf()`, before a core that isn't the menu loads. Waits for `smcd` (the game's save from the card, at most 30 s; a second or two normally), passing its card frames meanwhile |
| `smc_shim_menu_start()` | in `main()`, after `user_io_init()`, when Main starts with the menu core |
| `smc_shim_poll()` | in Main's loop: passes `smcd`'s frames (also while a game runs, through the core's `sys_top` driver) |

That is all of it in Main. Sockets (text lines):

```
/tmp/smcd.sock            smcd listens.  Main sends   LAUNCH <rbf>\t<mgl>   -> DONE (after the game's save; "DONE saves" when
                                                                              the game's saves are on the card: the OSD page)
                                                      MENU                 -> DONE (at once; smcd works after)
                                     the OSD page:    SAVES                -> save\t<card>\t<files>\t<time>\t<playing>\t<label> ...,
                                                                              game\t<sys>\t<name>, new\t<card>, END
                                                      USE <card>           -> use ok <card> | use failed, END
/tmp/smc_frontport.sock   Main listens.  smcd sends   FRAME <port> <speed> <ack ms> <hex>  -> OK <count> <hex> | ERR <code>
                                                      HOLD <0|1>                           -> OK
```

### `smcd`

Started once at boot, before Main (from Console Mode's start-up script):

```
smcd -S /media/fat/saves -T /media/fat/supermemcard -g /media/fat/games -g /media/usb0/games ... -d /media/fat/supermemcard/gamelists
```

It logs to `/tmp/smcd.log`. What it does:

- **A game starts** (`LAUNCH`): the save folder from the `.rbf` and the MGL's `<setname>`, the way Main names it
  (`GameboyColor.mgl` → `GBC`, `Game Gear.mgl` → `GameGear`; `Mega Duck` and `Atari 2600` aren't kept). The card shows
  the system and its buttons pick that system's virtual card (Core); the card's copy of that game's save comes in if
  it's newer. Every console core takes part, PSX too (its card images in `saves/PSX` are its saves); a PS1 read or
  write on the card ends system mode.
- **While it runs**: the game's save goes to the card as soon as the game saves (the files changed and settled for a
  second), in cores with the card driver in `sys_top` (below).
- **Back in the menu** (`MENU`): the game just played goes to the card if it changed, and the card's buttons are the
  PS1 card's again. Nothing happens if no game was played. Run it after Console Mode's own exit snapshot.
- **Only with a genuine Retro Remake card** (12): any other card is a PS1 card to `smcd`.

### Memory Cards (the memory manager)

The frontend sends `smcd` one command a line on `/tmp/smcd.sock`; each answer ends with `END`. With no card, or a card
that isn't Super MemCard, the answer is `nocard`; a Super MemCard card that isn't genuine: `notgenuine <card> <why>`.

| command | answer lines |
|---|---|
| `HELLO` | `card <id> version <n> features <hex> free_kb <n>` |
| `READ` | Read card, everything both ways: `read ok card=<id> pushed= pulled= unchanged= conflicts= skipped= failed= deleted=` |
| `SECTION` | the whole "other systems" section in one answer (below); `SECTION PSX` adds the PSX core's card images |
| `SYSTEMS` | `<system>\t<its current virtual card>` per system on the card |
| `LIST <sys> [<card>]` | per game: `name \t id \t time \t files \t bytes \t icon \t save name \t folder \t label` |
| `DELETE <sys>\t<save>` | `delete <rc>`: other SuperStations delete their copy at their next sync; nothing is lost |
| `CONFLICTS` | saves whose two copies both changed (played on two SuperStations) |
| `KEEP <local\|card> <sys>\t<save>` | `keep <rc>`: settles one |
| `NAME <sys>\t<card>\t<save name>\t<label>` | `name ok` \| `name nosave` \| `name failed`: the **Rename** button (below) |
| `PLAY <sys>\t<card>\t<save name>` | `play ok <card>` \| `play notfound` \| `play psx` \| `play failed`: the **Play** button (below) |

**Rename** (on a game's entry, per virtual card): a name the user gives that card's save of the game ("Main run",
"Speedrun"), typed on Console Mode's own on-screen keyboard, then `NAME`. smcd keeps it on the card beside the save
(`label.txt`), so it goes wherever the card goes; it's never a save on a SuperStation. Up to 24 characters, printable
ASCII (what the OSD's font shows); spaces round it are dropped; an empty name clears it. `name nosave`: that card has no
save of the game. `label` is LIST's and SECTION's last column ("" when none) and SAVES' last; the OSD's Saves page shows
it on the save's row: `Card 2  now  Speedrun  Oct 05 2026 20:47` ("now" by the card number stays put; the name and the
date scroll on the selected row when they don't fit, as Main's file names do).

**Play** (on a game's entry, per virtual card): starts the game with the save on that card. smcd switches the system to
that card the way the OSD's Saves page does (this SuperStation's saves to their card first, the picked card's in), then
tells Main to load the game (`load_core <mgl>` on `/dev/MiSTer_cmd`), the way Console Mode starts a game; the game's
LAUNCH then finds its save in place. `save name` is LIST's (or SECTION's) save name column, `card` its card. The game:

- played on this SuperStation before: the MGL of its last launch, which smcd keeps (`<state>/launch/<sys>/<save>.mgl`);
- otherwise its ROM, found by the save's name in the system's games folders, in an MGL made as Console Mode's own
  (`_@Homebrew`) ones are: NES, SNES, Game Boy, Game Boy Color, GBA, Mega Drive, N64, Master System, TurboGrafx-16.
  Other systems (CDs, arcade) start only from an earlier launch's MGL;
- `play notfound`: neither (the ROM isn't on this SuperStation): say so, nothing changes; `play psx`: the PSX core's
  saves are its card images in the port, not these.

Send it from Console Mode's menu (where the memory manager is). Checked on a PC (`smcd/test.sh`) and on an SS1 (Nov 10
2026): Final Fantasy VI started on card 1 from an MGL made from its ROM, then on card 2 (a blank), then with the real
save moved to card 2 and the blank to card 1, card 2's save loaded and the OSD's Saves page showed card 2 as "now".

`SECTION`, every system's saves at once, any number of them, in a fixed system order:

```
system  \t <folder> \t <title> \t <its current virtual card> \t <games>        then that system's games:
game    \t <folder> \t <name> \t <id> \t <time> \t <files> \t <bytes> \t <icon> \t <save name> \t <card folder> \t <both changed 0|1> \t <label>
section \t <games> \t <systems>                                             last, the totals
END
```

`name` is the official name (else the header's title, else the save's name); `id` the game's own code (`NSME`,
`SLUS-00594`, `CRC-1A2B3C4D`); `time` when the save was made (UTC). Show the save name too: two saves of one game on
one SuperStation (one ROM under two names) are two entries with the same name and ID. Icons: `icon.bin` beside the
saves, the PS1 save-icon layout (16x16, 16 colours, 1-3 frames; 05); every system has a default one for now.

### On the screen: the PS1 card's saves stay as they are

Memory Cards / Port 1 keeps everything it shows today (Card options, the PS1 saves, Free). Super MemCard's saves are
a section of their own, fed only by `SECTION`, so it can go wherever suits the screen, and moving it changes nothing
in `smcd`:

- **below the PSX saves**, with its own heading (`Other systems`), its own options row, then
  each system's heading and games; or
- **its own tab** (`PSX` | `Other systems`, L1/R1).

Mockups of both, on Console Mode's own screen: the "Super MemCard Saves Screen" design.

**Long lists** (a card can hold hundreds of saves): the list scrolls inside its own area, between the header and the
footer, and the selected row always stays in view; a scroll bar (or `21/48`) shows where you are; a name too long for
its row ends in `…` on one line, never wraps; the right-hand column (system and time, or blocks) keeps its width.
Nothing runs off the screen or over another row. System headings scroll with the list.

### Shared bus

One user of the card at a time: `/tmp/super_memcard.busy`, in cm_modules (`cmmod.h`: `cmod_mc_lock()` /
`cmod_mc_unlock()`; `mc_lock` / `mc_unlock` in the modules' `lib/common.sh`). A symbolic link to `"<pid> <who>"`, made in
one step, so two can't both get it, and free again as soon as its owner is gone, even after a crash.

- `smcd` holds it while it has the card open (waits up to 10 s for it, then leaves the card alone until next time).
- cmjobd's card and controller-tester jobs hold it.
- Memory Cards' own reads and writes of the PS1 card (`@memcard`): hold it around them. Not while waiting for
  `smcd` (READ and the other commands): `smcd` takes it itself.

### The OSD's Saves page: another save for the running game

While a game whose saves are on the card runs, the core's OSD page gets **Saves** (after the core's own options, page
one only). It lists the game's saves, one per virtual card that has it (the card and the time; "now" on the one it's
running with, "empty" on a card picked with no save yet), and **New save** on the first free card. Pick one, pick it
again ("Again: restart with this save"), and the game restarts with it: smcd takes this SuperStation's save to its card
first, puts the picked one in place and sets the system's card (`card.ini`); then Main loads the game's MGL again. Long
lists scroll (up/down, page up/down, home/end); every row fits the OSD. The card's buttons pressed during a game pick
the card for the next game, not this one (a game holds its save in memory: switching under it would mix two).

Games started from an MGL (or MRA) only: smcd names the game from it, and the restart loads it again. The shim keeps
its path in `/tmp/smc_game`, and a copy of the MGL in `/tmp/smc_game.mgl` in case the first goes; both go when the
menu is back. Not the PSX core (its own card in the port).

In `MiSTer_ConsoleMode`: copy `main/smc_osd.cpp/.h` as well, and in `menu.cpp` (the same as its Cheats page):

| where | what |
|---|---|
| the menu states | `MENU_SMC_SAVES1, MENU_SMC_SAVES2,` |
| the core's main OSD page, after its options (`page == 0`) | `if (smc_osd_available()) { smc_submenu = selentry; MenuWrite(entry, " Saves", menusub == selentry, 0); menumask = (menumask << 1) \| 1; entry++; selentry++; }` |
| that page's select | `if (smc_submenu == menusub && select && smc_osd_open()) { smc_ret_sub = menusub; menusub = 0; menustate = MENU_SMC_SAVES1; }` |
| `MENU_SMC_SAVES1` | `smc_osd_print(); menustate = MENU_SMC_SAVES2;` |
| `MENU_SMC_SAVES2` | `if (menu \|\| back) { menustate = MENU_GENERIC_MAIN1; menusub = smc_ret_sub; break; } smc_osd_scroll(); if (smc_osd_key(up, down, c, select)) menustate = MENU_SMC_SAVES1;` |

Checked on a PC (`smcd/test.sh`: the page, asked first, a new save, back to the first, 31 rows scrolling) and on an SS1
(Nov 10 2026, `smcd/ss1test.sh c`: an MGL launch kept for the page, SAVES, USE of a new save and back, with the card's
switch).

### During the game (phase 2)

Cores built with `tools/sys-patch.sh` (the front-port card driver in their `sys_top`, no change to the core itself;
public: f3bandit/super-memcard-cores) reach the card while the game runs, through the same shim. Not in the PSX core,
which uses the front ports itself, nor Saturn, which has no room left in the FPGA: those two, and any core without
the driver, get their saves to the card when the menu is back.

What the driver answers in a game core: `SMC_ID` (`80`, `0x534D` 'SM'), `SMC_MODE`, `SMC_RAW`, `SMC_RAW_STATUS` only
(no controller reads or sector commands). The shim turns the ports on only for the frames.

Built with the driver in all 13: NES, SNES, N64, Game Boy, GBA, SMS, Genesis, Mega Drive, Mega-CD, 32X,
TurboGrafx-16, Jaguar and Neo Geo (public builds: f3bandit/super-memcard-cores). All meet timing except Jaguar, which
misses it by the same amount without the driver (upstream's). On an SS1 (Nov 10 2026), each one read the PS1 card in
port 1 through its own driver: a whole sector, the card's ID and its ACKs (`smcd/coretest.sh`).

## 4. Settings

`SuperMemCard/settings.ini` on the card: `saves=0` turns Super MemCard off on the card itself (it then doesn't even
ACK `81 60`: a plain PS1 card). The card's own SuperStation menu sets it too. A Console Mode setting (Off / Front
port 1 / Front port 2) would skip the port search.

## 5. Game IDs and names

Nothing for Console Mode to do: the library finds each save's game in the games folders by Main's rule (the save is
named after the game file) and reads the game's own code from its header (N64, GBA, Mega Drive/Genesis/32X, SMS/Game
Gear, later SNES and Game Boy carts), the product code of a disc, else the ROM's CRC32. Saves on the card are filed
under it, so the same game finds its saves on every SuperStation whatever its file is called. Names come from
libretro's No-Intro/Redump lists if they're on the SD card (`tools/gamedb/fetch.sh <folder>`), else the game's
header title. Identities are cached (`<state>/ids.txt`), so a ROM is read once.

## 6. Testing without the card

- `sim/card`: the card's firmware running on a PC or the SS1 (`vcardd <sd folder> <socket>`), with the test suite
  (`make SMC=1 test`).
- `smcd -v <vcardd socket>` uses the simulated card instead of the front ports. `smcd/test.sh` runs the whole chain on
  a PC (the frontend's commands, Main's LAUNCH and MENU through the shim, a game saving); `smcd/ss1test.sh` does it on an
  SS1 with a test Main.
- `tools/smccli`: the same calls from a shell against vcardd, and `chip` (the card's chip ID, genuine or not). Build
  for the SS1: `make -C sim/card SMC=1 B=build-ss1 CC=arm-none-linux-gnueabihf-gcc LDFLAGS_STATIC=-static`.
- `sim/bus`: the front-port driver (`core/sys`) bit by bit against the card firmware (Icarus Verilog).

## 7. Checked against Console Mode 2.0-RC-1's SD card (Nov 9 2026)

A copy of its `saves` folder (the real one untouched, checksums and times before and after) went through Read card
onto a simulated card with the games on its USB drive, then back onto an empty saves folder as a second SuperStation
would get them: 32 saves in 5 systems, all back identical with their times; then single games launched from MGLs
written like Console Mode's (N64, and Game Boy Color through `GameboyColor.mgl` into `saves/GBC`). Two things it
found are fixed: MGL `<setname>` names the save folder (`smc_launch_system()`), and two saves of one game on one
SuperStation each get their own set. Details in 08.

## 7b. One game a system on an SS1 (Nov 10 2026, `smcd/sweep.sh`)

With the test Main, the simulated card and the real saves folder (backed up first): Play on card 2 (the game started
with no save; the right core), then Play on card 1 (every one of the game's save files back, byte for byte). After all
of them, every save on the SS1 as it was before.

| system (core) | game | its save |
|---|---|---|
| NES | The Legend of Zelda | `.sav` (battery RAM) |
| SNES | Tactics Ogre | `.sav` |
| Game Boy | Final Fantasy Legend II | `.sav` |
| Game Boy Color (Gameboy core, GBC saves) | Link's Awakening DX | `.sav`, 32 KB |
| GBA | The Minish Cap | `.sav` (EEPROM) |
| Mega Drive / Genesis (MegaDrive core; ROM in games/Genesis) | Phantasy Star IV | `.sav`, 64 KB |
| N64 | Ogre Battle 64 | `.sra` and four Controller Paks `_1`..`_4.cpk` |
| N64 | Majora's Mask | `.fla` (flash) |
| TurboGrafx-CD (TurboGrafx16 core, started from its MGL) | Dracula X (Rondo) | `.sav` (2 KB BRAM) |
| Neo Geo (started from its MGL) | Kabuki Klash | `.sav` |

On the way: Console Mode has no Genesis core (Genesis games are the MegaDrive core's: saves/MegaDrive), so smcd looks for
a system's ROMs in its other folders too (games/Genesis, games/TGFX16-CD; a CD folder by its `.cue`/`.chd`); Main makes
blank N64 saves as an N64 game loads with none; a Saturn game's discs share one save (Main's
`saturn_get_save_without_disk`, ported). Not yet on an SS1, for want of a game here: Master System, Game Gear, Mega-CD,
32X, Saturn and Jaguar (the saves are standard `<game>.sav`; Saturn's naming is in the card tests).

## 8. Open points

- The shipping card's firmware: sd2psXtd-based? Its source (GPL-3.0) to apply `firmware/` to.
- Raw frames in Console Mode's menu core: the same `SMC_RAW` or its own command; its timing.
- Where the shim's three calls go in `MiSTer_ConsoleMode`, and starting `smcd` at boot (a start script still to come).
- On an SS1, one game each of Master System, Game Gear, Mega-CD, 32X, Saturn and Jaguar (7b).
- Retro Remake's own signing key in `smcd` and the firmware (12); the test key until then.
- The SuperStation check (features only on a genuine SS1): how Console Mode recognizes one.
- The real card's speed limit, and a power pull mid-save on the real card (works in the simulation).
- On the real card with this firmware: a real PS1 and PS2 (plain PS1 card, no Hello ever sent), FreePSXBoot booting
  from it on a real PS1, and the stock PSX core and Memory Cards reading it as today.
