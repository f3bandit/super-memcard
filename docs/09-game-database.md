# Which game a save belongs to: the game database

**Draft, in progress.** Direction (Nov 9): every save carries a **game ID**, the identifier the game itself has
(cartridge header code, disc product code; the ROM's CRC32 for systems without one), so Console Mode can tell which
game a save is from, and save sets on the card are filed under it as sd2psXtd files PS1 cards (`SLUS-00594`).
Whether the folder is named by the ID or by the game's name (with the ID in `id.txt`) is still to decide.
`host/smc_ident.c` (phosphord's identification, copied) isn't built or used yet.

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

`host/smc_ident.c` is phosphord's code, copied. Identities are cached per file (path, size, time) in the state
folder, so a ROM is read once.

## 2. The game lists (names)

The same lists Phosphor uses: libretro-database's `metadat/no-intro` (cartridges) and `metadat/redump` (discs), one
`.dat` per system. They are **not** shipped in this repository or on the card: the frontend fetches them onto the
SuperStation (as Phosphor does), and they are used there under their own terms. `host/smc_gamedb.c` reads a `.dat`
(clrmamepro format) into a table of CRC32, product code and official name; a frontend that already has the lists in
its own database (Phosphor's library) can answer the lookups itself instead (`smc_sync_cfg.lookup`).

## 3. On the card

When the game is known, its save set is named after the **official name** (`SuperMemCard/SNES/Card1/Super Metroid
(Japan, USA) (En,Ja)/`), and `id.txt` beside the saves records what it is:

```
name=Super Metroid (Japan, USA) (En,Ja)
key=crc:d63ed5f8
crc=d63ed5f8
serial=
```

Every SuperStation with the game, whatever its ROM file is called, then finds the same save set. Unknown games (no
ROM found, no list, not in the list) keep today's naming: the save file's name. `name.txt` still records the local
save name the set came from, so a SuperStation without the ROM gets the save back under that name.

Pulling a save set onto a SuperStation: its local file name is the name of the ROM there with the same identity, so
Main finds it when the game starts; with no such ROM, the official name, else `name.txt`.

## 4. Later

- The icon (05, `icon.bin`): a game's own icon looked up by its identity.
- The card's main screen and Browse menu showing official names.
