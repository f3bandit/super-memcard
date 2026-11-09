/* Super MemCard tests (build with SMC=1): the card firmware's 81 60..69 commands, driven through
 * the host library (../../host/smc.c) the SS1 will use. Included by test_card.c. */
#include "../../../host/smc.h"

static void bus_sel(void *c, bool s) { (void)c; vcard_select(s); }
static uint8_t bus_x(void *c, uint8_t o, bool last, bool *ack) { (void)c; return vcard_xfer(o, last, ack); }
static const struct smc_bus vbus = {NULL, bus_sel, bus_x, NULL};

/* the front ports' way: a whole frame per call (phosphor-menu PH_RAW), here done byte by byte on the
 * virtual card, stopping after the first byte without an ACK like the FPGA does */
static int bus_frame_cb(void *c, const uint8_t *out, uint8_t *in, int len) {
    (void)c;
    int k;
    vcard_select(true);
    for (k = 0; k < len; k++) {
        bool ack = false;
        in[k] = vcard_xfer(out[k], k + 1 == len, &ack);
        if (!ack && k + 1 < len) {
            k++;
            break;
        }
    }
    vcard_select(false);
    return k;
}
static const struct smc_bus fbus = {NULL, NULL, NULL, bus_frame_cb};
static void fill(uint8_t *d, size_t n, int seed);

static void t_smc_frame_transport(void) {
    /* the same round trip through whole-frame calls */
    static uint8_t save[8192], back[8192];
    struct smc_file f;
    fill(save, sizeof(save), 31);
    CHECK_EQ(smc_hello(&fbus, SMC_HOST_TEST, 0, NULL), 1);
    CHECK_EQ(smc_select(&fbus, "SNES", "Frames"), 0);
    CHECK_EQ(smc_write_file(&fbus, "save.sav", save, sizeof(save), 3), SMC_OK);
    CHECK_EQ(smc_select(&fbus, "SNES", "Frames"), 1);
    CHECK_EQ(smc_info(&fbus, 0, &f), SMC_OK);
    CHECK_EQ(f.gen, 3);
    CHECK_EQ(smc_read_file(&fbus, 0, f.size, back), SMC_OK);
    CHECK(memcmp(save, back, sizeof(save)) == 0);
    vcard_stop();
    CHECK_EQ(vcard_start(sd), 0);
    bus_wake();
    /* before Hello, Select stops at the command byte */
    CHECK_EQ(smc_select(&fbus, "SNES", "Frames"), SMC_E_NO_REPLY);
}

static void fill(uint8_t *d, size_t n, int seed) {
    for (size_t i = 0; i < n; i++)
        d[i] = (uint8_t)(seed * 131 + i * 7 + (i >> 9));
}

static void t_smc_off_until_hello(void) {
    /* before a Hello the card is a stock card: 61..6F aren't ACKed */
    bus_wake();
    for (int c = 0x61; c <= 0x6F; c++) {
        uint8_t out[4] = {0x81, (uint8_t)c, 0, 0};
        bool ack[4];
        CHECK_EQ(bus_frame(out, NULL, ack, 4), 2);
        CHECK(!ack[1]);
    }
    /* a Hello without 'S' 'M' gets no answer, and doesn't turn anything on */
    uint8_t bad[5] = {'X', 'X', 1, SMC_HOST_TEST, 0}, a[16];
    CHECK_EQ(smc_frame(&vbus, 0x60, bad, 5, a, 16), SMC_E_NO_REPLY);
    CHECK_EQ(smc_select(&vbus, "SNES", "abc"), SMC_E_NO_REPLY);
    uint8_t r[9];
    CHECK_EQ(mc_get_id(r), 0);
}

static void t_smc_hello_and_card_id(void) {
    struct smc_card c1, c2;
    /* no bus_wake: Hello itself asks twice, for the dropped first command after power-up */
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, &c1), 1);
    CHECK_EQ(c1.version, 2);
    CHECK_EQ(strlen(c1.id), 16);
    char ini[64];
    CHECK_EQ(read_file_at("SuperMemCard/card.ini", 0, ini, 20), 0);
    CHECK(memcmp(ini + 3, c1.id, 16) == 0);
    vcard_stop();
    CHECK_EQ(vcard_start(sd), 0);
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, &c2), 1);
    CHECK(strcmp(c1.id, c2.id) == 0); /* the same card after a power-off */
}

static void t_smc_off_after_power_off(void) {
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, NULL), 1);
    vcard_stop();
    CHECK_EQ(vcard_start(sd), 0);
    bus_wake();
    CHECK_EQ(smc_select(&vbus, "SNES", "abc"), SMC_E_NO_REPLY); /* plain PS1 card again */
}

static void t_smc_save_round_trip(void) {
    static uint8_t save[8192], back[8192];
    struct smc_file f;
    fill(save, sizeof(save), 1);
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, NULL), 1);
    CHECK_EQ(smc_select(&vbus, "SNES", "Super Metroid (USA)"), 0);
    CHECK_EQ(smc_write_file(&vbus, "save.sav", save, sizeof(save), 7), SMC_OK);
    CHECK_EQ(smc_pending(&vbus), 0);
    CHECK_EQ(smc_select(&vbus, "SNES", "Super Metroid (USA)"), 1);
    CHECK_EQ(smc_info(&vbus, 0, &f), SMC_OK);
    CHECK(strcmp(f.name, "save.sav") == 0);
    CHECK_EQ(f.size, sizeof(save));
    CHECK_EQ(f.gen, 7);
    CHECK_EQ(smc_read_file(&vbus, 0, f.size, back), SMC_OK);
    CHECK(memcmp(save, back, sizeof(save)) == 0);
    /* on the card's SD card, outside every .mcd */
    CHECK_EQ(file_size("SuperMemCard/SNES/Card1/Super Metroid (USA)/save.sav"), sizeof(save));
    CHECK_EQ(read_file_at("SuperMemCard/SNES/Card1/Super Metroid (USA)/save.sav", 0, back, sizeof(save)), 0);
    CHECK(memcmp(save, back, sizeof(save)) == 0);
    /* and still there after a power-off */
    vcard_stop();
    CHECK_EQ(vcard_start(sd), 0);
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, NULL), 1);
    CHECK_EQ(smc_select(&vbus, "SNES", "Super Metroid (USA)"), 1);
    CHECK_EQ(smc_info(&vbus, 0, &f), SMC_OK);
    CHECK_EQ(f.gen, 7);
    memset(back, 0, sizeof(back));
    CHECK_EQ(smc_read_file(&vbus, 0, f.size, back), SMC_OK);
    CHECK(memcmp(save, back, sizeof(save)) == 0);
}

static void t_smc_n64_files_and_big_save(void) {
    /* an N64 game: EEPROM + a Controller Pak; and a 128 KB FlashRAM save */
    static uint8_t eep[2048], cpk[32768], fla[131072], back[131072];
    fill(eep, sizeof(eep), 2);
    fill(cpk, sizeof(cpk), 3);
    fill(fla, sizeof(fla), 4);
    struct smc_file f;
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, NULL), 1);
    CHECK_EQ(smc_select(&vbus, "N64", "NSME"), 0);
    CHECK_EQ(smc_write_file(&vbus, "save.eep", eep, sizeof(eep), 1), SMC_OK);
    CHECK_EQ(smc_write_file(&vbus, "cpak1.cpk", cpk, sizeof(cpk), 1), SMC_OK);
    CHECK_EQ(smc_select(&vbus, "N64", "NSME"), 2);
    CHECK_EQ(smc_info(&vbus, 0, &f), SMC_OK); /* sorted by name: cpak1.cpk first */
    CHECK(strcmp(f.name, "cpak1.cpk") == 0);
    CHECK_EQ(smc_read_file(&vbus, 0, f.size, back), SMC_OK);
    CHECK(memcmp(cpk, back, sizeof(cpk)) == 0);
    CHECK_EQ(smc_info(&vbus, 1, &f), SMC_OK);
    CHECK_EQ(smc_read_file(&vbus, 1, f.size, back), SMC_OK);
    CHECK(memcmp(eep, back, sizeof(eep)) == 0);
    CHECK_EQ(smc_info(&vbus, 2, &f), SMC_E_NOT_FOUND);

    CHECK_EQ(smc_select(&vbus, "N64", "NZSE"), 0);
    CHECK_EQ(smc_write_file(&vbus, "save.fla", fla, sizeof(fla), 3), SMC_OK);
    CHECK_EQ(smc_select(&vbus, "N64", "NZSE"), 1);
    CHECK_EQ(smc_info(&vbus, 0, &f), SMC_OK);
    CHECK_EQ(f.size, sizeof(fla));
    CHECK_EQ(smc_read_file(&vbus, 0, f.size, back), SMC_OK);
    CHECK(memcmp(fla, back, sizeof(fla)) == 0);
}

static void t_smc_new_save_replaces_old_only_at_commit(void) {
    /* power lost halfway through writing a newer save: the card still has the old one whole */
    static uint8_t v1[4096], v2[4096], back[4096];
    fill(v1, sizeof(v1), 5);
    fill(v2, sizeof(v2), 6);
    struct smc_file f;
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, NULL), 1);
    CHECK_EQ(smc_select(&vbus, "GBA", "Golden Sun"), 0);
    CHECK_EQ(smc_write_file(&vbus, "save.sav", v1, sizeof(v1), 1), SMC_OK);
    /* start v2 by hand: Create, half the blocks, then power off before Commit */
    uint8_t req[140] = {0, 0, 0x10, 0, 8, 's', 'a', 'v', 'e', '.', 's', 'a', 'v'}, a[2];
    CHECK_EQ(smc_frame(&vbus, 0x64, req, 13, a, 2), SMC_OK);
    CHECK_EQ(a[0], 0x47);
    for (int blk = 0; blk < 16; blk++) {
        req[0] = a[1];
        req[1] = 0;
        req[2] = (uint8_t)blk;
        memcpy(req + 3, v2 + blk * 128, 128);
        uint8_t chk = req[1] ^ req[2];
        for (int i = 0; i < 128; i++)
            chk ^= req[3 + i];
        req[131] = chk;
        uint8_t e;
        CHECK_EQ(smc_frame(&vbus, 0x63, req, 132, &e, 1), SMC_OK);
        CHECK_EQ(e, 0x47);
    }
    vcard_power_cut();
    CHECK_EQ(vcard_start(sd), 0);
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, NULL), 1);
    CHECK_EQ(smc_select(&vbus, "GBA", "Golden Sun"), 1); /* the unfinished .new isn't a file */
    CHECK_EQ(smc_info(&vbus, 0, &f), SMC_OK);
    CHECK_EQ(f.gen, 1);
    CHECK_EQ(smc_read_file(&vbus, 0, f.size, back), SMC_OK);
    CHECK(memcmp(v1, back, sizeof(v1)) == 0);
    /* and writing it again works */
    CHECK_EQ(smc_write_file(&vbus, "save.sav", v2, sizeof(v2), 2), SMC_OK);
    CHECK_EQ(smc_select(&vbus, "GBA", "Golden Sun"), 1);
    CHECK_EQ(smc_info(&vbus, 0, &f), SMC_OK);
    CHECK_EQ(f.gen, 2);
    CHECK_EQ(smc_read_file(&vbus, 0, f.size, back), SMC_OK);
    CHECK(memcmp(v2, back, sizeof(v2)) == 0);
}

static void t_smc_bad_checksum_and_names(void) {
    uint8_t data[256];
    fill(data, sizeof(data), 7);
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, NULL), 1);
    /* names that could leave SuperMemCard/ or reach a .mcd are refused */
    CHECK_EQ(smc_select(&vbus, "..", "x"), SMC_E_BAD);
    CHECK_EQ(smc_select(&vbus, "SNES", "../../MemoryCards"), SMC_E_BAD);
    CHECK_EQ(smc_select(&vbus, "SNES/../..", "x"), SMC_E_BAD);
    CHECK_EQ(smc_select(&vbus, "SNES", ".hidden"), SMC_E_BAD);
    CHECK_EQ(smc_select(&vbus, "SNES", "a\\b"), SMC_E_BAD);
    CHECK_EQ(smc_select(&vbus, "SNES", "c:x"), SMC_E_BAD);
    CHECK_EQ(smc_select(&vbus, "SNES", "trailing."), SMC_E_BAD);
    CHECK_EQ(smc_select(&vbus, "SNES", "Chrono Trigger (USA) [!]"), 0);
    CHECK_EQ(smc_select(&vbus, "SNES", "ok"), 0);
    CHECK_EQ(smc_write_file(&vbus, "../Card1-1.mcd", data, sizeof(data), 1), SMC_E_BAD);
    CHECK_EQ(smc_write_file(&vbus, "meta.ini", data, sizeof(data), 1), SMC_E_BAD);
    CHECK_EQ(smc_write_file(&vbus, "x.new", data, sizeof(data), 1), SMC_E_BAD);
    /* a block with a bad checksum gets 4E and isn't written */
    uint8_t req[13] = {0, 0, 1, 0, 4, 'a', '.', 's', 'v'}, a[2];
    CHECK_EQ(smc_frame(&vbus, 0x64, req, 9, a, 2), SMC_OK);
    CHECK_EQ(a[0], 0x47);
    uint8_t w[132] = {a[1], 0, 0};
    memcpy(w + 3, data, 128);
    w[131] = 0x55; /* wrong */
    uint8_t e;
    CHECK_EQ(smc_frame(&vbus, 0x63, w, 132, &e, 1), SMC_OK);
    CHECK_EQ(e, 0x4E);
    CHECK(file_size("MemoryCards/PS1/Card1/Card1-1.mcd") == 131072);
}

static void t_smc_list_games(void) {
    uint8_t d[128];
    char game[65];
    fill(d, sizeof(d), 8);
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, NULL), 1);
    const char *games[] = {"Alpha", "Bravo", "Charlie"};
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(smc_select(&vbus, "MegaDrive", games[i]), 0);
        CHECK_EQ(smc_write_file(&vbus, "save.sav", d, sizeof(d), 1), SMC_OK);
    }
    int seen = 0;
    for (int i = 0; i < 10; i++) {
        int rc = smc_list(&vbus, "MegaDrive", i, game);
        if (rc == SMC_E_NOT_FOUND)
            break;
        CHECK_EQ(rc, SMC_OK);
        for (int j = 0; j < 3; j++)
            if (strcmp(game, games[j]) == 0)
                seen |= 1 << j;
    }
    CHECK_EQ(seen, 7);
    CHECK_EQ(smc_list(&vbus, "Saturn", 0, game), SMC_E_NOT_FOUND);
    /* systems: "." */
    CHECK_EQ(smc_select(&vbus, "SNES", "Long name: Legend of Zelda, The - A Link to the Past (USA)"), SMC_E_BAD);
    CHECK_EQ(smc_select(&vbus, "SNES", "Legend of Zelda, The - A Link to the Past (USA) (Rev 1)"), 0);
    CHECK_EQ(smc_write_file(&vbus, "save.sav", d, sizeof(d), 1), SMC_OK);
    int sys = 0;
    for (int i = 0; i < 10; i++) {
        int rc = smc_list(&vbus, ".", i, game);
        if (rc == SMC_E_NOT_FOUND)
            break;
        sys |= strcmp(game, "MegaDrive") == 0 ? 1 : strcmp(game, "SNES") == 0 ? 2 : 4;
    }
    CHECK_EQ(sys, 3);
    CHECK_EQ(smc_list(&vbus, "SNES", 0, game), SMC_OK);
    CHECK(strcmp(game, "Legend of Zelda, The - A Link to the Past (USA) (Rev 1)") == 0);
}

static void t_smc_ps1_unaffected(void) {
    /* Super MemCard in use, then PS1 commands: the PS1 card is exactly as it was (rule 1) */
    uint8_t before[128], r[128], d[128];
    bus_wake();
    for (int s = 0; s < 1024; s += 97) {
        CHECK_EQ(mc_read((uint16_t)s, before), MC_GOOD);
    }
    char mcd_before[512], mcd_after[512];
    fingerprint("MemoryCards", mcd_before, sizeof(mcd_before));
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, NULL), 1);
    fill(d, sizeof(d), 9);
    CHECK_EQ(smc_select(&vbus, "PSX", "SLUS-00594"), 0);
    CHECK_EQ(smc_write_file(&vbus, "extra.sav", d, sizeof(d), 1), SMC_OK);
    vcard_settle();
    fingerprint("MemoryCards", mcd_after, sizeof(mcd_after));
    CHECK(strcmp(mcd_before, mcd_after) == 0);
    pattern(d, 3);
    CHECK_EQ(mc_write(5, d, 0), MC_GOOD);
    CHECK_EQ(mc_read(5, r), MC_GOOD);
    CHECK(memcmp(d, r, 128) == 0);
    uint8_t id[9];
    CHECK_EQ(mc_get_id(id), 0);
    CHECK_EQ(id[1], 0x5A);
}

static void t_smc_slow_sd_card(void) {
    /* the SS1's SD card is mounted sync: ~2.7 ms per small write. A 128 KB save must still go through:
     * no command may do work in proportion to the save's size before answering (08 findings) */
    static uint8_t big[131072], back[131072];
    fill(big, sizeof(big), 21);
    struct smc_file f;
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, NULL), 1);
    CHECK_EQ(smc_select(&vbus, "N64", "Slow SD"), 0);
    CHECK_EQ(smc_write_file(&vbus, "save.fla", big, sizeof(big), 1), SMC_OK);
    CHECK_EQ(smc_select(&vbus, "N64", "Slow SD"), 1);
    CHECK_EQ(smc_info(&vbus, 0, &f), SMC_OK);
    CHECK_EQ(f.size, sizeof(big));
    CHECK_EQ(smc_read_file(&vbus, 0, f.size, back), SMC_OK);
    CHECK(memcmp(big, back, sizeof(big)) == 0);
}

static void t_smc_commit_checks_every_block(void) {
    /* Create, write only some blocks, Commit: refused, the previous save stays */
    static uint8_t v1[1024];
    fill(v1, sizeof(v1), 22);
    struct smc_file f;
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, NULL), 1);
    CHECK_EQ(smc_select(&vbus, "SNES", "Partial"), 0);
    CHECK_EQ(smc_write_file(&vbus, "save.sav", v1, sizeof(v1), 1), SMC_OK);
    uint8_t req[140] = {0, 0, 0x04, 0, 8, 's', 'a', 'v', 'e', '.', 's', 'a', 'v'}, a[2]; /* 1024 bytes */
    CHECK_EQ(smc_frame(&vbus, 0x64, req, 13, a, 2), SMC_OK);
    CHECK_EQ(a[0], 0x47);
    req[0] = a[1];
    req[1] = 0;
    req[2] = 0;
    memset(req + 3, 0xAA, 128);
    uint8_t chk = 0;
    for (int i = 0; i < 128; i++)
        chk ^= req[3 + i];
    req[131] = chk;
    uint8_t e;
    CHECK_EQ(smc_frame(&vbus, 0x63, req, 132, &e, 1), SMC_OK); /* only block 0 of 8 */
    CHECK_EQ(e, 0x47);
    uint8_t c[9] = {a[1], 0, 0, 0, 2, 0, 0, 0, 0}; /* gen 2, time 0 */
    CHECK_EQ(smc_frame(&vbus, 0x65, c, 9, &e, 1), SMC_OK);
    CHECK_EQ(e, 0xFF);
    CHECK_EQ(smc_select(&vbus, "SNES", "Partial"), 1);
    CHECK_EQ(smc_info(&vbus, 0, &f), SMC_OK);
    CHECK_EQ(f.gen, 1);
    uint8_t back[1024];
    CHECK_EQ(smc_read_file(&vbus, 0, f.size, back), SMC_OK);
    CHECK(memcmp(v1, back, sizeof(v1)) == 0);
}

static void t_smc_longest_names(void) {
    /* system 32, game 64, file 16 characters: every path on the card must still fit */
    char sys[33], game[65];
    memset(sys, 'S', 32), sys[32] = 0;
    memset(game, 'G', 64), game[64] = 0;
    uint8_t d[300], back[300];
    fill(d, sizeof(d), 23);
    struct smc_file f;
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, NULL), 1);
    CHECK_EQ(smc_select(&vbus, sys, game), 0);
    CHECK_EQ(smc_write_file(&vbus, "sixteen_chars.ab", d, sizeof(d), 5), SMC_OK);
    CHECK_EQ(smc_write_file(&vbus, "sixteen_chars.ab", d, sizeof(d), 6), SMC_OK); /* replaces: rename over */
    CHECK_EQ(smc_select(&vbus, sys, game), 1);
    CHECK_EQ(smc_info(&vbus, 0, &f), SMC_OK);
    CHECK(strcmp(f.name, "sixteen_chars.ab") == 0);
    CHECK_EQ(f.gen, 6);
    CHECK_EQ(smc_read_file(&vbus, 0, f.size, back), SMC_OK);
    CHECK(memcmp(d, back, sizeof(d)) == 0);
}


static void t_smc_virtual_cards(void) {
    /* each system has virtual cards; Select and List take one, 0 meaning the system's current card */
    static uint8_t save[1000], back[1000];
    fill(save, sizeof(save), 21);
    int card = -1;
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, NULL), 1);
    CHECK_EQ(smc_core(&vbus, "SNES", false), 1); /* a fresh card: card 1 */
    CHECK_EQ(smc_select_card(&vbus, "SNES", "Chrono Trigger (USA)", 2, &card), 0);
    CHECK_EQ(card, 2);
    CHECK_EQ(smc_write_file(&vbus, "save.srm", save, sizeof(save), 1), SMC_OK);
    CHECK_EQ(file_size("SuperMemCard/SNES/Card2/Chrono Trigger (USA)/save.srm"), sizeof(save));
    CHECK_EQ(smc_select_card(&vbus, "SNES", "Chrono Trigger (USA)", 0, &card), 0); /* card 1 hasn't it */
    CHECK_EQ(card, 1);
    char game[65];
    CHECK_EQ(smc_list_card(&vbus, "SNES", 2, 0, game), SMC_OK);
    CHECK(strcmp(game, "Chrono Trigger (USA)") == 0);
    CHECK_EQ(smc_list_card(&vbus, "SNES", 0, 0, game), SMC_E_NOT_FOUND);
    CHECK_EQ(smc_list_card(&vbus, "SNES", 1, 0, game), SMC_E_NOT_FOUND);
    CHECK_EQ(smc_select_card(&vbus, "SNES", "x", 100, NULL), SMC_E_BAD);
    /* asking doesn't change anything: the buttons are still the PS1 card's */
    CHECK_EQ(smc_core(&vbus, "SNES", false), 1);
    vcard_key(VKEY_ENTER);
    vcard_settle();
    usleep(1700 * 1000); /* the PS1 card switches 1.5 s after the last press */
    vcard_settle();
    CHECK(strcmp(vcard_card_path(), "MemoryCards/PS1/Card2/Card2-1.mcd") == 0);
}

static void t_smc_core_buttons_pick_the_card(void) {
    /* Core: the SNES core runs, so the card's buttons pick SNES's card, not the PS1 card */
    static uint8_t save[600];
    fill(save, sizeof(save), 22);
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, NULL), 1);
    CHECK_EQ(smc_core(&vbus, "SNES", true), 1);
    vcard_key(VKEY_ENTER);
    vcard_key(VKEY_ENTER);
    vcard_key(VKEY_BACK);  /* 2 */
    CHECK_EQ(smc_core(&vbus, "SNES", false), 2);
    int card = 0;
    CHECK_EQ(smc_select_card(&vbus, "SNES", "Super Metroid (USA)", 0, &card), 0);
    CHECK_EQ(card, 2);
    CHECK_EQ(smc_write_file(&vbus, "save.sav", save, sizeof(save), 1), SMC_OK);
    CHECK_EQ(file_size("SuperMemCard/SNES/Card2/Super Metroid (USA)/save.sav"), sizeof(save));
    /* saved on the card after 1.5 s without presses; the PS1 card didn't move */
    usleep(1700 * 1000);
    vcard_settle();
    char ini[32] = {0};
    CHECK_EQ(read_file_at("SuperMemCard/SNES/card.ini", 0, ini, 7), 0);
    CHECK(strncmp(ini, "card=2\n", 7) == 0);
    CHECK(strcmp(vcard_card_path(), CARD1) == 0);
    /* other systems keep their own */
    CHECK_EQ(smc_core(&vbus, "NES", false), 1);
    /* never below card 1 */
    vcard_key(VKEY_BACK);
    vcard_key(VKEY_BACK);
    vcard_key(VKEY_PREV);
    CHECK_EQ(smc_core(&vbus, "SNES", false), 1);
    /* kept after a power-off, and the buttons are the PS1 card's again until a core names its system */
    usleep(1700 * 1000);
    vcard_settle();
    vcard_stop();
    CHECK_EQ(vcard_start(sd), 0);
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, NULL), 1);
    CHECK_EQ(smc_core(&vbus, "SNES", false), 1);
    CHECK_EQ(smc_core(&vbus, "SNES", true), 1);
    vcard_key(VKEY_NEXT);
    CHECK_EQ(smc_core(&vbus, "SNES", false), 2);
    usleep(1700 * 1000);
    vcard_stop();
    CHECK_EQ(vcard_start(sd), 0);
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, NULL), 1);
    CHECK_EQ(smc_core(&vbus, "SNES", false), 2);
    vcard_key(VKEY_ENTER);
    usleep(1700 * 1000);
    vcard_settle();
    CHECK(strcmp(vcard_card_path(), "MemoryCards/PS1/Card2/Card2-1.mcd") == 0);
}

static void t_smc_ps1_use_ends_system_mode(void) {
    /* a PS1 game reading the card (the stock PSX core after an SNES game): the buttons are the PS1 card's */
    uint8_t d[128];
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, NULL), 1);
    CHECK_EQ(smc_core(&vbus, "GBA", true), 1);
    CHECK_EQ(mc_read(0, d), MC_GOOD);
    vcard_settle();
    vcard_key(VKEY_ENTER);
    CHECK_EQ(smc_core(&vbus, "GBA", false), 1);
    usleep(1700 * 1000);
    vcard_settle();
    CHECK(strcmp(vcard_card_path(), "MemoryCards/PS1/Card2/Card2-1.mcd") == 0);
    /* and Core "." does it too (the first command after a card switch can go unanswered: ask again) */
    int rc = SMC_E_NO_REPLY;
    for (int t = 0; t < 3 && rc == SMC_E_NO_REPLY; t++) {
        bus_wake();
        usleep(20 * 1000);
        rc = smc_core(&vbus, "GBA", true);
    }
    CHECK_EQ(rc, 1);
    CHECK_EQ(smc_core(&vbus, NULL, true), 0);
    vcard_key(VKEY_BACK);
    CHECK_EQ(smc_core(&vbus, "GBA", false), 1);
    usleep(1700 * 1000);
    vcard_settle();
    CHECK(strcmp(vcard_card_path(), CARD1) == 0);
}

static void t_smc_menu_saves_off(void) {
    /* the card's menu: SuperStation > Saves: No. The card is then a stock card: 81 60 isn't even ACKed */
    struct smc_card c;
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, &c), 1);
    vcard_key(VMENU_SAVES_OFF);
    CHECK_EQ(smc_select(&vbus, "SNES", "x"), SMC_E_NO_REPLY);
    usleep(60 * 1000);
    uint8_t out[4] = {0x81, 0x60, 'S', 'M'};
    bool ack[4];
    CHECK_EQ(bus_frame(out, NULL, ack, 4), 2);
    CHECK(!ack[1]);
    usleep(60 * 1000);
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, &c), 0);
    char ini[16] = {0};
    CHECK_EQ(read_file_at("SuperMemCard/settings.ini", 0, ini, 8), 0);
    CHECK(strncmp(ini, "saves=0\n", 8) == 0);
    uint8_t d[128];
    CHECK_EQ(mc_read(0, d), MC_GOOD); /* the PS1 card as ever */
    /* still off after a power-off, on again from the menu */
    vcard_stop();
    CHECK_EQ(vcard_start(sd), 0);
    vcard_settle();
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, &c), 0);
    vcard_key(VMENU_SAVES_ON);
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, &c), 1);
}

static void t_smc_free_space(void) {
    /* Hello says FFFFFFFF until the menu has counted the free space, then that less what was written */
    static uint8_t save[64 * 1024];
    fill(save, sizeof(save), 31);
    struct smc_card c;
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, &c), 1);
    CHECK_EQ(c.free_kb, 0xFFFFFFFFu);
    vcard_key(VMENU_FREE);
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, &c), 1);
    CHECK(c.free_kb != 0xFFFFFFFFu && c.free_kb > 1024);
    uint32_t before = c.free_kb;
    CHECK(smc_select(&vbus, "GBA", "Space") >= 0);
    CHECK_EQ(smc_write_file(&vbus, "save.sav", save, sizeof(save), 1), SMC_OK);
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, &c), 1);
    CHECK_EQ(c.free_kb, before - 64);
}

#define SMC_TESTS \
    {"SMC: whole-frame transport (the front ports' way)", t_smc_frame_transport, true, NULL}, \
    {"SMC: slow SD card (2.7 ms a write), 128 KB save", t_smc_slow_sd_card, true, NULL}, \
    {"SMC: Commit refuses a save with blocks missing", t_smc_commit_checks_every_block, true, NULL}, \
    {"SMC: longest system, game and file names", t_smc_longest_names, true, NULL}, \
    {"SMC: off until Hello", t_smc_off_until_hello, true, NULL}, \
    {"SMC: Hello, card ID kept on the card", t_smc_hello_and_card_id, true, NULL}, \
    {"SMC: plain PS1 card again after power-off", t_smc_off_after_power_off, true, NULL}, \
    {"SMC: save round trip (SNES)", t_smc_save_round_trip, true, NULL}, \
    {"SMC: N64 files and a 128 KB save", t_smc_n64_files_and_big_save, true, NULL}, \
    {"SMC: new save replaces the old one only at Commit", t_smc_new_save_replaces_old_only_at_commit, true, NULL}, \
    {"SMC: bad checksum and unsafe names refused", t_smc_bad_checksum_and_names, true, NULL}, \
    {"SMC: list a system's games", t_smc_list_games, true, NULL}, \
    {"SMC: PS1 card unaffected", t_smc_ps1_unaffected, true, NULL}, \
    {"SMC: virtual cards per system", t_smc_virtual_cards, true, NULL}, \
    {"SMC: Core: the buttons pick the system's card", t_smc_core_buttons_pick_the_card, true, NULL}, \
    {"SMC: a PS1 read ends the system mode", t_smc_ps1_use_ends_system_mode, true, NULL}, \
    {"SMC: card menu: saves off makes it a stock card", t_smc_menu_saves_off, true, NULL}, \
    {"SMC: card menu: free space, then Hello reports it", t_smc_free_space, true, NULL},
