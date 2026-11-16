// Super MemCard in a MiSTer Main: the shim between Main and smcd (the Super MemCard program, a separate program on the
// SuperStation). Copyright (C) 2026 f3bandit. GPL-3.0-or-later, as Main_MiSTer.
//
// Main only passes card frames to the front ports for smcd and tells it when a game starts and when the menu is back.
// Everything else (which save, the sync, game IDs, the memory manager's commands) is smcd's. No smcd running: nothing
// happens, and Main works as without Super MemCard.
//
// Sockets (text lines):
//   /tmp/smcd.sock           smcd listens. Main sends   LAUNCH <rbf>\t<mgl>   (waits for DONE, passing frames meanwhile)
//                                                       ROM <rbf>\t<file>\t<driver>  (waits: DONE same|bounce|saves)
//                                                       MENU
//                                                       (the OSD Saves page: SAVES, USE <card>, answers end with END)
//   /tmp/smc_frontport.sock  Main listens. smcd sends   FRAME <port> <speed> <ack ms> <hex>  ->  OK <count> <hex> | ERR <code>
//                                                       HOLD <0|1>                           ->  OK
#pragma once
#include <stddef.h>

// Before a game core is loaded (in fpga_load_rbf(), when the core isn't the menu): smcd loads that game's save from the
// card. Waits for smcd (at most 30 s), passing its card frames meanwhile.
void smc_shim_core_start(const char *rbf, const char *mgl);

// Main has started with the menu core (after user_io_init()): smcd saves the game just played to the card.
void smc_shim_menu_start();
// In place of smc_shim_menu_start(), with Main's arguments (the core and MGL it started with): the menu as above; a
// core Main started straight into without its fpga_load_rbf() telling smcd (a core started from outside): LAUNCH then.
void smc_shim_main_start(const char *rbf, const char *mgl);

// A game file picked in the running core's OSD (menu.cpp, where the file browser hands Main the file: type 'f' a file
// sent to the core, 's' a mounted image, index the core's slot), before Main loads it. A core with the card driver:
// smcd puts the old game's save on the card and the new one's in place, then true (Main loads the file). A core
// without it: Main goes through the menu core (which reaches the card) and starts the core again with the file (an
// MGL): the menu core is loading, false. The same game again, no smcd, or no game core: true at once.
bool smc_shim_rom_load(const char *path, char type, int index);

// From Main's loop: passes smcd's card frames (also while a game runs, through the core's sys_top driver).
void smc_shim_poll();

// The OSD's Saves page (smc_osd.cpp): a game whose saves are on the card is running (smcd said so at its LAUNCH)
bool smc_shim_saves_available();
// smcd is passing card frames through Main now (Main's loop doesn't wait on its input poll meanwhile)
bool smc_shim_busy();
// a command to smcd and its answer (the lines before END) in out: its length, or -1 (no smcd, or no answer in time).
// Passes smcd's card frames while it waits
int smc_shim_ask(const char *cmd, char *out, size_t n, int timeout_ms = 20000);
// starts the running game again from its MGL (or MRA), so its core loads the save smcd just put in place
void smc_shim_restart();
