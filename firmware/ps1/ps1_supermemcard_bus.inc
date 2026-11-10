/* Copyright (C) 2026 f3bandit. Super MemCard additions to sd2psXtd: GPL-3.0-or-later, as sd2psXtd.
 * This program is distributed WITHOUT ANY WARRANTY; see the GNU General Public License. */
/* Super MemCard: the bus side (core 1) of commands 81 60..68.
 *
 * Included into sd2psXtd's src/ps1/ps1_memory_card.c (see firmware/patches), so it uses that
 * file's own receive/respond helpers and timing. It never touches the SD card: it collects a
 * command's bytes, hands them to core 0 (ps1_supermemcard.c) and sends back core 0's answer.
 *
 * Until a Hello has been seen since power-up the card behaves exactly like stock firmware: it
 * doesn't ACK the command byte, so a host learns it has a plain PS1 card (05 design, rule 2).
 *
 * Frame: 81 6x [request bytes] [answer bytes clocked out with 00]. The card ACKs each request
 * byte; the ACK of the last one comes when core 0 has the answer (an SD access, so it can be
 * milliseconds late; only Super MemCard hosts send these commands, and they wait for it). */

#include "ps1_supermemcard.h"

static uint8_t smc_req[SMC_MAX_REQ];
static uint8_t smc_ans[SMC_MAX_ANS];

static void __time_critical_func(mc_supermemcard)(uint8_t cmd) {
    if (cmd != SMC_HELLO && !ps1_supermemcard_active())
        return; /* no Hello yet: ignore, like stock firmware */

    int n = 0, need = ps1_supermemcard_req_len(cmd, smc_req, 0);
    if (need < 0)
        return; /* Super MemCard off in the card's menu: not even the command byte is ACKed */
    while (n < need) {
        respondOrNextCmd(0x00);
        receiveOrNextCmd(&smc_req[n]);
        n++;
        if (n == need)
            need = ps1_supermemcard_req_len(cmd, smc_req, n);
        if (need < 0 || need > SMC_MAX_REQ)
            return; /* malformed: stop answering, the host times out */
    }

    int alen = ps1_supermemcard_exec(cmd, smc_req, n, smc_ans, &reset);
    if (alen <= 0)
        return; /* SEL went up while core 0 was working, or the command failed */

    uint8_t _;
    for (int i = 0; i < alen; i++) {
        respondOrNextCmd(smc_ans[i]);
        if (i + 1 < alen)
            receiveOrNextCmd(&_);
    }
}
