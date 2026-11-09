# Testing without an SS1: the virtual SS1

A whole emulated SuperStation isn't practical: nothing emulates the Cyclone V FPGA, and simulating a real
core (N64, PSX) runs at seconds per frame. Super MemCard only touches four parts, and each can run on a PC
or in GitHub Actions on every push. The real SS1 stays the final check, not the test bed.

```
 virtual Main (phosphor-main, x86)  ──PH_CARD──▶  bus simulation (Verilog)  ──SIO bytes──▶  virtual card (sd2psXtd, x86)
   ├ fake core: hps_io save blocks                phosphor_ext.sv                            SD card = a folder
   └ real save files in a temp saves/            card_sector.sv                              MemoryCards/PS1, SuperMemCard/
```

## 1. Virtual card

sd2psXtd's PS1 code is plain C: `ps1_memory_card.c` (protocol), `ps1_cardman.c` (card files), `ps1_dirty.c`
(SD write-back), `ps1_mmce.c` (game ID / card switching). Built for Linux with two stand-ins:

- **SD card**: a folder (`sd_*` calls go to files);
- **bus**: the PIO byte receive/send becomes a byte stream (a socket or a function call from the simulation),
  with the card's select and ACK as flags.

It runs the card's real logic, not a model of it. Tests:

- every PS1 command (`R`, `W`, `S`, `B`) against a known `.mcd`;
- **the round trip (rule 1)**: replay a recorded real-PS1 session (boot, read the directory, save, format)
  against a card holding Super MemCard data, then check the `SuperMemCard/` folder is byte for byte unchanged
  and an SS1 session still works;
- our `81 6x` commands: Hello, Select, Read, Write, Create, Commit, Status, List; bad checksums, out of space,
  a card pulled mid-write (stop the stream), power-up state;
- that a card without Super MemCard (stock firmware) ignores `81 6x`, so the host falls back to PS1-only.

## 2. Bus simulation

`phosphor_ext.sv` and `card_sector.sv` (phosphor-menu) in a Verilog simulator (iverilog, as
SS1_MemCard_MiSTer's `sim/run.sh` already does; Verilator if it's too slow), with the virtual card on the
other end through VPI/DPI. Tests the timing that matters for software cards:

- 250 kHz clock, 30 µs between a card's bytes, ACKs up to 3 ms late, no ACK (no card);
- controllers aren't polled while a card operation is on the bus;
- the new `81 6x` frames go through the same driver unchanged.

## 3. Virtual Main

phosphor-main built for x86 with its FPGA layer swapped out:

- `fpga_io` / `spi_uio_*`: a fake that sends `PH_CARD` / `PH_CARD_STATUS` to the bus simulation (or, for
  fast tests, straight to the virtual card);
- **a fake core**: a small program that loads a "game", asks for its save, and reads and writes 512-byte
  blocks the way `hps_io` does, so Main's real mount and block-write paths (01) run;
- a temporary `saves/` folder and `/tmp/GAMEID`.

Tests the parts where a bug costs saves:

- game start: the card's newer save comes in; the mirror's newer save is kept;
- game end: changed saves go to the card, Commit, Status 0;
- the generation table (05 §5), including "both changed: keep both";
- a fresh install (empty `saves/`): every save comes back from the card;
- power lost mid-game: the SD file is current and goes to the card next time.

## 4. Frontend on a PC (optional, later)

Phosphor's frontend is LVGL, which has an SDL driver, so it could open in a window on a PC for the Super
MemCard screens (progress, "both changed", restore after install). There's no PC build yet and the first
version doesn't need one. Console Mode's frontend can't run this way: it's a closed ARM binary using
SS1-specific devices.

## In CI

Layers 1-3 run in GitHub Actions on every push, like SS1_MemCard_MiSTer's simulation job. A failing
round-trip test blocks the push.

## Only the real SS1 can test

- real cores saving through Main;
- the real Retro Remake card and its firmware (02/03 checklists);
- the real bus with controllers plugged in, and how it all feels on the TV.
