# How Super MemCard works with Console Mode

Console Mode is the target (rule 7). What it already does with cards (04) has to keep working
unchanged, and what Super MemCard needs from it is small.

## Nothing changes for what Console Mode does today

- **Media › Memory Cards** reads and writes front-port cards with `@memcard read/write`: those are
  PS1 commands, which always reach the PS1 card (05 §1). Same results as today.
- **The PSX core** uses its card images in `saves/PSX`, or a real card through the front port as a PS1 card.
- **A real PS1/PS2** with the card: unchanged, round trip included (rule 1).

## What Console Mode would add (Retro Remake's side)

1. **A setting**: *Settings › Saves on memory card: Off / Front port 1 / Front port 2* (off by default).
2. **Two moments in the game launch**, in Console Mode's host (`MiSTer_ConsoleMode`), around the
   existing launch path (exit code 10 / 11, see the RE notes):
   - before the core loads: Hello, Select, pull newer saves (05 §3, phase 1, step 1);
   - after the core exits, back in the menu core: push changed saves, Commit, wait for Status 0.
   Console Mode already does work at that moment: the backup module's `snap-…-exit-<core>` snapshot.
   Super MemCard runs after it, so the snapshot is always taken first.
3. **Game IDs**: Console Mode's host already has Main's `/tmp/GAMEID` writer; it writes only with
   `log_file_entry=1`, so Console Mode should turn that on (or call the same code itself).
4. **Screens**:
   - a progress bar while saves go to or come from the card;
   - the "both changed" question (05 §5);
   - after a fresh install: *"Your memory card has saves for 42 games. Put them back?"*
5. **One user of the bus at a time**: card jobs (Memory Cards screen, SS1 Tool, Super MemCard sync)
   take a lock, like `/tmp/ss1_bt.busy` for Bluetooth, so a sync and a backup never overlap.
   Controller reads pause during a card operation, as `phosphor_ext.sv` already does.

## What Retro Remake gets

- the protocol spec (05 §2);
- the card firmware changes (on sd2psXtd, GPL-3.0);
- the Main changes, built and tested in phosphor-main (Main_MiSTer-derived, so they apply to
  `MiSTer_ConsoleMode` nearly as they are);
- the `sys/` driver for phase 2.

Plus test results from an SS1 running Phosphor with a Retro Remake card.

## Open points for Taki

- Is the shipping card's firmware sd2psXtd-based, and can we have its source? It's GPL-3.0.
- Does Console Mode's menu core card driver match `phosphor_ext.sv`'s timing for software cards?
  Its card reads work today, so probably.
- Where in the launch path Console Mode wants the two hooks.
