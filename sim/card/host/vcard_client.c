/* The test side of the virtual card. Every power-up is a new card process forked from this
 * one (which never runs firmware code itself, so each boot starts from a clean RP2040); the
 * bus is a socket to it. Power-off is that process ending. The card's flash (its settings) is
 * memory shared between the two, so it outlives power-offs as on the real card. */
#define _GNU_SOURCE
#include "vcard.h"
#include "vcard_server.h"
#include "host_settings.h"
#include "vcard_wire.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

void host_set_flash(struct host_settings *f);

static struct host_settings *flash;
static int sock = -1;
static pid_t card = -1;

struct host_settings *host_settings(void) {
    if (!flash) {
        flash = mmap(NULL, sizeof(*flash), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (flash == MAP_FAILED)
            abort();
        *flash = (struct host_settings)HOST_SETTINGS_DEFAULTS;
    }
    return flash;
}

static void serve(int s, const char *sd_root) {
    host_set_flash(host_settings());
    struct vcard_rsp r;
    memset(&r, 0, sizeof(r));
    r.byte = vcs_start(sd_root) == 0;
    if (send(s, &r, sizeof(r), 0) != sizeof(r))
        _exit(3);
    for (;;) {
        struct vcard_req q;
        if (recv(s, &q, sizeof(q), 0) != sizeof(q))
            _exit(0); /* test side went away: power gone */
        bool more = vcard_handle(&q, &r);
        if (send(s, &r, sizeof(r), 0) != sizeof(r) || !more)
            _exit(0);
    }
}

static struct vcard_rsp call(uint8_t op, uint8_t a, uint8_t b, bool last) {
    struct vcard_req q = {op, a, b, last};
    struct vcard_rsp r;
    memset(&r, 0, sizeof(r));
    if (sock < 0 || send(sock, &q, sizeof(q), 0) != sizeof(q) || recv(sock, &r, sizeof(r), 0) != sizeof(r)) {
        fprintf(stderr, "virtual card: no card process\n");
        abort();
    }
    return r;
}

int vcard_start(const char *sd_root) {
    int sv[2];
    host_settings();
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0)
        return -1;
    fflush(NULL);
    card = fork();
    if (card < 0)
        return -1;
    if (card == 0) {
        close(sv[0]);
        serve(sv[1], sd_root);
        _exit(0);
    }
    close(sv[1]);
    sock = sv[0];
    struct vcard_rsp r;
    if (recv(sock, &r, sizeof(r), 0) != sizeof(r) || !r.byte) {
        vcard_power_cut();
        return -1;
    }
    return 0;
}

static void reap(void) {
    int st;
    if (card > 0)
        waitpid(card, &st, 0);
    card = -1;
    if (sock >= 0)
        close(sock);
    sock = -1;
}

void vcard_stop(void) {
    if (sock < 0)
        return;
    call(OP_STOP, 0, 0, false);
    reap();
}

void vcard_power_cut(void) {
    if (card > 0)
        kill(card, SIGKILL);
    reap();
}

void vcard_select(bool selected) { call(OP_SELECT, selected, 0, false); }
uint8_t vcard_xfer(uint8_t cmd, bool last, bool *ack) {
    struct vcard_rsp r = call(OP_XFER, cmd, 0, last);
    if (ack)
        *ack = r.ack;
    return r.byte;
}
uint8_t vcard_xfer_ctrl(uint8_t cmd, uint8_t dat, bool last, bool *ack) {
    struct vcard_rsp r = call(OP_XFER_CTRL, cmd, dat, last);
    if (ack)
        *ack = r.ack;
    return r.byte;
}
void vcard_settle(void) { call(OP_SETTLE, 0, 0, false); }
void vcard_key(int key) { call(OP_KEY, (uint8_t)key, 0, false); }
const char *vcard_card_path(void) {
    static char p[256];
    struct vcard_rsp r = call(OP_PATH, 0, 0, false);
    memcpy(p, r.path, sizeof(p));
    return p;
}
