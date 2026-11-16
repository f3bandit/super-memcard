/* smccert: Retro Remake's tool for Super MemCard card identities (docs/12-card-identity.md).
 * Copyright (C) 2026 f3bandit. Licensed under the Apache License 2.0 (LICENSE, NOTICE).
 *
 *   smccert key   <key file>                      a new signing key (Retro Remake's; keep it offline), prints its
 *                                                 public key for smcd and the card firmware
 *   smccert pub   <key file>                      prints the key's public key
 *   smccert issue <key file> <chip ID> <out>      a card's identity for the card with that chip ID (16 hex digits,
 *                                                 its CERT answer): copy <out> to the card's SD card as smc_key.bin;
 *                                                 the card moves it into its flash at the next start and deletes it
 *   smccert check <key file> <chip ID> <file>     checks an identity file against the key and chip ID
 *
 * Identity file (96 bytes): the card's key seed (32), then Retro Remake's signature (64) of
 * "SMC-CERT" + chip ID (8) + the card's public key (32). EdDSA (Monocypher, BLAKE2b). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>

#include "monocypher.h"

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

static int read_file(const char *path, uint8_t *buf, size_t n) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t got = fread(buf, 1, n, f);
    int extra = fgetc(f);
    fclose(f);
    return got == n && extra == EOF ? 0 : -1;
}

static int write_file(const char *path, const uint8_t *buf, size_t n, mode_t mode) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    int ok = fwrite(buf, 1, n, f) == n;
    ok &= fclose(f) == 0;
    chmod(path, mode);
    return ok ? 0 : -1;
}

static int parse_uid(const char *s, uint8_t uid[8]) {
    if (strlen(s) != 16)
        return -1;
    for (int i = 0; i < 8; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1)
            return -1;
        uid[i] = (uint8_t)v;
    }
    return 0;
}

static void print_pub(const uint8_t pk[32]) {
    printf("public key: ");
    for (int i = 0; i < 32; i++)
        printf("%02x", pk[i]);
    printf("\nas C (smcd's host/smc_rrkey.h and the firmware's ps1_supermemcard_rrkey.h):\n   ");
    for (int i = 0; i < 32; i++)
        printf(" 0x%02x,%s", pk[i], i % 8 == 7 && i < 31 ? "\n   " : "");
    printf("\n");
}

static void cert_msg(uint8_t m[48], const uint8_t uid[8], const uint8_t pk[32]) {
    memcpy(m, "SMC-CERT", 8);
    memcpy(m + 8, uid, 8);
    memcpy(m + 16, pk, 32);
}

int main(int argc, char **argv) {
    uint8_t sk[64], pk[32], seed[32];
    if (argc == 3 && !strcmp(argv[1], "key")) {
        struct stat st;
        if (stat(argv[2], &st) == 0) {
            fprintf(stderr, "smccert: %s exists: not replaced\n", argv[2]);
            return 1;
        }
        if (fill_random(seed, 32)) {
            fprintf(stderr, "smccert: no random numbers\n");
            return 1;
        }
        crypto_eddsa_key_pair(sk, pk, seed);
        if (write_file(argv[2], sk, 64, 0600)) {
            fprintf(stderr, "smccert: can't write %s\n", argv[2]);
            return 1;
        }
        crypto_wipe(sk, 64);
        print_pub(pk);
        return 0;
    }
    if (argc < 3 || read_file(argv[2], sk, 64)) {
        fprintf(stderr, "usage: smccert key|pub|issue|check <key file> ... (see the source)\n");
        return 2;
    }
    memcpy(pk, sk + 32, 32);
    if (argc == 3 && !strcmp(argv[1], "pub")) {
        print_pub(pk);
        return 0;
    }
    uint8_t uid[8], id[96], m[48], cpk[32], csk[64];
    if (argc != 5 || parse_uid(argv[3], uid)) {
        fprintf(stderr, "usage: smccert issue|check <key file> <chip ID, 16 hex digits> <file>\n");
        return 2;
    }
    if (!strcmp(argv[1], "issue")) {
        if (fill_random(id, 32)) {
            fprintf(stderr, "smccert: no random numbers\n");
            return 1;
        }
        memcpy(seed, id, 32);
        crypto_eddsa_key_pair(csk, cpk, seed);
        cert_msg(m, uid, cpk);
        crypto_eddsa_sign(id + 32, sk, m, sizeof(m));
        crypto_wipe(sk, 64);
        crypto_wipe(csk, 64);
        if (write_file(argv[4], id, 96, 0600)) {
            fprintf(stderr, "smccert: can't write %s\n", argv[4]);
            return 1;
        }
        crypto_wipe(id, 96);
        printf("identity for chip %s in %s (card public key ", argv[3], argv[4]);
        for (int i = 0; i < 32; i++)
            printf("%02x", cpk[i]);
        printf(")\n");
        return 0;
    }
    if (!strcmp(argv[1], "check")) {
        crypto_wipe(sk, 64);
        if (read_file(argv[4], id, 96)) {
            fprintf(stderr, "smccert: %s isn't an identity file (96 bytes)\n", argv[4]);
            return 1;
        }
        memcpy(seed, id, 32);
        crypto_eddsa_key_pair(csk, cpk, seed);
        crypto_wipe(csk, 64);
        cert_msg(m, uid, cpk);
        int ok = crypto_eddsa_check(id + 32, pk, m, sizeof(m)) == 0;
        crypto_wipe(id, 96);
        printf("%s\n", ok ? "good: signed by this key for this chip" : "BAD: not signed by this key for this chip");
        return ok ? 0 : 1;
    }
    fprintf(stderr, "smccert: unknown command %s\n", argv[1]);
    return 2;
}
