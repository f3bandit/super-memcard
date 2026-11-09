# Super MemCard design: the card's menu, the protocol and the host

A proposal to build and test on Phosphor, then hand to Retro Remake. Builds on 00-04.

## 1. The card stays a PS1 card

PS1 commands (`81 52` read, `81 57` write, `81 53` ID, `81 42`, and the MMCE game ID bytes
`81 20`-`27`) **always** go to the PS1 card, whatever else is going on. Super MemCard adds its own
command bytes next to them and never changes what the PS1 ones do. A real PS1, a PS2, the PSX core and
Console Mode's memory card manager see exactly the card they see today (rule 1). There are no modes to
get stuck in.

## 2. Protocol: `81 6x` (unused by PS1 mode, MMCE and every Sony console)

Same bus, same speed and the same 128-byte blocks as the PS1 protocol, so the SS1's existing driver
(`card_sector.sv` / `phosphor_ext.sv`) only needs the new command byte.

| Bytes | Command | Answer |
|---|---|---|
| `81 60 'S' 'M' VER HOST CAPS` | **Hello**: the host speaks Super MemCard; who it is (`HOST`: Phosphor Main, Console Mode, a core's `sys/` driver) and what it can do (`CAPS`) | `'S' 'M'`, the card's version, its features, free space; a card without it answers nothing (the host then treats it as a plain PS1 card) |
| `81 61 SYS LEN ID…` | **Select** a save set: system (1 byte, table below) and game ID (`SLUS-00067`, `NSME-USA`, a CRC32 in hex…) | how many files, sizes, each file's generation number (§5) |
| `81 62 FILE MSB LSB` | **Read** block N (128 bytes) of file FILE of the selected set | `MSB LSB [128] CHK 47`, as a PS1 read |
| `81 63 FILE MSB LSB [128] CHK` | **Write** block N | `47` good / `4E` bad checksum / `FF` out of space |
| `81 64 FILE SIZE…` | **Create / resize** a file | `47` / `FF` |
| `81 65 FILE GEN` | **Commit**: this file is complete; stores its generation number | `47` once it's on the SD card |
| `81 66` | **Status**: blocks still waiting for the SD card | count (0 = safe to remove) |
| `81 67 SYS` | **List** the games of a system (one per call, paged) | game IDs, sizes, generations |

### Who sends Hello, and when

The **core** sends Hello the first time it polls the card: a custom game core through its `sys/` card driver
(phase 2), or the menu core on Main's behalf (phase 1, Phosphor's or Console Mode's). Its answer tells both
sides what they have:

| on the SS1 | the card sees | result |
|---|---|---|
| a custom (approved) core or menu core | Hello with its `HOST` and `CAPS` | the features that host is allowed (e.g. everything for a Console Mode approved core) |
| a stock core, the stock PSX core | no Hello, only PS1 commands | a plain PS1 card |
| a real PS1 / PS2 | no Hello | a plain PS1 card |

| the host gets | it knows |
|---|---|
| Hello answered `'S' 'M' …` | a Super MemCard card: version and features |
| no ACK on the command byte, twice | a stock card (sd2psXtd doesn't ACK `81 60`–`6F`, tested) or no card: PS1 only |

- **Ask twice.** The first command after the card powers up or switches card can go unanswered (08, finding 4),
  so one missing reply means "ask again"; only a second one means a plain card.
- **Features only add commands.** The card stays powered while the SS1 changes core, so features can still be on
  when a stock core loads after an approved one. That's harmless because no feature changes what the PS1
  commands do (§1); the card drops back to plain PS1 at power-off or reset (rule 2).
- Hello identifies the host; it isn't a lock (any core could send the same bytes). It picks features, it doesn't
  protect anything.

On the card's SD card (rule 3):

```
SuperMemCard/<SYSTEM>/<game ID>/<file>        SuperMemCard/N64/NSME-USA/save.eep, cpak1.cpk
SuperMemCard/<SYSTEM>/<game ID>/meta.ini      file names, sizes, generations, game name
```

Systems: one byte each from a table in the spec (SNES, N64, GBA, Genesis, Saturn, PSX (only for its
non-card saves), Arcade…), named after Main's core names.

The card firmware uses PS1 mode's design (02): the selected files are loaded into RAM, writes mark dirty
blocks, a background task writes them to the SD card, and a flush happens before any switch.

## 3. The host

### Phase 1: at game start and game end (no core changes)

Both frontends can reach the card from their menu core (04).

1. **Game start**, before the core loads: Hello → Select (system, game ID from Main's `/tmp/GAMEID`,
   or the same calculation done before loading) → if the card's generation is newer than the
   mirror's, read the files into `saves/<core>/` (the files Main mounts, 01).
2. The game plays exactly as today: Main writes the save to the SD card as the core saves.
3. **Game end**, back in the menu: if the save files changed, write them to the card → Commit →
   Status until 0 → update the mirror's generation.

If the SS1 loses power mid-game, the SD card already has the newest save (Main writes `O_SYNC`); it
goes to the card at the next game end. The cost is speed: ~20-25 KB/s, so a 32 KB save takes ~1.5 s
and a 128 KB one ~5 s, shown with a progress bar.

### Phase 2 (later): during the game

The card driver goes into `sys/` (hps_io), so a game core rebuilt with it lets Main reach the card
while the game runs. Main then sends each written save block to the card as it lands, and nothing
waits at game end. This needs the cores rebuilt (Phosphor's updater can do it); phase 1 keeps working
for cores that aren't.

The N64 Controller Pak on the card (SS1_N64_MiSTer plan) is phase 2's first user.

## 4. The card's menu

New items in the card's existing menu (03); everything else stays as it is.

```
Main Menu
├─ Boot Mode / PS1 Settings / PS2 Settings / Display / Info     (as now)
└─ SuperStation  >
   ├─ Saves on card       on / off        off: the card ignores Hello and is only a PS1/PS2 card
   ├─ Browse          >   system list → game list (name, ID) → its files, sizes, last written
   ├─ Space               used / free on the SD card
   └─ Safe to remove      "Yes" or "Saving N blocks"
```

On the **main screen**, while an SS1 is talking to the card:

```
┌────────────────────────────┐
│ SuperStation   ● saving    │   header: SuperStation; "● saving" while blocks are waiting
│ N64  NSME-USA              │   the system and game selected
│ Super Mario 64             │   from the game database (sd2psXtd's game_db) when it knows it
│ PS1 card: Card1 ch 1       │   the PS1 card is still there and still the one a PS1 sees
└────────────────────────────┘
```

When the card goes back to plain PS1 use (no SS1 traffic for a while, or a power cycle), the main screen
is the normal PS1 one.

Deleting saves is **not** in the card's menu: deletes happen on the SS1 (or a PC), where there's a
proper screen and a backup; the card only shows.

## 5. Keeping the copies straight

Each save set has a **generation number** on the card and in the mirror. The host compares them:

| Card | Mirror | What happens |
|---|---|---|
| newer | as last synced | the card's copy is used (the save came from another SS1 or was restored) |
| as last synced | newer | the mirror's goes to the card |
| both changed | | **keep both**: the frontend asks which to use, and the other is kept as a backup |
| missing | there | the mirror's goes to the card |
| there | missing | the card's is used (a fresh install: the saves come back from the card) |

The last row is what makes a reinstall safe.

## 6. To decide

- Which systems in the first version (suggest: N64, SNES, GBA, Genesis/Mega Drive, then the rest).
- Whether the game ID for cartridge systems is the CRC32 (always available) or a database serial.
- Phase 2: which cores to rebuild first.
