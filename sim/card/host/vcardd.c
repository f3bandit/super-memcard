/* vcardd: the virtual card as a service, for a host on the SS1 (a test Main) to talk to
 * instead of the front port while there's no real card.
 *
 *   vcardd <sd-folder> [socket]      socket default /tmp/supermemcard.sock
 *
 * The card powers up at start and stays powered across connections; SIGTERM/SIGINT or an
 * OP_STOP request power it off cleanly (everything flushed to the SD folder). Settings (the
 * card's flash) are kept in <sd-folder>/.vcard-flash between runs. */
#define _GNU_SOURCE
#include "host_settings.h"
#include "vcard_server.h"
#include "vcard_wire.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

void host_set_flash(struct host_settings *f);

static struct host_settings flash = HOST_SETTINGS_DEFAULTS;
static char flash_path[512];
static volatile sig_atomic_t quit;
static void on_signal(int sig) { (void)sig; quit = 1; }

static void flash_load(void) {
    FILE *f = fopen(flash_path, "rb");
    if (f) {
        struct host_settings s;
        if (fread(&s, sizeof(s), 1, f) == 1)
            flash = s;
        fclose(f);
    }
}
static void flash_save(void) {
    FILE *f = fopen(flash_path, "wb");
    if (f) {
        fwrite(&flash, sizeof(flash), 1, f);
        fclose(f);
    }
}

static void power_off(void) {
    vcs_stop();
    flash_save();
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: vcardd <sd-folder> [socket]\n");
        return 2;
    }
    const char *sock_path = argc > 2 ? argv[2] : "/tmp/supermemcard.sock";
    snprintf(flash_path, sizeof(flash_path), "%s/.vcard-flash", argv[1]);
    flash_load();
    host_set_flash(&flash);

    struct sigaction sa = {0};
    sa.sa_handler = on_signal; /* no SA_RESTART: accept()/recv() return on a signal */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int ls = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock_path);
    unlink(sock_path);
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(ls, 1) != 0) {
        perror("vcardd: socket");
        return 1;
    }
    if (vcs_start(argv[1]) != 0) {
        fprintf(stderr, "vcardd: can't use %s as the SD card\n", argv[1]);
        return 1;
    }
    printf("vcardd: card on %s, SD card %s\n", sock_path, argv[1]);
    fflush(stdout);

    while (!quit) {
        int s = accept(ls, NULL, NULL);
        if (s < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        for (;;) {
            struct vcard_req q;
            struct vcard_rsp r;
            if (recv(s, &q, sizeof(q), 0) != sizeof(q))
                break; /* host disconnected; the card stays powered */
            if (!vcard_handle(&q, &r)) {
                flash_save();
                send(s, &r, sizeof(r), 0);
                close(s);
                unlink(sock_path);
                return 0;
            }
            if (send(s, &r, sizeof(r), 0) != sizeof(r))
                break;
        }
        close(s);
    }
    power_off();
    unlink(sock_path);
    printf("vcardd: powered off\n");
    return 0;
}
