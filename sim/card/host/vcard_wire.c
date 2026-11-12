#include "vcard_wire.h"
#include "vcard_server.h"

#include <stdio.h>
#include <string.h>

bool vcard_handle(const struct vcard_req *q, struct vcard_rsp *r) {
    bool ack = false;
    memset(r, 0, sizeof(*r));
    switch (q->op) {
    case OP_SELECT: vcs_select(q->a); break;
    case OP_XFER: r->byte = vcs_xfer(q->a, q->last, &ack); break;
    case OP_XFER_CTRL: r->byte = vcs_xfer_ctrl(q->a, q->b, q->last, &ack); break;
    case OP_SETTLE: vcs_settle(); break;
    case OP_PATH: snprintf(r->path, sizeof(r->path), "%s", vcs_card_path()); break;
    case OP_STOP: vcs_stop(); return false;
    case OP_PEEK: r->byte = vcs_peek(); break;
    case OP_KEY: vcs_key(q->a); break;
    case OP_SCREEN: snprintf(r->path, sizeof(r->path), "%s", vcs_screen()); break;
    case OP_BROWSE: snprintf(r->path, sizeof(r->path), "%s", vcs_browse()); break;
    }
    r->ack = ack;
    return true;
}
