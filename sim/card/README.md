# Virtual card

Layer 1 of the virtual SS1 ([docs/07](../../docs/07-virtual-ss1.md)): the sd2psXtd firmware's own PS1 memory card
code, built for a PC and driven over the bus like a PS1 or the SS1 drives it. The firmware files are used as
they are. Only the RP2040 underneath them is replaced:

- **two cores**: threads. Core 0 runs `ps1_init()` and then `ps1_task()` in a loop, as `main.c` does; core 1 runs
  `ps1_memory_card_main()`;
- **PIO**: the CMD reader, DAT writer and controller reader are byte FIFOs. A byte the card queues is its reply
  to the next byte, and queueing it is its ACK, as with `dat_writer`;
- **SEL**: the bus calls the firmware's GPIO interrupt handler on each edge;
- **SD card**: a folder;
- **flash (settings)**: memory shared with the test program, so it outlives a power-off;
- **power**: every power-up is a new card process with a clean RP2040. The test program talks to it over a socket.
  A clean power-off goes through the firmware's exit path; `vcard_power_cut()` kills the process.

```
./get-firmware.sh      # sd2psXtd at the commit the docs were written against
make test              # or: make SAN=address && build-address/test_card
build/test_card "game ID"   # just the tests whose name contains this
```

## On the SS1

```
make ss1                                   # build-ss1/vcardd, build-ss1/test_card: armv7, static
test_card                                  # the same tests, on the SS1 (run from /tmp)
vcardd /tmp/supermemcard/sd [socket]       # a card that stays powered, on /tmp/supermemcard.sock
```

Until there's a real card, Phosphor's Main talks to `vcardd` instead of the front port. The socket carries
the same thing the bus does (select, one byte out, one byte and ACK back; `host/vcard_wire.h`), so only that link
changes when the card arrives. SIGTERM powers it off cleanly; its settings are kept in `<folder>/.vcard-flash`.

## What the tests cover

ID, read, long read, write, flush to SD and power-off, bad checksum, a write cut off, a power cut after a write,
MMCE ping, the game ID card switch (and with the setting off), the controller combo, that stock firmware doesn't
ACK `81 60`–`6F` (so the host can tell a plain card), and rule 1: a full PS1 session (read everything, write,
switch game, format, power-off) leaves `SuperMemCard/` byte for byte.

## Found in the firmware so far

(Also in [docs/08-findings.md](../../docs/08-findings.md).)

- **Writes past sector 3FF overwrite RAM** (builds without PSRAM): `ps1_mc_data_interface_write_byte()` writes to
  `cache[address]` with no bounds check, up to 8 MB past the 128 KB card image, and the card answers 47 (good)
  where a Sony card answers FF. Games don't send these; homebrew or a buggy host could. Kept as a known-bug test.
- **`game_db.c:189`** copies the ID number with `strlcpy(idString, split, 11)` into a 10-byte buffer. It overflows
  only for IDs with 10 or more digits. glibc's `_FORTIFY_SOURCE` aborts on it, so this build turns that off.
- **A bad-checksum write still lands in RAM**: the card answers 4E and doesn't write the SD card, but it copied
  each byte into its RAM image as the byte arrived. Reads return the rejected data until a good write or a
  power-off.
- **The first command after power-up or a card switch can go unanswered**: the card's loop waits for SEL to rise
  once, and after a switch whether it does depends on timing. A PS1 never notices because it polls the pad on the
  same select line. The Super MemCard host must ask again after one missing reply rather than decide there's no
  card.

The firmware is GPL-3.0; it's fetched, not copied here.
