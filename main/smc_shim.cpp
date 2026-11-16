// Super MemCard: the shim between a MiSTer Main and smcd (smc_shim.h). Copyright (C) 2026 f3bandit.
// GPL-3.0-or-later, as Main_MiSTer.
#include "smc_shim.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/wait.h>
#include <time.h>

#include "smc_frontport.h"
#include "file_io.h"   // getRootDir
#include "user_io.h"   // user_io_get_core_name, is_menu

#define SMCD_SOCK  "/tmp/smcd.sock"
#define FRONT_SOCK "/tmp/smc_frontport.sock"
// the game running with its saves on the card, for the OSD's Saves page: its MGL's (or MRA's) path, and a copy of an
// MGL (the restart's, should the first one be gone). In /tmp: Main starts again after each core, and these outlive it
#define GAME_FILE  "/tmp/smc_game"
#define GAME_COPY  "/tmp/smc_game.mgl"
// the core smcd last heard of ("<rbf>\n<mgl>\n"), and a game waiting for the menu core to reach the card first (the
// bounce: a core without the card driver, a game or core picked in its OSD): "<rbf>\n<mgl>\n", with its MGL
#define CORE_FILE   "/tmp/smc_core"
#define BOUNCE_FILE "/tmp/smc_bounce"
#define BOUNCE_MGL  "/tmp/smc_bounce.mgl"

int xml_load(const char *xml);   // Main's MGL/MRA loader (support/arcade/mra_loader.cpp)
int fpga_load_rbf(const char *name, const char *cfg, const char *xml);   // Main's core loader (fpga_io.cpp)

static int restarting;           // the OSD's Saves page restarts the game: LAUNCH again, from the game's core

static int listen_fd = -1, peer_fd = -1;
static char peer_buf[1024];
static size_t peer_len;

static int unix_connect(const char *path)
{
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	struct sockaddr_un a;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a))) { close(fd); return -1; }
	return fd;
}

static void send_line(int fd, const char *s)
{
	size_t n = strlen(s), o = 0;
	while (o < n)
	{
		ssize_t w = send(fd, s + o, n - o, MSG_NOSIGNAL);
		if (w < 0 && errno == EINTR) continue;
		if (w <= 0) return;
		o += (size_t)w;
	}
}

static void listen_front()
{
	if (listen_fd >= 0) return;
	unlink(FRONT_SOCK);
	listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (listen_fd < 0) return;
	struct sockaddr_un a;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", FRONT_SOCK);
	if (bind(listen_fd, (struct sockaddr *)&a, sizeof(a)) || listen(listen_fd, 2))
	{
		close(listen_fd);
		listen_fd = -1;
	}
}

static int hexval(int c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// one request from smcd
static void serve_line(char *l)
{
	char out[600];
	if (!strncmp(l, "FRAME ", 6))
	{
		int port, speed, ms, used = 0;
		if (sscanf(l + 6, "%d %d %d %n", &port, &speed, &ms, &used) < 3) { send_line(peer_fd, "ERR -1\n"); return; }
		const char *h = l + 6 + used;
		uint8_t o[255], in[255];
		int len = 0;
		while (len < 255 && hexval(h[2 * len]) >= 0 && hexval(h[2 * len + 1]) >= 0)
		{
			o[len] = (uint8_t)(hexval(h[2 * len]) << 4 | hexval(h[2 * len + 1]));
			len++;
		}
		int n = smc_frontport_raw(port, o, in, len, ms, speed);
		if (n < 0) { snprintf(out, sizeof(out), "ERR %d\n", n); send_line(peer_fd, out); return; }
		int k = snprintf(out, sizeof(out), "OK %d ", n);
		for (int i = 0; i < len && k + 3 < (int)sizeof(out); i++) k += snprintf(out + k, sizeof(out) - k, "%02x", in[i]);
		snprintf(out + k, sizeof(out) - k, "\n");
		send_line(peer_fd, out);
	}
	else if (!strncmp(l, "HOLD ", 5))
	{
		smc_frontport_hold(atoi(l + 5) ? 1 : 0);
		send_line(peer_fd, "OK\n");
	}
	else send_line(peer_fd, "ERR -1\n");
}

// pass smcd's frames: accept it, read its lines, answer them. wait_ms: how long to wait for something to do. Returns
// how many of smcd's requests it answered
static int serve(int wait_ms)
{
	listen_front();
	if (listen_fd < 0) return 0;
	struct pollfd p[2] = { { listen_fd, POLLIN, 0 }, { peer_fd, POLLIN, 0 } };
	if (poll(p, peer_fd >= 0 ? 2 : 1, wait_ms) <= 0) return 0;
	if (p[0].revents & POLLIN)
	{
		int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
		if (fd >= 0)
		{
			if (peer_fd >= 0) close(peer_fd);   // smcd keeps one connection; a new one replaces it
			peer_fd = fd;
			peer_len = 0;
		}
		return 0;
	}
	if (peer_fd < 0 || !(p[1].revents & (POLLIN | POLLHUP | POLLERR))) return 0;
	ssize_t r = recv(peer_fd, peer_buf + peer_len, sizeof(peer_buf) - 1 - peer_len, MSG_DONTWAIT);
	if (r <= 0)
	{
		if (r < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
		close(peer_fd);
		peer_fd = -1;
		smc_frontport_hold(0);   // smcd went away: the front ports are the core's again
		return 0;
	}
	peer_len += (size_t)r;
	peer_buf[peer_len] = 0;
	char *nl;
	int answered = 0;
	while ((nl = strchr(peer_buf, '\n')))
	{
		*nl = 0;
		serve_line(peer_buf);
		answered++;
		size_t used = (size_t)(nl + 1 - peer_buf);
		memmove(peer_buf, nl + 1, peer_len - used + 1);
		peer_len -= used;
	}
	if (peer_len >= sizeof(peer_buf) - 1) peer_len = 0;   // a line too long: dropped
	return answered;
}

static int read_two(const char *file, char *a, size_t an, char *b, size_t bn)
{
	a[0] = b[0] = 0;
	FILE *f = fopen(file, "r");
	if (!f) return 0;
	if (fgets(a, (int)an, f)) a[strcspn(a, "\n")] = 0;
	if (fgets(b, (int)bn, f)) b[strcspn(b, "\n")] = 0;
	fclose(f);
	return a[0] != 0;
}

static void write_two(const char *file, const char *a, const char *b)
{
	FILE *f = fopen(file, "w");
	if (!f) return;
	fprintf(f, "%s\n%s\n", a ? a : "", b ? b : "");
	fclose(f);
}

static void bounce_go();

// The children this Main got from the program it replaced (a Main started by exec, as MiSTer_Quake's wrapper goes back
// to the menu: its quake-mister): nobody else waits for them, so they're waited for here when they end (no zombies
// left). Only those: the children this Main starts itself are its own to wait for (system(), popen()).
static int inherited[16], n_inherited;
static void find_inherited()
{
	n_inherited = 0;
	DIR *d = opendir("/proc");
	if (!d) return;
	struct dirent *e;
	pid_t me = getpid();
	while ((e = readdir(d)) && n_inherited < (int)(sizeof(inherited) / sizeof(inherited[0])))
	{
		int pid = atoi(e->d_name);
		if (pid <= 0 || pid == me) continue;
		char p[64], st[512];
		snprintf(p, sizeof(p), "/proc/%d/stat", pid);
		FILE *f = fopen(p, "r");
		if (!f) continue;
		size_t n = fread(st, 1, sizeof(st) - 1, f);
		fclose(f);
		st[n] = 0;
		const char *r = strrchr(st, ')');   // pid (comm) state ppid ...
		int ppid = 0;
		char state;
		if (r && sscanf(r + 1, " %c %d", &state, &ppid) == 2 && ppid == me) inherited[n_inherited++] = pid;
	}
	closedir(d);
}
static void reap_inherited()
{
	static struct timespec last;
	struct timespec t;
	if (!n_inherited) return;
	clock_gettime(CLOCK_MONOTONIC, &t);
	if ((t.tv_sec - last.tv_sec) * 1000 + (t.tv_nsec - last.tv_nsec) / 1000000 < 500) return;
	last = t;
	for (int i = 0; i < n_inherited;)
	{
		pid_t r = waitpid(inherited[i], NULL, WNOHANG);
		if (r == inherited[i] || (r < 0 && errno == ECHILD)) inherited[i] = inherited[--n_inherited];
		else i++;
	}
}

void smc_shim_poll()
{
	reap_inherited();
	static char core[64];
	if (strncmp(core, user_io_get_core_name(), sizeof(core) - 1))
	{
		snprintf(core, sizeof(core), "%s", user_io_get_core_name());
		smc_frontport_reset();   // another core: ask its driver again
	}
	if (is_menu() && !access(BOUNCE_FILE, R_OK)) bounce_go();   // back in the menu core for a bounce: the game now
	// smcd sends its frames one after another, each waiting for the last one's answer: while it's sending, the next
	// frames are passed here (a few ms each) rather than one a turn of Main's loop (a 128 KB card: a minute, not 5 s).
	// At most 20 ms of Main's loop at a time in the menu, 4 ms while a core runs (its input stays prompt)
	if (!serve(0)) return;
	int budget = is_menu() ? 20 : 4;
	struct timespec t0, t;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;)
	{
		if (!serve(2)) return;   // smcd paused: Main's loop goes on
		clock_gettime(CLOCK_MONOTONIC, &t);
		if ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000 >= budget) return;
	}
}

// tell smcd itself (a Main with no Coreshell installed: Console Mode's); with wait, wait for its DONE (passing frames
// meanwhile) and give its answer in reply
static void tell_smcd(const char *line, int wait, char *reply, size_t rn)
{
	if (reply && rn) reply[0] = 0;
	listen_front();   // before smcd hears of it: it connects to pass its frames straight away
	int fd = unix_connect(SMCD_SOCK);
	if (fd < 0) return;   // no smcd: Super MemCard isn't installed or running
	send_line(fd, line);
	if (!wait) { close(fd); return; }
	char buf[64];
	size_t len = 0;
	for (int t = 0; t < 3000; t++)   // 30 s
	{
		serve(0);
		struct pollfd p = { fd, POLLIN, 0 };
		if (poll(&p, 1, 10) > 0)
		{
			ssize_t r = recv(fd, buf + len, sizeof(buf) - 1 - len, 0);
			if (r <= 0) break;
			len += (size_t)r;
			buf[len] = 0;
			if (strstr(buf, "DONE")) { if (reply) snprintf(reply, rn, "%s", buf); break; }
			if (len >= sizeof(buf) - 1) len = 0;
		}
	}
	close(fd);
}

// Coreshell installed (Phosphor): it owns every launch's card steps and their log, so smcd is told through it (its
// card_event), never straight; Coreshell being restarted (its supervisor): up to 3 s for it, the frames still passed
static int coreshell_installed()
{
	char p[1024];
	snprintf(p, sizeof(p), "%s/coreshell/coreshell.sh", getRootDir());
	return !access(p, X_OK);
}

static int smcd_up()
{
	int fd = unix_connect(SMCD_SOCK);
	if (fd < 0) return 0;
	close(fd);
	return 1;
}

#define CORESHELL_SOCK "/tmp/coreshell.sock"
// tell smcd; with wait, wait for its DONE (passing frames meanwhile: smcd reads the card through them), in reply
static void tell(const char *line, int wait, char *reply = NULL, size_t rn = 0)
{
	if (reply && rn) reply[0] = 0;
	listen_front();   // before smcd hears of it: it connects to pass its frames straight away
	if (!smcd_up()) return;   // no smcd: Super MemCard isn't installed or running
	if (!coreshell_installed()) { tell_smcd(line, wait, reply, rn); return; }
	int fd = -1;
	for (int t = 0; t < 300 && fd < 0; t++)   // 3 s
	{
		fd = unix_connect(CORESHELL_SOCK);
		if (fd < 0) serve(10);
	}
	if (fd < 0) { printf("Super MemCard: Coreshell isn't running: the card wasn't told (%.40s)\n", line); return; }
	char req[2600];
	size_t k = (size_t)snprintf(req, sizeof(req), "{\"v\":1,\"cmd\":\"card_event\",\"line\":\"");
	for (const char *c = line; *c && k + 8 < sizeof(req); c++)   // JSON-escaped
	{
		if (*c == '\n') continue;
		if (*c == '"' || *c == '\\') { req[k++] = '\\'; req[k++] = *c; }
		else if (*c == '\t') { req[k++] = '\\'; req[k++] = 't'; }
		else if ((unsigned char)*c < 0x20) k += (size_t)snprintf(req + k, sizeof(req) - k, "\\u%04x", *c);
		else req[k++] = *c;
	}
	snprintf(req + k, sizeof(req) - k, "\"}\n");
	send_line(fd, req);
	char buf[512];
	size_t len = 0;
	int tries = wait ? 3500 : 300;   // 35 s (smcd's own 30 s and Coreshell's), the menu's: 3 s (smcd says DONE first)
	for (int t = 0; t < tries; t++)
	{
		serve(0);
		struct pollfd p = { fd, POLLIN, 0 };
		if (poll(&p, 1, 10) > 0)
		{
			ssize_t r = recv(fd, buf + len, sizeof(buf) - 1 - len, 0);
			if (r <= 0) break;
			len += (size_t)r;
			buf[len] = 0;
			if (strchr(buf, '\n'))
			{
				const char *q = strstr(buf, "\"reply\": \"");
				if (q && reply)
				{
					q += 10;
					size_t e = strcspn(q, "\"");
					snprintf(reply, rn, "%.*s", (int)e, q);
				}
				break;
			}
			if (len >= sizeof(buf) - 1) len = 0;
		}
	}
	close(fd);
}

static void copy_file(const char *from, const char *to)
{
	FILE *a = fopen(from, "rb");
	if (!a) return;
	FILE *b = fopen(to, "wb");
	if (b)
	{
		char t[4096];
		size_t n;
		while ((n = fread(t, 1, sizeof(t), a)) > 0) fwrite(t, 1, n, b);
		fclose(b);
	}
	fclose(a);
}


// the running core has the card driver (core/sys/smc_port.sv): the card can be reached while it runs
static int core_has_driver()
{
	return smc_frontport_raw(1, NULL, NULL, 0, 1, 0) != -1;
}

// a core without the card driver: through the menu core (it reaches the card) and back, smcd seeing the change
static void bounce(const char *rbf, const char *mgl)
{
	write_two(BOUNCE_FILE, rbf, mgl);
	printf("Super MemCard: through the menu core to reach the card, then %s %s\n", rbf, mgl ? mgl : "");
	fpga_load_rbf("menu.rbf", NULL, NULL);   // Main starts again with the menu core: smc_shim_poll() goes on
}

static void bounce_go()
{
	char rbf[1024], mgl[1024];
	int ok = read_two(BOUNCE_FILE, rbf, sizeof(rbf), mgl, sizeof(mgl));
	unlink(BOUNCE_FILE);   // once only, whatever happens next
	if (!ok) return;
	fpga_load_rbf(rbf, NULL, mgl[0] ? mgl : NULL);   // from the menu: LAUNCH, as any game
}

// a menu core by its path ("/media/fat/menu.rbf" as load_core gives it, Console Mode's menu_ConsoleMode.rbf): not a game
static int is_menu_rbf(const char *rbf)
{
	const char *b = strrchr(rbf, '/');
	return !strncasecmp(b ? b + 1 : rbf, "menu", 4);
}

void smc_shim_core_start(const char *rbf, const char *mgl)
{
	if (rbf && is_menu_rbf(rbf)) return;   // the menu's start (smc_shim_main_start) tells smcd
	if (!is_menu() && !restarting)
	{
		// a core loaded from a game's OSD, not from the menu: the Saves page's game is that one's no longer
		unlink(GAME_FILE);
		unlink(GAME_COPY);
		if (!rbf || !smcd_up()) return;
		if (!core_has_driver()) { bounce(rbf, mgl); return; }   // the old game's save can't reach the card from here
		// the running core reaches the card: LAUNCH from here (smcd puts the old game's save on the card first)
	}
	if (!rbf) return;
	write_two(CORE_FILE, rbf, mgl);
	restarting = 0;
	int again = mgl && !strcmp(mgl, GAME_COPY);   // the restart's copy: the game's file stays what it was
	if (!again) { unlink(GAME_FILE); unlink(GAME_COPY); }
	char l[1200], reply[64];
	snprintf(l, sizeof(l), "LAUNCH %s\t%s\n", rbf, mgl ? mgl : "");
	tell(l, 1, reply, sizeof(reply));
	if (!strstr(reply, "DONE saves") || !mgl || !mgl[0]) { unlink(GAME_FILE); unlink(GAME_COPY); return; }
	if (again) return;
	FILE *f = fopen(GAME_FILE, "w");
	if (f) { fprintf(f, "%s\n", mgl); fclose(f); }
	size_t n = strlen(mgl);
	if (n > 4 && !strcasecmp(mgl + n - 4, ".mgl")) copy_file(mgl, GAME_COPY);
}

void smc_shim_menu_start()
{
	if (!is_menu()) return;   // Main starts again after every core: only the menu's start is the game's end
	unlink(GAME_FILE);
	unlink(GAME_COPY);
	unlink(CORE_FILE);
	if (!access(BOUNCE_FILE, R_OK)) return;   // a bounce: the next game's LAUNCH (smc_shim_poll) does it all
	tell("MENU\n", 0);
}

static const char *base_name(const char *p)
{
	const char *b = strrchr(p, '/');
	return b ? b + 1 : p;
}

void smc_shim_main_start(const char *rbf, const char *mgl)
{
	find_inherited();
	if (is_menu()) { smc_shim_menu_start(); return; }
	// Main started straight into a core: smcd heard of it if this Main's last fpga_load_rbf() said so (it restarts Main
	// after every core); else (a core started by something else) LAUNCH now, late: the core is already in
	char was[1024], wmgl[1024];
	read_two(CORE_FILE, was, sizeof(was), wmgl, sizeof(wmgl));
	if (!rbf || !rbf[0] || (was[0] && !strcasecmp(base_name(was), base_name(rbf)))) return;
	if (!smcd_up()) return;
	write_two(CORE_FILE, rbf, mgl);
	char l[2200], reply[64];
	snprintf(l, sizeof(l), "LAUNCH %s\t%s\n", rbf, mgl ? mgl : "");
	tell(l, 1, reply, sizeof(reply));
}

// an MGL starting the running core with this file: the running game's own setname kept (Game Boy Color on the Game Boy
// core, ...)
static int write_rom_mgl(const char *rbf, const char *path, char type, int index)
{
	char setname[512] = "";
	FILE *g = fopen(GAME_COPY, "r");
	if (g)
	{
		static char t[16384];
		size_t n = fread(t, 1, sizeof(t) - 1, g);
		t[n] = 0;
		fclose(g);
		char *a = strstr(t, "<setname"), *e = a ? strstr(a, "</setname>") : NULL;
		if (a && e && e - a + 10 < (long)sizeof(setname)) snprintf(setname, sizeof(setname), "%.*s", (int)(e - a + 10), a);
	}
	char r[1024];
	const char *root = getRootDir();
	size_t rl = strlen(root);
	snprintf(r, sizeof(r), "%s", !strncmp(rbf, root, rl) && rbf[rl] == '/' ? rbf + rl + 1 : rbf);
	size_t n = strlen(r);
	if (n > 4 && !strcasecmp(r + n - 4, ".rbf")) r[n - 4] = 0;
	FILE *f = fopen(BOUNCE_MGL, "w");
	if (!f) return 0;
	fprintf(f, "<mistergamedescription>\n\t<rbf>%s</rbf>\n", r);
	if (setname[0]) fprintf(f, "\t%s\n", setname);
	fprintf(f, "\t<file delay=\"1\" type=\"%c\" index=\"%d\" path=\"", type, index);
	for (const char *c = path; *c; c++)   // XML-escaped
	{
		if (*c == '&') fputs("&amp;", f);
		else if (*c == '"') fputs("&quot;", f);
		else if (*c == '<') fputs("&lt;", f);
		else if (*c == '>') fputs("&gt;", f);
		else fputc(*c, f);
	}
	fprintf(f, "\"/>\n</mistergamedescription>\n");
	fclose(f);
	return 1;
}

bool smc_shim_rom_load(const char *file, char type, int index)
{
	if (is_menu() || !file || !file[0]) return true;
	char path[1200];   // the OSD's paths are under Main's root ("../usb0/games/..."): whole, for smcd and the MGL
	if (file[0] == '/') snprintf(path, sizeof(path), "%s", file);
	else snprintf(path, sizeof(path), "%s/%s", getRootDir(), file);
	char rbf[1024], mgl[1024];
	if (!read_two(CORE_FILE, rbf, sizeof(rbf), mgl, sizeof(mgl))) return true;   // no game core smcd knows of
	int driver = core_has_driver();
	char l[2400], reply[64];
	snprintf(l, sizeof(l), "ROM %s\t%s\t%d\n", rbf, path, driver);
	tell(l, 1, reply, sizeof(reply));
	if (strstr(reply, "DONE bounce"))
	{
		if (!write_rom_mgl(rbf, path, type, index)) return true;
		unlink(GAME_FILE);
		unlink(GAME_COPY);
		bounce(rbf, BOUNCE_MGL);
		return false;   // (a Main whose fpga_load_rbf() comes back: the menu core is loading)
	}
	if (strstr(reply, "DONE saves"))   // the Saves page's game is this one now
	{
		unlink(GAME_COPY);
		FILE *f = fopen(GAME_FILE, "w");
		if (f && write_rom_mgl(rbf, path, type, index)) { fprintf(f, "%s\n", BOUNCE_MGL); copy_file(BOUNCE_MGL, GAME_COPY); }
		if (f) fclose(f);
	}
	else if (!strstr(reply, "DONE same")) { unlink(GAME_FILE); unlink(GAME_COPY); }
	return true;
}

bool smc_shim_busy()
{
	return peer_fd >= 0;   // smcd is connected: it's reading or writing the card through us
}

bool smc_shim_saves_available()
{
	return !is_menu() && (!access(GAME_FILE, R_OK) || !access(GAME_COPY, R_OK));
}

int smc_shim_ask(const char *cmd, char *out, size_t n, int timeout_ms)
{
	if (!out || n < 2) return -1;
	out[0] = 0;
	listen_front();
	int fd = unix_connect(SMCD_SOCK);
	if (fd < 0) return -1;
	char l[300];
	snprintf(l, sizeof(l), "%s\n", cmd);
	send_line(fd, l);
	size_t len = 0;
	int done = 0;
	for (int t = 0; t < timeout_ms / 10 && !done; t++)   // smcd reads the card through our frames meanwhile
	{
		serve(0);
		struct pollfd p = { fd, POLLIN, 0 };
		if (poll(&p, 1, 10) <= 0) continue;
		ssize_t r = recv(fd, out + len, n - 1 - len, 0);
		if (r <= 0) break;
		len += (size_t)r;
		out[len] = 0;
		char *e = strstr(out, "END\n");
		if (e && (e == out || e[-1] == '\n')) { *e = 0; len = (size_t)(e - out); done = 1; }
		if (len >= n - 1) break;   // too long for out: what fitted
	}
	close(fd);
	return done ? (int)len : -1;
}

void smc_shim_restart()
{
	char path[1024] = "";
	FILE *f = fopen(GAME_FILE, "r");
	if (f)
	{
		if (!fgets(path, sizeof(path), f)) path[0] = 0;
		fclose(f);
		path[strcspn(path, "\n")] = 0;
	}
	if (!path[0] || access(path, R_OK)) snprintf(path, sizeof(path), "%s", GAME_COPY);
	if (access(path, R_OK)) return;
	restarting = 1;   // LAUNCH again: smcd's card for the game from now on
	xml_load(path);
}
