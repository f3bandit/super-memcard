/* Tests for the virtual card: sd2psXtd's PS1 code, driven over the bus like a PS1 or the SS1.
 * Each test runs in its own process with its own SD folder, so every test is a fresh power-up. */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bus.h"
#include "host_settings.h"
#include "vcard.h"

static char sd[256];

#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            fprintf(stderr, "    %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
            _exit(1);                                                         \
        }                                                                     \
    } while (0)
#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        long _a = (long)(a), _b = (long)(b);                                  \
        if (_a != _b) {                                                       \
            fprintf(stderr, "    %s:%d: %s = %ld (0x%lx), expected %ld (0x%lx)\n", __FILE__, __LINE__, #a, _a, \
                    (unsigned long)_a, _b, (unsigned long)_b);                \
            _exit(1);                                                         \
        }                                                                     \
    } while (0)

/* ---- SD folder helpers ---- */

static void path(char *out, size_t n, const char *rel) { snprintf(out, n, "%s/%s", sd, rel); }

static long file_size(const char *rel) {
    char p[512];
    struct stat st;
    path(p, sizeof(p), rel);
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

static int read_file_at(const char *rel, long off, void *buf, size_t n) {
    char p[512];
    path(p, sizeof(p), rel);
    int fd = open(p, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t r = pread(fd, buf, n, off);
    close(fd);
    return r == (ssize_t)n ? 0 : -1;
}

static void write_file(const char *rel, const void *buf, size_t n) {
    char p[512], dir[512];
    path(p, sizeof(p), rel);
    snprintf(dir, sizeof(dir), "mkdir -p \"$(dirname '%s')\"", p);
    CHECK(system(dir) == 0);
    FILE *f = fopen(p, "wb");
    CHECK(f);
    CHECK(fwrite(buf, 1, n, f) == n);
    fclose(f);
}

/* A listing of every file under the SD folder with its size and an FNV hash of its bytes. */
static void fingerprint(const char *rel, char *out, size_t n) {
    char cmd[600];
    char p[512];
    path(p, sizeof(p), rel);
    snprintf(cmd, sizeof(cmd), "cd '%s' 2>/dev/null && find . -type f | LC_ALL=C sort | xargs -r sha1sum", p);
    FILE *f = popen(cmd, "r");
    CHECK(f);
    size_t got = fread(out, 1, n - 1, f);
    out[got] = 0;
    pclose(f);
}

static void pattern(uint8_t *d, int seed) {
    for (int i = 0; i < 128; i++)
        d[i] = (uint8_t)(seed * 31 + i * 7 + (i >> 3));
}

#define CARD1 "MemoryCards/PS1/Card1/Card1-1.mcd"

/* ---- tests ---- */

static void t_get_id(void) {
    uint8_t r[9];
    bus_wake();
    CHECK_EQ(mc_get_id(r), 0);
    const uint8_t want[9] = {0x08, 0x5A, 0x5D, 0x5C, 0x5D, 0x04, 0x00, 0x00, 0x80};
    for (int i = 0; i < 9; i++)
        CHECK_EQ(r[i], want[i]);
}

static void t_first_command_needs_sel_rise(void) {
    /* After power-up the card waits for SEL to rise once before it answers. The PS1 polls the
     * controller on the same select line, so it never notices; a host talking only to the
     * card must not count a missing reply to its first frame as "no card". */
    uint8_t r[9];
    CHECK_EQ(mc_get_id(r), MC_NO_REPLY);
    CHECK_EQ(mc_get_id(r), 0);
}

static void t_fresh_sd_makes_formatted_card(void) {
    uint8_t s0[128], f0[128];
    bus_wake();
    CHECK_EQ(mc_read(0, s0), MC_GOOD);
    CHECK_EQ(s0[0], 'M');
    CHECK_EQ(s0[1], 'C');
    CHECK(strcmp(vcard_card_path(), CARD1) == 0);
    CHECK_EQ(file_size(CARD1), 131072);
    CHECK_EQ(read_file_at(CARD1, 0, f0, 128), 0);
    CHECK(memcmp(s0, f0, 128) == 0);
}

static void t_write_read_flush_reboot(void) {
    uint8_t d[128], r[128], f[128];
    bus_wake();
    pattern(d, 5);
    uint8_t id[9];
    CHECK_EQ(mc_write(77, d, 0), MC_GOOD);
    CHECK_EQ(mc_flag, 0x08); /* FLAG bit 3: no write since power-up... */
    CHECK_EQ(mc_get_id(id), 0);
    CHECK_EQ(mc_flag, 0x00); /* ...cleared by the write, seen on the next command */
    CHECK_EQ(mc_read(77, r), MC_GOOD);
    CHECK(memcmp(d, r, 128) == 0);
    vcard_settle();
    CHECK_EQ(read_file_at(CARD1, 77 * 128, f, 128), 0);
    CHECK(memcmp(d, f, 128) == 0);
    vcard_stop();
    CHECK_EQ(vcard_start(sd), 0);
    bus_wake();
    memset(r, 0, sizeof(r));
    CHECK_EQ(mc_read(77, r), MC_GOOD);
    CHECK(memcmp(d, r, 128) == 0);
}

static void t_long_read(void) {
    /* 81 42 keeps streaming sectors until SEL rises; the checksum is over every byte sent. */
    uint8_t a[128], b[128];
    bus_wake();
    pattern(a, 1);
    pattern(b, 2);
    CHECK_EQ(mc_write(10, a, 0), MC_GOOD);
    CHECK_EQ(mc_write(11, b, 0), MC_GOOD);
    uint8_t out[10 + 256] = {0x81, 'B', 0, 0, 0, 10}, in[10 + 256];
    CHECK_EQ(bus_frame(out, in, NULL, sizeof(out)), sizeof(out));
    CHECK(memcmp(in + 10, a, 128) == 0);
    CHECK(memcmp(in + 138, b, 128) == 0);
}

static void t_bad_checksum(void) {
    uint8_t d[128], before[128], after[128], r[128];
    bus_wake();
    CHECK_EQ(read_file_at(CARD1, 3 * 128, before, 128), 0);
    pattern(d, 9);
    CHECK_EQ(mc_write(3, d, 0x01), MC_BAD_CHECKSUM);
    vcard_settle();
    /* the SD card keeps the old sector */
    CHECK_EQ(read_file_at(CARD1, 3 * 128, after, 128), 0);
    CHECK(memcmp(before, after, 128) == 0);
    /* Not like a Sony card: sd2psXtd copies each byte into its RAM copy as it arrives, so until
     * the next good write of this sector (or power-off) a read returns the rejected data. */
    CHECK_EQ(mc_read(3, r), MC_GOOD);
    CHECK(memcmp(r, d, 128) == 0);
    vcard_stop();
    CHECK_EQ(vcard_start(sd), 0);
    bus_wake();
    CHECK_EQ(mc_read(3, r), MC_GOOD);
    CHECK(memcmp(r, before, 128) == 0);
}

static void t_write_cut_off(void) {
    uint8_t d[128], before[128], after[128];
    bus_wake();
    CHECK_EQ(read_file_at(CARD1, 40 * 128, before, 128), 0);
    pattern(d, 4);
    mc_write_cut(40, d, 64);
    vcard_settle();
    CHECK_EQ(read_file_at(CARD1, 40 * 128, after, 128), 0);
    CHECK(memcmp(before, after, 128) == 0);
    /* and the card answers the next command */
    uint8_t r[9];
    CHECK_EQ(mc_get_id(r), 0);
}

static void t_mmce_ping(void) {
    bus_wake();
    CHECK_EQ(mmce_ping(), 0);
}

/* The first command after a card switch: the card restarts its loop and may miss it, and one
 * wake isn't always enough (about 1 run in 20, 08). A host asks again, as here. */
static int mc_write_after_switch(uint16_t sector, const uint8_t d[128]) {
    int rc = MC_NO_REPLY;
    for (int t = 0; t < 3 && rc == MC_NO_REPLY; t++) {
        bus_wake();
        usleep(20 * 1000);
        rc = mc_write(sector, d, 0);
    }
    return rc;
}

static void t_game_id_switches_card(void) {
    bus_wake();
    CHECK_EQ(mmce_game_id("cdrom:\\SLUS_012.34;1"), 4 + 20 - 1); /* every byte but the last */
    vcard_settle();
    CHECK(strcmp(vcard_card_path(), "MemoryCards/PS1/SLUS-01234/SLUS-01234-1.mcd") == 0);
    CHECK_EQ(file_size("MemoryCards/PS1/SLUS-01234/SLUS-01234-1.mcd"), 131072);
    /* After a switch the card restarts its loop and, depending on timing, waits for SEL to
     * rise again: the first command may get no reply. The PS1 never notices (it polls the pad
     * on the same select); a host that only talks to the card must ask again. */
    uint8_t d[128], f[128];
    pattern(d, 12);
    CHECK_EQ(mc_write_after_switch(20, d), MC_GOOD);
    vcard_settle();
    CHECK_EQ(read_file_at("MemoryCards/PS1/SLUS-01234/SLUS-01234-1.mcd", 20 * 128, f, 128), 0);
    CHECK(memcmp(d, f, 128) == 0);
    CHECK_EQ(read_file_at(CARD1, 20 * 128, f, 128), 0);
    CHECK(memcmp(d, f, 128) != 0);
}

static void t_game_id_off(void) {
    bus_wake();
    mmce_game_id("cdrom:\\SLUS_012.34;1");
    vcard_settle();
    CHECK(strcmp(vcard_card_path(), CARD1) == 0);
}

static void ctrl_poll(uint16_t sw) {
    /* digital pad: 01 42 00 00 00, the pad answers FF 41 5A swlo swhi (0 = pressed) */
    const uint8_t cmd[5] = {0x01, 0x42, 0x00, 0x00, 0x00};
    const uint8_t dat[5] = {0xFF, 0x41, 0x5A, (uint8_t)~sw, (uint8_t)(~sw >> 8)};
    vcard_select(true);
    for (int i = 0; i < 5; i++)
        vcard_xfer_ctrl(cmd[i], dat[i], true, NULL);
    vcard_select(false);
}

static void t_controller_combo_next_card(void) {
    /* L1+L2+R1+R2 held, Up pressed and released: next card (03 menu doc) */
    enum { UP = 1 << 4, L2 = 1 << 8, R2 = 1 << 9, L1 = 1 << 10, R1 = 1 << 11 };
    /* held for a few frames, then let go, as a PS1 polls (60 a second): one poll each was sometimes
     * missed when the PC was busy (08) */
    bus_wake();
    for (int i = 0; i < 4; i++) {
        ctrl_poll(L1 | L2 | R1 | R2 | UP);
        usleep(16 * 1000);
    }
    for (int i = 0; i < 4; i++) {
        ctrl_poll(0);
        usleep(16 * 1000);
    }
    vcard_settle();
    CHECK(strcmp(vcard_card_path(), "MemoryCards/PS1/Card2/Card2-1.mcd") == 0);
}

#ifndef WITH_SUPERMEMCARD
static void t_stock_card_ignores_81_6x(void) {
    /* Super MemCard's commands are 81 60..67. Stock firmware doesn't ACK the command byte, so
     * the host knows it has a plain card and falls back to PS1-only (05 design). */
    bus_wake();
    for (int c = 0x60; c <= 0x6F; c++) {
        uint8_t out[4] = {0x81, (uint8_t)c, 0, 0}, in[4];
        bool ack[4];
        CHECK_EQ(bus_frame(out, in, ack, 4), 2);
        CHECK(ack[0]);
        CHECK(!ack[1]);
    }
    uint8_t r[9];
    CHECK_EQ(mc_get_id(r), 0);
}
#endif

static void t_ps1_session_leaves_super_memcard_alone(void) {
    /* Rule 1: Super MemCard keeps other systems' saves on the same SD card. A PS1 (or the SS1
     * in PS1 mode) using the card, switching games, formatting it, must not touch them. */
    uint8_t blob[4096];
    for (size_t i = 0; i < sizeof(blob); i++)
        blob[i] = (uint8_t)(i * 13);
    write_file("SuperMemCard/N64/NSME/mario64.eep", blob, 512);
    write_file("SuperMemCard/SNES/0x1A2B3C4D/zelda.srm", blob, 4096);
    write_file("SuperMemCard/index.dat", blob, 100);
    write_file("SuperMemCard/PSX/SLUS-00594/mirror.mcd", blob, 4096);
    char before[8192], after[8192];
    fingerprint("SuperMemCard", before, sizeof(before));
    CHECK(strstr(before, "mario64.eep"));

    bus_wake();
    uint8_t d[128];
    for (int s = 0; s < 1024; s++)
        CHECK_EQ(mc_read((uint16_t)s, d), MC_GOOD);
    for (int s = 0; s < 64; s++) {
        pattern(d, s);
        CHECK_EQ(mc_write((uint16_t)s, d, 0), MC_GOOD);
    }
    mmce_game_id("cdrom:\\SCUS_944.26;1");
    vcard_settle();
    /* format: header and an empty directory, as the BIOS writes it */
    memset(d, 0, sizeof(d));
    d[0] = 'M';
    d[1] = 'C';
    d[127] = 0x0E;
    CHECK_EQ(mc_write_after_switch(0, d), MC_GOOD);
    for (int s = 1; s < 16; s++) {
        memset(d, 0, sizeof(d));
        d[0] = 0xA0;
        d[8] = 0xFF;
        d[9] = 0xFF;
        d[127] = 0xA0;
        CHECK_EQ(mc_write((uint16_t)s, d, 0), MC_GOOD);
    }
    vcard_settle();
    vcard_stop();
    CHECK_EQ(vcard_start(sd), 0);
    bus_wake();
    CHECK_EQ(mc_read(0, d), MC_GOOD);

    fingerprint("SuperMemCard", after, sizeof(after));
    CHECK(strcmp(before, after) == 0);
}

static void t_power_cut_after_write(void) {
    /* Power lost right after a write's end byte: after power-up the sector is either the old
     * one or the new one, never a mix. */
    uint8_t d[128], before[128], r[128];
    bus_wake();
    CHECK_EQ(mc_read(200, before), MC_GOOD);
    pattern(d, 77);
    CHECK_EQ(mc_write(200, d, 0), MC_GOOD);
    vcard_power_cut();
    CHECK_EQ(vcard_start(sd), 0);
    bus_wake();
    CHECK_EQ(mc_read(200, r), MC_GOOD);
    CHECK(memcmp(r, d, 128) == 0 || memcmp(r, before, 128) == 0);
}

static void t_controller_combo_survives_power_off(void) {
    /* the card number a combo picked is saved in the card's flash */
    enum { UP = 1 << 4, L2 = 1 << 8, R2 = 1 << 9, L1 = 1 << 10, R1 = 1 << 11 };
    /* held for a few frames, then let go, as a PS1 polls (60 a second): one poll each was sometimes
     * missed when the PC was busy (08) */
    bus_wake();
    for (int i = 0; i < 4; i++) {
        ctrl_poll(L1 | L2 | R1 | R2 | UP);
        usleep(16 * 1000);
    }
    for (int i = 0; i < 4; i++) {
        ctrl_poll(0);
        usleep(16 * 1000);
    }
    vcard_settle();
    vcard_stop();
    CHECK_EQ(vcard_start(sd), 0);
    CHECK(strcmp(vcard_card_path(), "MemoryCards/PS1/Card2/Card2-1.mcd") == 0);
}

static void t_sector_out_of_range(void) {
    /* A Sony card answers a write to a sector past 3FF with end byte FF and changes nothing.
     * (Games never do this; homebrew or a buggy host could.) */
    uint8_t d[128];
    bus_wake();
    pattern(d, 3);
    CHECK_EQ(mc_write(0x400, d, 0), MC_BAD_SECTOR);
    uint8_t r[128];
    CHECK_EQ(mc_read(0, r), MC_GOOD);
}

#ifdef WITH_SUPERMEMCARD
#include "test_smc.inc"
#include "test_sync.inc"
#else
#define SMC_TESTS
#define SYNC_TESTS
#endif

/* ---- runner ---- */

struct test {
    const char *name;
    void (*fn)(void);
    bool game_id;
    const char *known_bug; /* a firmware bug this test shows: expected to fail */
};
static const struct test tests[] = {
    {"get ID", t_get_id, true},
    {"first command after power-up needs SEL to rise", t_first_command_needs_sel_rise, true},
    {"fresh SD card: a formatted Card1-1.mcd", t_fresh_sd_makes_formatted_card, true},
    {"write, read back, flushed to SD, survives power-off", t_write_read_flush_reboot, true},
    {"long read streams sectors", t_long_read, true},
    {"bad checksum: 4E, SD unchanged", t_bad_checksum, true},
    {"write cut off: SD unchanged", t_write_cut_off, true},
    {"MMCE ping", t_mmce_ping, true},
    {"game ID switches to the game's card", t_game_id_switches_card, true},
    {"game ID setting off: stays on Card1", t_game_id_off, false},
    {"controller combo L1+L2+R1+R2+Up: next card", t_controller_combo_next_card, true},
#ifndef WITH_SUPERMEMCARD
    {"stock card doesn't ACK 81 60..6F", t_stock_card_ignores_81_6x, true},
#endif
    {"PS1 session leaves SuperMemCard/ byte for byte", t_ps1_session_leaves_super_memcard_alone, true},
    {"power cut after a write: old or new sector, never a mix", t_power_cut_after_write, true},
    {"card picked by the combo is kept after power-off", t_controller_combo_survives_power_off, true},
#ifdef WITH_SUPERMEMCARD   /* fixed in Super MemCard's firmware (firmware/patches/0006) */
    {"write past sector 3FF: end byte FF, nothing changed", t_sector_out_of_range, true},
#else
    {"write past sector 3FF: end byte FF, nothing changed", t_sector_out_of_range, true,
     "without PSRAM, ps1_mc_data_interface_write_byte() writes past the 128 KB card image in RAM "
     "(up to 8 MB past it) and the card answers 47"},
#endif
    SMC_TESTS
    SYNC_TESTS
};

int main(int argc, char **argv) {
    int failed = 0, run = 0, known = 0;
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (argc > 1 && !strstr(tests[i].name, argv[1]))
            continue;
        run++;
        snprintf(sd, sizeof(sd), "/tmp/vcard-XXXXXX");
        const char *tmp = getenv("TMPDIR");
        if (tmp)
            snprintf(sd, sizeof(sd), "%s/vcard-XXXXXX", tmp);
        CHECK(mkdtemp(sd));
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            alarm(180); /* a slow or busy PC (CI) runs the big saves slowly */
            host_settings()->ps1_game_id = tests[i].game_id;
            /* the card process reads this at its first SD write */
            if (strstr(tests[i].name, "slow SD"))
                setenv("VCARD_SD_WRITE_US", "2700", 1);
            if (vcard_start(sd) != 0)
                _exit(2);
            tests[i].fn();
            vcard_stop();
            _exit(0);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
        if (tests[i].known_bug) {
            printf("%s  %s\n", ok ? "FIXED?" : "known", tests[i].name);
            if (!ok)
                printf("    firmware bug: %s\n", tests[i].known_bug);
            else
                printf("    passes now: the firmware may have fixed it; make it a normal test\n");
            known++;
            failed += ok;
        } else if (ok) {
            printf("pass  %s\n", tests[i].name);
        } else {
            printf("FAIL  %s\n", tests[i].name);
            failed++;
            if (WIFSIGNALED(st))
                printf("    killed by signal %d\n", WTERMSIG(st));
        }
        char rm[700];
        snprintf(rm, sizeof(rm), "rm -rf '%s' '%s-host'", sd, sd);
        if (system(rm) != 0) {
        }
    }
    printf("\n%d of %d passed, %d known firmware bug(s)\n", run - known - failed, run - known, known);
    return failed ? 1 : 0;
}
