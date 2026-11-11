// Super MemCard: the OSD's Saves page (smc_osd.h). Copyright (C) 2026 f3bandit. GPL-3.0-or-later, as Main_MiSTer.
#include "smc_osd.h"

#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "menu.h"      // Info
#include "osd.h"       // OsdWrite, OsdSetTitle, OsdGetSize
#include "smc_shim.h"

#define MAX_SAVES 100

struct save_row
{
	int card;      // the virtual card
	int files;
	long time;     // its newest file's time; 0 for the new one
	int playing;   // the one the game runs with now
	int is_new;    // "New save": the first free card, the game starts with no save
};

static save_row rows[MAX_SAVES + 1];
static int nrows, sel, top, confirm = -1;
static char game[128];

bool smc_osd_available()
{
	return smc_shim_saves_available();
}

int smc_osd_open()
{
	static char ans[8192];
	nrows = sel = top = 0;
	confirm = -1;
	game[0] = 0;
	if (smc_shim_ask("SAVES", ans, sizeof(ans)) < 0) { Info("The memory card didn't answer", 2000); return 0; }
	if (!strncmp(ans, "nocard", 6)) { Info("No memory card in the front port", 2000); return 0; }
	if (!strncmp(ans, "notgenuine", 10)) { Info("Not a Retro Remake memory card", 2000); return 0; }
	if (!strncmp(ans, "nogame", 6)) { Info("This game's saves aren't on the card", 2000); return 0; }
	int new_card = 0;
	for (char *l = strtok(ans, "\n"); l; l = strtok(NULL, "\n"))
	{
		int card, files, playing;
		long t;
		char sys[64], name[128];
		if (sscanf(l, "save\t%d\t%d\t%ld\t%d", &card, &files, &t, &playing) == 4 && nrows < MAX_SAVES)
			rows[nrows++] = { card, files, t, playing, 0 };
		else if (sscanf(l, "game\t%63[^\t]\t%127[^\n]", sys, name) == 2)
			snprintf(game, sizeof(game), "%s", name);
		else if (sscanf(l, "new\t%d", &card) == 1)
			new_card = card;
	}
	if (new_card) rows[nrows++] = { new_card, 0, 0, 0, 1 };   // last: rows has room for it
	if (!nrows) { Info("This game's saves aren't on the card", 2000); return 0; }
	for (int i = 0; i < nrows; i++) if (rows[i].playing) sel = i;
	return 1;
}

static int list_rows() { return OsdGetSize() - 3; }   // the game's name and a gap above, the hint below

static void fit()
{
	if (sel < 0) sel = 0;
	if (sel >= nrows) sel = nrows - 1;
	if (sel < top) top = sel;
	if (sel >= top + list_rows()) top = sel - list_rows() + 1;
	if (top < 0) top = 0;
}

void smc_osd_print()
{
	char s[64];
	fit();
	OsdSetTitle("Saves");
	snprintf(s, sizeof(s), " %.27s", game);
	OsdWrite(0, s, 0, 1);
	OsdWrite(1, "");
	for (int i = 0; i < list_rows(); i++)
	{
		int k = top + i;
		if (k >= nrows) { OsdWrite(2 + i, ""); continue; }
		const save_row &r = rows[k];
		if (r.is_new) snprintf(s, sizeof(s), " New save (card %d)", r.card);
		else
		{
			char when[24] = "";
			time_t t = (time_t)r.time;
			if (t) strftime(when, sizeof(when), "%b %d %H:%M", localtime(&t));
			snprintf(s, sizeof(s), " Card %-2d  %s%s", r.card, r.files ? when : "empty", r.playing ? "  now" : "");
		}
		OsdWrite(2 + i, s, k == sel, 0);
	}
	const char *hint = confirm == sel ? " Again: restart with this save" : nrows > list_rows() ? " More: up/down" : "";
	OsdWrite(OsdGetSize() - 1, hint, confirm == sel, 0);
}

int smc_osd_key(int up, int down, int key, int select)
{
	int was = sel;
	if (up) sel--;
	if (down) sel++;
	if (key == KEY_PAGEUP || key == KEY_LEFT) sel -= list_rows();
	if (key == KEY_PAGEDOWN || key == KEY_RIGHT) sel += list_rows();
	if (key == KEY_HOME) sel = 0;
	if (key == KEY_END) sel = nrows - 1;
	fit();
	if (sel != was) { confirm = -1; return 1; }
	if (!select || !nrows) return 0;

	const save_row &r = rows[sel];
	if (r.playing) { Info("The game is running with this save", 2000); return 0; }
	if (confirm != sel) { confirm = sel; return 1; }   // asked first: a game restarts, its unsaved progress is lost

	char cmd[32], ans[256];
	snprintf(cmd, sizeof(cmd), "USE %d", r.card);
	if (smc_shim_ask(cmd, ans, sizeof(ans)) < 0 || strncmp(ans, "use ok", 6))
	{
		confirm = -1;
		Info("The save couldn't be switched", 2000);
		return 1;
	}
	Info(r.is_new ? "Starting with a new save" : "Starting with this save", 1500);
	smc_shim_restart();   // Main starts again with the game
	return 1;
}
