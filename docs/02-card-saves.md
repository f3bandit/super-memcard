# How the Retro Remake memory card reads and writes saves

The SuperStation memory card is built on **SD2PSX** (Time Extension, Oct 2025), open-source firmware
for an RP2040 board that emulates PS1/PS2 cards from an SD card. This describes **sd2psXtd**
(github.com/sd2psXtd/firmware, GPL-3.0, commit 8a20bd9, Jul 8 2026), the maintained SD2PSX firmware.
**Retro Remake's shipping firmware hasn't been seen yet**: everything here has to be checked against
it (see "To check on a real card").

## The card files on its SD card

```
MemoryCards/PS1/Card1/Card1-1.mcd        card 1, channel 1 (each card has channels 1..N, set in card.ini)
MemoryCards/PS1/Card2/Card2-1.mcd
MemoryCards/PS1/BOOT/BootCard-1.mcd      the boot card (older firmware: BOOT/BootCard.mcd)
MemoryCards/PS1/SLUS-00067/SLUS-00067-1.mcd   a game's own card, picked by game ID
```

src/ps1/ps1_cardman.c builds these paths (lines 163-208). Each `.mcd` is a raw 128 KB PS1 card.

## Talking to the PS1 (or the SS1)

The card answers the standard PS1 memory card protocol on the controller port bus (250 kHz, the card
selected with the port's select line). src/ps1/ps1_memory_card.c, `mc_main_loop()`:

| Bytes from the console | What | Code |
|---|---|---|
| `81 52 00 00 MSB LSB …` | **R**: read one 128-byte sector (0-1023), checksum, `47` "G" | `mc_cmd_read(false)` |
| `81 42 …` | **B**: the same, but keeps reading the following sectors | `mc_cmd_read(true)` |
| `81 57 00 00 MSB LSB [128 bytes] CHK …` | **W**: write one sector; answers `47` good / `4E` bad checksum | `mc_cmd_write()` |
| `81 53 …` | **S**: card ID (`5A 5D 5C 5D 04 00 00 80`) | `mc_cmd_get_card_id()` |
| `81 20 …` | MMCE ping (answers `27`) | `mc_mmce_ping()` |
| `81 21 00 LEN [ID…]` | MMCE **set game ID** (e.g. `SLUS-00067`) | `mc_mmce_set_game_id()` |
| `81 22` / `81 23` | previous / next channel | |
| `81 24` / `81 25` | previous / next card | |
| `81 27` | reset to the default card | |

The `0x2x` commands are the MemCard PRO / MMCE game ID protocol that ODEs and BIOS patches use; a
real PS1 never sends them. The firmware can also watch the controller on the same bus for button
combos (`mc_read_controller()`: hold L1+L2+R1+R2, then Up/Down switch card, Left/Right channel, Select the boot card).

## Where the data goes

1. The whole `.mcd` is loaded into the board's RAM (or PSRAM) when a card is opened
   (`ps1_cardman_open()`, `ps1_mc_data_interface`).
2. A **read** is answered from RAM, as fast as the PS1 asks.
3. A **write** goes into RAM and the sector is marked dirty (`ps1_dirty_mark()`).
4. A background task (`ps1_dirty_task()`, src/ps1/ps1_dirty.c) writes dirty sectors to the `.mcd`
   on the SD card, up to ~100 ms of work at a time, after a short lockout so a burst of writes from the
   game finishes first.
5. **Switching card** (game ID, buttons, menu): the card leaves the bus, flushes every dirty sector,
   closes the file, waits 250 ms (`CARD_SWITCH_DELAY_MS`, so the console sees a card change), opens
   the new one and comes back (`ps1_mmce_task()`, src/ps1/ps1_mmce.c).

So the SD card is never more than a moment behind the console, and a card switch never loses a write.

## What Super MemCard adds (see 05)

A third kind of storage next to `MemoryCards/PS1` and `MemoryCards/PS2`:

```
SuperMemCard/<system>/<game ID>/<save files>     e.g. SuperMemCard/N64/NSME-USA/save.eep, cpak1.cpk
```

reached only through new command bytes after a handshake (rule 2), using the same RAM-plus-dirty-sector
design as PS1 mode so the SD card wear and timing stay the same.

## To check on a real card

- That the shipping card runs SD2PSX / sd2psXtd (or a fork), and which version; get its source (GPL-3.0).
- Its SD card layout (same `MemoryCards/PS1/...` folders?).
- Whether it answers the `81 2x` game ID commands, and its byte timing (SS1_MemCard_MiSTer found
  software cards need ~30 µs between bytes and ACKs up to ~3 ms late).
