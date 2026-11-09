/* Phase 1 sync tests (SMC=1): the SS1's saves folder <-> the virtual card, through host/smc_sync.c.
 * Two "SuperStations" (two saves folders, each with its own sync state) can share one card. */
#include "../../../host/smc_sync.h"

static char ss[2][300];      /* <ss>/saves, <ss>/state */
static int quiet_log;
static void slog(const char *l) {
    if (!quiet_log && getenv("SYNC_LOG"))
        fprintf(stderr, "      sync: %s\n", l);
}

static void ss_init(void) {
    for (int i = 0; i < 2; i++) {
        snprintf(ss[i], sizeof(ss[i]), "%s-host/ss%d", sd, i + 1);
        char cmd[400];
        snprintf(cmd, sizeof(cmd), "mkdir -p '%s/saves' '%s/state'", ss[i], ss[i]);
        CHECK(system(cmd) == 0);
    }
}

static struct smc_sync_report run_sync(int which) {
    char saves[400], state[400];
    snprintf(saves, sizeof(saves), "%s/saves", ss[which]);
    snprintf(state, sizeof(state), "%s/state", ss[which]);
    struct smc_sync_cfg cfg = {saves, state, smc_console_systems, SMC_HOST_TEST, slog};
    struct smc_sync_report r;
    CHECK_EQ(smc_sync(&vbus, &cfg, &r), 0);
    return r;
}

/* files in a saves folder: path relative to <ss>/saves */
static void put_save(int which, const char *rel, const uint8_t *d, size_t n) {
    char p[600];
    snprintf(p, sizeof(p), "%s/saves/%s", ss[which], rel);
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "mkdir -p \"$(dirname \"%s\")\"", p);
    CHECK(system(cmd) == 0);
    FILE *f = fopen(p, "wb");
    CHECK(f);
    CHECK(fwrite(d, 1, n, f) == n);
    fclose(f);
}
static long get_save(int which, const char *rel, uint8_t *d, size_t max) {
    char p[600];
    snprintf(p, sizeof(p), "%s/saves/%s", ss[which], rel);
    FILE *f = fopen(p, "rb");
    if (!f)
        return -1;
    long n = (long)fread(d, 1, max, f);
    fclose(f);
    return n;
}
static void wipe(int which) { /* a reinstall: no saves, no sync state */
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s/saves' '%s/state' && mkdir -p '%s/saves' '%s/state'", ss[which], ss[which],
             ss[which], ss[which]);
    CHECK(system(cmd) == 0);
}

static uint8_t sA[8192], sB[8192], eep[512], cpk[32768], got[32768];

static void sync_setup(void) {
    ss_init();
    fill(sA, sizeof(sA), 11);
    fill(sB, sizeof(sB), 12);
    fill(eep, sizeof(eep), 13);
    fill(cpk, sizeof(cpk), 14);
}

static void t_sync_to_card_then_unchanged(void) {
    sync_setup();
    put_save(0, "SNES/Super Metroid (USA).sav", sA, sizeof(sA));
    put_save(0, "N64/Super Mario 64 (USA).eep", eep, sizeof(eep));
    put_save(0, "N64/Super Mario 64 (USA)_1.cpk", cpk, sizeof(cpk));
    put_save(0, "AO486/not a console.sav", sA, 64); /* not one of the 15 consoles: stays off the card */
    struct smc_sync_report r = run_sync(0);
    CHECK(r.card);
    CHECK_EQ(r.pushed, 3);
    CHECK_EQ(r.failed, 0);
    CHECK_EQ(file_size("SuperMemCard/SNES/Super Metroid (USA)/save.sav"), sizeof(sA));
    CHECK_EQ(file_size("SuperMemCard/N64/Super Mario 64 (USA)/save.eep"), sizeof(eep));
    CHECK_EQ(file_size("SuperMemCard/N64/Super Mario 64 (USA)/save_1.cpk"), sizeof(cpk));
    CHECK_EQ(file_size("SuperMemCard/AO486"), -1);
    r = run_sync(0);
    CHECK_EQ(r.pushed, 0);
    CHECK_EQ(r.pulled, 0);
    CHECK_EQ(r.unchanged, 3);
    /* a changed save goes to the card as the next generation */
    sA[100] ^= 0xFF;
    put_save(0, "SNES/Super Metroid (USA).sav", sA, sizeof(sA));
    r = run_sync(0);
    CHECK_EQ(r.pushed, 1);
    CHECK_EQ(r.unchanged, 2);
    CHECK_EQ(smc_select(&vbus, "SNES", "Super Metroid (USA)"), 1);
    struct smc_file f;
    CHECK_EQ(smc_info(&vbus, 0, &f), SMC_OK);
    CHECK_EQ(f.gen, 2);
    CHECK_EQ(smc_read_file(&vbus, 0, f.size, got), SMC_OK);
    CHECK(memcmp(got, sA, sizeof(sA)) == 0);
}

static void t_sync_reinstall_gets_everything_back(void) {
    sync_setup();
    put_save(0, "SNES/Super Metroid (USA).sav", sA, sizeof(sA));
    put_save(0, "N64/Super Mario 64 (USA).eep", eep, sizeof(eep));
    put_save(0, "N64/Super Mario 64 (USA)_1.cpk", cpk, sizeof(cpk));
    CHECK_EQ(run_sync(0).pushed, 3);
    wipe(0);
    struct smc_sync_report r = run_sync(0);
    CHECK_EQ(r.pulled, 3);
    CHECK_EQ(r.failed, 0);
    CHECK_EQ(get_save(0, "SNES/Super Metroid (USA).sav", got, sizeof(got)), sizeof(sA));
    CHECK(memcmp(got, sA, sizeof(sA)) == 0);
    CHECK_EQ(get_save(0, "N64/Super Mario 64 (USA).eep", got, sizeof(got)), sizeof(eep));
    CHECK(memcmp(got, eep, sizeof(eep)) == 0);
    CHECK_EQ(get_save(0, "N64/Super Mario 64 (USA)_1.cpk", got, sizeof(got)), sizeof(cpk));
    CHECK(memcmp(got, cpk, sizeof(cpk)) == 0);
    CHECK_EQ(run_sync(0).unchanged, 3);
}

static void t_sync_between_two_superstations(void) {
    sync_setup();
    put_save(0, "GBA/Golden Sun (USA).sav", sA, sizeof(sA));
    CHECK_EQ(run_sync(0).pushed, 1);
    /* the card goes to a second SS1: it gets the save */
    CHECK_EQ(run_sync(1).pulled, 1);
    CHECK_EQ(get_save(1, "GBA/Golden Sun (USA).sav", got, sizeof(got)), sizeof(sA));
    CHECK(memcmp(got, sA, sizeof(sA)) == 0);
    /* played there, synced there; back on the first: the newer save comes in */
    put_save(1, "GBA/Golden Sun (USA).sav", sB, sizeof(sB));
    CHECK_EQ(run_sync(1).pushed, 1);
    struct smc_sync_report r = run_sync(0);
    CHECK_EQ(r.pulled, 1);
    CHECK_EQ(r.conflicts, 0);
    CHECK_EQ(get_save(0, "GBA/Golden Sun (USA).sav", got, sizeof(got)), sizeof(sB));
    CHECK(memcmp(got, sB, sizeof(sB)) == 0);
}

static void t_sync_both_changed_keeps_both(void) {
    sync_setup();
    put_save(0, "SMS/Phantasy Star (USA).sav", sA, sizeof(sA));
    CHECK_EQ(run_sync(0).pushed, 1);
    CHECK_EQ(run_sync(1).pulled, 1);
    /* both SuperStations play before syncing again */
    uint8_t a2[8192], b2[8192];
    memcpy(a2, sA, sizeof(a2));
    memcpy(b2, sA, sizeof(b2));
    a2[1] ^= 1;
    b2[2] ^= 1;
    put_save(0, "SMS/Phantasy Star (USA).sav", a2, sizeof(a2));
    put_save(1, "SMS/Phantasy Star (USA).sav", b2, sizeof(b2));
    CHECK_EQ(run_sync(1).pushed, 1); /* the second one syncs first: the card has b2 */
    struct smc_sync_report r = run_sync(0);
    CHECK_EQ(r.conflicts, 1);
    CHECK_EQ(r.pushed, 0);
    CHECK_EQ(r.pulled, 0);
    /* nothing overwritten: the local save is a2, the card still b2, and b2 kept beside */
    CHECK_EQ(get_save(0, "SMS/Phantasy Star (USA).sav", got, sizeof(got)), sizeof(a2));
    CHECK(memcmp(got, a2, sizeof(a2)) == 0);
    CHECK_EQ(smc_select(&vbus, "SMS", "Phantasy Star (USA)"), 1);
    struct smc_file f;
    CHECK_EQ(smc_info(&vbus, 0, &f), SMC_OK);
    CHECK_EQ(smc_read_file(&vbus, 0, f.size, got), SMC_OK);
    CHECK(memcmp(got, b2, sizeof(b2)) == 0);
    char p[700];
    snprintf(p, sizeof(p), "%s/state/%s/conflicts/SMS/Phantasy Star (USA)/save.sav.gen%u", ss[0], r.card_id,
             (unsigned)f.gen);
    FILE *k = fopen(p, "rb");
    CHECK(k);
    CHECK_EQ(fread(got, 1, sizeof(got), k), sizeof(b2));
    fclose(k);
    CHECK(memcmp(got, b2, sizeof(b2)) == 0);
    /* reported again next time, not copied again */
    r = run_sync(0);
    CHECK_EQ(r.conflicts, 1);
}

static void t_sync_long_game_name(void) {
    sync_setup();
    const char *name = "Legend of Zelda, The - A Link to the Past & Four Swords (USA, Australia) (Rev 1) (Virtual Console)";
    char rel[300], id[65];
    snprintf(rel, sizeof(rel), "GBA/%s.sav", name);
    put_save(0, rel, sA, sizeof(sA));
    CHECK_EQ(run_sync(0).pushed, 1);
    smc_game_id(name, id);
    CHECK_EQ(strlen(id), 64);
    CHECK(id[55] == '~');
    char cp[200];
    snprintf(cp, sizeof(cp), "SuperMemCard/GBA/%s/name.txt", id);
    CHECK_EQ(file_size(cp), (long)strlen(name));
    wipe(0);
    CHECK_EQ(run_sync(0).pulled, 1);
    CHECK_EQ(get_save(0, rel, got, sizeof(got)), sizeof(sA)); /* back under its full name */
    CHECK(memcmp(got, sA, sizeof(sA)) == 0);
}

static void t_sync_another_card(void) {
    /* a different card: the sync state is per card, so the saves go onto it too */
    sync_setup();
    put_save(0, "NES/Zelda (USA).sav", sA, 8192);
    CHECK_EQ(run_sync(0).pushed, 1);
    vcard_stop();
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s/SuperMemCard'", sd); /* a new card */
    CHECK(system(cmd) == 0);
    CHECK_EQ(vcard_start(sd), 0);
    struct smc_sync_report r = run_sync(0);
    CHECK_EQ(r.pushed, 1);
    CHECK_EQ(file_size("SuperMemCard/NES/Zelda (USA)/save.sav"), 8192);
}

static uint8_t never(void *c, uint8_t o, bool l, bool *ack) { (void)c; (void)o; (void)l; *ack = false; return 0xFF; }
static void t_sync_no_card(void) {
    sync_setup();
    put_save(0, "SNES/Super Metroid (USA).sav", sA, sizeof(sA));
    struct smc_bus none = {NULL, bus_sel, never, NULL};
    char saves[400], state[400];
    snprintf(saves, sizeof(saves), "%s/saves", ss[0]);
    snprintf(state, sizeof(state), "%s/state", ss[0]);
    struct smc_sync_cfg cfg = {saves, state, smc_console_systems, SMC_HOST_TEST, slog};
    struct smc_sync_report r;
    CHECK_EQ(smc_sync(&none, &cfg, &r), 0);
    CHECK(!r.card);
    CHECK_EQ(r.pushed, 0);
    CHECK_EQ(get_save(0, "SNES/Super Metroid (USA).sav", got, sizeof(got)), sizeof(sA));
}

#define SYNC_TESTS \
    {"sync: saves to the card, then nothing to do, then a change", t_sync_to_card_then_unchanged, true, NULL}, \
    {"sync: a reinstall gets every save back", t_sync_reinstall_gets_everything_back, true, NULL}, \
    {"sync: one card, two SuperStations", t_sync_between_two_superstations, true, NULL}, \
    {"sync: both changed: both kept, nothing overwritten", t_sync_both_changed_keeps_both, true, NULL}, \
    {"sync: game names over 64 characters", t_sync_long_game_name, true, NULL}, \
    {"sync: another card gets the saves too", t_sync_another_card, true, NULL}, \
    {"sync: no Super MemCard card: nothing changes", t_sync_no_card, true, NULL},
