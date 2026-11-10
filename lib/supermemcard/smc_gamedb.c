/* Copied from super-memcard host/smc_gamedb.c (port, do not edit here: change it there and copy). */
/* Super MemCard: the official names of games, from libretro-database's game lists (No-Intro for cartridges, Redump
 * for discs) (docs/09-game-database.md). The .dat reader is ported from f3bandit's game library code
 * (src_libretro.c). The lists aren't shipped: a frontend fetches them
 * onto the SuperStation (tools/gamedb/fetch.sh does it for testing) and they're used there under their own terms.
 * Copyright (C) 2026 f3bandit. */
#include "smc_gamedb.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Main's save folder -> libretro list(s) (by save folder); disc systems use Redump */
static const struct { const char *sys; const char *lr[3]; int disc; } MAP[] = {
	{ "PSX",       { "Sony - PlayStation" }, 1 },
	{ "Saturn",    { "Sega - Saturn" }, 1 },
	{ "MegaCD",    { "Sega - Mega-CD - Sega CD" }, 1 },
	{ "NES",       { "Nintendo - Nintendo Entertainment System", "Nintendo - Family Computer Disk System" }, 0 },
	{ "SNES",      { "Nintendo - Super Nintendo Entertainment System", "Nintendo - Satellaview" }, 0 },
	{ "Genesis",   { "Sega - Mega Drive - Genesis" }, 0 },
	{ "MegaDrive", { "Sega - Mega Drive - Genesis" }, 0 },
	{ "S32X",      { "Sega - 32X" }, 0 },
	{ "N64",       { "Nintendo - Nintendo 64" }, 0 },
	{ "SMS",       { "Sega - Master System - Mark III", "Sega - Game Gear", "Sega - SG-1000" }, 0 },
	{ "TGFX16",    { "NEC - PC Engine - TurboGrafx 16", "NEC - PC Engine SuperGrafx" }, 0 },
	{ "GAMEBOY",   { "Nintendo - Game Boy", "Nintendo - Game Boy Color" }, 0 },
	{ "GBC",       { "Nintendo - Game Boy Color", "Nintendo - Game Boy" }, 0 },   /* Console Mode's own folder */
	{ "GBA",       { "Nintendo - Game Boy Advance" }, 0 },
	{ "GameGear",  { "Sega - Game Gear", "Sega - Master System - Mark III" }, 0 },   /* Console Mode's Game Gear.mgl */
	{ "Jaguar",    { "Atari - Jaguar" }, 0 },
};
#define NMAP (int)(sizeof(MAP) / sizeof(MAP[0]))

int smc_gamedb_lists(const char *sys, const char *names[3], int *disc) {
	for (int i = 0; i < NMAP; i++)
		if (!strcmp(MAP[i].sys, sys)) {
			int n = 0;
			for (int k = 0; k < 3; k++) {
				names[k] = MAP[i].lr[k];
				n += MAP[i].lr[k] != NULL;
			}
			*disc = MAP[i].disc;
			return n;
		}
	return 0;
}

typedef struct {
	char name[400], comment[400], serial[64], crc[12], description[4000];
	char developer[160], publisher[160], genre[100], year[12], players[12];
	long size;
	int have_rom;
} entry_t;

/* tokens: ( ) word "quoted string" */
static const char *token(const char *p, char *out, size_t n, int *quoted)
{
	while (*p && isspace((unsigned char)*p)) p++;
	*quoted = 0;
	if (!*p) return NULL;
	size_t j = 0;
	if (*p == '(' || *p == ')') { out[0] = *p; out[1] = 0; return p + 1; }
	if (*p == '"') {
		*quoted = 1;
		p++;
		while (*p && *p != '"') {
			if (*p == '\\' && p[1]) p++;
			if (j + 1 < n) out[j++] = *p;
			p++;
		}
		if (*p) p++;
	} else {
		while (*p && !isspace((unsigned char)*p) && *p != '(' && *p != ')') { if (j + 1 < n) out[j++] = *p; p++; }
	}
	out[j] = 0;
	return p;
}

/* calls cb for every game ( ... ) block */
static int parse_dat(const char *path, void (*cb)(void *, entry_t *), void *u)
{
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc((size_t)len + 1);
	if (!buf) { fclose(f); return -1; }
	len = (long)fread(buf, 1, (size_t)len, f);
	buf[len] = 0;
	fclose(f);
	static char tok[4096], key[64];
	const char *p = buf;
	int q, games = 0;
	while ((p = token(p, tok, sizeof(tok), &q))) {
		if (q || strcmp(tok, "game")) continue;
		p = token(p, tok, sizeof(tok), &q);
		if (!p || strcmp(tok, "(")) continue;
		entry_t *e = calloc(1, sizeof(*e));
		int depth = 1, in_rom = 0;
		key[0] = 0;
		while (depth > 0 && (p = token(p, tok, sizeof(tok), &q))) {
			if (!q && !strcmp(tok, "(")) { depth++; if (!strcmp(key, "rom")) in_rom = !e->have_rom; key[0] = 0; continue; }
			if (!q && !strcmp(tok, ")")) { depth--; if (in_rom && depth == 1) { in_rom = 0; e->have_rom = 1; } continue; }
			if (!key[0]) { snprintf(key, sizeof(key), "%s", tok); continue; }
			/* key value */
			if (depth == 1) {
				if (!strcmp(key, "name")) snprintf(e->name, sizeof(e->name), "%s", tok);
				else if (!strcmp(key, "comment")) snprintf(e->comment, sizeof(e->comment), "%s", tok);
				else if (!strcmp(key, "serial")) snprintf(e->serial, sizeof(e->serial), "%s", tok);
				else if (!strcmp(key, "description")) snprintf(e->description, sizeof(e->description), "%s", tok);
				else if (!strcmp(key, "developer")) snprintf(e->developer, sizeof(e->developer), "%s", tok);
				else if (!strcmp(key, "publisher")) snprintf(e->publisher, sizeof(e->publisher), "%s", tok);
				else if (!strcmp(key, "genre")) snprintf(e->genre, sizeof(e->genre), "%s", tok);
				else if (!strcmp(key, "releaseyear")) snprintf(e->year, sizeof(e->year), "%s", tok);
				else if (!strcmp(key, "users") || !strcmp(key, "maxusers")) snprintf(e->players, sizeof(e->players), "%s", tok);
			} else if (in_rom) {
				if (!strcmp(key, "crc")) { for (int i = 0; tok[i] && i < 8; i++) e->crc[i] = (char)tolower((unsigned char)tok[i]); }
				else if (!strcmp(key, "size")) e->size = atol(tok);
				else if (!strcmp(key, "serial") && !e->serial[0]) snprintf(e->serial, sizeof(e->serial), "%s", tok);
			}
			key[0] = 0;
		}
		cb(u, e);
		free(e);
		games++;
		if (!p) break;
	}
	free(buf);
	return games;
}

/* product codes written the same way on both sides: "GM T-81036 -00" -> "T-81036" */
static void serial_norm(const char *in, char *out, size_t n)
{
	const char *p = in;
	while (*p == ' ') p++;
	if (!strncasecmp(p, "GM ", 3)) p += 3;
	size_t j = 0;
	for (; *p && j + 1 < n; p++) if (!isspace((unsigned char)*p)) out[j++] = (char)toupper((unsigned char)*p);
	out[j] = 0;
	size_t l = strlen(out);
	if (l > 4 && out[l - 3] == '-' && isdigit((unsigned char)out[l - 2]) && isdigit((unsigned char)out[l - 1])) out[l - 3] = 0;
}

/* libretro's lists have "???" where a name was lost (e.g. "Nintendo ???"): drop those words */
static void tidy(char *s)
{
	char out[4000];
	size_t j = 0;
	for (char *p = s; *p;) {
		char *w = p;
		while (*p && *p != ' ') p++;
		size_t len = (size_t)(p - w);
		int junk = len > 0;
		for (size_t i = 0; i < len; i++) if (w[i] != '?') junk = 0;
		if (!junk && len) {
			if (j && j + 1 < sizeof(out)) out[j++] = ' ';
			for (size_t i = 0; i < len && j + 1 < sizeof(out); i++) out[j++] = w[i];
		}
		while (*p == ' ') p++;
	}
	out[j] = 0;
	memcpy(s, out, j + 1);
}

/* ---- the loaded lists: one system at a time (the sync goes system by system) ---- */

struct ent { char crc[9], serial[32]; char *name; };
static struct ent *ents;
static int nents, cap;
static char loaded_sys[40], loaded_dir[1024];

static void on_entry(void *u, entry_t *e) {
	(void)u;
	if (!e->name[0])
		return;
	if (nents == cap) {
		int c = cap ? cap * 2 : 4096;
		struct ent *n = realloc(ents, (size_t)c * sizeof(*n));
		if (!n)
			return;
		ents = n, cap = c;
	}
	struct ent *x = &ents[nents];
	snprintf(x->crc, sizeof(x->crc), "%s", e->crc);
	serial_norm(e->serial, x->serial, sizeof(x->serial));
	tidy(e->name);
	x->name = strdup(e->name);
	if (x->name)
		nents++;
}

static void unload(void) {
	for (int i = 0; i < nents; i++)
		free(ents[i].name);
	free(ents);
	ents = NULL;
	nents = cap = 0;
	loaded_sys[0] = 0;
}

static int load(const char *dir, const char *sys) {
	if (!strcmp(sys, loaded_sys) && !strcmp(dir, loaded_dir))
		return nents;
	unload();
	snprintf(loaded_sys, sizeof(loaded_sys), "%s", sys);
	snprintf(loaded_dir, sizeof(loaded_dir), "%s", dir);
	const char *lr[3];
	int disc;
	int n = smc_gamedb_lists(sys, lr, &disc);
	for (int i = 0; i < n; i++) {
		char p[1400];
		snprintf(p, sizeof(p), "%s/%s.dat", dir, lr[i]);
		parse_dat(p, on_entry, NULL);
	}
	return nents;
}

const char *smc_gamedb_name(const char *dir, const char *sys, const char *crc, const char *serial) {
	if (!dir || !load(dir, sys))
		return NULL;
	if (crc && crc[0]) {
		char c[9];
		int k = 0;
		for (; crc[k] && k < 8; k++)
			c[k] = (char)tolower((unsigned char)crc[k]);
		c[k] = 0;
		for (int i = 0; i < nents; i++)
			if (!strcmp(ents[i].crc, c))
				return ents[i].name;
	}
	if (serial && serial[0]) {
		char s[64];
		serial_norm(serial, s, sizeof(s));
		for (int i = 0; i < nents; i++)
			if (ents[i].serial[0] && !strcmp(ents[i].serial, s))
				return ents[i].name;
	}
	return NULL;
}

void smc_gamedb_close(void) { unload(); }
