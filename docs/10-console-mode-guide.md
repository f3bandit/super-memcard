# Super MemCard in Console Mode: the guide for Retro Remake

Everything Console Mode needs to add, in one place, in the order it would be built. The details are in 05 (design and
protocol), 06 (Console Mode as it is), 09 (game IDs and names) and 08 (what was found testing). All of it is built
and tested on an SS1 with Phosphor as the test bench, and against Console Mode 2.0-RC-1's SD card (section 7).

What it does: every console core's saves also go to the Retro Remake memory card, by game, so they survive a
reinstall and follow the card to another SuperStation. Like a PS1, the card is only used when a save is loaded or
saved. The card stays a plain PS1 card for everything else (a real PS1/PS2, the PSX core, Memory Cards' card reads).

## 1. The pieces

| where | what | from |
|---|---|---|
| the card (RP2040 firmware) | `81 60`-`6A` commands, the saves on its SD card, virtual cards per system, its own screen | `firmware/` (a patch to sd2psXtd, GPL-3.0) |
| menu core | send a frame of any length (up to 255 bytes) to a front port and give back the card's bytes | phosphor-menu `PH_RAW` (`phosphor_ext.sv`, version 3) |
| host (`MiSTer_ConsoleMode`) | the library, two calls around each game, `@memcard` commands for the screen | `host/` (plain C, no allocation) |
| frontend | a Super MemCard view in Media › Memory Cards | (Console Mode's own screens) |

## 2. Menu core: raw frames

The library needs one thing from the bus: send `out[0..len-1]` in one card select and give back the card's byte
for each, stopping after the first byte the card doesn't ACK (`struct smc_bus.frame`, `host/smc.h`). The ACK after
a Super MemCard request can be up to 100 ms late (the card reads its SD card): the wait has to be that long.

phosphor-menu's version (`rtl/phosphor/phosphor_ext.sv`), as Phosphor's Main drives it over `EXT_BUS`:

```
87 PH_RAW         write {port, len}, {speed[15:14], ack wait[13:0] in 1024-clock units}, then the bytes as words
88 PH_RAW_STATUS  read  {busy, done, count}, then the card's bytes as words
speed: 0 = 250 kHz as a PS1 (Hello and every PS1 command), 1 = 500 kHz, 2 = 1 MHz, 3 = 2 MHz
```

Phosphor sends Hello at speed 0 and Super MemCard frames at 2, and steps down by one when a frame comes back short
(`front_frame()` in phosphor-main `supermemcard.cpp`). The speed the real card handles is still to measure. PS1
reads in Console Mode 2.0-RC-1 take ~17 ms a sector (06): if its menu core has its own raw command, only the frame
length (≤255) and the ACK wait matter.

Controller reads pause while a frame runs (they share the bus), as `phosphor_ext.sv` does.

## 3. Host: the library

Copy `host/` (smc, smc_sync, smc_gameid, smc_ident, smc_gamedb, smc_icons; smc_vcard only for testing) into the
host and fill a `struct smc_sync_cfg`:

```c
static const char *games[] = { "/media/fat/games", "/media/usb0/games", /* ... usb5 */ "/media/fat/cifs/games", NULL };
struct smc_sync_cfg cfg = {
    "/media/fat/saves",                 /* saves_dir */
    "/media/fat/supermemcard",          /* state_dir: what was last synced, per card */
    smc_console_systems,                /* the save folders the card keeps */
    SMC_HOST_CONSOLE_MODE, log_fn,
    games,                              /* to find each save's game: its game ID */
    "/media/fat/supermemcard/gamelists" /* libretro's lists (tools/gamedb/fetch.sh): official names */
};
```

Opening the card: send Hello on port 1 then port 2 (`smc_hello()`, it asks twice). 1 = a Super MemCard card; 0 = a
plain PS1 card or none: do nothing more. After a refused Hello leave the card alone ~100 ms.

### The two moments around a game

**Before the core loads** (where Main loads the `.rbf`; Phosphor calls it from `fpga_load_rbf()` when the core
isn't the menu):

```c
struct smc_mgl m;
if (smc_mgl_read(mgl_path, &m)) memset(&m, 0, sizeof(m));     /* no MGL: just the .rbf */
const char *sys = smc_launch_system(rbf, m.setname, m.same_dir); /* the save folder, as Main's CoreName2 */
if (sys) {
    int card = smc_core(&bus, sys, true);          /* the card shows the system, its buttons pick its virtual card */
    if (card > 0 && m.game[0])
        smc_sync_game(&bus, &cfg, sys, m.game, &rep);  /* the card's newer copy comes in before the game starts */
    remember(sys, m.game);                         /* for the menu, after the game */
}
```

`smc_launch_system()` follows the MGL's `<setname>` the way Main names the save folder: Console Mode's
`GameboyColor.mgl` starts the Gameboy core with saves in `GBC`, `Game Gear.mgl` the SMS core with `GameGear`;
`Mega Duck.mgl` and `Atari 2600.mgl` aren't systems the card keeps (NULL). Every console core takes part, PSX too:
its per-game card images in `saves/PSX` are that game's saves. If the PSX core uses the PS1 card itself, the card's
first PS1 read or write ends system mode, and its buttons pick PS1 cards again.

**Back in the menu core** after that game (not at every menu start):

```c
if (game[0]) smc_sync_game(&bus, &cfg, sys, game, &rep);  /* its save goes to the card if it changed */
else         smc_sync_changed(&bus, &cfg, sys, &rep);     /* started from the core's own file browser */
smc_core(&bus, NULL, true);                               /* the card's buttons are the PS1 card's again */
```

Run it after Console Mode's own exit snapshot, so the snapshot is always first. Nothing happens if no game was
played. Both calls only touch one game (or the saves that changed) and return quickly when nothing changed.

### Memory Cards (the memory manager)

- **Read card** (the step the screen already has before its other options): `smc_sync()`, everything both ways.
- **The list**, per system (`smc_list(&bus, ".", i, sys)` gives the systems the card has):
  `smc_list_card(&bus, sys, 0, i, id)` then `smc_game_info(&bus, sys, 0, id, &gi)`: `gi.name` (official name, else
  `gi.title`, else `gi.save_name`), `gi.id` (the game's own code: `NSME`, `SLUS-00594`, `CRC-1A2B3C4D`), `gi.time`
  (when the save was made, UTC), `gi.files`, `gi.bytes`, `gi.has_icon`. Show `gi.save_name` too: two saves of one
  game on one SuperStation (one ROM under two names, each with its own progress) are two entries with the same name
  and ID.
- **The icon**: `icon.bin` beside the saves, the PS1 save-icon layout (16x16, 16 colours, 1-3 frames; 05). Every
  system has a default one for now (`icons/sheet.png`); PS1 saves have their own and need none.
- **Delete**: `smc_delete_save(&bus, &cfg, sys, save_file_name)`. Other SuperStations delete their copy at their
  next sync; nothing is lost (the card and each SuperStation keep the file aside).
- **Both copies changed** (played on two SuperStations): the sync keeps both and counts it in `rep.conflicts`;
  `smc_conflicts()` lists them, `smc_resolve(..., SMC_KEEP_LOCAL | SMC_KEEP_CARD)` settles one.
- **Virtual cards**: `smc_core(&bus, sys, false)` asks which one a system is on; `smc_list_card()`/`smc_game_info()`
  take a card number (1-99) to look at another.

Suggested `@memcard` commands for the frontend, next to SS1 Tool's `read`/`write` (06): `smc-read`, `smc-list
<sys>`, `smc-delete <sys>|<save>`, `smc-keep <local|card> <sys>|<save>`, answers in `/tmp/consolemode_memcard.txt`
as today. `tools/smccli` does each of these from a shell (section 6), so their output can be copied from it.

### Shared bus

One user of the card at a time: Memory Cards, SS1 Tool's card reads and the Super MemCard calls take a lock file
(like `/tmp/ss1_bt.busy` for Bluetooth) while they run.

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
  (`make test`).
- `tools/smccli`: each frontend call from a shell against vcardd: `hello`, `launch <rbf> [<mgl>]`, `back <sys>
  [<game>]`, `read`, `systems`, `list <sys>`, `delete`, `conflicts`, `keep`, `core`, `mgl <file>`. Build for the SS1:
  `make -C sim/card SMC=1 B=build-ss1-smc CC=arm-linux-gnueabihf-gcc LDFLAGS_STATIC=-static`.
- `sim/bus`: phosphor-menu's front-port RTL bit by bit against the card firmware (Icarus Verilog): checks a menu
  core's raw frames against the real card code.

## 7. Checked against Console Mode 2.0-RC-1's SD card (Nov 9 2026)

A copy of its `saves` folder (the real one untouched, checksums and times before and after) went through Read card
onto a simulated card with the games on its USB drive, then back onto an empty saves folder as a second SuperStation
would get them: 32 saves in 5 systems, all back identical with their times; then single games launched from MGLs
written like Console Mode's (N64, and Game Boy Color through `GameboyColor.mgl` into `saves/GBC`). Two things it
found are fixed: MGL `<setname>` names the save folder (`smc_launch_system()`), and two saves of one game on one
SuperStation each get their own set. Details in 08.

## 8. Open points

- The shipping card's firmware: sd2psXtd-based? Its source (GPL-3.0) to apply `firmware/` to.
- Raw frames in Console Mode's menu core: the same `PH_RAW` or its own command; its timing.
- Where the two hooks go in `MiSTer_ConsoleMode`'s launch path.
- The real card's speed limit, and a power pull mid-save on the real card (works in the simulation).
