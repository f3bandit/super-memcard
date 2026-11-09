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
| `68` **Info** | `file` | `status name[16] size[4] gen[4]` |
| `62` **Read** | `file blk_hi blk_lo` | `data[128] chk end` (as a PS1 read; chk = XOR of blk bytes and data) |
| `64` **Create** | `size[4] namelen name` | `status file`: starts writing `<name>.new` |
| `63` **Write** | `file blk_hi blk_lo data[128] chk` | `end`: `47` good, `4E` bad checksum, `FF` failed |
| `65` **Commit** | `file gen[4]` | `end`: `<name>.new` replaces `<name>`, generation saved |
| `66` **Status** | | `pending[2]` (0: every write is already on the SD card) |
| `67` **List** | `syslen sys card idx[2]` | `status id[64]`: the idx-th game the card has for that system on that virtual card (0: current); sys `.`: the idx-th system |
| `69` **Core** | `syslen sys flags` | `status card`: the system's current virtual card. Flags bit 0: this system's core is running now (the card shows it, its buttons pick this system's card); sys `.` with bit 0: none is. Without bit 0 it only asks |

Status bytes: `47` good, `4F` not found, `53` no game selected, `FF` failed. Before a Hello, `61`-`69` aren't
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
```

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

Both frontends can reach the card from their menu core (04).

1. **Game start**, before the core loads: Hello → Core (the system, running) → Select (system, game ID from Main's `/tmp/GAMEID`,
   or the same calculation done before loading) → if the card's generation is newer than the
   mirror's, read the files into `saves/<core>/` (the files Main mounts, 01).
2. The game plays exactly as today: Main writes the save to the SD card as the core saves.
3. **Game end**, back in the menu: if the save files changed, write them to the card → Commit →
   Status until 0 → update the mirror's generation.

If the SS1 loses power mid-game, the SD card already has the newest save (Main writes `O_SYNC`); it
goes to the card at the next game end. The cost is speed: ~11 KB/s measured on the SS1 (08), so a 32 KB save takes ~3 s
and a 128 KB one ~11 s, shown with a progress bar.

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

## 6. To decide

- Which systems in the first version (suggest: N64, SNES, GBA, Genesis/Mega Drive, then the rest).
- Whether the game ID for cartridge systems is the CRC32 (always available) or a database serial.
- Phase 2: which cores to rebuild first.
