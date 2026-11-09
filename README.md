# Super MemCard

Saves from every SuperStation One core on the Retro Remake SD memory card in a front port, so they
live on the card instead of the SS1's SD card and survive a Console Mode reinstall or a new SD card.
The card stays a normal PS1 memory card on a real PS1/PS2, in Console Mode and in the PSX core.

Target: Console Mode. Developed and tested on Phosphor first (its Main is an open fork of Main_MiSTer),
then handed to Retro Remake as a set: protocol spec, `sys/` and Main patches, card firmware changes.

## Documents

| | |
|---|---|
| [docs/00-rules.md](docs/00-rules.md) | the rules every part follows (the card stays a normal PS1 card, round trip included) |
| [docs/01-how-cores-save.md](docs/01-how-cores-save.md) | how MiSTer, Console Mode and Phosphor cores read and write saves |
| [docs/02-card-saves.md](docs/02-card-saves.md) | how the Retro Remake card (SD2PSX) reads and writes saves |
| [docs/03-card-menu.md](docs/03-card-menu.md) | how the card's on-screen menu works |
| [docs/04-memory-card-managers.md](docs/04-memory-card-managers.md) | Console Mode's memory card manager, Phosphor's and SS1 Tool's |
| [docs/05-design.md](docs/05-design.md) | Super MemCard: the protocol, the host, the card's menu |
| [docs/06-console-mode.md](docs/06-console-mode.md) | how it works with Console Mode |
| [docs/07-virtual-ss1.md](docs/07-virtual-ss1.md) | testing without an SS1: virtual card, bus simulation, virtual Main |
| [sim/card](sim/card/README.md) | the virtual card (layer 1, working): sd2psXtd's PS1 code on a PC, with tests |

## How it works

1. **Card firmware** (Retro Remake's card is built on the open-source SD2PSX, GPL-3.0): plain PS1
   mode by default. An extra command set, next to the existing game ID / per-game card commands and
   the MMCE file protocol: *select system + game*, then *read / write that save*. The card keeps one
   file per system and game on its SD card, so there's no 15-block limit.
2. **Game ID from the host, no BIOS patches**: Main knows exactly which file it loaded (path / hash),
   and the PSX core already reads the disc ID. The host tells the card "system N64, game NUS-NSME-USA".
3. **One change for all cores**: every MiSTer core shares the `sys/` framework, and save reads/writes
   already go through Main (`hps_io`). So: the card driver goes into `sys/` once (`card_sector.sv`
   from SS1_MemCard_MiSTer), Main sends save I/O to the card, and cores are rebuilt with the new `sys/`.
4. **Speed**: the PS1 card bus does ~20-25 KB/s (a 128 KB save takes ~5 s), so the host keeps the save
   in memory and writes changes back to the card in the background.
5. **Safety**: the card is where saves live, so a mirror is kept (SS1 SD card / NVMe / NAS); losing
   the card is never worse than a reinstall is today.

## Plan

1. Card side: test the protocol on the bench (stock SD2PSX or the MC/PS MemCard Pro board).
2. Phosphor prototype: `sys/` card driver + Main save routing + game IDs; rebuild PSX, N64 and one
   cartridge core (e.g. SNES) and test on the SS1.
3. N64 first use: the Controller Pak lives on the card (see SS1_N64_MiSTer plan).
4. Hand-off for Console Mode: the spec, the `sys/` and Main patches (MiSTer_ConsoleMode derives from
   Main_MiSTer), the card firmware changes; the Console Mode side (setting + card status screen) is
   Retro Remake's.

## Open questions

- Confirm the shipping Retro Remake card is still SD2PSX-based and get its firmware source.
- Licence (the card firmware and MiSTer parts are GPL-3.0; f3bandit's own code TBD).

By f3bandit.
