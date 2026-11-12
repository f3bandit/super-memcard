// Super MemCard in a MiSTer Main: the reference integration (https://github.com/f3bandit/super-memcard, docs/05-design.md
// section 3, docs/10-console-mode-guide.md). Built and tested in a fork of Main_MiSTer on an SS1.
// Copyright (C) 2026 f3bandit.
//
// Like a PS1, the card is only used when a save is loaded or saved. Around a game: just before its core loads, its
// save is loaded from the card if the card's copy is newer (supermemcard_core_start); back in the menu, its save goes
// to the card if it changed (supermemcard_menu_start). During the game (phase 2), in a core whose sys_top carries the
// front-port driver (core/sys), its save goes to the card as soon as the game saves (supermemcard_poll). Main writes
// the game's save to the SD card as the core saves, as always.
//
// The card: the virtual card (vcardd) on /tmp/supermemcard.sock when it's running (testing), else the card in front
// port 1 or 2 through the front-port driver's raw frames (smc_frontport.cpp): the first port whose card answers Hello.
// A plain PS1 card doesn't answer it and is left alone. No card: nothing happens.
#include "supermemcard.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "file_io.h"
#include "hardware.h"
#include "user_io.h"
#include "smc_frontport.h"

extern "C" {
#include "smc_sync.h"
#include "smc_vcard.h"
}

#ifndef SMC_STATE_DIR
#define SMC_STATE_DIR "supermemcard"
#endif
#ifndef SMC_GAMELISTS_DIR
#define SMC_GAMELISTS_DIR "supermemcard/gamelists"   // libretro's lists, fetched there (super-memcard tools/gamedb)
#endif

static FILE *smc_log_file;
static void smc_log(const char *line)
{
	printf("SuperMemCard: %s\n", line);
	if (smc_log_file)
	{
		time_t t = time(0);
		char ts[32];
		strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
		fprintf(smc_log_file, "%s %s\n", ts, line);
		fflush(smc_log_file);
	}
}

// Super MemCard frames faster than a PS1 clocks the card (each byte is still ACK-paced, so the card sets the pace);
// Hello always at PS1 speed: it's what a plain card hears. A fast frame that isn't answered whole is sent again one
// speed lower, which then stays for the rest of the session (on the SS1 a Sony card read cleanly at 500 kHz and lost
// ~3 frames in 100 at 1 MHz, never with wrong data: super-memcard 08; a BitFunX Gen2 at 500 kHz did corrupt bytes in
// whole frames, so the identity's frames, which have no checksum, go at PS1 speed too, and a save frame that fails its
// checksum steps down as well). SMC_SPEED (0..3) is the speed tried first.
#ifndef SMC_SPEED
#define SMC_SPEED 2
#endif
static int frame_speed = SMC_SPEED;

// the front ports' raw frames as the library's whole-frame transport; ctx: the port number
static int front_frame(void *ctx, const uint8_t *out, uint8_t *in, int len)
{
	int port = *(int *)ctx;
	int speed = (len > 1 && (out[1] == 0x60 || (out[1] >= 0x6B && out[1] <= 0x6D))) ? 0 : frame_speed;   // Hello, CERT/AUTH/AUTHSIG
	int n = smc_frontport_raw(port, out, in, len, SMC_ACK_TIMEOUT_MS, speed);
	while (speed && n >= 0 && n < len)
	{
		frame_speed = --speed;
		static const char *const names[] = { "250 kHz", "500 kHz", "1 MHz" };
		char l[80];
		snprintf(l, sizeof(l), "a fast frame wasn't answered whole: %s from now on", names[speed]);
		smc_log(l);
		usleep(60000);   // a card that missed a frame answers the next one after a moment (08)
		n = smc_frontport_raw(port, out, in, len, SMC_ACK_TIMEOUT_MS, speed);
	}
	return n;
}

// a whole frame whose data failed its checksum: one speed down from now on
static void front_bad_data(void *)
{
	if (frame_speed <= 0) return;
	frame_speed--;
	static const char *const names[] = { "250 kHz", "500 kHz", "1 MHz" };
	char l[80];
	snprintf(l, sizeof(l), "a fast frame's data failed its checksum: %s from now on", names[frame_speed]);
	smc_log(l);
	usleep(60000);
}

// The card: vcardd when it runs, else the first front port whose card answers Hello. Leaves the ports held
// on (front ports) until smc_close(). 1: a card, 0: none.
struct smc_conn { struct smc_bus bus; int port, vcard; };
static int smc_open(struct smc_conn *c)
{
	struct stat st;
	static int port;
	memset(c, 0, sizeof(*c));
	if (stat(SMC_VCARD_SOCKET, &st) == 0 && smc_vcard_open(SMC_VCARD_SOCKET, &c->bus) == 0)
	{
		c->vcard = 1;
		return smc_hello(&c->bus, SMC_HOST_MAIN, 0, NULL) == 1 ? 1 : (smc_vcard_close(&c->bus), 0);
	}
	port = 0;
	if (smc_frontport_raw(1, NULL, NULL, 0, 0, 0) < 0) return 0;    // no raw frames in this menu core
	smc_frontport_hold(1);
	c->bus.frame = front_frame;
	c->bus.bad_data = front_bad_data;
	for (int p = 1; p <= 2 && !port; p++)
	{
		int q = p;
		c->bus.ctx = &q;
		if (smc_hello(&c->bus, SMC_HOST_MAIN, 0, NULL) == 1) port = p;
	}
	if (!port)
	{
		// no Super MemCard card (a plain PS1 card, or none). A card that refused Hello can miss the
		// next frame for a moment: leave it alone a little before anyone else talks to it
		usleep(100000);
		smc_frontport_hold(0);
		return 0;
	}
	c->bus.ctx = &port;
	c->port = port;
	return 1;
}

static void smc_close(struct smc_conn *c)
{
	if (c->vcard) smc_vcard_close(&c->bus);
	else smc_frontport_hold(0);
}

#define LAST_GAME "/tmp/supermemcard.last"   // the game started from the menu: "<system>\t<save name>"

static struct smc_sync_cfg sync_cfg()
{
	static char saves[1024], state[1024], g0[1024], lists[1024];
	snprintf(saves, sizeof(saves), "%s/%s", getRootDir(), SAVE_DIR);
	snprintf(state, sizeof(state), "%s/%s", getRootDir(), SMC_STATE_DIR);
	// each save's game is found in the games folders (MiSTer's: the SD card's, then USB drives), for its game ID
	snprintf(g0, sizeof(g0), "%s/games", getRootDir());
	static char g1[1024];
	snprintf(g1, sizeof(g1), "%s/cifs/games", getRootDir());
	static const char *games[] = { g0, "/media/usb0/games", "/media/usb1/games", "/media/usb2/games",
	                               "/media/usb3/games", "/media/usb4/games", "/media/usb5/games", g1, NULL };
	snprintf(lists, sizeof(lists), "%s/%s", getRootDir(), SMC_GAMELISTS_DIR);
	struct smc_sync_cfg cfg = { saves, state, smc_console_systems, SMC_HOST_MAIN, smc_log, games, lists };
	return cfg;
}

// Back in the menu. Like a PS1, the card is only used when a save is loaded or saved: after a game, its save goes to
// the card (if it changed), and nothing else happens. "Read card" (everything) is the memory manager's: Console Mode's
// menu; on this test bench, /tmp/supermemcard.full.
void supermemcard_menu_start()
{
	if (!is_menu()) return;
	char last[400] = "";
	FILE *lf = fopen(LAST_GAME, "r");
	if (lf) { if (!fgets(last, sizeof(last), lf)) last[0] = 0; fclose(lf); unlink(LAST_GAME); }
	last[strcspn(last, "\n")] = 0;
	int full = access("/tmp/supermemcard.full", F_OK) == 0;
	if (full) unlink("/tmp/supermemcard.full");
	if (!last[0] && !full) return;   // no game was played: the card isn't touched

	struct smc_conn c;
	if (!smc_open(&c)) return;
	smc_log_file = fopen("/tmp/supermemcard.log", "a");
	struct smc_sync_cfg cfg = sync_cfg();
	struct smc_sync_report rep;
	memset(&rep, 0, sizeof(rep));
	int rc = 0;
	char *tab = strchr(last, '\t');
	if (tab) *tab = 0;
	if (full) rc = smc_sync(&c.bus, &cfg, &rep);
	else if (tab && tab[1]) rc = smc_sync_game(&c.bus, &cfg, last, tab + 1, &rep);   // the game just played
	else rc = smc_sync_changed(&c.bus, &cfg, last, &rep);   // started from the core's file browser: its system
	if (rc < 0) smc_log("sync stopped with an error");
	// no game core runs now: the card's buttons are the PS1 card's again
	smc_core(&c.bus, NULL, true);
	if (!c.vcard)
	{
		char l[64];
		snprintf(l, sizeof(l), "(the card in front port %d)", c.port);
		smc_log(l);
	}
	smc_close(&c);
	if (smc_log_file) fclose(smc_log_file);
	smc_log_file = 0;
}

// Before a game core loads: the card shows the system (its buttons pick that system's virtual card), and the game's
// save is loaded from the card if the card's copy is newer (another SuperStation played it, or a reinstall).
void supermemcard_core_start(const char *rbf, const char *mgl)
{
	if (!is_menu()) return;
	// the save folder: the core's, or an MGL's <setname> (Console Mode's GameboyColor.mgl: saves/GBC), and the save's
	// name from the game file the MGL starts
	struct smc_mgl m;
	if (smc_mgl_read(mgl, &m)) memset(&m, 0, sizeof(m));
	const char *sys = smc_launch_system(rbf, m.setname, m.same_dir);
	if (!sys) return;
	const char *game = m.game;
	int known = game[0] != 0;
	FILE *lf = fopen(LAST_GAME, "w");   // so the menu saves it afterwards
	if (lf) { fprintf(lf, "%s\t%s\n", sys, known ? game : ""); fclose(lf); }

	struct smc_conn c;
	if (!smc_open(&c)) return;
	smc_log_file = fopen("/tmp/supermemcard.log", "a");
	int card = smc_core(&c.bus, sys, true);
	char l[96];
	if (card > 0) snprintf(l, sizeof(l), "%s core: its card %d (the card's buttons pick another)", sys, card);
	else snprintf(l, sizeof(l), "%s core: the card didn't answer Core", sys);
	smc_log(l);
	if (card > 0 && known)
	{
		struct smc_sync_cfg cfg = sync_cfg();
		struct smc_sync_report rep;
		if (smc_sync_game(&c.bus, &cfg, sys, game, &rep) < 0) smc_log("game sync stopped with an error");
	}
	smc_close(&c);
	if (smc_log_file) fclose(smc_log_file);
	smc_log_file = 0;
}

// The newest time of a game's save files in saves/<sys> ("<game>.sav", "<game>_1.cpk" ...); 0: none
static time_t game_saves_time(const char *sys, const char *game)
{
	char dir[1024];
	snprintf(dir, sizeof(dir), "%s/%s/%s", getRootDir(), SAVE_DIR, sys);
	DIR *d = opendir(dir);
	if (!d) return 0;
	size_t n = strlen(game);
	time_t newest = 0;
	struct dirent *de;
	while ((de = readdir(d)))
	{
		const char *f = de->d_name;
		if (strncmp(f, game, n) || (f[n] != '.' && !(f[n] == '_' && f[n + 1] >= '1' && f[n + 1] <= '4' && f[n + 2] == '.')))
			continue;
		char p[1300];
		struct stat st;
		snprintf(p, sizeof(p), "%s/%s", dir, f);
		if (!stat(p, &st) && S_ISREG(st.st_mode) && st.st_mtime > newest) newest = st.st_mtime;
	}
	closedir(d);
	return newest;
}

void supermemcard_poll()
{
	static unsigned long next;
	if (!CheckTimer(next)) return;
	next = GetTimer(1000);
	static char key[400];          // the game being watched: "<system>\t<save name>"
	static time_t seen, synced;    // its saves' newest time: last seen, last on the card
	static int no_card;            // this game's core has no front ports, or no card answered: stop asking
	// another core loaded: ask its driver again (a core without one answers nothing)
	static char core[64];
	if (strncmp(core, user_io_get_core_name(), sizeof(core) - 1))
	{
		snprintf(core, sizeof(core), "%s", user_io_get_core_name());
		smc_frontport_reset();
	}
	if (is_menu()) { key[0] = 0; return; }

	char last[400] = "";
	FILE *lf = fopen(LAST_GAME, "r");
	if (lf) { if (!fgets(last, sizeof(last), lf)) last[0] = 0; fclose(lf); }
	last[strcspn(last, "\n")] = 0;
	char *tab = strchr(last, '\t');
	if (!tab || !tab[1] || !strncmp(last, "PSX\t", 4)) return;   // no game known; PSX uses the front ports itself
	if (strcmp(key, last))
	{
		// a new game: its save was loaded from the card at its start, so what's there now is in step
		snprintf(key, sizeof(key), "%s", last);
		*tab = 0;
		seen = synced = game_saves_time(last, tab + 1);
		no_card = 0;
		return;
	}
	if (no_card) return;
	*tab = 0;
	const char *sys = last, *game = tab + 1;
	time_t t = game_saves_time(sys, game);
	if (t != seen) { seen = t; return; }       // still being written: wait until it settles (a second)
	if (!t || t == synced) return;

	struct smc_conn c;
	if (!smc_open(&c)) { no_card = 1; smc_log("in-game: no card reachable from this core (the save goes after the game)"); return; }
	smc_log_file = fopen("/tmp/supermemcard.log", "a");
	struct smc_sync_cfg cfg = sync_cfg();
	struct smc_sync_report rep;
	memset(&rep, 0, sizeof(rep));
	if (smc_sync_game(&c.bus, &cfg, sys, game, &rep) < 0) smc_log("in-game sync stopped with an error");
	else synced = t;
	smc_close(&c);
	if (smc_log_file) fclose(smc_log_file);
	smc_log_file = 0;
}
