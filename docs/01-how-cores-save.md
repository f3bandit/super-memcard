# How MiSTer and Console Mode cores read and write saves

Sources: f3bandit's test Main (a fork of Main_MiSTer, Nov 7 2026), MiSTer-devel
N64_MiSTer (5725381), and what was measured on a SuperStation One with Console Mode 2.0-RC-1.
Line numbers are the test Main's.

## The save file

When a game is loaded from Main's file browser (or by a frontend through Main), Main works out the
save file's name from the game's:

```
FileGenerateSavePath()  file_io.cpp:985
saves/<core name>/<game file name, extension replaced>.sav      e.g. saves/SNES/Super Metroid (USA).sav
```

`user_io_file_tx()` (user_io.cpp:2919) sends the game to the core, then, if the core asked for a save
(`opensave`), mounts that file as the core's "SD image" 0 (user_io.cpp:3152):

```cpp
FileGenerateSavePath(name, buf);
user_io_file_mount(buf, 0, 1);      // writable: O_RDWR | O_SYNC, and allowed to grow
```

The file doesn't need to exist: it's created when the core first writes to it.

## How a core reads and writes it

Cores never see files. Through `hps_io` (the shared `sys/` framework every core is built with) a core
asks for 512-byte blocks by number:

- **read**: the core sets `sd_lba` and `sd_rd`; Main reads that block of the file and sends it
  (`UIO_SECTOR_RD`). Cores read their whole save into their own RAM (BRAM/SDRAM) when the game starts.
- **write**: the core sets `sd_lba` and `sd_wr`; Main fetches the block (`UIO_SECTOR_WR`) and writes it
  straight into the file at `lba * 512`, opened `O_SYNC`, so it's on the SD card at once
  (user_io.cpp:3670-3730). `menu_process_save()` starts the save indicator.

**When** a core writes is the core's choice. Most keep the save in RAM while the game runs and write
it when the OSD opens with *Autosave* on, or on *Save Backup RAM* in the OSD; some write on every
change. So the file on the SD card is as new as the core's last write, not as the game's last save.

## Systems that differ

| System | What's different | Where |
|---|---|---|
| PSX | Memory card images: `saves/PSX/<game>.sav`, 128 KB (`MCD_SIZE`), mounted as image 2; a new one gets a formatted card header | support/psx/psx.cpp:496 |
| N64 | Main splits the core's one save stream into separate files: `<game>.eep` / `.sra` / `.fla` (cartridge), `<game>_1.cpk`..`_4.cpk` (Controller Pak per port), `_N.tpk` (Transfer Pak). The core keeps the paks in SDRAM at `0x500000 + port × 32 KB` | support/n64/n64.cpp:1179-1220, N64_MiSTer rtl/Gamepad.vhd, rtl/savemem.vhd |
| 64DD | Disk writes in `saves/N64/<disk>.ram` | n64.cpp:1738 |
| Saturn | Two save slots (cartridge and internal) | user_io.cpp:3720 (`is_saturn() && disk == 1`) |
| Arcade | Per MRA (NVRAM, high scores) | support/arcade |

Savestates (`savestates/`) are separate and not part of this project.

## The game's ID

After a game loads, Main writes `/tmp/GAMEID` (`user_io_write_gameid()`, user_io.cpp:2779) when
`log_file_entry=1` is set in MiSTer.ini:

```
CRC32: 1FA4CB9C          cartridge games: the ROM's CRC32 (user_io.cpp:3148)
Serial: SLUS-00067       PSX: the disc's ID (psx.cpp:942); Saturn: its UUID; N64: the cartridge ID
                         (n64.cpp:2972); arcade: the MRA's set name
```

BIOS and boot files are skipped. **This is Super MemCard's game ID**: no BIOS patches.

## Console Mode

Console Mode's host, `MiSTer_ConsoleMode`, is derived from Main_MiSTer (GPL-3.0; its source isn't
public). Its strings match Main's save handling (`SavePath=%s`, `saves/%s/`) and it has the same
`/tmp/GAMEID` writer, so saves land in the same `saves/<core>/` files. Console Mode adds, measured on an SS1:

- **save snapshots**: when a core exits, the backup module packs the saves it changed into
  `ConsoleMode/tools/backups/saves/snap-<date>-<time>-exit-<core>.tar.gz` (seen Nov 8 2026 after an
  Acorn Atom session);
- **its memory card manager** for PS1 cards (see 04).

A reinstall that replaces the SD card loses `saves/` and those snapshots: the reason for this project.

## The test Main

the test Main is a fork of Main_MiSTer, so saves work exactly as above. It adds RetroAchievements
hooks around save I/O (`achievements_notify_save_io()`), and its card path to the front ports (04).

## Where Super MemCard hooks in

All save I/O goes through two places in Main, for every core:

1. **mount** (`user_io_file_mount` for a save): if the card has a newer copy of this save, it's put in
   place first (the file stays the core's working copy and the mirror, rule 4);
2. **block write** (the `op == 2` branch): after Main writes the block to the file, it marks it to be
   sent to the card; a background job sends changed blocks in the card's protocol (02, 05).

Because cores only talk to Main, **no core changes are needed for this part**. The cores only need the
front-port driver when the card is reached while a game core is loaded (05).
