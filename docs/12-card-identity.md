# The card's identity: only genuine Retro Remake cards

Super MemCard works only with a genuine Retro Remake card. Any other card, including one running the
same open firmware, is an sd2psXtd PS1 card to the SuperStation: smcd leaves it alone, and PS1 games
use it as always.

## How it works

Every genuine card has its own key pair, and a certificate: Retro Remake's signature of the card's
chip ID and public key. Retro Remake's signing key never leaves Retro Remake; smcd and the firmware
hold only its public key.

When smcd opens the card (at a game's start, back in the menu, in game, for the memory manager), after
Hello:

1. **CERT** (81 6B): the card answers its chip ID, its public key and its certificate. smcd checks
   Retro Remake signed that key for that chip.
2. **AUTH** (81 6C): smcd sends 32 random bytes. The card signs them (with its chip ID) after
   answering, on core 0, so the bus is never held up.
3. **AUTHSIG** (81 6D): the signature (status 50 while it's being made; smcd asks again). smcd checks
   it with the certificate's key.

Anything wrong and the card isn't genuine. smcd logs why, once per card, and the memory manager's
HELLO answers `notgenuine <card> <why>`.

| what someone tries | what happens |
|---|---|
| the open firmware on their own card | no identity: not genuine |
| an identity made with their own key | the card refuses it (renamed `smc_key.bad`), smcd too |
| a genuine card's identity file on another card | signed for another chip: refused |
| a genuine card's flash copied onto another card | the card checks the certificate against its own chip at every start: no identity |
| a genuine card's certificate replayed, without its key | can't sign the challenge: not genuine |

All of these are tests (`sim/card`: `test_card "SMC id"`; `smcd/test.sh`).

## At the factory

1. Flash the card's firmware (built with Retro Remake's public key, below).
2. Read the card's chip ID: `smccli -s <card> chip` (or CERT on any SuperStation).
3. `smccert issue <Retro Remake key> <chip ID> smc_key.bin` and copy it to the card's SD card.
4. Restart the card. It checks Retro Remake signed the file for its chip, keeps it in its own flash,
   and deletes the file. `smccli chip` says `genuine`.

This is how sd2psXtd already installs a PS2 card's key (`civ.bin` → flash, keystore.c).

## Retro Remake's key

`smccert key <file>` makes it (keep the file offline: whoever has it can make genuine cards). It
prints the public key; put it in both:

- `host/smc_rrkey.h` (smcd)
- `firmware/ps1/ps1_supermemcard_rrkey.h` (the card)

and set `SMC_RR_KEY_IS_TEST` to 0 in both. Until then they hold the **test key**
(`tools/smccert/test-rr.key`, in this repository, not secret), and smcd says in its log that it was
built with it.

## In the card

- `firmware/ps1/ps1_supermemcard_id.c` (GPL, with the rest of the firmware additions); signatures are
  EdDSA from [Monocypher](../third_party/monocypher) 4.0.2 (public domain).
- The identity lives in its own 4 KB of flash, just below sd2psXtd's splash screen's: nothing of
  sd2psXtd's moves, and updating the firmware keeps it.
- The chip ID is the RP2040's flash chip's unique ID (`pico_get_unique_board_id`).
- Hello's features gain bit 0x10 (`SMC_FEATURE_IDENTITY`). With "Saves on card" off in the card's
  menu, the card answers none of it, like stock firmware.

## Limits

An RP2040 can't hide its flash: someone who takes a genuine card apart and reads its flash chip has
its key, and can make that one card's twin (on a card with the same flash chip ID). It can't make
other cards, and Retro Remake can refuse a known copied identity in an smcd update. The RP2350 can
keep the key in its OTP memory, readable only by signed firmware (secure boot), which closes that; a
card built on it should use that.

To check on the real card: how long AUTH's signature takes on the RP2040 (smcd waits up to 3 s), and
that a new identity is taken at the first start.
