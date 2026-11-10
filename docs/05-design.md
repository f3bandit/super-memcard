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

Built and tested in the virtual card (`firmware/`, `sim/card`, version 2: version 1 had no Core and no virtual cards). Every frame is `81 6x`, the request
bytes, then the answer clocked out with `00`s. The card ACKs each request byte; the ACK of the last one comes once
the card has the answer (an SD access, so up to tens of milliseconds late: Super MemCard hosts wait for it).

| Cmd | Request | Answer |
|---|---|---|
| `60` **Hello** | `'S' 'M' ver host caps` | `'S' 'M' ver features id[8] free_kb[4]` (16) |
| `61` **Select** | `syslen sys idlen id card`: system = Main's save folder name (`SNES`, `TGFX16`), game ID = Main's save name (`Super Metroid (USA)`) or a serial, card = the system's virtual card 1-99 (0: its current one) | `status nfiles card` |
| `68` **Info** | `file` | `status name[16] size[4] gen[4] time[4]`; status `44` 'D': deleted at generation gen (a tombstone) |
| `62` **Read** | `file blk_hi blk_lo` | `data[128] chk end` (as a PS1 read; chk = XOR of blk bytes and data) |
| `64` **Create** | `size[4] namelen name` | `status file`: starts writing `<name>.new` |
| `63` **Write** | `file blk_hi blk_lo data[128] chk` | `end`: `47` good, `4E` bad checksum, `FF` failed |
| `65` **Commit** | `file gen[4] time[4]`: time = when the save was made (Unix, UTC: the save file's time on the SuperStation; the card has no clock) | `end`: `<name>.new` replaces `<name>`, generation saved |
| `66` **Status** | | `pending[2]` (0: every write is already on the SD card) |
| `67` **List** | `syslen sys card idx[2]` | `status id[64]`: the idx-th game the card has for that system on that virtual card (0: current); sys `.`: the idx-th system |
| `69` **Core** | `syslen sys flags` | `status card`: the system's current virtual card. Flags bit 0: this system's core is running now (the card shows it, its buttons pick this system's card); sys `.` with bit 0: none is. Without bit 0 it only asks |
| `6A` **Delete** | `file gen[4] time[4]` | `end`: the save is deleted at generation gen; the card keeps a tombstone (meta.ini `<file>=-<gen>`) and the file itself in `<game>/.deleted/` |

Status bytes: `47` good, `44` deleted, `4F` not found, `53` no game selected, `FF` failed. Before a Hello, `61`-`6A` aren't
ACKed (exactly like stock firmware). Names that could leave `SuperMemCard/` (`/`, `\`, `:`, `..`, leading or
trailing dots and spaces) are refused. A save only replaces the previous one at Commit, so a card pulled or powered
off mid-save keeps the old one whole (tested).

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
- **Why the core asks rather than the card announcing itself:** on the PS1 card bus the host clocks every byte
  and the card only answers the byte being clocked; it can't start a message. Every place the card could hide
  one (the FLAG byte, the ID reply `5A 5D 5C 5D 04 00 00 80`, an extra ACK after a command's last byte) is read
  by a real PS1, the stock PSX core and Console Mode's memory card manager, so it would break rule 1; and the
  card can't answer the address byte `81` itself, because it only knows the frame is for it once that byte has
  arrived. So the card's "message" is its answer to Hello.
- **Card ID, not a marker in the card image.** Block 0 frames 56-62 of a PS1 card are unused and a core could
  read them, but a marker there travels with the image (copied to a stock card, it still says "Super MemCard"),
  can be wiped by a PS1 format or a PC tool, and two cards copied from one image would share it. So the card's
  firmware says what it can do (Hello), and its ID comes in Hello's answer from the card's own SD card; nothing
  is ever written into a `.mcd`. Backups of stock cards keep matching by the saves on them (ps1_memcard_lib).
- Hello identifies the host; it isn't a lock (any core could send the same bytes). It picks features, it doesn't
  protect anything.

On the card's SD card (rule 3):

```
SuperMemCard/card.ini                                 id=<card ID>
SuperMemCard/<SYSTEM>/card.ini                        card=<N>: the system's current virtual card
SuperMemCard/<SYSTEM>/Card<N>/<game ID>/<file>        SuperMemCard/N64/Card1/NSME-USA/save.eep, cpak1.cpk
SuperMemCard/<SYSTEM>/Card<N>/<game ID>/meta.ini      file names and generations
SuperMemCard/<SYSTEM>/Card<N>/<game ID>/icon.bin      the game's save icon (below)
```

### Save icons

Each game on the card has an icon for a frontend's save list (Console Mode's system menu), like a PS1 save's:
`icon.bin` beside its saves, a normal file on the card that the sync never copies back as a save. For now it's the
system's own default icon (`icons/`, drawn by `tools/icons/make_icons.py`: original shapes and the system's
initials, no logos, no characters), put there the first time the game goes to the card; a game's own icon can
replace it later (any host can write `icon.bin`).

```
icon.bin   'S' 'M' 'I' 'C', version 1, frames (1-3), width 16, height 16,
           palette: 16 x u16 little-endian PS1 colour (r | g<<5 | b<<10, 5 bits each; colour 0 transparent),
           frames x 128 bytes: 4 bits a pixel, left pixel in the low nibble, rows top to bottom
```

The same pixel layout as a PS1 save icon, so a frontend that draws PS1 icons draws these with the same code.

### Virtual cards, and which core is running

Like PS1 mode's Card1, Card2..., every system has virtual cards (1-99): one set of saves per card, for two
players, or a second playthrough. The card can't know what an SS1 is running, so the core says it: **Core** with
the system and bit 0 set, when the game starts (phase 1: Main from the menu core, just before it loads the game
core; phase 2: the core's own card driver at its first poll). From then on the card's main screen shows
`SuperStation`, the system and its card number, and its four buttons step that system's card up or down (the PS1
card doesn't move). The choice is saved on the card 1.5 s after the last press, as PS1 mode waits before switching.

That system mode ends at a power-off, a Core `.`, or the first PS1 read or write (a PS1 game, the stock PSX core
or a memory card manager using the card): the buttons then pick the PS1 card exactly as before (rule 1).

The host keeps track of which virtual card its local saves came from. When the card says another one is in use
(the player pressed the buttons), the local saves first go to the old card, then are moved aside (not deleted:
`<state>/<card ID>/cards/<SYSTEM>/Card<old>/`) and the new card's saves come in. If the old card couldn't take
every save (no answer, or both sides changed), nothing moves until it can.

Systems are named, not numbered: Main's save folder names, which don't always match the core's name
(TurboGrafx16 saves to `TGFX16`, 08).

The card firmware uses PS1 mode's design (02): the selected files are loaded into RAM, writes mark dirty
blocks, a background task writes them to the SD card, and a flush happens before any switch.

## 3. The host

### Phase 1: at game start and game end (no core changes)

Like a PS1, the card is only read or written when a save is loaded or saved; nothing happens at menu start. The card
is reachable from the menu core only, so around a game (`host/smc_sync.h`):

1. **Game start**, before the core loads: Core (the system, running), then that one game (`smc_sync_game()`, the
   game from the MGL that starts it): the card's newer copy comes in ("load"). A reinstall gets each game's save
   back this way, the first time the game is started.
2. The game plays exactly as today: Main writes the save to the SD card as the core saves.
3. **Game end**, back in the menu: that game's save goes to the card if it changed ("save"), then Core `.` gives the
   card's buttons back to the PS1 card. A game started from the core's own file browser (no MGL): that system's saves
   that changed (`smc_sync_changed()`, a folder read; nothing sent when nothing changed).
4. **Read card** (Console Mode's memory manager, which has to read the card before its other options): everything
   (`smc_sync()`).

### Phase 2: during the game

The SuperStation One's front ports are SNAC, wired straight to the FPGA's user port, and in a game only the PSX core
uses them (Retro Remake's SuperStation documentation: the built-in ports work "only within the MiSTer core designed for
the original hardware"). So while any other core runs the bus is free, and Main can reach the card then too.

- **The card driver is in `sys_top`, not in the cores** (`core/sys/smc_sys.sv`, added to every core's `sys/` by
  `tools/sys-patch.sh` at build time). It's phosphor-menu's own front-port logic (`phosphor_ext`, copied, port don't
  rewrite) answering the same commands (`80`-`88`) on the UIO channel next to `sys_top`'s own; hps_io doesn't use
  those and the cores' own `EXT_BUS` users answer `34`/`35`. Its timings scale with the core's clock, measured against
  the board's 50 MHz clock, so the bus keeps a PS1's timing in every core (bus simulation at 33-100 MHz, and through
  `smc_sys` at 42.95 and 85.9 MHz). The user port stays the core's own until Main turns the front ports on for a
  frame, so a core that uses SNAC isn't disturbed; Main never does it in the PSX core.
- **Main** (phosphor-main `supermemcard_poll()`): while a game runs, when its save files change (Main wrote them as
  the core saved) and have settled for a second, that game's save goes to the card (`smc_sync_game()`), there and
  then. A core without the driver, or no card: nothing, and the save goes after the game as in phase 1.
- Phase 1 stays as it is around every game: the card's newer save in before the game, the save out after it, Core.

The N64 Controller Pak on the card (SS1_N64_MiSTer plan) is phase 2's first user that needs more than whole files.

## 4. The card's menu

New items in the card's existing menu (03); everything else stays as it is.

```
Main Menu
├─ Boot Mode / PS1 Settings / PS2 Settings / Display     (as now)
├─ SuperStation  >                                         built (firmware/patches, src/gui.c)
│  ├─ Browse        >  systems > a system's games on its current card, by official name (game.ini), else the
│  │                   header's name, else the game ID (up to 32 systems, 40 games a system)
│  ├─ Saves            Yes / No      No: the card doesn't even ACK 81 60, exactly a stock card (settings.ini)
│  ├─ Free             Count         press: counts the SD card's free space (a FAT scan, seconds on a big card,
│  │                                 so only when asked); Hello then reports it, less what was written since
│  └─ Safe to remove   Yes / Saving  "Saving" while an SS1 is writing saves (a command in the last 1.5 s)
└─ Info  >                                                 (as now)
```

Browse's pages are filled when they're opened (listing the card's folders, as List does).
While an SS1 writes saves the main screen's header says "SuperStation: saving", in the PS1 view too.

On the **main screen**, while an SS1 core has named its system (Core), built as:

```
┌────────────────────────────┐
│ SuperStation               │   header
│ Card                     2 │   the system's virtual card: the buttons step it (1-99)
│ Channel                  - │
│ SNES saves                 │   the system
└────────────────────────────┘
```

It uses the PS1 screen's own labels, so it looks like the PS1 screen with the card number. Later: the game selected
(from sd2psXtd's game_db where it knows it) and "● saving" while blocks are waiting.

When the system mode ends (a PS1 read or write, Core `.`, or a power cycle), the main screen is the normal PS1 one.

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

**Both changed** is never settled by guessing: the sync keeps both (the local file in place, the card's copy in
`<state>/<card ID>/conflicts/`) and reports it every time until the frontend settles it (`smc_conflicts()` lists
them, `smc_resolve()` keeps the SuperStation's copy, which goes to the card, or the card's, which replaces the local
file). The other copy is kept, renamed `.old`.

**Deleting** a save is something the frontend does (`smc_delete_save()`), never inferred from a missing file (a
missing file is what a reinstall looks like). The card's copy gets a tombstone (Delete), so every SuperStation that
syncs afterwards moves its own copy aside too, unless it was played there since: then the played save comes back.
Nothing is ever erased: the card keeps the file in `<game>/.deleted/`, each SuperStation in
`<state>/<card ID>/deleted/`.

## 6. To decide

- Which systems in the first version (suggest: N64, SNES, GBA, Genesis/Mega Drive, then the rest).
- Whether the game ID for cartridge systems is the CRC32 (always available) or a database serial.
- Phase 2: which cores to rebuild first.
