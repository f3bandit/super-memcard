// Super MemCard in a MiSTer Main: the shim between Main and smcd (the Super MemCard program, a separate program on the
// SuperStation). Copyright (C) 2026 f3bandit. GPL-3.0-or-later, as Main_MiSTer.
//
// Main only passes card frames to the front ports for smcd and tells it when a game starts and when the menu is back.
// Everything else (which save, the sync, game IDs, the memory manager's commands) is smcd's. No smcd running: nothing
// happens, and Main works as without Super MemCard.
//
// Sockets (text lines):
//   /tmp/smcd.sock           smcd listens. Main sends   LAUNCH <rbf>\t<mgl>   (waits for DONE, passing frames meanwhile)
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

// From Main's loop: passes smcd's card frames (also while a game runs, through the core's sys_top driver).
void smc_shim_poll();

// The OSD's Saves page (smc_osd.cpp): a game whose saves are on the card is running (smcd said so at its LAUNCH)
bool smc_shim_saves_available();
// a command to smcd and its answer (the lines before END) in out: its length, or -1 (no smcd, or no answer in 20 s).
// Passes smcd's card frames while it waits
int smc_shim_ask(const char *cmd, char *out, size_t n);
// starts the running game again from its MGL (or MRA), so its core loads the save smcd just put in place
void smc_shim_restart();
