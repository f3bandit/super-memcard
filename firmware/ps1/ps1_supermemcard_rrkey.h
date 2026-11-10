/* Copyright (C) 2026 f3bandit. Super MemCard additions to sd2psXtd: GPL-3.0-or-later, as sd2psXtd.
 * This program is distributed WITHOUT ANY WARRANTY; see the GNU General Public License. */
/* Retro Remake's public key: the card takes an identity (smc_key.bin) only when Retro Remake signed it for this
 * card's chip (docs/12-card-identity.md). This is the TEST key (tools/smccert/test-rr.key); Retro Remake's own goes
 * here, the same as in smcd's host/smc_rrkey.h. */
#pragma once
#include <stdint.h>
#define SMC_RR_KEY_IS_TEST 1
static const uint8_t smc_rr_pub[32] = {
    0x9f, 0xe4, 0x2b, 0xa1, 0xa1, 0x00, 0xdb, 0x53,
    0x23, 0x37, 0x7b, 0x7e, 0xbc, 0xb6, 0xae, 0xd1,
    0x24, 0x68, 0x7a, 0x1c, 0xcc, 0x7e, 0x6f, 0x3a,
    0xa1, 0xca, 0x44, 0x5e, 0x05, 0xde, 0x8f, 0xf1,
};
