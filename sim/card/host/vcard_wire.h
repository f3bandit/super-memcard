/* The virtual card's bus over a socket: one request, one reply. Used between the test program
 * and its card process (vcard_client.c), and by vcardd for a host on the SS1 (Phosphor's Main). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { OP_SELECT, OP_XFER, OP_XFER_CTRL, OP_SETTLE, OP_PATH, OP_STOP, OP_PEEK, OP_KEY };
/* OP_KEY: a=0 left/prev, 1 right/next, 2 back, 3 enter: a press of the card's buttons on its main screen */
/* OP_PEEK: the byte the card has queued for the next transfer (FF: none), for a bit-level bus model */
struct vcard_req { uint8_t op, a, b, last; };      /* SELECT a=selected; XFER a=cmd; XFER_CTRL a=cmd b=dat */
struct vcard_rsp { uint8_t byte, ack; char path[256]; };

/* Carry out one request on the running card. Returns false for OP_STOP (after stopping it). */
bool vcard_handle(const struct vcard_req *q, struct vcard_rsp *r);
