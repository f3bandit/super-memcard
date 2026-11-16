/* Super MemCard: Retro Remake's public key, which signs every genuine card's identity (docs/12-card-identity.md).
 * Copyright (C) 2026 f3bandit. Licensed under the Apache License 2.0 (LICENSE, NOTICE).
 * This is the TEST key (tools/smccert/test-rr.key): Retro Remake's own goes here, the same as in the firmware's
 * ps1_supermemcard_rrkey.h, and smcd built with the test key says so in its log. */
#pragma once
#include <stdint.h>
#define SMC_RR_KEY_IS_TEST 1
static const uint8_t smc_rr_pub[32] = {
    0x9f, 0xe4, 0x2b, 0xa1, 0xa1, 0x00, 0xdb, 0x53,
    0x23, 0x37, 0x7b, 0x7e, 0xbc, 0xb6, 0xae, 0xd1,
    0x24, 0x68, 0x7a, 0x1c, 0xcc, 0x7e, 0x6f, 0x3a,
    0xa1, 0xca, 0x44, 0x5e, 0x05, 0xde, 0x8f, 0xf1,
};
