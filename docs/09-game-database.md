# Which game a save belongs to: the game database

**Built** (tested in the virtual card): every save set on the card is filed under its game's **game ID**, the
identifier the game itself carries (cartridge header code, disc product code; the ROM's CRC32 for systems without
one), as sd2psXtd files PS1 cards (`SLUS-00594`), with `game.ini` (ID, the name in its header) and the save's date
and time. That's what Console Mode's memory manager shows: **game name, game ID, date and time** (and the icon),
through `smc_list_card()` and `smc_game_info()`. Not built yet: the official names from the game lists (section 2).

Today a save set on the card is named after Main's save file, which is named after the ROM file
(`saves/SNES/Super Metroid (USA).sav` for `games/SNES/Super Metroid (USA).sfc`). That works on one SuperStation, but
the same game can carry different file names on two of them (`Super Metroid.sfc`, a renamed or re-dumped copy), and
then the card holds two save sets for one game. A frontend also wants the game's real name (and later its icon) to
show beside the save, not a file name.

So each save is tied to the **game** it comes from, the way Phosphor's library already identifies games (port,
don't rewrite: phosphord `ident.c` and its libretro source).

## 1. What a game is (identity)

From the ROM or disc file the save belongs to (Main's rule: same name, without the extension, in the system's games
folders), exactly as phosphord does it:

| | identity | looked up by |
|---|---|---|
| cartridge | CRC32 of the ROM data, counted as the No-Intro sets do: copier headers left out (iNES/FDS 16 bytes, SNES 512), N64 in big-endian `.z64` order | CRC32 |
| disc (`.cue`/`.bin`/`.iso`) | product code + data track size + CRC of its first megabyte (fast: under a second) | product code (PlayStation `SYSTEM.CNF`, Saturn and Mega-CD headers) |
| `.chd` | the SHA-1 the `.chd` header carries | (product code needs libchdr: Phosphor has it, the library doesn't) |

The **game ID** (`host/smc_gameid.c`, `smc_game_id_of()`): the code in the game's own header where it has one, else
the identity above.

| system | game ID | example |
|---|---|---|
| N64 | 4-character game code (header 0x3B), any byte order | `NSME` |
| GBA | 4-character game code (0xAC) | `AMTE` |
| Genesis, MegaDrive, S32X | product number (0x180) | `GM 00001009-00` |
| SMS (and Game Gear) | product code in the TMR SEGA header | `SMS-7069` |
| SNES, GAMEBOY | 4-character game code of later carts (checked header) | `SNES-AXXE`, `GB-AXXE` |
| PSX, Saturn, MegaCD | disc product code | `SLUS-00594` |
| the rest (NES, TurboGrafx-16, Jaguar, older carts) | CRC32 as No-Intro counts it | `CRC-1A2B3C4D` |
| no ROM found for the save | the save's name, as before | |

Neo Geo saves are already named after the game's set name, so they stay as they are. Header IDs can be shared
by revisions of one game (N64 Rev 1 keeps its code); their saves are compatible, so they share a save set.

`host/smc_ident.c` is phosphord's code, copied. Identities are cached per file (path, size, time) in the state
folder, so a ROM is read once.

## 2. The game lists (names)

The same lists Phosphor uses: libretro-database's `metadat/no-intro` (cartridges) and `metadat/redump` (discs), one
`.dat` per system. They are **not** shipped in this repository or on the card: the frontend fetches them onto the
SuperStation (as Phosphor does), and they are used there under their own terms. `host/smc_gamedb.c` reads a `.dat`
(clrmamepro format) into a table of CRC32, product code and official name; a frontend that already has the lists in
its own database (Phosphor's library) can answer the lookups itself instead (`smc_sync_cfg.lookup`).

## 3. On the card

When the game is known, its save set is named after its **game ID** (`SuperMemCard/N64/Card1/NSME/`), and
`game.ini` beside the saves records what it is:

```
id=NSME
title=SUPER MARIO 64
```

Each save file's time (when Main wrote it) goes with it to the card (Commit) and comes back onto other SuperStations'
files.

Every SuperStation with the game, whatever its ROM file is called, then finds the same save set. Unknown games (no
ROM found, no list, not in the list) keep today's naming: the save file's name. `name.txt` still records the local
save name the set came from, so a SuperStation without the ROM gets the save back under that name.

Pulling a save set onto a SuperStation: its local file name is the name of the ROM there with the same game ID, so
Main finds it when the game starts (tested: `Mario.z64` on one, `Super Mario 64 (USA).v64` on another); with no such
ROM, `name.txt`. Game IDs worked out are kept in `<state>/ids.txt` (by path, size and time), so a ROM is read once.

## 4. Later

- The icon (05, `icon.bin`): a game's own icon looked up by its identity.
- The card's main screen and Browse menu showing official names.
