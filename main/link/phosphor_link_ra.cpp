// Phosphor's in-game link in MiSTer_RA (odelot's RetroAchievements Main, with the Super MemCard / Coreshell shim).
// MiSTer_RA is the RetroAchievements client and does all of it (login, tracking, unlocks, hardcore, as configured in
// its retroachievements.cfg); this only shows its results to the frontend, read-only:
//   - its events, as JSON lines in /tmp/mister_ra.events (what Phosphor and Coreshell read: game, unlocked, mastered,
//     progress, challenge, leaderboard, login and server errors);
//   - "ra" and "ra list" on Phosphor's link (phosphor.cpp): the game, the counts, hardcore, every achievement;
//   - while Phosphor's shell has the menu button, its own pop-ups for these instead of MiSTer_RA's OSD ones.
// Nothing here talks to RetroAchievements or changes what MiSTer_RA sends.
// Copyright (C) 2026 f3bandit. GPL-3.0 (see LICENSE).
#include "phosphor.h"
#include "rc_client.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define RA_EVENTS "/tmp/mister_ra.events"

rc_client_t *achievements_rc_client(void);   // achievements.cpp (the link's lines there)
int achievements_popup_pos(void);
void InfoAligned(const char *message, int timeout, int align, int frame);   // MiSTer_RA's menu.cpp
int phosphor_osd_card(const uint32_t *ic, const char *head, const char *title, const char *body, const char *badge,
                      int timeout, int align);   // phosphor_osd_card.cpp
#include "phosphor_osd_font.h"

// the unlocks (their full title, description and points: MiSTer_RA's pop-up text has them cut to 28 characters). Its
// pop-ups wait in a queue, 4 s each: a card finds its own unlock by the title its text starts with, not the latest
#define UNLOCKS 16
static struct { char title[128], desc[256]; unsigned points; } un[UNLOCKS];
static unsigned un_next;

static void json_str(FILE *f, const char *s)
{
	fputc('"', f);
	for (; s && *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
		else if (c == '\n') fputs("\\n", f);
		else if (c < 0x20) fprintf(f, "\\u%04x", c);
		else fputc(c, f);
	}
	fputc('"', f);
}

// an event line: "key", "value" pairs; a key starting with '#' takes a long
static void emit(const char *event, ...)
{
	FILE *f = fopen(RA_EVENTS, "a");
	if (!f) return;
	fprintf(f, "{\"event\":");
	json_str(f, event);
	va_list ap;
	va_start(ap, event);
	const char *k;
	while ((k = va_arg(ap, const char *))) {
		if (k[0] == '#') { fprintf(f, ","); json_str(f, k + 1); fprintf(f, ":%ld", va_arg(ap, long)); }
		else { fprintf(f, ","); json_str(f, k); fprintf(f, ":"); json_str(f, va_arg(ap, const char *)); }
	}
	va_end(ap);
	fprintf(f, ",\"time\":%ld,\"main\":\"MiSTer_RA\"}\n", (long)time(NULL));
	fclose(f);
}

static void badge_url(const rc_client_achievement_t *a, char *out, size_t n)
{
	if (rc_client_achievement_get_image_url(a, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED, out, n) != RC_OK) out[0] = 0;
}

// MiSTer_RA's event handler, first thing (achievements.cpp ra_event_handler)
void phosphor_ra_event(const rc_client_event_t *e, rc_client_t *c)
{
	char badge[256];
	switch (e->type) {
	case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
		{
			unsigned i = un_next++ % UNLOCKS;
			snprintf(un[i].title, sizeof(un[i].title), "%s", e->achievement->title ? e->achievement->title : "");
			snprintf(un[i].desc, sizeof(un[i].desc), "%s", e->achievement->description ? e->achievement->description : "");
			un[i].points = e->achievement->points;
		}
		badge_url(e->achievement, badge, sizeof(badge));
		emit("unlocked", "title", e->achievement->title, "description", e->achievement->description,
		     "badge", badge, "#id", (long)e->achievement->id, "#points", (long)e->achievement->points, NULL);
		break;
	case RC_CLIENT_EVENT_GAME_COMPLETED: {
		const rc_client_game_t *g = rc_client_get_game_info(c);
		emit("mastered", "title", g ? g->title : "", NULL);
		break;
	}
	case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
		emit("challenge_show", "title", e->achievement->title, "#id", (long)e->achievement->id, NULL); break;
	case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
		emit("challenge_hide", "#id", (long)e->achievement->id, NULL); break;
	case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
	case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
		emit("progress", "title", e->achievement->title, "progress", e->achievement->measured_progress, NULL); break;
	case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
		emit("leaderboard_started", "title", e->leaderboard->title, NULL); break;
	case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
		emit("leaderboard_failed", "title", e->leaderboard->title, NULL); break;
	case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
		emit("leaderboard_submitted", "title", e->leaderboard->title, "score", e->leaderboard->tracker_value, NULL); break;
	case RC_CLIENT_EVENT_SERVER_ERROR:
		emit("server_error", "message", e->server_error->error_message, "api", e->server_error->api, NULL); break;
	case RC_CLIENT_EVENT_DISCONNECTED:
		emit("disconnected", NULL); break;
	case RC_CLIENT_EVENT_RECONNECTED:
		emit("reconnected", NULL); break;
	}
}

// its login and game-load callbacks (achievements.cpp ra_login_callback, ra_load_game_callback)
void phosphor_ra_login(int result, const char *err, rc_client_t *c)
{
	if (result != RC_OK) { emit("login_failed", "message", err ? err : "", NULL); return; }
	const rc_client_user_t *usr = rc_client_get_user_info(c);
	emit("logged_in", "user", usr ? usr->display_name : "", NULL);
}

void phosphor_ra_game(int result, const char *err, rc_client_t *c)
{
	if (result != RC_OK) {
		emit(result == RC_NO_GAME_LOADED ? "unknown_game" : "error", "message", err ? err : "couldn't load the game", NULL);
		return;
	}
	unlink(RA_EVENTS);                                 // a new game's events (a new file: readers start again)
	const rc_client_game_t *g = rc_client_get_game_info(c);
	rc_client_user_game_summary_t s;
	rc_client_get_user_game_summary(c, &s);
	char img[256] = "";
	rc_client_game_get_image_url(g, img, sizeof(img));
	emit("game", "title", g->title, "badge", img, "#game_id", (long)g->id, "#achievements", (long)s.num_core_achievements,
	     "#unlocked", (long)s.num_unlocked_achievements, "#hardcore", (long)rc_client_get_hardcore_enabled(c), NULL);
}

// Phosphor's pop-ups in the corner the user set for RetroAchievements' (retroachievements.cfg popup_position)
static void popup_log(const char *who, const char *text)
{
	FILE *f = fopen("/tmp/phosphor/ra-popups.log", "a");   // (the test bench: whose pop-up was shown, whose kept back)
	if (!f) return;
	fprintf(f, "%ld %s: ", (long)time(NULL), who);
	for (const char *c = text; *c; c++) fputc(*c == '\n' ? '|' : *c, f);
	fputc('\n', f);
	fclose(f);
}
// phosphor.cpp's own RA pop-ups (from the events file) are for Mains that draw none; here MiSTer_RA's are drawn as
// Phosphor's cards (phosphor_ra_draw), so these would be doubles
void phosphor_ra_popup(const char *msg, int ms) { (void)ms; popup_log("phosphor events (not drawn here)", msg); }

static void line_at(const char *text, int k, char *out, size_t n)   // line k of a pop-up text (empty lines count)
{
	for (int i = 0; i < k && text; i++) { text = strchr(text, '\n'); if (text) text++; }
	if (!text) { out[0] = 0; return; }
	snprintf(out, n, "%.*s", (int)strcspn(text, "\n"), text);
}

// lines k on, put back together (MiSTer_RA wraps them at 28 characters)
static void lines_from(const char *text, int k, char *out, size_t n)
{
	size_t o = 0;
	char l[160];
	out[0] = 0;
	for (int i = k; i < 16; i++)
	{
		line_at(text, i, l, sizeof(l));
		char *p = l;
		while (*p == ' ') p++;
		if (p[0] == '\\' && p[1] == '-' && p[2] == '>' && p[3] == ' ') p += 4;   // (its "\\-> " before a description)
		if (!*p) continue;
		o += snprintf(out + o, o < n ? n - o : 0, "%s%s", o ? " " : "", p);
		if (o >= n) break;
	}
}

static int first_text_line(const char *text, int from)   // the first non-empty line from <from>
{
	char l[160];
	for (int k = from; k < 16; k++) { line_at(text, k, l, sizeof(l)); if (l[0]) return k; }
	return from;
}

// MiSTer_RA's pop-up drawing step (InfoAligned in achievements.cpp, and menu.cpp's hardcore cheats line): the same
// pop-up at the same moment, for the same time, in the same corner, drawn as Phosphor's card while Phosphor is the
// frontend, as MiSTer_RA's own box otherwise
void phosphor_ra_draw(const char *text, int ms, int align, int frame)
{
	if (!phosphor_frontend_here()) { popup_log("MiSTer_RA box", text); InfoAligned(text, ms, align, frame); return; }
	popup_log("phosphor card", text);

	char l0[160], l1[160], head[64] = "RETROACHIEVEMENTS", title[160] = "", body[256] = "", badge[16] = "";
	line_at(text, 0, l0, sizeof(l0));
	line_at(text, 1, l1, sizeof(l1));
	const uint32_t *ic = ph_icon_ra;
	rc_client_t *c = achievements_rc_client();
	int k;

	if (!strncmp(l0, ">> ACHIEVEMENT", 14))
	{
		ic = ph_icon_trophy;
		snprintf(head, sizeof(head), "ACHIEVEMENT UNLOCKED");
		k = first_text_line(text, 1);
		char shown[160];
		line_at(text, k, shown, sizeof(shown));       // (its title, cut to 28 and maybe "...")
		size_t n = strlen(shown);
		if (n > 3 && !strcmp(shown + n - 3, "...")) n -= 3;
		if (n > 24) n = 24;
		int hit = -1;
		for (unsigned j = 0; j < UNLOCKS && j < un_next && n; j++)      // newest first: the same title twice is one
		{
			unsigned i = (un_next - 1 - j) % UNLOCKS;
			if (!strncmp(un[i].title, shown, n)) { hit = (int)i; break; }
		}
		if (hit >= 0)
		{
			snprintf(title, sizeof(title), "%s", un[hit].title);
			snprintf(body, sizeof(body), "%s", un[hit].desc);
			if (un[hit].points) snprintf(badge, sizeof(badge), "%u", un[hit].points);
		}
		else { snprintf(title, sizeof(title), "%s", shown); lines_from(text, k + 1, body, sizeof(body)); }
	}
	else if (!strncmp(l0, "** GAME COMPLETED", 17))
	{
		ic = ph_icon_flag;
		snprintf(head, sizeof(head), "GAME COMPLETED");
		const rc_client_game_t *g = c ? rc_client_get_game_info(c) : NULL;
		snprintf(title, sizeof(title), "%s", g && g->title ? g->title : "Congratulations!");
		snprintf(body, sizeof(body), "Every achievement unlocked");
	}
	else if (!strncmp(l0, "** SUBSET ", 10))
	{
		ic = ph_icon_flag;
		snprintf(head, sizeof(head), "SUBSET %s", strstr(l0, "MASTERED") ? "MASTERED" : "COMPLETED");
		lines_from(text, 1, title, sizeof(title));
	}
	else if (!strncmp(l0, "LEADERBOARD", 11) || !strncmp(l0, "LB ", 3))
	{
		ic = ph_icon_podium;
		snprintf(head, sizeof(head), "%s", l0);
		k = first_text_line(text, 1);
		line_at(text, k, title, sizeof(title));
		lines_from(text, k + 1, body, sizeof(body));
	}
	else if (!strncmp(l0, "CHALLENGE", 9))
	{
		ic = ph_icon_bars;
		snprintf(head, sizeof(head), "%s", l0);
		lines_from(text, 1, title, sizeof(title));
	}
	else if (strstr(text, "Progress"))
	{
		ic = ph_icon_bars;
		snprintf(head, sizeof(head), "PROGRESS");
		snprintf(title, sizeof(title), "%s", l0);
		lines_from(text, 1, body, sizeof(body));
	}
	else if (strstr(l1, "(HC:"))
	{
		ic = ph_icon_user;
		snprintf(head, sizeof(head), "SIGNED IN");
		snprintf(title, sizeof(title), "%s", l0);
		unsigned hc = 0, sc = 0;
		sscanf(l1, "(HC:%u SC:%u)", &hc, &sc);
		snprintf(body, sizeof(body), "Hardcore %u points - Softcore %u points", hc, sc);
	}
	else if (strstr(l1, " achievements") || strstr(l1, "No achievements"))
	{
		snprintf(title, sizeof(title), "%s", l0);
		rc_client_user_game_summary_t s;
		memset(&s, 0, sizeof(s));
		if (c) rc_client_get_user_game_summary(c, &s);
		if (s.num_core_achievements)
			snprintf(body, sizeof(body), "%u of %u unlocked - %u of %u points", s.num_unlocked_achievements,
			         s.num_core_achievements, s.points_unlocked, s.points_core);
		else lines_from(text, 1, body, sizeof(body));
	}
	else if (!strncasecmp(l0, "HARDCORE", 8))
	{
		ic = ph_icon_warn;
		snprintf(head, sizeof(head), "HARDCORE");
		const char *p = l0 + 8;
		while (*p == ' ' || *p == ':') p++;
		if (!strncmp(p, "mode ", 5)) p += 5;
		snprintf(title, sizeof(title), "%s", p);
		lines_from(text, 1, body, sizeof(body));
	}
	else if (!strncmp(l0, "RetroAchievements", 17))
	{
		ic = ph_icon_warn;
		k = first_text_line(text, 1);
		line_at(text, k, title, sizeof(title));
		lines_from(text, k + 1, body, sizeof(body));
	}
	else
	{
		snprintf(title, sizeof(title), "%s", l0);
		lines_from(text, 1, body, sizeof(body));
	}
	phosphor_osd_card(ic, head, title, body, badge, ms, align);
}

static size_t jesc(char *o, size_t n, const char *s)
{
	size_t k = 0;
	for (; s && *s && k + 7 < n; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') { o[k++] = '\\'; o[k++] = c; }
		else if (c < 0x20) k += snprintf(o + k, n - k, "\\u%04x", c);
		else o[k++] = c;
	}
	o[k] = 0;
	return k;
}

void phosphor_ra_reply(int list, char *out, size_t n)
{
	rc_client_t *c = achievements_rc_client();
	const rc_client_game_t *g = c ? rc_client_get_game_info(c) : NULL;
	const rc_client_user_t *u = c ? rc_client_get_user_info(c) : NULL;
	if (!list) {
		rc_client_user_game_summary_t s;
		memset(&s, 0, sizeof(s));
		if (g && g->id) rc_client_get_user_game_summary(c, &s);
		char t[256];
		jesc(t, sizeof(t), g ? g->title : "");
		snprintf(out, n, "{\"ok\":true,\"active\":%s,\"logged_in\":%s,\"game\":%u,\"title\":\"%s\",\"total\":%u,\"unlocked\":%u,"
		         "\"points\":%u,\"points_unlocked\":%u,\"hardcore\":%s,\"main\":\"MiSTer_RA\"}",
		         g && g->id ? "true" : "false", u ? "true" : "false", g ? g->id : 0, t, s.num_core_achievements,
		         s.num_unlocked_achievements, s.points_core, s.points_unlocked,
		         c && rc_client_get_hardcore_enabled(c) ? "true" : "false");
		return;
	}
	size_t k = snprintf(out, n, "{\"ok\":true,\"achievements\":[");
	rc_client_achievement_list_t *l = (g && g->id) ? rc_client_create_achievement_list(c, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
	                                                 RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS) : NULL;
	int first = 1;
	for (uint32_t b = 0; l && b < l->num_buckets; b++)
		for (uint32_t i = 0; i < l->buckets[b].num_achievements && k + 900 < n; i++) {
			const rc_client_achievement_t *a = l->buckets[b].achievements[i];
			char t[256], d[512], p[64];
			jesc(t, sizeof(t), a->title); jesc(d, sizeof(d), a->description); jesc(p, sizeof(p), a->measured_progress);
			k += snprintf(out + k, n - k, "%s{\"id\":%u,\"title\":\"%s\",\"description\":\"%s\",\"points\":%u,\"unlocked\":%s,\"progress\":\"%s\"}",
			              first ? "" : ",", a->id, t, d, a->points, a->unlocked ? "true" : "false", p);
			first = 0;
		}
	if (l) rc_client_destroy_achievement_list(l);
	snprintf(out + k, n - k, "]}");
}
