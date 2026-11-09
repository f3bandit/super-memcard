# Rules

These hold for every part of Super MemCard. A design that breaks one of them is wrong.

1. **The card is always a normal PS1 memory card first, and survives the round trip.** It powers up
   as a plain PS1 card and answers a real PS1, a PS2, Console Mode, Phosphor and the PSX core exactly
   as it does today. A user can take it from the SS1 to a real PlayStation and back, any number of
   times, and nothing breaks in either direction:
   - a real PS1 only ever reaches the PS1 card file (`.mcd`); even formatting the card in a PS1 only
     clears that PS1 card. The other systems' saves are outside it and PS1 mode has no command that
     reaches them (rule 3);
   - the card keeps **no Super MemCard state** that a PS1 could change: every SS1 session sends the
     handshake and sets everything it needs again, so a different card or channel picked in the PS1
     (by its buttons or an ODE's game ID) can't upset the SS1;
   - it always powers up in plain PS1 mode, so a card pulled out in the middle of an SS1 session works
     normally in the PS1;
   - writes go to the card's SD card a sector at a time and are flushed before any switch; on the SS1
     the card's screen shows "saving, don't remove" while writes are waiting. A card pulled at the worst
     moment loses at most the last unwritten sector of one save, and the host's mirror still has it
     (rule 4).
2. **Super MemCard mode only starts on a handshake from a SuperStation host**, and only until the card
   is powered off or reset. A real PS1/PS2 never sends that handshake (it uses command bytes no Sony
   console or game sends), so it can never switch the card by accident.
3. **Other systems' saves are invisible in PS1 mode.** They live in their own folder on the card's
   SD card (`SuperMemCard/`), never inside a `.mcd`, so PS1 mode can't show, use or damage them.
4. **The card never holds the only copy.** The host keeps a mirror (SS1 SD card, NVMe or NAS).
   Losing or breaking the card is never worse than a Console Mode reinstall is today.
5. **Port, don't rewrite.** The SS1's card driver (`card_sector.sv`, `phosphor_ext.sv`), Main's card
   commands (phosphor-main `PH_CARD`), phosphord's card jobs and SS1 Tool's card code already exist
   and are tested on the SS1: they are ported, never written again.
6. **No BIOS patches.** Game IDs come from the host (Main's `/tmp/GAMEID`, see 01), never from
   patched system BIOSes.
7. **Console Mode is the target; Phosphor is the test bed.** Everything is built and tested on
   Phosphor (open source: phosphor-main, phosphor-menu) and handed to Retro Remake as a set.
