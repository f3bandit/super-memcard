// Super MemCard in a MiSTer Main: the OSD's Saves page. Copyright (C) 2026 f3bandit. GPL-3.0-or-later, as Main_MiSTer.
//
// While a game whose saves are on the card runs, the core's OSD page gets a "Saves" entry. It lists that game's saves,
// one per virtual card on the card, and a new one on the first free card. Picking one restarts the game with that
// save (smcd puts it in place first; this SuperStation's save goes to its card before).
//
// In Main's menu.cpp (the same as its Cheats page):
//   on the core's main OSD page, after its options:   if (!page && smc_osd_available()) { saves_row = selentry; MenuWrite(entry++, " Saves", menusub == selentry++, 0); ... }
//   on select of that row:                             if (smc_osd_open()) { menusub = 0; menustate = MENU_SMC_SAVES1; }
//   MENU_SMC_SAVES1:                                   smc_osd_print(); menustate = MENU_SMC_SAVES2;
//   MENU_SMC_SAVES2:                                   menu/back: the core's page again; else smc_osd_scroll();
//                                                      if (smc_osd_key(up, down, c, select)) menustate = MENU_SMC_SAVES1;
#pragma once

// a game with its saves on the card is running: the core's OSD page shows "Saves"
bool smc_osd_available();

// asks smcd for the game's saves: 1 to show the page (when something's wrong, the page says so instead of a list)
int smc_osd_open();

// draws the page (title, the game, its saves, the hint)
void smc_osd_print();

// from the page's loop (MENU_SMC_SAVES2, every pass): the selected row scrolls when it's longer than the OSD (its name
// and date), as Main's file names do
void smc_osd_scroll();

// a key on the page (up, down, the key code for page up/down/home/end, select): 1 when it needs drawing again.
// Select on a save asks first ("again: restart"), then restarts the game with it (Main starts again). Its messages are
// on the page's last row: Main's Info() pop-up doesn't show over a menu page.
int smc_osd_key(int up, int down, int key, int select);
