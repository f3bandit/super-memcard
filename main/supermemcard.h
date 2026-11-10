// Super MemCard in a MiSTer Main (supermemcard.cpp). Copyright (C) 2026 f3bandit.
#pragma once
// Back in the menu core after a game: that game's save goes to the Super MemCard card if it changed
// (lib/supermemcard/smc_sync.c). Does nothing without a card, or when no game was played.
void supermemcard_menu_start();
// Before a game core is loaded from the menu core: tells the card which system is about to run (Core), so its
// screen shows that system's virtual card and its buttons pick another. Does nothing without a card.
// mgl: the MGL it's started from, if any (that game's save is loaded from the card first)
void supermemcard_core_start(const char *rbf, const char *mgl);
// While a game core runs (phase 2): when the game's save changes (Main wrote it as the core saved) and has settled, it
// goes to the card right away, through the game core's own sys_top (super-memcard core/sys). Cores without it, the PSX
// core (it uses the front ports itself) and no card: nothing; the save goes back in the menu as in phase 1.
void supermemcard_poll();
