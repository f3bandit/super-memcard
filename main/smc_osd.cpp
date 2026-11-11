// Super MemCard: the OSD's Saves page (smc_osd.h). Copyright (C) 2026 f3bandit. GPL-3.0-or-later, as Main_MiSTer.
#include "smc_osd.h"

#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "osd.h"       // OsdWrite, OsdSetTitle, OsdGetSize, OsdUpdate, ScrollText, ScrollReset
#include "smc_shim.h"

#define MAX_SAVES 100

struct save_row
{
	int card;      // the virtual card
	int files;
	long time;     // its newest file's time; 0 for the new one
	int playing;   // the one the game runs with now
	int is_new;    // "New save": the first free card, the game starts with no save
	char label[25];   // the name the user gave it (the memory manager's Rename), "" if none
};

#define ROW_W  29   // a row's characters on the OSD (its last columns stay clear)

static save_row rows[MAX_SAVES + 1];
static int nrows, sel, top, confirm = -1;
static char game[128];
static char msg[40];   // on the page's last row (Main's Info() pop-up doesn't show over a menu page)

static void say(const char *m) { snprintf(msg, sizeof(msg), " %s", m); }

bool smc_osd_available()
{
	return smc_shim_saves_available();
}

int smc_osd_open()
{
	static char ans[8192];
	nrows = sel = top = 0;
	confirm = -1;
	game[0] = msg[0] = 0;
	ScrollReset();
	// what's wrong is said on the page itself (no rows then)
	if (smc_shim_ask("SAVES", ans, sizeof(ans)) < 0) { say("The card didn't answer"); return 1; }
	if (!strncmp(ans, "nocard", 6)) { say("No memory card in the port"); return 1; }
	if (!strncmp(ans, "notgenuine", 10)) { say("Not a Retro Remake card"); return 1; }
	if (!strncmp(ans, "nogame", 6)) { say("No saves for it on the card"); return 1; }
	int new_card = 0;
	for (char *l = strtok(ans, "\n"); l; l = strtok(NULL, "\n"))
	{
		int card, files, playing;
		long t;
		char sys[64], name[128], label[25] = "";
		if (sscanf(l, "save\t%d\t%d\t%ld\t%d\t%24[^\n]", &card, &files, &t, &playing, label) >= 4 && nrows < MAX_SAVES)
		{
			rows[nrows] = { card, files, t, playing, 0, "" };
			snprintf(rows[nrows].label, sizeof(rows[nrows].label), "%s", label);
			nrows++;
		}
		else if (sscanf(l, "game\t%63[^\t]\t%127[^\n]", sys, name) == 2)
			snprintf(game, sizeof(game), "%s", name);
		else if (sscanf(l, "new\t%d", &card) == 1)
			new_card = card;
	}
	if (new_card) rows[nrows++] = { new_card, 0, 0, 0, 1, "" };   // last: rows has room for it
	if (!nrows) say("No saves for it on the card");
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

// a row's whole text: " Card 2  now  Speedrun  Oct 05 2026 20:47". Its head (" Card 2  now  ") stays put; the rest,
// the name and the date, scrolls on the selected row when it's longer than the OSD. Returns the head's length
static int row_text(const save_row &r, char *s, size_t n)
{
	if (r.is_new) { snprintf(s, n, " New save (card %d)", r.card); return 0; }
	char when[24] = "";
	time_t t = (time_t)r.time;
	if (r.files && t) strftime(when, sizeof(when), "%b %d %Y %H:%M", localtime(&t));
	int head = snprintf(s, n, " Card %-2d  %s", r.card, r.playing ? "now  " : "");
	snprintf(s + head, n - head, "%s%s%s", r.label, r.label[0] ? "  " : "", r.files ? when : "empty");
	return head;
}

void smc_osd_scroll()
{
	if (sel < 0 || sel >= nrows || msg[0] || confirm == sel) return;   // the hint row speaks: the row stays still
	char s[96];
	int head = row_text(rows[sel], s, sizeof(s));
	int len = (int)strlen(s);
	if (len > ROW_W) ScrollText(2 + sel - top, s, head, len, ROW_W + 1, 1);
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
		char full[96];
		row_text(rows[k], full, sizeof(full));
		snprintf(s, sizeof(s), "%.*s", ROW_W, full);   // cut to the OSD; the selected one scrolls (smc_osd_scroll)
		OsdWrite(2 + i, s, k == sel, 0);
	}
	const char *hint = msg[0] ? msg : confirm == sel ? " Again: restart with this save" : nrows > list_rows() ? " More: up/down" : "";
	OsdWrite(OsdGetSize() - 1, hint, msg[0] || confirm == sel, 0);
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
	if (sel != was) { confirm = -1; msg[0] = 0; ScrollReset(); return 1; }
	if (!select || !nrows) return 0;

	const save_row &r = rows[sel];
	if (r.playing) { say("Already running this save"); return 1; }
	if (confirm != sel) { confirm = sel; msg[0] = 0; return 1; }   // asked first: a game restarts, its unsaved progress is lost

	// the system's saves move between cards: seconds (more with many saves). Said on the page before Main waits
	char cmd[32], ans[256];
	say("Switching saves...");
	smc_osd_print();
	OsdUpdate();
	snprintf(cmd, sizeof(cmd), "USE %d", r.card);
	if (smc_shim_ask(cmd, ans, sizeof(ans), 600000) < 0 || strncmp(ans, "use ok", 6))
	{
		confirm = -1;
		say("Couldn't switch the save");
		return 1;
	}
	say("Restarting the game...");
	smc_osd_print();
	OsdUpdate();
	smc_shim_restart();   // Main starts again with the game
	return 1;
}
