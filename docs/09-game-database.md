# Which game a save belongs to: the game database

**Built** (tested in the virtual card): every save set on the card is filed under its game's **game ID**, the
identifier the game itself carries (cartridge header code, disc product code; the ROM's CRC32 for systems without
one), as sd2psXtd files PS1 cards (`SLUS-00594`), with `game.ini` (ID, the name in its header) and the save's date
and time. That's what Console Mode's memory manager shows: **game name, game ID, date and time** (and the icon),
through `smc_list_card()` and `smc_game_info()`. The official names come from the game lists (section 2) when the
frontend has them on the SuperStation (`smc_sync_cfg.gamedb_dir`).

Today a save set on the card is named after Main's save file, which is named after the ROM file
(`saves/SNES/Super Metroid (USA).sav` for `games/SNES/Super Metroid (USA).sfc`). That works on one SuperStation, but
the same game can carry different file names on two of them (`Super Metroid.sfc`, a renamed or re-dumped copy), and
then the card holds two save sets for one game. A frontend also wants the game's real name (and later its icon) to
show beside the save, not a file name.

So each save is tied to the **game** it comes from, the way f3bandit's game library already identifies games (port,
don't rewrite: f3bandit's game library: `ident.c` and its libretro source).

## 1. What a game is (identity)

From the ROM or disc file the save belongs to (Main's rule: same name, without the extension, in the system's games
folders), exactly as the game library does it:

| | identity | looked up by |
|---|---|---|
| cartridge | CRC32 of the ROM data, counted as the No-Intro sets do: copier headers left out (iNES/FDS 16 bytes, SNES 512), N64 in big-endian `.z64` order | CRC32 |
| disc (`.cue`/`.bin`/`.iso`) | product code + data track size + CRC of its first megabyte (fast: under a second) | product code (PlayStation `SYSTEM.CNF`, Saturn and Mega-CD headers) |
| `.chd` | the SHA-1 the `.chd` header carries | (product code needs libchdr: the game library has it, this one doesn't) |

The **game ID** (`host/smc_gameid.c`, `smc_game_id_of()`): the code in the game's own header where it has one, else
the identity above.

| system | game ID | example |
|---|---|---|
| N64 | 4-character game code (header 0x3B), any byte order | `NSME` |
| GBA | 4-character game code (0xAC) | `AMTE` |
| Genesis, MegaDrive | product number (0x180) | `GM 00001009-00` |
| SMS, GameGear | product code in the TMR SEGA header | `SMS-7069` (Game Gear: `GG-…`) |
| SNES, GAMEBOY | 4-character game code of later carts (checked header) | `SNES-AXXE`, `GB-AXXE` |
| PSX, Saturn, MegaCD | disc product code | `SLUS-00594` |
| the rest (NES, TurboGrafx-16, older carts) | CRC32 as No-Intro counts it | `CRC-1A2B3C4D` |
| no ROM found for the save | the save's name, as before | |

Neo Geo saves are already named after the game's set name, so they stay as they are. Homebrew's placeholder product
numbers (`GM 00000000-00`) aren't IDs: those games fall back to the CRC. Checked on the SS1's own games
(N64, GBA, Genesis, 32X, SMS, SNES, Game Boy, NES, TurboGrafx-16, PlayStation .cue): an N64 ROM takes about a
second to identify over USB, once (kept in `ids.txt`); finding another SuperStation's ROM for a save set reads only
headers until the code matches.

`host/smc_ident.c` is f3bandit's game library code, copied. Identities are cached per file (path, size, time) in the state
folder, so a ROM is read once.

## 2. The game lists (names)

The lists: libretro-database's `metadat/no-intro` (cartridges) and `metadat/redump` (discs), one
`.dat` per system. They are **not** shipped in this repository or on the card: the frontend fetches them onto the
SuperStation, and they are used there under their own terms. `host/smc_gamedb.c` reads a `.dat`
(clrmamepro format) into a table of CRC32, product code and official name; a frontend that already has the lists in
its own database (f3bandit's game library) can answer the lookups itself instead (`smc_sync_cfg.lookup`).

## 3. On the card

When the game is known, its save set is filed under its **game ID**: for a cartridge with a code, the code with its
ROM's CRC32 (`SuperMemCard/N64/Card1/NSME-1A2B3C4D/`), for a disc its product code, else `CRC-<crc32>`. The CRC is
there because hacks and revisions keep the original's code but may not take its save: on the SS1, *Castlevania: Aria
of Sorrow* and a hack of it both say `A2CE` (now `A2CE-35536183` and `A2CE-C647E8A8`), while two copies of
*Pokemon Emerald* under different file names are one save set (`BPEE-1F1C08FB`). `game.ini` beside the saves records
what it is; the game ID a frontend shows is its `id`, the game's own code:

```
id=NSME
title=SUPER MARIO 64
name=Super Mario 64 (USA)
```

`name` is looked up by the ROM's CRC (cartridges) or the product code (discs). Checked against libretro's lists with
the SS1's own games: `007 - The World Is Not Enough (USA)`, `Pokemon - Emerald Version (USA, Europe)`,
`Alien Resurrection (USA)`, `Aero Blasters (USA)`; the Castlevania hack isn't in any list, so it keeps its header
name (`CASTLEVANIA2`). A frontend shows `name`, else `title`, else the save's file name.

Each save file's time (when Main wrote it) goes with it to the card (Commit) and comes back onto other SuperStations'
files.

Every SuperStation with the game, whatever its ROM file is called, then finds the same save set. Two saves of one game
on one SuperStation (one ROM under two names, each with its own progress) are two sets: the second is
`<id>~<CRC of its save name>`, and both come back under their own names (08). Unknown games (no
ROM found, no list, not in the list) keep today's naming: the save file's name. `name.txt` still records the local
save name the set came from, so a SuperStation without the ROM gets the save back under that name.

Pulling a save set onto a SuperStation: its local file name is the name of the ROM there with the same game ID, so
Main finds it when the game starts (tested: `Mario.z64` on one, `Super Mario 64 (USA).v64` on another); with no such
ROM, `name.txt`. Game IDs worked out are kept in `<state>/ids.txt` (by path, size and time), so a ROM is read once.

## 4. Later

- The icon (05, `icon.bin`): a game's own icon looked up by its identity.
- The card's main screen and Browse menu showing official names.
