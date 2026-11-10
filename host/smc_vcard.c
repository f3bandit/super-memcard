/* The Super MemCard bus over the virtual card's socket (sim/card vcardd), for hosts on the SS1
 * while there's no real card: the same bytes the front port would carry. */
#include "smc_vcard.h"

#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* sim/card/host/vcard_wire.h */
enum { OP_SELECT, OP_XFER, OP_XFER_CTRL, OP_SETTLE, OP_PATH, OP_STOP };
struct req { uint8_t op, a, b, last; };
struct rsp { uint8_t byte, ack; char path[256]; };

static bool call(int fd, uint8_t op, uint8_t a, bool last, struct rsp *r) {
    struct req q = {op, a, 0, last};
    return send(fd, &q, sizeof(q), 0) == sizeof(q) && recv(fd, r, sizeof(*r), 0) == sizeof(*r);
}

static void v_select(void *ctx, bool s) {
    struct rsp r;
    call(*(int *)ctx, OP_SELECT, s, false, &r);
}

static uint8_t v_xfer(void *ctx, uint8_t out, bool last, bool *ack) {
    struct rsp r;
    if (!call(*(int *)ctx, OP_XFER, out, last, &r)) {
        *ack = false;
        return 0xFF;
    }
    *ack = r.ack;
    return r.byte;
}

static int vfd = -1;

int smc_vcard_open(const char *path, struct smc_bus *bus) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path ? path : SMC_VCARD_SOCKET, sizeof(a.sun_path) - 1);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    vfd = fd;
    memset(bus, 0, sizeof(*bus));   /* byte by byte: no frame callback (left unset, it held whatever was there) */
    bus->ctx = &vfd;
    bus->select = v_select;
    bus->xfer = v_xfer;
    return 0;
}

void smc_vcard_close(struct smc_bus *bus) {
    (void)bus;
    if (vfd >= 0)
        close(vfd);
    vfd = -1;
}
