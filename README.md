# Super MemCard

Game saves from MiSTer console cores on the SuperStation One, kept on the Retro Remake SD memory card in
a front port as a redundant backup, so they survive a Console Mode reinstall or a new SD card:

- **phase 1, any core**: the host (Main) copies a game's save to and from the card at game start and
  end. Cores are unchanged; only Main changes (the test Main's, then Console Mode's);
- **phase 2, custom MiSTer/SS1 cores**: cores rebuilt with a card driver in `sys/` save to the card
  while the game runs (the N64 Controller Pak on the card is the first one).
The card stays a normal PS1 memory card on a real PS1/PS2, in Console Mode and in the PSX core.

Target: Console Mode. Developed and tested on a test Main first (a fork of Main_MiSTer),
then handed to Retro Remake as a set: protocol spec, `sys/` and Main patches, card firmware changes.

## Documents

| | |
|---|---|
| [docs/00-rules.md](docs/00-rules.md) | the rules every part follows (the card stays a normal PS1 card, round trip included) |
| [docs/01-how-cores-save.md](docs/01-how-cores-save.md) | how MiSTer and Console Mode cores read and write saves |
| [docs/02-card-saves.md](docs/02-card-saves.md) | how the Retro Remake card (SD2PSX) reads and writes saves |
| [docs/03-card-menu.md](docs/03-card-menu.md) | how the card's on-screen menu works |
| [docs/04-memory-card-managers.md](docs/04-memory-card-managers.md) | Console Mode's memory card manager, f3bandit's card tools and SS1 Tool's |
| [docs/05-design.md](docs/05-design.md) | Super MemCard: the protocol, the host, the card's menu |
| [docs/06-console-mode.md](docs/06-console-mode.md) | how it works with Console Mode |
| [docs/07-virtual-ss1.md](docs/07-virtual-ss1.md) | testing without an SS1: virtual card, bus simulation, virtual Main |
| [docs/08-findings.md](docs/08-findings.md) | findings log: bugs found and fixed, facts learned along the way |
| [docs/09-game-database.md](docs/09-game-database.md) | which game a save belongs to: game IDs, official names |
| [docs/10-console-mode-guide.md](docs/10-console-mode-guide.md) | the guide for Retro Remake: everything Console Mode adds, in one place |
| [docs/12-card-identity.md](docs/12-card-identity.md) | only genuine Retro Remake cards: the card's identity, issuing it at the factory |
| [tools/smccli](tools/smccli/smccli.c) | each frontend call from a shell, against the simulated card (testing, and a reference) |
| [cores/](cores/README.md) | the 15 console cores we build against, pinned to MiSTer-devel and watched for changes |
| [sim/card](sim/card/README.md) | the virtual card (layer 1, working): sd2psXtd's PS1 code on a PC, with tests |

## How it works

1. **Card firmware** (Retro Remake's card is built on the open-source SD2PSX, GPL-3.0): plain PS1
   mode by default. An extra command set, next to the existing game ID / per-game card commands and
   the MMCE file protocol: *select system + game*, then *read / write that save*. The card keeps one
   file per system and game on its SD card, so there's no 15-block limit.
2. **Game ID from the host, no BIOS patches**: Main knows exactly which file it loaded (path / hash),
   and the PSX core already reads the disc ID. The host tells the card "system N64, game NUS-NSME-USA".
3. **Main first, then `sys/`**: every core's save reads and writes already go through Main (`hps_io`),
   so phase 1 is a change to Main alone and works with stock cores. For phase 2, every MiSTer core
   shares the `sys/` framework: the card driver goes into `sys/` once (`smc_card_sector.sv` from
   SS1_MemCard_MiSTer) and the custom cores are rebuilt with it.
4. **Speed**: measured on the SS1's front ports, ~11 KB/s of save data (a 140-byte frame per 128 bytes, ~11 ms
   each): an 8 KB save ~0.7 s, a 128 KB one ~11 s. So the host keeps the save in memory and writes changes
   back to the card in the background.
5. **Safety**: the card is where saves live, so a mirror is kept (SS1 SD card / NVMe / NAS); losing
   the card is never worse than a reinstall is today.

## Plan

1. Card side: test the protocol on the bench (stock SD2PSX or the MC/PS MemCard Pro board).
2. Prototype on the test Main: `sys/` card driver + Main save routing + game IDs; rebuild PSX, N64 and one
   cartridge core (e.g. SNES) and test on the SS1.
3. N64 first use: the Controller Pak lives on the card (see SS1_N64_MiSTer plan).
4. Hand-off for Console Mode: the spec, the `sys/` and Main patches (MiSTer_ConsoleMode derives from
   Main_MiSTer), the card firmware changes; the Console Mode side (setting + card status screen) is
   Retro Remake's.

## Open questions

- Confirm the shipping Retro Remake card is still SD2PSX-based and get its firmware source.
- Licence (the card firmware and MiSTer parts are GPL-3.0; f3bandit's own code TBD).

By f3bandit.

The cores are built in the public repo [f3bandit/super-memcard-cores](https://github.com/f3bandit/super-memcard-cores)
(free Actions minutes): the pinned cores, the card driver for `sys/` and the build workflow. `core/sys/` here is
the same driver, for the bus simulation; a change to it goes to both.

Licence: closed, all rights reserved, licensed exclusively to Retro Remake, except the GPL parts
(card firmware additions, core driver, Main front-port code): see [LICENSE](LICENSE) and
[docs/11-licensing.md](docs/11-licensing.md).
