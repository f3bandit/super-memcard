/*
 * smcview: Console Mode's Memory Cards screen (the ribbon theme, rebuilt from snapshots as fpga_offload_module's
 * cmscreens) with the Super MemCard section under it, read live from smcd: each system with saves on the card, its
 * current virtual card, and its games with their icons (from the card's icon files: cover art, or the system's
 * placeholder), names and when they were saved. A look at what Console Mode's memory manager would show, on the TV.
 *
 *   smcview [-T <smcd state dir>]... [-f <file>] [-s <file>] [seconds]
 *
 * -f: the section from a file saved before (-s saves it), for a Main without the card (Console Mode's own, for now);
 * the icons are still read from the state dir.
 *
 * Run in a folder with Outfit-Medium.ttf (the theme's font) and cmrec/ (cmprep.py's button glyphs), as cmscreens. The
 * framebuffer is Console Mode's 1280 x 720. Up and down (keyboard or controller) move the selection, the list scrolling
 * with it; left alone it moves down every 3 s. Any other button, or [seconds] (0: none), ends it, and the screen is put
 * back as it was. The controllers are its own while it runs (grabbed), so nothing reaches Console Mode meanwhile;
 * smcview.sh pauses Console Mode's frontend around it, so the two don't draw over each other.
 * Drawing code from fpga_offload_module's cmscreens (f3bandit). Copyright (C) 2026 f3bandit. Licensed under the Apache License 2.0 (LICENSE, NOTICE).
 */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "smc_sync.h"
#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define W 1280
#define H 720
#define TEXT  0xEEF0F6                  /* the ribbon theme's colours */
#define MUTED 0xA7B0C2
#define ACCENT 0xFF002B
static int stride;
static uint8_t *fbm;
static uint32_t bb[W * H];              /* the back buffer (ARGB) */

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static double cpu(void) { struct rusage r; getrusage(RUSAGE_SELF, &r); return r.ru_utime.tv_sec + r.ru_utime.tv_usec / 1e6 + r.ru_stime.tv_sec + r.ru_stime.tv_usec / 1e6; }

/* ------------------------------------------------------------------ drawing (cmsim's) */
typedef struct { uint8_t *bm; int w, h, xo, yo, adv; } glyph;
typedef struct { glyph g[96]; int asc, desc, px; } face;
static face f_list, f_chrome;
static void face_make(face *f, stbtt_fontinfo *fi, float px)
{
	float sc = stbtt_ScaleForPixelHeight(fi, px);
	int a, d, l;
	stbtt_GetFontVMetrics(fi, &a, &d, &l);
	f->asc = (int)(a * sc); f->desc = (int)(-d * sc); f->px = (int)px;
	for(int c = 32; c < 128; c++) {
		glyph *g = &f->g[c - 32];
		int adv, lsb;
		stbtt_GetCodepointHMetrics(fi, c, &adv, &lsb);
		g->adv = (int)(adv * sc + 0.5f);
		g->bm = stbtt_GetCodepointBitmap(fi, sc, sc, c, &g->w, &g->h, &g->xo, &g->yo);
	}
}
static int text_w(face *f, const char *s) { int w = 0; for(; *s; s++) if(*s >= 32 && *s < 128) w += f->g[*s - 32].adv; return w; }
static int cx0, cy0, cx1 = W, cy1 = H;  /* where drawing goes */
static inline void blend(uint32_t *d, uint32_t rgb, int a)
{
	if(a <= 0) return;
	if(a >= 255) { *d = 0xFF000000u | rgb; return; }
	uint32_t o = *d, da = o >> 24;
	if(da < 255) {                     /* over something see-through: straight alpha, as the module mixes it */
		uint32_t na = a + da * (255 - a) / 255;
		if(!na) return;
		uint32_t wd = da * (255 - a) / 255, r = 0, g = 0, b = 0;
		r = (((rgb >> 16) & 255) * a + ((o >> 16) & 255) * wd) / na;
		g = (((rgb >> 8) & 255) * a + ((o >> 8) & 255) * wd) / na;
		b = ((rgb & 255) * a + (o & 255) * wd) / na;
		*d = (na << 24) | (r << 16) | (g << 8) | b;
		return;
	}
	uint32_t ia = 256 - a - (a >> 7), w = a + (a >> 7);
	uint32_t rb = (((rgb & 0xFF00FF) * w + (o & 0xFF00FF) * ia) >> 8) & 0xFF00FF;
	uint32_t g  = (((rgb & 0x00FF00) * w + (o & 0x00FF00) * ia) >> 8) & 0x00FF00;
	uint32_t na = w + (((o >> 24) * ia) >> 8); if(na > 255) na = 255;
	*d = (na << 24) | rb | g;
}
static inline int in(int x, int y) { return x >= cx0 && x < cx1 && y >= cy0 && y < cy1; }
/* text in a row: vertically centred in the box top..top+h */
static void text(face *f, int x, int top, int h, const char *s, uint32_t rgb)
{
	int y = top + (h - (f->asc + f->desc)) / 2;
	for(; *s; s++) {
		if(*s < 32 || *s >= 128) continue;
		glyph *g = &f->g[*s - 32];
		for(int j = 0; j < g->h; j++)
			for(int i = 0; i < g->w; i++) {
				int xx = x + g->xo + i, yy = y + f->asc + g->yo + j;
				if(in(xx, yy)) blend(&bb[yy * W + xx], rgb, g->bm[j * g->w + i]);
			}
		x += g->adv;
	}
}
static void text_r(face *f, int xr, int top, int h, const char *s, uint32_t rgb) { text(f, xr - text_w(f, s), top, h, s, rgb); }
static void rrect(int x, int y, int w, int h, int rad, uint32_t rgb)   /* rounded, anti-aliased corners */
{
	for(int j = 0; j < h; j++)
		for(int i = 0; i < w; i++) {
			if(!in(x + i, y + j)) continue;
			float dx = i < rad ? rad - i - 0.5f : i >= w - rad ? i - (w - rad) + 0.5f : 0;
			float dy = j < rad ? rad - j - 0.5f : j >= h - rad ? j - (h - rad) + 0.5f : 0;
			float d = (dx > 0 && dy > 0) ? sqrtf(dx * dx + dy * dy) - rad + 0.5f : 0;
			int a = d <= 0 ? 255 : d >= 1 ? 0 : (int)(255 * (1 - d));
			blend(&bb[(y + j) * W + x + i], rgb, a);
		}
}

/* ------------------------------------------------------------------ the pictures (cmprep.py's) */
typedef struct { int w, h; uint32_t *px; } pic;
static pic load(const char *path)
{
	pic p = { 0, 0, 0 };
	FILE *f = fopen(path, "rb");
	if(!f) return p;
	if(fscanf(f, "%d %d", &p.w, &p.h) == 2 && fgetc(f) == '\n' && p.w > 0 && p.h > 0 && p.w * p.h <= 2048 * 2048) {
		p.px = malloc((size_t)p.w * p.h * 4);
		if(fread(p.px, 4, (size_t)p.w * p.h, f) != (size_t)p.w * p.h) { free(p.px); p.px = 0; }
	}
	fclose(f);
	return p;
}
static void blit(pic *p, int x, int y)
{
	if(!p->px) return;
	for(int j = 0; j < p->h; j++)
		for(int i = 0; i < p->w; i++) {
			if(!in(x + i, y + j)) continue;
			uint32_t s = p->px[j * p->w + i];
			blend(&bb[(y + j) * W + x + i], s & 0xFFFFFF, s >> 24);
		}
}

/* ------------------------------------------------------------------ the background, as Console Mode draws it */
static uint32_t grad[H];
static void gradient_init(void)        /* (measured: 33,4,9 at the top to 91,22,55 at the bottom) */
{
	for(int y = 0; y < H; y++) {
		float t = (float)y / (H - 1);
		grad[y] = 0xFF000000u | ((33 + (int)(58 * t)) << 16) | ((4 + (int)(18 * t)) << 8) | (9 + (int)(46 * t));
	}
}
/* Console Mode's ribbon look: bundles of thin bright lines that spread and gather, over a soft see-through band;
   one period across the texture (shown twice as wide), so it repeats without a seam */
static void ribbon_texture(uint32_t *t, int tw, int th)
{
	memset(t, 0, (size_t)tw * th * 4);
	static const struct { float y, amp, ph, spread, a; } b[2] = { { 330, 60, 0.3f, 16, 1.0f }, { 400, 45, 2.1f, 11, 0.6f } };
	for(int k = 0; k < 2; k++)
		for(int x = 0; x < tw; x++) {
			float u = 6.2832f * x / tw;
			float yc = b[k].y + b[k].amp * sinf(u + b[k].ph);
			float sp = b[k].spread * (0.35f + 0.65f * (0.5f + 0.5f * sinf(u + b[k].ph * 2 + 1.7f)));
			float ys[6], lo = 1e9f, hi = -1e9f;
			for(int i = 0; i < 6; i++) {
				ys[i] = yc + (i - 2.5f) * sp + 6 * sinf(2 * u + i * 1.3f + k);
				if(ys[i] < lo) lo = ys[i];
				if(ys[i] > hi) hi = ys[i];
			}
			for(int y = (int)lo - 24; y <= (int)hi + 24; y++) {
				if(y < 0 || y >= th) continue;
				float a = 0;
				float edge = y < lo ? lo - y : y > hi ? y - hi : 0;          /* the soft band between the lines */
				a += 26 * (edge < 24 ? 1 - edge / 24 : 0);
				for(int i = 0; i < 6; i++) { float d = fabsf(y - ys[i]); if(d < 1.6f) a += 120 * (1 - d / 1.6f); }
				a *= b[k].a;
				uint32_t *p = &t[y * tw + x];
				int na = (int)a + (int)(*p >> 24) * (255 - (int)a) / 255;
				*p = ((uint32_t)(na > 255 ? 255 : na) << 24) | 0xFFE8F0;
			}
		}
}

static uint32_t rtex[512 * 1024];
/* the ARM's way: the gradient, then the ribbon texture over it, moving (as the module's image layer: 2x across) */
static void draw_bg(float t)
{
	int ofs = (int)(t * 30);
	for(int y = 0; y < H; y++) {
		uint32_t c = grad[y], *p = &bb[y * W], *r = &rtex[y * 512];
		for(int x = 0; x < W; x++) { uint32_t s = r[((x + ofs) >> 1) & 511]; p[x] = c; if(s >> 24) blend(&p[x], s & 0xFFFFFF, s >> 24); }
	}
}


/* ------------------------------------------------------------------ smcd's SECTION */
typedef struct { int head; char sys[40], title[64], name[200], id[100], when[32]; int card, games; pic icon; } row;
static row R[400];
static int nr;
static const char *state_dirs[4] = {"/media/fat/ConsoleMode/supermemcard"};
static int nstate = 1, state_set;

static pic icon_pic(const uint8_t *b, size_t n)        /* icon.bin (SMIC): 16 PS1 colours, 4 bits a pixel */
{
	pic p = { 0, 0, 0 };
	if(n < 40 || memcmp(b, "SMIC", 4)) return p;
	int w = b[6], h = b[7];
	if(!w || !h || n < 40 + (size_t)w * h / 2) return p;
	uint32_t pal[16];
	for(int i = 0; i < 16; i++) {
		unsigned v = b[8 + 2 * i] | b[9 + 2 * i] << 8;
		pal[i] = v ? 0xFF000000u | ((v & 31) << 19) | (((v >> 5) & 31) << 11) | (((v >> 10) & 31) << 3) : 0;
	}
	p.w = 32; p.h = 32; p.px = malloc(32 * 32 * 4);
	for(int y = 0; y < 32; y++)
		for(int x = 0; x < 32; x++) {
			int sx = x * w / 32, sy = y * h / 32, k = sy * w + sx;
			uint8_t by = b[40 + k / 2];
			p.px[y * 32 + x] = pal[k & 1 ? by >> 4 : by & 15];
		}
	return p;
}

static pic game_icon(const char *sys, const char *id)
{
	char path[600];
	static uint8_t buf[2048];
	char base[100];
	snprintf(base, sizeof base, "%s", id);
	char *tl = strchr(base, '~');           /* a long ID ends ~<CRC of the whole name>; smcd keeps its icon by the rest */
	if(tl) *tl = 0;
	for(int i = 0; i < nstate; i++) {   /* the icon the card has: made by smcd from cover art, kept in its state */
		snprintf(path, sizeof path, "%s/icons/%s/%s.bin", state_dirs[i], sys, base);
		FILE *f = fopen(path, "rb");
		if(f) { size_t n = fread(buf, 1, sizeof buf, f); fclose(f); pic p = icon_pic(buf, n); if(p.px) return p; }
	}
	size_t n; const uint8_t *d = smc_system_icon(sys, &n);
	return d ? icon_pic(d, n) : (pic){ 0, 0, 0 };
}

static char *field(char **s) { char *f = *s, *t = strchr(f, '\t'); if(t) { *t = 0; *s = t + 1; } else *s = f + strlen(f); return f; }

static int read_section(const char *from, const char *save)
{
	FILE *f, *out = save ? fopen(save, "w") : NULL;
	if(from) f = fopen(from, "r");
	else {
		int fd = socket(AF_UNIX, SOCK_STREAM, 0);
		struct sockaddr_un a = { .sun_family = AF_UNIX };
		snprintf(a.sun_path, sizeof a.sun_path, "/tmp/smcd.sock");
		if(fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a)) { perror("smcd"); return -1; }
		if(write(fd, "SECTION\n", 8) != 8) return -1;
		f = fdopen(fd, "r");
	}
	if(!f) { perror(from); return -1; }
	static char line[4096];
	nr = 0;
	while(fgets(line, sizeof line, f) && nr < 400) {
		if(out) fputs(line, out);
		line[strcspn(line, "\n")] = 0;
		char *s = line, *k = field(&s);
		if(!strcmp(k, "system")) {
			row *r = &R[nr++]; memset(r, 0, sizeof *r); r->head = 1;
			snprintf(r->sys, sizeof r->sys, "%s", field(&s)); snprintf(r->title, sizeof r->title, "%s", field(&s));
			r->card = atoi(field(&s)); r->games = atoi(field(&s));
		} else if(!strcmp(k, "game")) {
			row *r = &R[nr++]; memset(r, 0, sizeof *r);
			snprintf(r->sys, sizeof r->sys, "%s", field(&s)); snprintf(r->name, sizeof r->name, "%s", field(&s));
			snprintf(r->id, sizeof r->id, "%s", field(&s));
			time_t t = (time_t)atoll(field(&s));
			struct tm tm; localtime_r(&t, &tm);
			strftime(r->when, sizeof r->when, "%m/%d/%y %I:%M%p", &tm);
			r->icon = game_icon(r->sys, r->id);
		} else if(!strcmp(k, "section") || !strcmp(k, "nocard") || !strcmp(k, "END"))
			break;
	}
	fclose(f);
	if(out) fclose(out);
	return nr;
}

/* ------------------------------------------------------------------ the screen (the memory card snapshot's measures) */
static pic btn_a, btn_b;
static int sel, top;                    /* sel: a game row; top: the first row shown */
#define ROW 45
#define Y0 108
#define VIS 12
static int hint(int xr, pic *b, const char *word)
{
	int tw = text_w(&f_chrome, word);
	text(&f_chrome, xr - tw, 666, 28, word, MUTED);
	if(b->px) blit(b, xr - tw - 6 - b->w, 680 - b->h / 2);
	return xr - tw - 6 - (b->px ? b->w : 0) - 14;
}
static void fit(char *s, face *f, int w)   /* cut a name to fit, with "..." */
{
	if(text_w(f, s) <= w) return;
	size_t n = strlen(s);
	while(n > 0 && (s[n] = 0, text_w(f, s) + text_w(f, "...") > w)) n--;
	strcat(s, "...");
}
static void draw_ui(void)
{
	char buf[256];
	time_t now_t = time(0); struct tm tm; localtime_r(&now_t, &tm);
	strftime(buf, sizeof buf, "%a %d %b %l:%M %p", &tm);
	text(&f_chrome, 64, 26, 28, "Console Mode", MUTED);
	text_r(&f_chrome, 1216, 26, 28, buf, MUTED);
	text(&f_chrome, 64, 666, 28, "Memory Cards / Super MemCard (Port 2)", MUTED);
	int x = hint(1216, &btn_a, "OK"); hint(x, &btn_b, "Back");
	for(int i = 0; i < VIS && top + i < nr; i++) {
		row *r = &R[top + i];
		int y = Y0 + i * ROW;
		if(r->head) {
			snprintf(buf, sizeof buf, "%s", r->title);
			text(&f_list, 254, y, 37, buf, ACCENT);
			snprintf(buf, sizeof buf, "Card %d  >", r->card);
			text_r(&f_list, 1026, y, 37, buf, MUTED);
			continue;
		}
		if(top + i == sel) rrect(243, y, 794, 37, 8, ACCENT);
		blit(&r->icon, 256, y + 2);
		snprintf(buf, sizeof buf, "%s", r->name);
		fit(buf, &f_list, 1026 - 296 - 230);
		text(&f_list, 300, y, 37, buf, TEXT);
		text_r(&f_list, 1026, y, 37, r->when, top + i == sel ? TEXT : MUTED);
	}
}
static void present(void) { for(int y = 0; y < H; y++) memcpy(fbm + (size_t)y * stride, &bb[y * W], W * 4); }
static int next_game(int from) { for(int i = from + 1; i < nr; i++) if(!R[i].head) return i; for(int i = 0; i < nr; i++) if(!R[i].head) return i; return 0; }

/* ------------------------------------------------------------------ the controllers, its own while it runs */
static struct pollfd in_fd[32];
static int n_in;
static void input_open(void)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	while(d && (e = readdir(d)) && n_in < 32) {
		if(strncmp(e->d_name, "event", 5)) continue;
		char p[300]; snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
		int fd = open(p, O_RDONLY | O_NONBLOCK);
		if(fd < 0) continue;
		ioctl(fd, EVIOCGRAB, 1);
		in_fd[n_in].fd = fd; in_fd[n_in].events = POLLIN; n_in++;
	}
	if(d) closedir(d);
}
static void input_close(void) { for(int i = 0; i < n_in; i++) { ioctl(in_fd[i].fd, EVIOCGRAB, 0); close(in_fd[i].fd); } n_in = 0; }
/* waits up to ms for a press: -1 up, 1 down, 2 another button (leave), 0 none */
static int input_wait(int ms)
{
	if(!n_in) { usleep(ms * 1000); return 0; }
	if(poll(in_fd, n_in, ms) <= 0) return 0;
	struct input_event ev;
	for(int i = 0; i < n_in; i++)
		while(read(in_fd[i].fd, &ev, sizeof ev) == sizeof ev) {
			if(ev.type == EV_KEY && ev.value == 1) {
				if(ev.code == KEY_UP || ev.code == BTN_DPAD_UP) return -1;
				if(ev.code == KEY_DOWN || ev.code == BTN_DPAD_DOWN) return 1;
				return 2;
			}
			if(ev.type == EV_ABS && (ev.code == ABS_HAT0Y) && ev.value) return ev.value < 0 ? -1 : 1;
		}
	return 0;
}

int main(int argc, char **argv)
{
	int a = 1;
	const char *from = NULL, *save = NULL;
	while(a + 1 < argc && argv[a][0] == '-') {
		if(!strcmp(argv[a], "-T")) { if(!state_set++) nstate = 0; if(nstate < 4) state_dirs[nstate++] = argv[a + 1]; }
		else if(!strcmp(argv[a], "-f")) from = argv[a + 1];
		else if(!strcmp(argv[a], "-s")) save = argv[a + 1];
		a += 2;
	}
	double secs = a < argc ? atof(argv[a]) : 30;
	if(read_section(from, save) <= 0) { fprintf(stderr, "smcview: nothing from smcd (no card?)\n"); return 1; }
	if(save && secs <= 0) return 0;   /* only saved */
	int fd = open("/dev/fb0", O_RDWR);
	if(fd < 0) { perror("/dev/fb0"); return 1; }
	struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
	ioctl(fd, FBIOGET_VSCREENINFO, &v); ioctl(fd, FBIOGET_FSCREENINFO, &f);
	if(v.xres != W || v.yres != H || v.bits_per_pixel != 32) { fprintf(stderr, "the framebuffer is %dx%d %d bits, not 1280x720\n", v.xres, v.yres, v.bits_per_pixel); return 1; }
	stride = f.line_length;
	fbm = mmap(0, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	FILE *ff = fopen("Outfit-Medium.ttf", "rb");
	if(!ff) { fprintf(stderr, "no Outfit-Medium.ttf here\n"); return 1; }
	static unsigned char font[400000]; size_t n = fread(font, 1, sizeof font, ff); fclose(ff); (void)n;
	stbtt_fontinfo fi; stbtt_InitFont(&fi, font, 0);
	face_make(&f_list, &fi, 0.030f * H); face_make(&f_chrome, &fi, 0.024f * H);
	btn_a = load("cmrec/btn_a.raw"); btn_b = load("cmrec/btn_b.raw");
	gradient_init();
	ribbon_texture(rtex, 512, 1024);
	static uint32_t saved[W * H];          /* the screen as it was, put back at the end */
	for(int y = 0; y < H; y++) memcpy(&saved[y * W], fbm + (size_t)y * stride, W * 4);
	input_open();
	sel = next_game(-1); top = 0;
	double t0 = now(), tsel = t0;
	for(;;) {
		draw_bg(0);                         /* still: the ribbons where Console Mode has them at rest */
		draw_ui(); present();
		int k = 0;
		while(!k) {
			if(secs > 0 && now() - t0 >= secs) { k = 2; break; }
			k = input_wait(100);
			if(!k && now() - tsel >= 3) k = 3;   /* left alone: the next one */
		}
		if(k == 2) break;
		tsel = now();
		if(k < 0) { int p = sel; for(int i = sel - 1; i >= 0; i--) if(!R[i].head) { p = i; break; } sel = p; }
		else sel = next_game(sel);
		if(sel == next_game(-1)) top = 0;
		if(sel - 1 < top) top = sel - 1 < 0 ? 0 : sel - 1;   /* its system's heading in view too */
		if(sel >= top + VIS) top = sel - VIS + 1;
	}
	input_close();
	for(int y = 0; y < H; y++) memcpy(fbm + (size_t)y * stride, &saved[y * W], W * 4);
	printf("smcview: %d rows\n", nr);
	return 0;
}
