/* VPI bridge: the bus simulation's card is the virtual card (sim/card vcardd: sd2psXtd's own PS1 code with
 * Super MemCard), reached over vcardd's socket. Verilog calls:
 *   $vcard_open           connect to $VCARD_SOCKET (default /tmp/supermemcard-sim.sock); 0 ok
 *   $vcard_sel(selected)  SEL low (1) / high (0)
 *   $vcard_byte(b)        the byte the host just clocked out; returns {ack, next[7:0]}: whether the card
 *                         ACKed it, and the byte it put up for the next transfer (FF: none)
 * Simulation time stands still during a call; the card's own processing happens in real time meanwhile. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vpi_user.h>

enum { OP_SELECT, OP_XFER, OP_XFER_CTRL, OP_SETTLE, OP_PATH, OP_STOP, OP_PEEK }; /* sim/card/host/vcard_wire.h */
struct req { uint8_t op, a, b, last; };
struct rsp { uint8_t byte, ack; char path[256]; };

static int fd = -1;

static int call(uint8_t op, uint8_t a, struct rsp *r) {
    struct req q = {op, a, 0, 0};
    if (fd < 0 || send(fd, &q, sizeof(q), 0) != sizeof(q) || recv(fd, r, sizeof(*r), 0) != sizeof(*r)) {
        vpi_printf("vcard: no answer from vcardd\n");
        vpi_control(vpiFinish, 1);
        return -1;
    }
    return 0;
}

static int arg_int(vpiHandle call_h) {
    vpiHandle it = vpi_iterate(vpiArgument, call_h), a = vpi_scan(it);
    s_vpi_value v = {.format = vpiIntVal};
    vpi_get_value(a, &v);
    vpi_free_object(it);
    return v.value.integer;
}

static void ret_int(vpiHandle call_h, int x) {
    s_vpi_value v = {.format = vpiIntVal};
    v.value.integer = x;
    vpi_put_value(call_h, &v, NULL, vpiNoDelay);
}

static PLI_INT32 open_tf(PLI_BYTE8 *u) {
    (void)u;
    vpiHandle h = vpi_handle(vpiSysTfCall, NULL);
    const char *path = getenv("VCARD_SOCKET");
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path ? path : "/tmp/supermemcard-sim.sock", sizeof(a.sun_path) - 1);
    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    int ok = fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0;
    ret_int(h, ok ? 0 : -1);
    return 0;
}

static PLI_INT32 sel_tf(PLI_BYTE8 *u) {
    (void)u;
    struct rsp r;
    call(OP_SELECT, (uint8_t)arg_int(vpi_handle(vpiSysTfCall, NULL)), &r);
    return 0;
}

static PLI_INT32 byte_tf(PLI_BYTE8 *u) {
    (void)u;
    vpiHandle h = vpi_handle(vpiSysTfCall, NULL);
    struct rsp r, p;
    if (call(OP_XFER, (uint8_t)arg_int(h), &r) || call(OP_PEEK, 0, &p))
        return 0;
    ret_int(h, (r.ack ? 0x100 : 0) | p.byte);
    return 0;
}

static PLI_INT32 size32(PLI_BYTE8 *u) { (void)u; return 32; }

static void reg(const char *name, PLI_INT32 (*fn)(PLI_BYTE8 *), int func) {
    s_vpi_systf_data d;
    memset(&d, 0, sizeof(d));
    d.type = func ? vpiSysFunc : vpiSysTask;
    d.sysfunctype = vpiIntFunc;
    d.tfname = (PLI_BYTE8 *)name;
    d.calltf = fn;
    d.sizetf = func ? size32 : NULL;
    vpi_register_systf(&d);
}

static void register_all(void) {
    reg("$vcard_open", open_tf, 1);
    reg("$vcard_sel", sel_tf, 0);
    reg("$vcard_byte", byte_tf, 1);
}

void (*vlog_startup_routines[])(void) = {register_all, 0};
