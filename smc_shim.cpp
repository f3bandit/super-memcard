// Super MemCard: the shim between a MiSTer Main and smcd (smc_shim.h). Copyright (C) 2026 f3bandit.
// GPL-3.0-or-later, as Main_MiSTer.
#include "smc_shim.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "smc_frontport.h"
#include "user_io.h"   // user_io_get_core_name, is_menu

#define SMCD_SOCK  "/tmp/smcd.sock"
#define FRONT_SOCK "/tmp/smc_frontport.sock"

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

// pass smcd's frames: accept it, read its lines, answer them. wait_ms: how long to wait for something to do
static void serve(int wait_ms)
{
	listen_front();
	if (listen_fd < 0) return;
	struct pollfd p[2] = { { listen_fd, POLLIN, 0 }, { peer_fd, POLLIN, 0 } };
	if (poll(p, peer_fd >= 0 ? 2 : 1, wait_ms) <= 0) return;
	if (p[0].revents & POLLIN)
	{
		int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
		if (fd >= 0)
		{
			if (peer_fd >= 0) close(peer_fd);   // smcd keeps one connection; a new one replaces it
			peer_fd = fd;
			peer_len = 0;
		}
		return;
	}
	if (peer_fd < 0 || !(p[1].revents & (POLLIN | POLLHUP | POLLERR))) return;
	ssize_t r = recv(peer_fd, peer_buf + peer_len, sizeof(peer_buf) - 1 - peer_len, MSG_DONTWAIT);
	if (r <= 0)
	{
		if (r < 0 && (errno == EAGAIN || errno == EINTR)) return;
		close(peer_fd);
		peer_fd = -1;
		smc_frontport_hold(0);   // smcd went away: the front ports are the core's again
		return;
	}
	peer_len += (size_t)r;
	peer_buf[peer_len] = 0;
	char *nl;
	while ((nl = strchr(peer_buf, '\n')))
	{
		*nl = 0;
		serve_line(peer_buf);
		size_t used = (size_t)(nl + 1 - peer_buf);
		memmove(peer_buf, nl + 1, peer_len - used + 1);
		peer_len -= used;
	}
	if (peer_len >= sizeof(peer_buf) - 1) peer_len = 0;   // a line too long: dropped
}

void smc_shim_poll()
{
	static char core[64];
	if (strncmp(core, user_io_get_core_name(), sizeof(core) - 1))
	{
		snprintf(core, sizeof(core), "%s", user_io_get_core_name());
		smc_frontport_reset();   // another core: ask its driver again
	}
	serve(0);
}

// tell smcd; with wait, wait for its DONE (passing frames meanwhile)
static void tell(const char *line, int wait)
{
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
			if (strstr(buf, "DONE")) break;
			if (len >= sizeof(buf) - 1) len = 0;
		}
	}
	close(fd);
}

void smc_shim_core_start(const char *rbf, const char *mgl)
{
	if (!is_menu() || !rbf) return;
	char l[1200];
	snprintf(l, sizeof(l), "LAUNCH %s\t%s\n", rbf, mgl ? mgl : "");
	tell(l, 1);
}

void smc_shim_menu_start()
{
	if (is_menu()) tell("MENU\n", 0);
}
