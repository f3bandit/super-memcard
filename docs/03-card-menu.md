# How the card's on-screen menu works

From sd2psXtd's source (src/gui.c, src/ui_menu.c, src/input.c, src/config.h), GPL-3.0, commit 8a20bd9.
**Retro Remake's card may differ** (screen, buttons, menu items): see "To check on a real card".

## Hardware

- A 128 × 64 monochrome OLED over I²C (`DISPLAY_WIDTH 128`, `DISPLAY_HEIGHT 64`, src/oled.c), drawn
  with LVGL (`flush_cb`, gui.c:187), with optional splash images and per-game pictures.
- Two buttons, **left** and **right** (PMC boards have a third, *boot*). src/input.c turns them into keys:

| Key | How | Main screen | In the menu |
|---|---|---|---|
| PREV | short press left | previous channel | move up |
| NEXT | short press right | next channel | move down |
| BACK | hold left | previous card | back |
| ENTER | hold right | next card | choose |
| MENU | both together | open the menu | close the menu |

## Screens

**Main screen** (`create_main_screen`): the mode in the header ("PS1 Memory Card", "PS2 Memory Card",
"USB Mode"…), the card (`Card1`, `BOOT` or a game ID) and the channel, the game's name from the game
database when there's an ID, and `< Menu >` at the bottom. Card and channel changes from the buttons
are delayed 1.5 s (`ps1_mmce_*(true)`) so a few presses only switch once.

**Card switch screen** ("Loading card", with read speed) while a card is being flushed and opened.

**Menu** (`create_menu_screen`, gui.c:1022), pages of rows, each row a label and a value or `>`:

```
Main Menu
├─ Boot Mode        PS1 / PS2   (mode page: confirm, with the "Do not insert / It may damage" warning)
├─ PS1 Settings  >  Autoboot (boot card at power-up), Game ID (follow game IDs), Combos (controller combos)
├─ PS2 Settings  >  Variant (Retail / Proto / Arcade / Conquest), Autoboot, Game ID, Default size
├─ Display       >  Auto off, Contrast, VCOMH, Flip
└─ Info          >  Version, Commit, Debug, SD card MID / OID / date
```

Pages are built with `ui_menu_subpage_create()`, rows with `ui_menu_cont_create_nav()` +
`ui_label_create_grow()`, and a row opens a page with `ui_menu_set_load_page_event()`. Settings are
kept by src/settings.c (in the RP2040's flash, wear-levelled) and per card by `card.ini`
(src/card_config.c: channel count, names).

### With Super MemCard (firmware 3, on a BitFunX Gen2, Nov 12 2026)

One menu (f3bandit): the card's own settings as they are, a divider, then the SuperStation's:

```
Main Menu
├─ Boot Mode        PS1 / PS2
├─ PS1 Settings  >
├─ PS2 Settings  >
├─ Display       >
├─ Info          >
├─ --SuperStation--         (a divider: the cursor skips it)
├─ Saves on card >  the systems with saves > a system's games (its current card, by name) > that game's saves,
│                   each with the date and time it was made (each step a long press)
├─ Saving           On / Off   (Off: a plain PS1 card, it doesn't even answer 81 60)
├─ Clock            12h / 24h  (the times on the card's screens; 12h by default)
├─ Free             Count      (the SD card's free space)
└─ Safe to remove   Yes / Saving
```

- Games show by name only: the game list's name (`game.ini`, when the SuperStation has the game lists), else the
  save's own name (`name.txt`, which smcd writes with every set). Game IDs (CRCs, serials) never show on the card:
  they're for the SuperStation's side (game IDs, icons).
- Times are in the SuperStation's local time: smcd sends its UTC offset with the screen (`SMC_CORE_TZ`), the card
  keeps it (`settings.ini tz=`). Before the first one: UTC.
- `SuperMemCard/settings.ini`: `saves=0|1`, `clock=12|24`, `tz=<minutes>`.
- The menu's own scrolling counts the divider row (`evt_menu_page` scrolls to the row's index, not the count of
  items).

**Main screen in a system's mode** (a non-PSX core running): see 10, "The card's screen for the other systems".

**The log on the USB serial port.** With the card on a USB port (the SS1's or a PC's), `/dev/ttyACM*` carries a
`smc:` line for each Super MemCard command (its size, status and time), one line per file transferred (blocks,
bytes, B/s), and each mode and screen change. Only while something has the port open: with nothing reading, a full
USB buffer would hold the card up. `smcd/card_serial.py` keeps it in `/tmp/card_serial.log` with times.

## To check on a real card

- Screen size and type, number of buttons, and whether the menu matches sd2psXtd's.
- Whether Retro Remake added their own pages or branding.
- How its firmware is updated (sd2psXtd: UF2 over USB in boot mode), since Super MemCard needs a new build. From an
  SS1 with the card on its USB: open `/dev/ttyACM*` at 1200 baud (the Pico SDK's reset into the bootloader, no
  button), the `RPI-RP2` drive mounts as `/media/usbN`, copy the `.uf2` onto it (`smcd/flashcard.sh`).
