/* Super MemCard: is the card a genuine Retro Remake card? (docs/12-card-identity.md)
 * Copyright (C) 2026 f3bandit. Licensed under the Apache License 2.0 (LICENSE, NOTICE). */
#define _GNU_SOURCE
#include "smc.h"
#include "smc_rrkey.h"

#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include "monocypher.h"

bool smc_genuine_test_key(void) { return SMC_RR_KEY_IS_TEST; }

static int fill_random(uint8_t *p, size_t n) {
    while (n) {
        ssize_t r = getrandom(p, n, 0);
        if (r <= 0)
            return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int smc_genuine(const struct smc_bus *b, const struct smc_card *card, char *why, size_t why_len) {
    char dummy[8];
    if (!why) {
        why = dummy;
        why_len = sizeof(dummy);
    }
    why[0] = 0;
    if (!(card->features & SMC_FEATURE_IDENTITY)) {
        snprintf(why, why_len, "its firmware has no card identity");
        return 0;
    }
    uint8_t chip[8], pub[32], cert[64], m[48];
    int rc = smc_cert(b, chip, pub, cert);
    if (rc < 0)
        return rc;
    if (rc == 0) {
        snprintf(why, why_len, "it has no identity from Retro Remake");
        return 0;
    }
    /* Retro Remake signed this chip's key */
    memcpy(m, "SMC-CERT", 8);
    memcpy(m + 8, chip, 8);
    memcpy(m + 16, pub, 32);
    if (crypto_eddsa_check(cert, smc_rr_pub, m, sizeof(m))) {
        snprintf(why, why_len, "its identity isn't signed by Retro Remake");
        return 0;
    }
    /* and the card holds that key: it signs a challenge no card has seen */
    uint8_t challenge[32], sig[64];
    if (fill_random(challenge, sizeof(challenge))) {
        snprintf(why, why_len, "no random numbers on this SuperStation");
        return SMC_E_BAD;
    }
    rc = smc_auth(b, challenge);
    if (rc == SMC_E_NOT_FOUND) {
        snprintf(why, why_len, "it has no identity from Retro Remake");
        return 0;
    }
    if (rc < 0)
        return rc;
    /* the card signs after answering (tens of ms on the RP2040; meanwhile it may not answer a frame at all) */
    long long until = now_ms() + 3000;
    rc = 0;
    for (int tries = 0; now_ms() < until; tries++) {
        usleep(tries ? 50000 : 20000);
        rc = smc_authsig(b, sig);
        if (rc != 0 && rc != SMC_E_NO_REPLY)
            break;
    }
    if (rc == 0 || rc == SMC_E_NO_REPLY) {
        snprintf(why, why_len, "it didn't sign the challenge");
        return SMC_E_NO_REPLY;
    }
    if (rc < 0)
        return rc;
    memcpy(m, "SMC-AUTH", 8);
    memcpy(m + 8, challenge, 32);
    memcpy(m + 40, chip, 8);
    if (crypto_eddsa_check(sig, pub, m, sizeof(m))) {
        snprintf(why, why_len, "it doesn't hold the key its identity names");
        return 0;
    }
    return 1;
}
