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

## To check on a real card

- Screen size and type, number of buttons, and whether the menu matches sd2psXtd's.
- Whether Retro Remake added their own pages or branding.
- How its firmware is updated (sd2psXtd: UF2 over USB in boot mode), since Super MemCard needs a new build.
