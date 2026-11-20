// Phosphor's pop-ups over a game, drawn into MiSTer's OSD (1-bit, 256 pixels wide): white text and an icon on the
// dimmed game, no frame: a heading, a bold title, a line or two of text and a badge (an achievement's points), in a
// pixel font (Pixel Operator, CC0) at its own size. For MiSTer_RA's RetroAchievements pop-ups: they land exactly when
// and where its own would (its two lanes, its timings and sounds, its popup_position corner, retroachievements.cfg's
// settings); only the drawing is Phosphor's, and plainly not MiSTer_RA's framed 8x8 text box.
// Copyright (C) 2026 f3bandit. GPL-3.0 (see LICENSE).
#include "phosphor_osd_font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void PhosphorInfoBitmap(const uint8_t *cols, int wch, int hrows, int timeout, int align);   // menu.cpp (the link's)

#define CW 248                      // the widest card: 31 OSD characters
#define CH 64                       // the tallest: 8 rows
static uint8_t px[CH][CW];          // 1: lit

static void plot(int x, int y) { if (x >= 0 && y >= 0 && x < CW && y < CH) px[y][x] = 1; }
static void clear(int x, int y) { if (x >= 0 && y >= 0 && x < CW && y < CH) px[y][x] = 0; }

static void rrect(int x0, int y0, int x1, int y1, int r, int fill)
{
	for (int y = y0; y <= y1; y++)
		for (int x = x0; x <= x1; x++)
		{
			int dx = x < x0 + r ? x0 + r - x : x > x1 - r ? x - (x1 - r) : 0;
			int dy = y < y0 + r ? y0 + r - y : y > y1 - r ? y - (y1 - r) : 0;
			if (dx * dx + dy * dy <= r * r) { if (fill) plot(x, y); else clear(x, y); }
		}
}

// the three faces: 0 the text (Pixel Operator 16), 1 the title (its bold), 2 the heading (Pixel Operator 8, capitals)
enum { F_TEXT, F_BOLD, F_HEAD };
// the glyphs: ASCII 32-126, then Latin-1 160-255 (the strings are put in Latin-1 first: latin1())
static int gi(char ch)
{
	unsigned c = (unsigned char)ch;
	return c >= 32 && c <= 126 ? c - 32 : c >= 160 ? 95 + c - 160 : '?' - 32;
}
static int glyph_w(int f, char c)
{
	int i = gi(c);
	return f == F_BOLD ? ph_bold[i].w : f == F_HEAD ? ph_head[i].w : ph_text[i].w;
}
static const uint32_t *glyph_rows(int f, char c, int *h)
{
	int i = gi(c);
	*h = f == F_BOLD ? ph_bold_h : f == F_HEAD ? ph_head_h : ph_text_h;
	return f == F_BOLD ? ph_bold[i].r : f == F_HEAD ? ph_head[i].r : ph_text[i].r;
}

// RetroAchievements' text is UTF-8: Latin-1 as it is, a few common others as their nearest, the rest '?'
static void latin1(const char *in, char *out, size_t n)
{
	size_t o = 0;
	const unsigned char *s = (const unsigned char *)(in ? in : "");
	while (*s && o + 4 < n)
	{
		unsigned c = *s++;
		if (c >= 0x80)
		{
			int k = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
			c &= k == 3 ? 0x07 : k == 2 ? 0x0F : 0x1F;
			for (; k > 0 && (*s & 0xC0) == 0x80; k--) c = (c << 6) | (*s++ & 0x3F);
		}
		const char *rep = NULL;
		if (c == 0x2018 || c == 0x2019 || c == 0x2032) rep = "'";
		else if (c == 0x201C || c == 0x201D || c == 0x2033) rep = "\"";
		else if (c == 0x2013 || c == 0x2014 || c == 0x2212) rep = "-";
		else if (c == 0x2026) rep = "...";
		else if (c == 0x2122) rep = "TM";
		else if (c == 0x2605 || c == 0x2606) rep = "*";
		if (rep) { while (*rep && o + 1 < n) out[o++] = *rep++; continue; }
		out[o++] = (c < 32 || (c > 126 && c < 160) || c > 255) ? '?' : (char)c;
	}
	out[o] = 0;
}

static int text_w(int f, const char *s, int n = -1)
{
	int w = 0;
	for (int i = 0; s[i] && (n < 0 || i < n); i++) w += glyph_w(f, s[i]);
	return w;
}

static int text(int x, int y, int f, const char *s, int maxw, int ink = 1)
{
	char buf[264];
	snprintf(buf, sizeof(buf), "%s", s);
	if (text_w(f, buf) > maxw)                     // too long: cut, with an ellipsis
	{
		int n = strlen(buf);
		while (n > 0 && text_w(f, buf, n) + text_w(f, "...") > maxw) n--;
		while (n > 0 && buf[n - 1] == ' ') n--;
		strcpy(buf + n, "...");
	}
	for (const char *c = buf; *c; c++)
	{
		int h;
		const uint32_t *r = glyph_rows(f, *c, &h);
		for (int yy = 0; yy < h; yy++)
			for (int b = 0; b < 32; b++) if (r[yy] & (1u << b)) { if (ink) plot(x + b, y + yy); else clear(x + b, y + yy); }
		x += glyph_w(f, *c);
	}
	return x;
}

static void icon(int x, int y, const uint32_t *ic)
{
	for (int r = 0; r < ph_icon_h; r++) for (int b = 0; b < ph_icon_w; b++) if (ic[r] & (1u << b)) plot(x + b, y + r);
}

// wrap s into at most two lines of maxw pixels (the second one cut with an ellipsis)
static int wrap2(const char *s, int maxw, char *l1, char *l2, size_t n)
{
	l1[0] = l2[0] = 0;
	int len = strlen(s), cut = len;
	if (text_w(F_TEXT, s) > maxw)
	{
		cut = 0;
		for (int i = 0; i <= len; i++)
			if ((s[i] == ' ' || !s[i]) && text_w(F_TEXT, s, i) <= maxw) cut = i;
		if (!cut) cut = len;
	}
	snprintf(l1, n, "%.*s", cut, s);
	if (cut < len) snprintf(l2, n, "%s", s + cut + (s[cut] == ' '));
	return l2[0] ? 2 : 1;
}

// the card: icon, heading (small caps line), title (bold), text (one or two lines), badge (bold, in a pill; "" none)
int phosphor_osd_card(const uint32_t *ic, const char *head, const char *title, const char *body, const char *badge,
                      int timeout, int align)
{
	memset(px, 0, sizeof(px));
	static char h1[96], t1[256], b1[512], g1[32];
	latin1(head, h1, sizeof(h1)); latin1(title, t1, sizeof(t1)); latin1(body, b1, sizeof(b1)); latin1(badge, g1, sizeof(g1));
	head = h1; title = t1; body = b1; badge = g1;
	const int left = ph_icon_w + 10, gap = 8;
	int bw = badge && badge[0] ? text_w(F_BOLD, badge) + 12 : 0;
	int want = left + text_w(F_HEAD, head) + (bw ? bw + gap : 0);
	int tw = left + text_w(F_BOLD, title) + (bw ? bw + gap : 0);
	if (tw > want) want = tw;
	int bwid = left + text_w(F_TEXT, body);
	if (bwid > want) want = bwid;
	int w = want < 120 ? 120 : want > CW ? CW : want;
	w = (w + 7) & ~7;
	char l1[160], l2[160];
	int lines = body && body[0] ? wrap2(body, w - left, l1, l2, sizeof(l1)) : 0;
	// heading, title, a line or two of text: the rows overlap only in the faces' empty descender space
	const int y_head = 1, y_title = y_head + ph_head_base + 4, y_text = y_title + ph_bold_base + 5, line = ph_text_base + 4;
	int h = lines == 2 ? y_text + line + ph_text_h : lines ? y_text + ph_text_h : y_title + ph_bold_h;
	h = (h + 7) & ~7;
	if (h > CH) h = CH;

	// no border: white icon and text straight on the game, dimmed by the OSD
	if (ic) icon(0, (h - ph_icon_h) / 2, ic);
	int x1 = w - (bw ? bw + gap : 0);
	text(left, y_head, F_HEAD, head, x1 - left);
	text(left, y_title, F_BOLD, title, x1 - left);
	if (lines) text(left, y_text, F_TEXT, l1, w - left);
	if (lines == 2) text(left, y_text + line, F_TEXT, l2, w - left);
	if (bw)   // the points: a pill, the number cut out of it
	{
		int ph = ph_bold_base + 5, ty = y_title - 1;
		rrect(w - bw, ty, w - 1, ty + ph, ph / 2, 1);
		text(w - bw + 6, ty + 3, F_BOLD, badge, bw, 0);
	}

	// MiSTer's OSD rows: 8 pixels high, a byte per column, bit 0 at the top
	static uint8_t cols[(CH / 8) * CW];
	int wch = w / 8, hrows = h / 8;
	for (int r = 0; r < hrows; r++)
		for (int x = 0; x < w; x++)
		{
			uint8_t v = 0;
			for (int b = 0; b < 8; b++) if (px[r * 8 + b][x]) v |= 1 << b;
			cols[r * w + x] = v;
		}
	PhosphorInfoBitmap(cols, wch, hrows, timeout, align);
	return 1;
}
