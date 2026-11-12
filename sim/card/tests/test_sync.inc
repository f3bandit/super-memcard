/* Phase 1 sync tests (SMC=1): the SS1's saves folder <-> the virtual card, through host/smc_sync.c.
 * Two "SuperStations" (two saves folders, each with its own sync state) can share one card. */
#include "../../../host/smc_sync.h"
#include "../../../host/smc_ident.h"
#include <sys/stat.h>
#include <utime.h>

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
    CHECK_EQ(file_size("SuperMemCard/SNES/Card1/Super Metroid (USA)/save.sav"), sizeof(sA));
    CHECK_EQ(file_size("SuperMemCard/N64/Card1/Super Mario 64 (USA)/save.eep"), sizeof(eep));
    CHECK_EQ(file_size("SuperMemCard/N64/Card1/Super Mario 64 (USA)/save_1.cpk"), sizeof(cpk));
    CHECK_EQ(file_size("SuperMemCard/AO486"), -1);
    /* each game new on the card got its system's icon (icon.bin, 168 bytes, magic SMIC) */
    char magic[5] = {0};
    CHECK_EQ(file_size("SuperMemCard/SNES/Card1/Super Metroid (USA)/icon.bin"), 168);
    CHECK_EQ(read_file_at("SuperMemCard/SNES/Card1/Super Metroid (USA)/icon.bin", 0, magic, 4), 0);
    CHECK(strcmp(magic, "SMIC") == 0);
    CHECK_EQ(file_size("SuperMemCard/N64/Card1/Super Mario 64 (USA)/icon.bin"), 168);
    /* a PS1 save carries its own icons: no default one */
    put_save(0, "PSX/Final Fantasy VII (USA).sav", sA, 8192);
    CHECK_EQ(run_sync(0).pushed, 1);
    CHECK_EQ(file_size("SuperMemCard/PSX/Card1/Final Fantasy VII (USA)/save.sav"), 8192);
    CHECK_EQ(file_size("SuperMemCard/PSX/Card1/Final Fantasy VII (USA)/icon.bin"), -1);
    r = run_sync(0);
    CHECK_EQ(r.pushed, 0);
    CHECK_EQ(r.pulled, 0);
    CHECK_EQ(r.unchanged, 4);
    /* a changed save goes to the card as the next generation */
    sA[100] ^= 0xFF;
    put_save(0, "SNES/Super Metroid (USA).sav", sA, sizeof(sA));
    r = run_sync(0);
    CHECK_EQ(r.pushed, 1);
    CHECK_EQ(r.unchanged, 3);
    CHECK_EQ(smc_select(&vbus, "SNES", "Super Metroid (USA)"), 2); /* icon.bin, save.sav (by name) */
    struct smc_file f;
    CHECK_EQ(smc_info(&vbus, 1, &f), SMC_OK);
    CHECK_EQ(f.gen, 2);
    CHECK_EQ(smc_read_file(&vbus, 1, f.size, got), SMC_OK);
    CHECK(memcmp(got, sA, sizeof(sA)) == 0);
}

/* a PSX card image's own save icon goes to the card as its icon.bin: palette and frames as the PS1 has them */
static void t_sync_psx_own_icon(void) {
    sync_setup();
    static uint8_t img[131072], icon[8 + 32 + 384];
    memset(img, 0, sizeof(img));
    img[0] = 'M';
    img[1] = 'C';
    img[128 * 2] = 0x51;               /* the save starts in block 2 (block 1 free) */
    uint8_t *t = img + 8192 * 2;
    t[0] = 'S';
    t[1] = 'C';
    t[2] = 0x13;                       /* 3 frames */
    for (int k = 0; k < 32; k++)
        t[0x60 + k] = (uint8_t)(k * 7 + 1);
    for (int k = 0; k < 384; k++)
        t[0x80 + k] = (uint8_t)(k * 13 + 5);
    put_save(0, "PSX/Lahan Village.sav", img, sizeof(img));
    CHECK_EQ(run_sync(0).pushed, 1);
    CHECK_EQ(file_size("SuperMemCard/PSX/Card1/Lahan Village/icon.bin"), (long)sizeof(icon));
    CHECK_EQ(read_file_at("SuperMemCard/PSX/Card1/Lahan Village/icon.bin", 0, (char *)icon, sizeof(icon)), 0);
    CHECK(memcmp(icon, "SMIC\x01\x03\x10\x10", 8) == 0);
    CHECK(memcmp(icon + 8, t + 0x60, 32) == 0);
    CHECK(memcmp(icon + 40, t + 0x80, 384) == 0);
    /* and nothing else on the card changes at the next sync */
    struct smc_sync_report r = run_sync(0);
    CHECK_EQ(r.pushed, 0);
    CHECK_EQ(r.failed, 0);
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
    CHECK_EQ(smc_select(&vbus, "SMS", "Phantasy Star (USA)"), 2); /* icon.bin, save.sav (by name) */
    struct smc_file f;
    CHECK_EQ(smc_info(&vbus, 1, &f), SMC_OK);
    CHECK_EQ(smc_read_file(&vbus, 1, f.size, got), SMC_OK);
    CHECK(memcmp(got, b2, sizeof(b2)) == 0);
    char p[700];
    snprintf(p, sizeof(p), "%s/state/%s/conflicts/SMS/Card1/Phantasy Star (USA)/save.sav.gen%u", ss[0], r.card_id,
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
    snprintf(cp, sizeof(cp), "SuperMemCard/GBA/Card1/%s/name.txt", id);
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
    CHECK_EQ(file_size("SuperMemCard/NES/Card1/Zelda (USA)/save.sav"), 8192);
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


/* the card's buttons move SNES to another virtual card, as a player would during a game */
static void pick_card(const char *sys, int want) {
    CHECK(smc_core(&vbus, sys, true) >= 1);
    for (int n = smc_core(&vbus, sys, false); n != want; n += n < want ? 1 : -1)
        vcard_key(n < want ? VKEY_ENTER : VKEY_BACK);
    CHECK_EQ(smc_core(&vbus, sys, false), want);
    CHECK_EQ(smc_core(&vbus, NULL, true), 0);
}

/* Core carries the card's screen for the running system; Core again without it keeps it; a new system clears it;
 * the PS1 card again: none */
static void t_core_screen(void) {
    sync_setup();
    char n[SMC_GAME_NAME_MAX + 1];
    smc_display_name("SNES/Final Fantasy III (USA) (Rev 1).sfc", n, sizeof(n));
    CHECK(strcmp(n, "Final Fantasy III") == 0);
    smc_display_name("Crystalis", n, sizeof(n));
    CHECK(strcmp(n, "Crystalis") == 0);
    struct smc_card card;
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, &card), 1);
    CHECK(card.version >= SMC_PROTOCOL_GAME);
    const char *l[SMC_SCREEN_LINES] = {"SNES", "Final Fantasy III", "11/11/26 21:40", "On SSMC", "8 KB, 1 file"};
    CHECK(smc_core_screen(&vbus, "SNES", l) >= 1);
    CHECK(strcmp(vcard_screen(), "SNES|SNES|Final Fantasy III|11/11/26 21:40|On SSMC|8 KB, 1 file") == 0);
    CHECK(smc_core(&vbus, "SNES", true) >= 1);   /* Core again in-game, no screen: it stays */
    CHECK(strcmp(vcard_screen(), "SNES|SNES|Final Fantasy III|11/11/26 21:40|On SSMC|8 KB, 1 file") == 0);
    const char *longer[SMC_SCREEN_LINES] = {"Game Boy Color", "The Legend of Zelda: Link's Awakening DX (USA, Europe) (Rev 2)",
                                            "", "No save yet", NULL};
    CHECK(smc_core_screen(&vbus, "GBC", longer) >= 1);   /* a long game name is cut at 48, the rest of the frame still read */
    CHECK(strcmp(vcard_screen(), "GBC|Game Boy Color|The Legend of Zelda: Link's Awakening DX (USA, E||No save yet|") == 0);
    CHECK(smc_core(&vbus, "NES", true) >= 1);
    CHECK(strcmp(vcard_screen(), "NES|||||") == 0);
    CHECK_EQ(smc_core(&vbus, NULL, true), 0);
    CHECK(vcard_screen()[0] == 0);
}

static void t_sync_virtual_card_switch(void) {
    sync_setup();
    char p[700];
    put_save(0, "SNES/Super Metroid (USA).sav", sA, sizeof(sA));
    CHECK_EQ(run_sync(0).pushed, 1);
    /* card 2 picked, and the game saved again (still the card 1 save it started from) */
    pick_card("SNES", 2);
    put_save(0, "SNES/Super Metroid (USA).sav", sB, sizeof(sB));
    struct smc_sync_report r = run_sync(0);
    CHECK_EQ(r.switched, 1);
    CHECK_EQ(r.pushed, 1); /* to card 1, where it belongs */
    CHECK_EQ(r.failed, 0);
    CHECK_EQ(read_file_at("SuperMemCard/SNES/Card1/Super Metroid (USA)/save.sav", 0, got, sizeof(sB)), 0);
    CHECK(memcmp(got, sB, sizeof(sB)) == 0);
    /* card 2 is empty: the local save is moved aside, not deleted */
    CHECK_EQ(get_save(0, "SNES/Super Metroid (USA).sav", got, sizeof(got)), -1);
    snprintf(p, sizeof(p), "%s/state/%s/cards/SNES/Card1/Super Metroid (USA).sav", ss[0], r.card_id);
    FILE *k = fopen(p, "rb");
    CHECK(k);
    CHECK_EQ(fread(got, 1, sizeof(got), k), sizeof(sB));
    fclose(k);
    /* a new game on card 2 */
    put_save(0, "SNES/Chrono Trigger (USA).srm", sA, 4096);
    r = run_sync(0);
    CHECK_EQ(r.switched, 0);
    CHECK_EQ(r.pushed, 1);
    CHECK_EQ(file_size("SuperMemCard/SNES/Card2/Chrono Trigger (USA)/save.srm"), 4096);
    CHECK_EQ(file_size("SuperMemCard/SNES/Card1/Chrono Trigger (USA)/save.srm"), -1);
    /* back to card 1: its saves come back, card 2's go aside */
    pick_card("SNES", 1);
    r = run_sync(0);
    CHECK_EQ(r.switched, 1);
    CHECK_EQ(r.pulled, 1);
    CHECK_EQ(get_save(0, "SNES/Super Metroid (USA).sav", got, sizeof(got)), sizeof(sB));
    CHECK(memcmp(got, sB, sizeof(sB)) == 0);
    CHECK_EQ(get_save(0, "SNES/Chrono Trigger (USA).srm", got, sizeof(got)), -1);
    /* other systems weren't touched */
    CHECK_EQ(smc_core(&vbus, "NES", false), 1);
    r = run_sync(0);
    CHECK_EQ(r.switched + r.pushed + r.pulled + r.failed, 0);
}

static struct smc_sync_cfg cfg_of(int which);

/* the OSD's Saves page: a game's saves on every virtual card, and switching to one (smc_game_saves, smc_use_card) */
struct saves_seen { int n, card[8], files[8], playing[8]; char label[8][32]; };
static void saves_cb(int card, int files, uint32_t time, bool playing, const char *label, void *ctx) {
    (void)time;
    struct saves_seen *s = ctx;
    if (s->n < 8) {
        s->card[s->n] = card;
        s->files[s->n] = files;
        s->playing[s->n] = playing;
        snprintf(s->label[s->n], sizeof(s->label[0]), "%s", label ? label : "");
        s->n++;
    }
}

static void t_sync_osd_pick_a_save(void) {
    sync_setup();
    struct smc_sync_cfg cfg = cfg_of(0);
    /* Final Fantasy VI on card 1, and another playthrough on card 2 */
    put_save(0, "SNES/Final Fantasy VI.srm", sA, sizeof(sA));
    CHECK_EQ(run_sync(0).pushed, 1);
    pick_card("SNES", 2);
    put_save(0, "SNES/Final Fantasy VI.srm", sA, sizeof(sA));   /* (the switch moves card 1's aside) */
    run_sync(0);
    put_save(0, "SNES/Final Fantasy VI.srm", sB, sizeof(sB));
    CHECK_EQ(run_sync(0).pushed, 1);
    CHECK_EQ(read_file_at("SuperMemCard/SNES/Card2/Final Fantasy VI/save.srm", 0, got, sizeof(sB)), 0);
    CHECK(memcmp(got, sB, sizeof(sB)) == 0);

    /* the list: both cards, card 2 is the one being played */
    struct saves_seen sv = {0};
    CHECK_EQ(smc_game_saves(&vbus, &cfg, "SNES", "Final Fantasy VI", saves_cb, &sv), 2);
    CHECK(sv.n == 2 && sv.card[0] == 1 && sv.card[1] == 2 && sv.files[0] == 1 && !sv.playing[0] && sv.playing[1]);

    /* while it's played on card 2, the game saves again; then card 1's save is picked */
    sB[10] ^= 0x5A;
    put_save(0, "SNES/Final Fantasy VI.srm", sB, sizeof(sB));
    struct smc_sync_report r;
    memset(&r, 0, sizeof(r));
    CHECK_EQ(smc_use_card(&vbus, &cfg, "SNES", "Final Fantasy VI", 1, &r), 0);
    CHECK_EQ(r.switched, 1);
    /* card 2 kept the latest progress, and this SuperStation now has card 1's save */
    CHECK_EQ(read_file_at("SuperMemCard/SNES/Card2/Final Fantasy VI/save.srm", 0, got, sizeof(sB)), 0);
    CHECK(memcmp(got, sB, sizeof(sB)) == 0);
    CHECK_EQ(get_save(0, "SNES/Final Fantasy VI.srm", got, sizeof(got)), sizeof(sA));
    CHECK(memcmp(got, sA, sizeof(sA)) == 0);
    CHECK_EQ(smc_core(&vbus, "SNES", false), 1);
    memset(&sv, 0, sizeof(sv));
    CHECK_EQ(smc_game_saves(&vbus, &cfg, "SNES", "Final Fantasy VI", saves_cb, &sv), 2);
    CHECK(sv.playing[0] && !sv.playing[1]);
    /* a card with nothing on it: refused, nothing changes */
    memset(&r, 0, sizeof(r));
    CHECK(smc_use_card(&vbus, &cfg, "SNES", "Final Fantasy VI", 0, &r) < 0);
    CHECK_EQ(smc_core(&vbus, "SNES", false), 1);
}

/* the memory manager's card picker: a system's cards (games, in use), and switching the system to one, blank or not */
struct cards_seen { int n, card[8], games[8], current[8]; };
static void cards_cb(int card, int games, uint32_t newest, bool current, void *ctx) {
    (void)newest;
    struct cards_seen *s = ctx;
    if (s->n < 8) {
        s->card[s->n] = card;
        s->games[s->n] = games;
        s->current[s->n] = current;
        s->n++;
    }
}
static void t_sync_card_picker(void) {
    sync_setup();
    struct smc_sync_cfg cfg = cfg_of(0);
    put_save(0, "SNES/Final Fantasy VI.srm", sA, sizeof(sA));
    put_save(0, "SNES/Chrono Trigger.srm", sB, sizeof(sB));
    CHECK_EQ(run_sync(0).pushed, 2);
    struct cards_seen cs = {0};
    CHECK_EQ(smc_system_cards(&vbus, &cfg, "SNES", cards_cb, &cs), 1);
    CHECK(cs.card[0] == 1 && cs.games[0] == 2 && cs.current[0]);
    /* a blank card: the system's saves go to card 1 first, and the games start with none */
    struct smc_sync_report r;
    memset(&r, 0, sizeof(r));
    CHECK_EQ(smc_use_card(&vbus, &cfg, "SNES", "", 3, &r), 0);
    CHECK(get_save(0, "SNES/Final Fantasy VI.srm", got, sizeof(got)) < 0 && get_save(0, "SNES/Chrono Trigger.srm", got, sizeof(got)) < 0);
    memset(&cs, 0, sizeof(cs));
    CHECK_EQ(smc_system_cards(&vbus, &cfg, "SNES", cards_cb, &cs), 2);
    CHECK(cs.card[0] == 1 && cs.games[0] == 2 && !cs.current[0] && cs.card[1] == 3 && cs.games[1] == 0 && cs.current[1]);
    /* back to card 1: both saves here again */
    memset(&r, 0, sizeof(r));
    CHECK_EQ(smc_use_card(&vbus, &cfg, "SNES", "", 1, &r), 0);
    CHECK_EQ(get_save(0, "SNES/Final Fantasy VI.srm", got, sizeof(got)), sizeof(sA));
    CHECK(!memcmp(got, sA, sizeof(sA)));
    CHECK_EQ(get_save(0, "SNES/Chrono Trigger.srm", got, sizeof(got)), sizeof(sB));
    /* another system isn't touched */
    memset(&cs, 0, sizeof(cs));
    CHECK_EQ(smc_system_cards(&vbus, &cfg, "NES", cards_cb, &cs), 1);
    CHECK(cs.games[0] == 0 && cs.current[0] && cs.card[0] == 1);
}

/* a Saturn game's discs share one save, named as Main names it (saturn_get_save_without_disk) */
static void t_sync_saturn_disc_save_name(void) {
    static const char *const t[][2] = {
        {"Chou Jikuu Yousai Macross - Ai Oboete Imasu ka (Japan) (Disc 1)", "Chou Jikuu Yousai Macross - Ai Oboete Imasu ka (Japan)"},
        {"Panzer Dragoon Saga (USA) (Disc 4)", "Panzer Dragoon Saga (USA)"},
        {"Lunar Silver Star Story (disc 2)", "Lunar Silver Star Story"},
        {"Grandia (Japan) (Disc A)", "Grandia (Japan)"},
        {"Daytona USA (USA)", "Daytona USA (USA)"},
        {"Discworld (USA)", "Discworld (USA)"},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        char b[128];
        snprintf(b, sizeof(b), "%s", t[i][0]);
        smc_saturn_save_name(b);
        if (strcmp(b, t[i][1])) printf("    saturn name: \"%s\" -> \"%s\", Main: \"%s\"\n", t[i][0], b, t[i][1]);
        CHECK(!strcmp(b, t[i][1]));
    }
}

/* the first switch ever on this SuperStation (nothing synced here yet): its saves go to the card in use before, not to
 * the one picked (found on an SS1: a fresh card, Play on card 2) */
static void t_sync_first_switch_keeps_saves_on_the_old_card(void) {
    sync_setup();
    struct smc_sync_cfg cfg = cfg_of(0);
    put_save(0, "SNES/Final Fantasy VI.srm", sA, sizeof(sA));
    struct smc_sync_report r;
    memset(&r, 0, sizeof(r));
    CHECK_EQ(smc_use_card(&vbus, &cfg, "SNES", "Final Fantasy VI", 2, &r), 0);
    CHECK_EQ(read_file_at("SuperMemCard/SNES/Card1/Final Fantasy VI/save.srm", 0, got, sizeof(sA)), 0);
    CHECK(memcmp(got, sA, sizeof(sA)) == 0);
    CHECK(read_file_at("SuperMemCard/SNES/Card2/Final Fantasy VI/save.srm", 0, got, sizeof(sA)) != 0);
    CHECK(get_save(0, "SNES/Final Fantasy VI.srm", got, sizeof(got)) < 0);   /* card 2 has none: the game starts new */
    memset(&r, 0, sizeof(r));
    CHECK_EQ(smc_use_card(&vbus, &cfg, "SNES", "Final Fantasy VI", 1, &r), 0);
    CHECK_EQ(get_save(0, "SNES/Final Fantasy VI.srm", got, sizeof(got)), sizeof(sA));
    CHECK(memcmp(got, sA, sizeof(sA)) == 0);
}

/* the memory manager's Rename: a name for a game's save on one card, kept on the card, never a save on a SuperStation */
static void t_sync_save_names(void) {
    sync_setup();
    struct smc_sync_cfg cfg = cfg_of(0);
    put_save(0, "SNES/Final Fantasy VI.srm", sA, sizeof(sA));
    CHECK_EQ(run_sync(0).pushed, 1);
    CHECK_EQ(smc_set_label(&vbus, &cfg, "SNES", "Final Fantasy VI", 1, "Main run"), 0);
    CHECK_EQ(smc_set_label(&vbus, &cfg, "SNES", "Final Fantasy VI", 2, "Nothing here"), -2);   /* no save on card 2 */
    CHECK_EQ(smc_set_label(&vbus, &cfg, "SNES", "Final Fantasy VI", 1, "a name far too long for the OSD"), -1);
    CHECK_EQ(smc_set_label(&vbus, &cfg, "SNES", "Final Fantasy VI", 1, "tab\there"), -1);
    CHECK_EQ(smc_set_label(&vbus, &cfg, "SNES", "Final Fantasy VI", 1, "caf\xc3\xa9"), -1);       /* not the OSD's font */
    struct saves_seen sv = {0};
    CHECK_EQ(smc_game_saves(&vbus, &cfg, "SNES", "Final Fantasy VI", saves_cb, &sv), 1);
    CHECK(sv.n == 1 && !strcmp(sv.label[0], "Main run") && sv.files[0] == 1);   /* the name isn't counted as a save */
    struct smc_game_info gi;
    char id[65] = "";
    CHECK_EQ(smc_list_card(&vbus, "SNES", 1, 0, id), SMC_OK);
    CHECK_EQ(smc_game_info(&vbus, "SNES", 1, id, &gi), SMC_OK);
    CHECK(!strcmp(gi.label, "Main run") && gi.files == 1);
    /* a sync on another SuperStation, and this one: the name stays on the card, never in a saves folder */
    sB[3] ^= 0x11;
    put_save(0, "SNES/Final Fantasy VI.srm", sB, sizeof(sB));
    run_sync(0);
    run_sync(1);
    CHECK(get_save(1, "SNES/label.txt", got, sizeof(got)) < 0 && get_save(0, "SNES/label.txt", got, sizeof(got)) < 0);
    CHECK(get_save(1, "SNES/Final Fantasy VI.srm", got, sizeof(got)) > 0);
    memset(&sv, 0, sizeof(sv));
    smc_game_saves(&vbus, &cfg, "SNES", "Final Fantasy VI", saves_cb, &sv);
    CHECK(sv.n == 1 && !strcmp(sv.label[0], "Main run"));
    /* cleared */
    CHECK_EQ(smc_set_label(&vbus, &cfg, "SNES", "Final Fantasy VI", 1, ""), 0);
    memset(&sv, 0, sizeof(sv));
    smc_game_saves(&vbus, &cfg, "SNES", "Final Fantasy VI", saves_cb, &sv);
    CHECK(sv.n == 1 && !sv.label[0][0]);
}

/* the card's buttons pick another card while a game runs: its saves keep going to the card it started from */
static void t_sync_buttons_mid_game_wait_for_the_next(void) {
    sync_setup();
    put_save(0, "SNES/Super Metroid (USA).sav", sA, sizeof(sA));
    CHECK_EQ(run_sync(0).pushed, 1);
    smc_sync_hold_card(true);    /* the game starts */
    pick_card("SNES", 3);
    put_save(0, "SNES/Super Metroid (USA).sav", sB, sizeof(sB));
    struct smc_sync_report r = run_sync(0);
    CHECK_EQ(r.switched, 0);
    CHECK_EQ(r.pushed, 1);
    CHECK_EQ(read_file_at("SuperMemCard/SNES/Card1/Super Metroid (USA)/save.sav", 0, got, sizeof(sB)), 0);
    CHECK(memcmp(got, sB, sizeof(sB)) == 0);
    CHECK_EQ(file_size("SuperMemCard/SNES/Card3/Super Metroid (USA)/save.sav"), -1);
    smc_sync_hold_card(false);   /* the game ends: now the switch to card 3 */
    r = run_sync(0);
    CHECK_EQ(r.switched, 1);
    CHECK_EQ(get_save(0, "SNES/Super Metroid (USA).sav", got, sizeof(got)), -1);   /* card 3 has none: aside */
}

static void t_sync_switch_waits_when_old_card_fails(void) {
    /* a switch only happens once the old card has every save: with a conflict there, it waits */
    sync_setup();
    put_save(0, "SMS/Phantasy Star (USA).sav", sA, sizeof(sA));
    put_save(1, "SMS/Phantasy Star (USA).sav", sB, sizeof(sB));
    CHECK_EQ(run_sync(1).pushed, 1);
    struct smc_sync_report r = run_sync(0); /* both changed on card 1 */
    CHECK_EQ(r.conflicts, 1);
    pick_card("SMS", 2);
    r = run_sync(0);
    CHECK_EQ(r.switched, 0);
    CHECK_EQ(get_save(0, "SMS/Phantasy Star (USA).sav", got, sizeof(got)), sizeof(sA)); /* still in place */
    CHECK(memcmp(got, sA, sizeof(sA)) == 0);
    CHECK_EQ(file_size("SuperMemCard/SMS/Card2/Phantasy Star (USA)/save.sav"), -1);
}

static void t_sync_core_system_names(void) {
    CHECK(strcmp(smc_core_system("_Console/SNES_20250101.rbf"), "SNES") == 0);
    CHECK(strcmp(smc_core_system("/media/fat/_Console/TurboGrafx16_20240922.rbf"), "TGFX16") == 0);
    CHECK(strcmp(smc_core_system("Gameboy.rbf"), "GAMEBOY") == 0);
    CHECK(strcmp(smc_core_system("_Console/NeoGeo_20250301.rbf"), "NEOGEO") == 0);
    CHECK(strcmp(smc_core_system("_Console/PSX_20250101.rbf"), "PSX") == 0); /* every console core */
    CHECK(smc_core_system("_Console/NESticle_1.rbf") == NULL);
    CHECK(smc_core_system("menu.rbf") == NULL);
    CHECK(smc_core_system("_Computer/ao486_20250101.rbf") == NULL);
}

/* Console Mode's MGLs (seen on its SD card, 08): <setname> is the save folder, as Main's CoreName2 */
static void t_sync_mgl_setname(void) {
    char p[400];
    struct smc_mgl m;
    sync_setup();
    snprintf(p, sizeof(p), "%s/gbc.mgl", ss[0]);
    FILE *f = fopen(p, "w");
    CHECK(f);
    fputs("<mistergamedescription>\n\t<rbf>_Console/Gameboy</rbf>\n\t<setname>GBC</setname>\n"
          "\t<file delay=\"1\" type=\"f\" index=\"1\" path=\"../../../media/usb0/games/GBC/Zelda &amp; Link (USA).gbc\"/>\n"
          "</mistergamedescription>\n", f);
    fclose(f);
    CHECK(smc_mgl_read(p, &m) == 0);
    CHECK(strcmp(m.rbf, "_Console/Gameboy") == 0);
    CHECK(strcmp(m.game, "Zelda & Link (USA)") == 0);
    CHECK(strcmp(smc_launch_system(m.rbf, m.setname, m.same_dir), "GBC") == 0);
    CHECK(strcmp(smc_launch_system("_Console/SMS", "GameGear", false), "GameGear") == 0);
    CHECK(strcmp(smc_launch_system("_Console/Gameboy", "Tetris", true), "GAMEBOY") == 0);   /* same_dir="1" */
    CHECK(smc_launch_system("_Console/Gameboy", "MegaDuck", false) == NULL);
    CHECK(smc_launch_system("_Console/Atari7800", "Atari2600", false) == NULL);
    CHECK(strcmp(smc_launch_system("_Console/SNES_20260603.rbf", "", false), "SNES") == 0);
    CHECK(smc_system_icon("GameGear", &(size_t){0}) != NULL);
    CHECK(smc_mgl_read("/nonexistent.mgl", &m) != 0);
}

static struct smc_sync_cfg cfg_of(int which) {
    static char saves[2][400], state[2][400];
    snprintf(saves[which], sizeof(saves[which]), "%s/saves", ss[which]);
    snprintf(state[which], sizeof(state[which]), "%s/state", ss[which]);
    struct smc_sync_cfg cfg = {saves[which], state[which], smc_console_systems, SMC_HOST_TEST, slog};
    return cfg;
}

static void t_sync_delete_reaches_every_superstation(void) {
    sync_setup();
    char p[800];
    put_save(0, "SNES/Zelda (USA).srm", sA, 8192);
    CHECK_EQ(run_sync(0).pushed, 1);
    CHECK_EQ(run_sync(1).pulled, 1);
    /* deleted on the first SuperStation */
    struct smc_sync_cfg c0 = cfg_of(0);
    CHECK_EQ(smc_delete_save(&vbus, &c0, "SNES", "Zelda (USA).srm"), SMC_OK);
    CHECK_EQ(get_save(0, "SNES/Zelda (USA).srm", got, sizeof(got)), -1);
    CHECK_EQ(file_size("SuperMemCard/SNES/Card1/Zelda (USA)/save.srm"), -1);
    CHECK_EQ(file_size("SuperMemCard/SNES/Card1/Zelda (USA)/.deleted/save.srm"), 8192); /* kept on the card */
    struct smc_sync_report r = run_sync(0);
    snprintf(p, sizeof(p), "%s/state/%s/deleted/SNES/Card1/Zelda (USA).srm", ss[0], r.card_id);
    FILE *k = fopen(p, "rb");
    CHECK(k); /* and kept on the SuperStation */
    if (k)
        fclose(k);
    CHECK_EQ(r.pushed + r.pulled, 0); /* not brought back */
    /* the second one deletes its copy too, aside */
    r = run_sync(1);
    CHECK_EQ(r.deleted, 1);
    CHECK_EQ(get_save(1, "SNES/Zelda (USA).srm", got, sizeof(got)), -1);
    r = run_sync(1);
    CHECK_EQ(r.deleted + r.pushed + r.pulled, 0);
    /* played again later: the save comes back everywhere */
    put_save(1, "SNES/Zelda (USA).srm", sB, 8192);
    CHECK_EQ(run_sync(1).pushed, 1);
    CHECK_EQ(run_sync(0).pulled, 1);
    CHECK_EQ(get_save(0, "SNES/Zelda (USA).srm", got, sizeof(got)), 8192);
    CHECK(memcmp(got, sB, 8192) == 0);
}

static void t_sync_delete_vs_played_since(void) {
    /* deleted on one, played on the other before it synced: the played save wins, nothing lost */
    sync_setup();
    put_save(0, "NES/Metroid (USA).sav", sA, 8192);
    CHECK_EQ(run_sync(0).pushed, 1);
    CHECK_EQ(run_sync(1).pulled, 1);
    struct smc_sync_cfg c0 = cfg_of(0);
    CHECK_EQ(smc_delete_save(&vbus, &c0, "NES", "Metroid (USA).sav"), SMC_OK);
    put_save(1, "NES/Metroid (USA).sav", sB, 8192);
    struct smc_sync_report r = run_sync(1);
    CHECK_EQ(r.deleted, 0);
    CHECK_EQ(r.pushed, 1);
    CHECK_EQ(run_sync(0).pulled, 1);
    CHECK_EQ(get_save(0, "NES/Metroid (USA).sav", got, sizeof(got)), 8192);
    CHECK(memcmp(got, sB, 8192) == 0);
}

static int nconf;
static char conf_save[300];
static void on_conflict(const char *sys, int card, const char *save, uint32_t gen, void *ctx) {
    (void)ctx, (void)gen;
    nconf++;
    CHECK(strcmp(sys, "SMS") == 0);
    CHECK_EQ(card, 1);
    snprintf(conf_save, sizeof(conf_save), "%s", save);
}

static void t_sync_conflict_settled(void) {
    sync_setup();
    static uint8_t a2[8192], b2[8192];
    fill(a2, sizeof(a2), 41);
    fill(b2, sizeof(b2), 42);
    put_save(0, "SMS/Phantasy Star (USA).sav", a2, sizeof(a2));
    put_save(1, "SMS/Phantasy Star (USA).sav", b2, sizeof(b2));
    CHECK_EQ(run_sync(1).pushed, 1);
    struct smc_sync_report r = run_sync(0);
    CHECK_EQ(r.conflicts, 1);
    struct smc_sync_cfg c0 = cfg_of(0);
    nconf = 0;
    CHECK_EQ(smc_conflicts(&c0, r.card_id, on_conflict, NULL), 1);
    CHECK(strcmp(conf_save, "Phantasy Star (USA).sav") == 0);
    /* keep the card's: the local one is replaced (and kept beside) */
    CHECK_EQ(smc_resolve(&vbus, &c0, "SMS", conf_save, SMC_KEEP_CARD), SMC_OK);
    CHECK_EQ(get_save(0, "SMS/Phantasy Star (USA).sav", got, sizeof(got)), sizeof(b2));
    CHECK(memcmp(got, b2, sizeof(b2)) == 0);
    r = run_sync(0);
    CHECK_EQ(r.conflicts, 0);
    CHECK_EQ(r.pushed + r.pulled, 0);
    nconf = 0;
    CHECK_EQ(smc_conflicts(&c0, r.card_id, on_conflict, NULL), 0);
    char p[800];
    snprintf(p, sizeof(p), "%s/state/%s/conflicts/SMS/Card1/Phantasy Star (USA)/save.sav.local", ss[0], r.card_id);
    FILE *k = fopen(p, "rb");
    CHECK(k);
    if (k) {
        CHECK_EQ(fread(got, 1, sizeof(got), k), sizeof(a2));
        CHECK(memcmp(got, a2, sizeof(a2)) == 0);
        fclose(k);
    }
    /* again, keeping the SuperStation's this time: it goes to the card, and on to the other one */
    put_save(0, "SMS/Phantasy Star (USA).sav", a2, sizeof(a2));
    fill(b2, sizeof(b2), 43);
    put_save(1, "SMS/Phantasy Star (USA).sav", b2, sizeof(b2));
    CHECK_EQ(run_sync(1).pushed, 1);
    CHECK_EQ(run_sync(0).conflicts, 1);
    CHECK_EQ(smc_resolve(&vbus, &c0, "SMS", "Phantasy Star (USA).sav", SMC_KEEP_LOCAL), SMC_OK);
    CHECK_EQ(run_sync(0).conflicts, 0);
    CHECK_EQ(run_sync(1).pulled, 1);
    CHECK_EQ(get_save(1, "SMS/Phantasy Star (USA).sav", got, sizeof(got)), sizeof(a2));
    CHECK(memcmp(got, a2, sizeof(a2)) == 0);
}

/* ROM files with real headers, made up (no game's data) */
static void put_file(const char *path, const uint8_t *d, size_t n) {
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "mkdir -p \"$(dirname \"%s\")\"", path);
    CHECK(system(cmd) == 0);
    FILE *f = fopen(path, "wb");
    CHECK(f);
    CHECK(fwrite(d, 1, n, f) == n);
    fclose(f);
}
static uint8_t rom[1 << 20];
static void make_n64(const char *path, bool byteswapped) {
    memset(rom, 0x11, 4096);
    const uint8_t magic[4] = {0x80, 0x37, 0x12, 0x40};
    memcpy(rom, magic, 4);
    memcpy(rom + 0x20, "SUPER MARIO 64      ", 20);
    memcpy(rom + 0x3B, "NSME", 4);
    if (byteswapped)
        for (int i = 0; i < 4096; i += 2) { uint8_t t = rom[i]; rom[i] = rom[i + 1]; rom[i + 1] = t; }
    put_file(path, rom, 4096);
}
static void make_genesis(const char *path) {
    memset(rom, 0, 0x400);
    memcpy(rom + 0x100, "SEGA MEGA DRIVE ", 16);
    memcpy(rom + 0x150, "SONIC THE HEDGEHOG                              ", 48);
    memcpy(rom + 0x180, "GM 00001009-00", 14);
    put_file(path, rom, 0x400);
}
static void make_gba(const char *path) {
    memset(rom, 0, 0x200);
    memcpy(rom + 0xA0, "METROID4USA ", 12);
    memcpy(rom + 0xAC, "AMTE", 4);
    put_file(path, rom, 0x200);
}
static void make_sms(const char *path) {
    memset(rom, 0xFF, 0x8000);
    memcpy(rom + 0x7FF0, "TMR SEGA", 8);
    rom[0x7FFC] = 0x69, rom[0x7FFD] = 0x70, rom[0x7FFE] = 0x00; /* product code 7069 */
    put_file(path, rom, 0x8000);
}
static void make_nes(const char *path) {
    memset(rom, 0, 16 + 0x8000);
    memcpy(rom, "NES\x1a", 4);
    for (int i = 0; i < 0x8000; i++)
        rom[16 + i] = (uint8_t)(i * 7);
    put_file(path, rom, 16 + 0x8000);
}

static void t_game_ids_from_headers(void) {
    /* the game's own code, and on the card that code with the ROM's CRC (one dump, any byte order: one save set) */
    char p[600], id[65], id2[65], title[65], want[65];
    snprintf(p, sizeof(p), "%s-host/roms/a.z64", sd);
    make_n64(p, false);
    CHECK_EQ(smc_game_header_id("N64", p, id, title), 0);
    CHECK(strcmp(id, "NSME") == 0 && strcmp(title, "SUPER MARIO 64") == 0);
    CHECK_EQ(smc_game_id_of("N64", p, id, title), 0);
    snprintf(want, sizeof(want), "NSME-%08X", (unsigned)smc_crc32(rom, 4096, 0));
    CHECK(strcmp(id, want) == 0);
    snprintf(p, sizeof(p), "%s-host/roms/b.v64", sd);
    make_n64(p, true);
    CHECK_EQ(smc_game_id_of("N64", p, id2, title), 0);
    CHECK(strcmp(id, id2) == 0);
    snprintf(p, sizeof(p), "%s-host/roms/c.md", sd);
    make_genesis(p);
    CHECK_EQ(smc_game_header_id("MegaDrive", p, id, title), 0);
    CHECK(strcmp(id, "GM 00001009-00") == 0 && strcmp(title, "SONIC THE HEDGEHOG") == 0);
    CHECK_EQ(smc_game_id_of("MegaDrive", p, id, title), 0);
    CHECK(strncmp(id, "GM 00001009-00-", 15) == 0);
    /* product numbers with a letter and dash in them (seen on the SS1), and homebrew's placeholder (not an ID) */
    memcpy(rom + 0x180, "GM G-004130-00", 14);
    put_file(p, rom, 0x400);
    CHECK_EQ(smc_game_header_id("Genesis", p, id, title), 0);
    CHECK(strcmp(id, "GM G-004130-00") == 0);
    memcpy(rom + 0x180, "GM 00000000-00", 14);
    put_file(p, rom, 0x400);
    CHECK_EQ(smc_game_header_id("Genesis", p, id, title), -1);
    CHECK_EQ(smc_game_id_of("Genesis", p, id, title), 0);
    CHECK(strncmp(id, "CRC-", 4) == 0);
    snprintf(p, sizeof(p), "%s-host/roms/d.gba", sd);
    make_gba(p);
    CHECK_EQ(smc_game_header_id("GBA", p, id, title), 0);
    CHECK(strcmp(id, "AMTE") == 0);
    snprintf(p, sizeof(p), "%s-host/roms/e.sms", sd);
    make_sms(p);
    CHECK_EQ(smc_game_header_id("SMS", p, id, title), 0);
    CHECK(strcmp(id, "SMS-7069") == 0);
    CHECK_EQ(smc_game_header_id("GameGear", p, id, title), 0); /* Console Mode's Game Gear.mgl: saves/GameGear */
    CHECK(strcmp(id, "GG-7069") == 0);
    snprintf(p, sizeof(p), "%s-host/roms/f.nes", sd);
    make_nes(p);
    CHECK_EQ(smc_game_header_id("NES", p, id, title), -1);
    CHECK_EQ(smc_game_id_of("NES", p, id, title), 0);
    snprintf(want, sizeof(want), "CRC-%08X", (unsigned)smc_crc32(rom + 16, 0x8000, 0)); /* the header left out */
    CHECK(strcmp(id, want) == 0);
}

static struct smc_sync_report run_sync_games(int which) {
    static char saves[2][400], state[2][400], games[2][400];
    static const char *dirs[2][2];
    snprintf(saves[which], 400, "%s/saves", ss[which]);
    snprintf(state[which], 400, "%s/state", ss[which]);
    snprintf(games[which], 400, "%s/games", ss[which]);
    static char lists[2][400];
    dirs[which][0] = games[which];
    dirs[which][1] = NULL;
    snprintf(lists[which], 400, "%s/lists", ss[which]);
    struct smc_sync_cfg cfg = {saves[which], state[which], smc_console_systems, SMC_HOST_TEST, slog, dirs[which],
                               lists[which]};
    struct smc_sync_report r;
    CHECK_EQ(smc_sync(&vbus, &cfg, &r), 0);
    return r;
}

static void t_sync_by_game_id(void) {
    /* the same game under two file names on two SuperStations is one save set on the card, filed under the game's
     * ID, and the save keeps its time */
    sync_setup();
    char p[700];
    snprintf(p, sizeof(p), "%s/games/N64/Mario.z64", ss[0]);
    make_n64(p, false);
    snprintf(p, sizeof(p), "%s/games/N64/Ultra/Super Mario 64 (USA).v64", ss[1]);
    make_n64(p, true);
    /* a game list (libretro's format) that knows the game by its ROM's CRC */
    char dat[600], line[300];
    snprintf(p, sizeof(p), "%s/games/N64/Mario.z64", ss[0]);
    char f0[65], t0[65];
    CHECK_EQ(smc_game_id_of("N64", p, f0, t0), 0);
    snprintf(line, sizeof(line), "clrmamepro (\n\tname \"Nintendo - Nintendo 64\"\n)\n\ngame (\n\tname \"Super Mario 64 "
             "(USA)\"\n\tregion \"USA\"\n\trom ( name \"x.z64\" size 4096 crc %s serial \"NSME\" )\n)\n", strrchr(f0, '-') + 1);
    snprintf(dat, sizeof(dat), "%s/lists/Nintendo - Nintendo 64.dat", ss[0]);
    put_file(dat, (const uint8_t *)line, strlen(line));
    put_save(0, "N64/Mario.eep", eep, sizeof(eep));
    snprintf(p, sizeof(p), "%s/saves/N64/Mario.eep", ss[0]);
    struct utimbuf ut = {1700000000, 1700000000};
    CHECK(utime(p, &ut) == 0);
    CHECK_EQ(run_sync_games(0).pushed, 1);
    char folder[65], t[65], cp[300];
    snprintf(p, sizeof(p), "%s/games/N64/Mario.z64", ss[0]);
    CHECK_EQ(smc_game_id_of("N64", p, folder, t), 0); /* NSME-<crc> */
    snprintf(cp, sizeof(cp), "SuperMemCard/N64/Card1/%s/save.eep", folder);
    CHECK_EQ(file_size(cp), sizeof(eep));
    char ini[64] = {0};
    snprintf(cp, sizeof(cp), "SuperMemCard/N64/Card1/%s/game.ini", folder);
    CHECK_EQ(read_file_at(cp, 0, ini, 29), 0);
    CHECK(strncmp(ini, "id=NSME\ntitle=SUPER MARIO 64\nname=Super Mario 64 (USA)\n", 29) == 0);
    /* the other SuperStation gets it under its own ROM's name, with the save's time */
    CHECK_EQ(run_sync_games(1).pulled, 1);
    CHECK_EQ(get_save(1, "N64/Super Mario 64 (USA).eep", got, sizeof(got)), sizeof(eep));
    CHECK(memcmp(got, eep, sizeof(eep)) == 0);
    struct stat st;
    snprintf(p, sizeof(p), "%s/saves/N64/Super Mario 64 (USA).eep", ss[1]);
    CHECK(stat(p, &st) == 0);
    CHECK_EQ((long)st.st_mtime, 1700000000L);
    /* and back: still one save set */
    put_save(1, "N64/Super Mario 64 (USA).eep", sB, sizeof(eep));
    CHECK_EQ(run_sync_games(1).pushed, 1);
    CHECK_EQ(run_sync_games(0).pulled, 1);
    CHECK_EQ(get_save(0, "N64/Mario.eep", got, sizeof(got)), sizeof(eep));
    CHECK(memcmp(got, sB, sizeof(eep)) == 0);
    char game[65];
    CHECK_EQ(smc_list_card(&vbus, "N64", 1, 1, game), SMC_E_NOT_FOUND);
    /* what Console Mode's memory manager shows */
    struct smc_game_info gi;
    CHECK_EQ(smc_list_card(&vbus, "N64", 1, 0, game), SMC_OK);
    CHECK_EQ(smc_game_info(&vbus, "N64", 0, game, &gi), SMC_OK);
    CHECK(strcmp(gi.id, "NSME") == 0);
    CHECK(strcmp(gi.title, "SUPER MARIO 64") == 0);
    CHECK(strcmp(gi.name, "Super Mario 64 (USA)") == 0); /* from the game list */
    CHECK(strcmp(gi.save_name, "Super Mario 64 (USA)") == 0); /* the name it last came from */
    CHECK_EQ(gi.files, 1);
    CHECK(gi.has_icon);
    CHECK(gi.time > 1700000000u); /* the newer save, made just now */
    /* a Mega Drive ROM is often .md (not a text file) */
    snprintf(p, sizeof(p), "%s/games/MegaDrive/Sonic.md", ss[0]);
    make_genesis(p);
    put_save(0, "MegaDrive/Sonic.sav", sA, 8192);
    CHECK_EQ(run_sync_games(0).pushed, 1);
    CHECK_EQ(smc_list_card(&vbus, "MegaDrive", 1, 0, game), SMC_OK);
    CHECK(strncmp(game, "GM 00001009-00-", 15) == 0);
    /* a game with no ROM here keeps the old naming */
    put_save(0, "N64/Homebrew.eep", eep, sizeof(eep));
    CHECK_EQ(run_sync_games(0).pushed, 1);
    CHECK_EQ(file_size("SuperMemCard/N64/Card1/Homebrew/save.eep"), sizeof(eep));
}

/* a bus that counts the frames sent */
static int frames;
static void cnt_sel(void *c, bool on) { if (on) frames++; bus_sel(c, on); }
static const struct smc_bus cbus = {NULL, cnt_sel, bus_x, NULL};
static struct smc_sync_report quick(int which) {
    struct smc_sync_cfg c = cfg_of(which);
    struct smc_sync_report r;
    CHECK_EQ(smc_sync_changed(&cbus, &c, NULL, &r), 0);
    return r;
}
static struct smc_sync_report one_game(int which, const char *sys, const char *game) {
    struct smc_sync_cfg c = cfg_of(which);
    struct smc_sync_report r;
    CHECK_EQ(smc_sync_game(&cbus, &c, sys, game, &r), 0);
    return r;
}

/* one game, with the games folder (game IDs) */
static struct smc_sync_report game_roms(int which, const char *sys, const char *game) {
    static char saves[400], state[400], games[400], lists[400];
    static const char *dirs[2];
    snprintf(saves, 400, "%s/saves", ss[which]);
    snprintf(state, 400, "%s/state", ss[which]);
    snprintf(games, 400, "%s/games", ss[which]);
    snprintf(lists, 400, "%s/lists", ss[which]);
    dirs[0] = games;
    dirs[1] = NULL;
    struct smc_sync_cfg cfg = {saves, state, smc_console_systems, SMC_HOST_TEST, slog, dirs, lists};
    struct smc_sync_report r;
    CHECK_EQ(smc_sync_game(&vbus, &cfg, sys, game, &r), 0);
    return r;
}

static void t_sync_two_saves_one_game(void) {
    /* one ROM under two names here, each with its own progress (Console Mode's SD card: "Final Fantasy III (TWUE)" and
     * "Final Fantasy VI (TWUE)" are one ROM): two save sets, neither dropped, and both come back */
    sync_setup();
    char p[700];
    snprintf(p, sizeof(p), "%s/games/NES/FF3.nes", ss[0]);
    make_nes(p);
    snprintf(p, sizeof(p), "%s/games/NES/FF6.nes", ss[0]);
    make_nes(p);
    put_save(0, "NES/FF3.sav", sA, 8192);
    put_save(0, "NES/FF6.sav", sB, 8192);
    struct smc_sync_report r = run_sync_games(0);
    CHECK_EQ(r.pushed, 2);
    CHECK_EQ(r.skipped + r.failed + r.conflicts, 0);
    char g0[65], g1[65], g2[65];
    CHECK_EQ(smc_list_card(&vbus, "NES", 1, 0, g0), SMC_OK);
    CHECK_EQ(smc_list_card(&vbus, "NES", 1, 1, g1), SMC_OK);
    CHECK_EQ(smc_list_card(&vbus, "NES", 1, 2, g2), SMC_E_NOT_FOUND);
    CHECK(strchr(g0, '~') || strchr(g1, '~'));
    struct smc_game_info gi;
    CHECK_EQ(smc_game_info(&vbus, "NES", 0, strchr(g0, '~') ? g0 : g1, &gi), SMC_OK);
    CHECK(strncmp(gi.id, "CRC-", 4) == 0 && !strchr(gi.id, '~')); /* the game's ID, for the memory manager */
    CHECK(strcmp(gi.save_name, "FF3") == 0 || strcmp(gi.save_name, "FF6") == 0);
    r = run_sync_games(0);
    CHECK_EQ(r.pushed + r.pulled, 0);
    CHECK_EQ(r.unchanged, 2);
    /* one played: it goes to its own set */
    sleep(1);
    put_save(0, "NES/FF6.sav", sA, 8192);
    r = game_roms(0, "NES", "FF6");
    CHECK_EQ(r.pushed, 1);
    r = game_roms(0, "NES", "FF3");
    CHECK_EQ(r.pushed + r.pulled, 0);
    /* another SuperStation with the same two ROMs gets both, each under its own name */
    snprintf(p, sizeof(p), "%s/games/NES/FF3.nes", ss[1]);
    make_nes(p);
    snprintf(p, sizeof(p), "%s/games/NES/FF6.nes", ss[1]);
    make_nes(p);
    CHECK_EQ(run_sync_games(1).pulled, 2);
    CHECK_EQ(get_save(1, "NES/FF3.sav", got, sizeof(got)), 8192);
    CHECK(memcmp(got, sA, 8192) == 0);
    CHECK_EQ(get_save(1, "NES/FF6.sav", got, sizeof(got)), 8192);
    CHECK(memcmp(got, sA, 8192) == 0);
    /* a reinstall without the ROMs: both back under their names */
    wipe(0);
    snprintf(p, sizeof(p), "rm -rf '%s/games/NES'", ss[0]);
    CHECK(system(p) == 0);
    CHECK_EQ(run_sync_games(0).pulled, 2);
    CHECK_EQ(get_save(0, "NES/FF3.sav", got, sizeof(got)), 8192);
    CHECK(memcmp(got, sA, 8192) == 0);
    CHECK_EQ(get_save(0, "NES/FF6.sav", got, sizeof(got)), 8192);
}

static void t_sync_two_sets_one_save_file(void) {
    /* a card with two sets for one save file (filed by its name before game IDs, by its ID since): only the newer
     * comes in; the older stays on the card */
    sync_setup();
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, NULL), 1);
    CHECK(smc_select_card(&vbus, "NES", "Zelda", 1, NULL) >= 0);
    CHECK_EQ(smc_write_file_at(&vbus, "save.sav", sA, 8192, 1, 1700000000), SMC_OK);
    CHECK(smc_select_card(&vbus, "NES", "CRC-12345678", 1, NULL) >= 0);
    CHECK_EQ(smc_write_file_at(&vbus, "save.sav", sB, 8192, 1, 1700000500), SMC_OK);
    CHECK_EQ(smc_write_file(&vbus, "name.txt", (const uint8_t *)"Zelda", 5, 1), SMC_OK);
    struct smc_sync_report r = run_sync(0);
    CHECK_EQ(r.pulled, 1);
    CHECK_EQ(r.skipped, 1);
    CHECK_EQ(get_save(0, "NES/Zelda.sav", got, sizeof(got)), 8192);
    CHECK(memcmp(got, sB, 8192) == 0);
    r = run_sync(0); /* settled: the older is still left alone */
    CHECK_EQ(r.pulled + r.pushed, 0);
    CHECK_EQ(get_save(0, "NES/Zelda.sav", got, sizeof(got)), 8192);
    CHECK(memcmp(got, sB, 8192) == 0);
}

static void t_sync_clear_card_only(void) {
    /* a card cleared from the memory manager: off the card only, no SuperStation loses a save */
    sync_setup();
    put_save(0, "SNES/Mine.srm", sA, 8192);
    CHECK_EQ(run_sync(0).pushed, 1);
    CHECK_EQ(run_sync(1).pulled, 1); /* the other one has it, in step with the card */
    CHECK_EQ(smc_hello(&vbus, SMC_HOST_TEST, 0, NULL), 1);
    CHECK(smc_select_card(&vbus, "SNES", "Test Only", 1, NULL) >= 0); /* a save that is only on the card */
    CHECK_EQ(smc_write_file_at(&vbus, "save.srm", sB, 8192, 1, 1700000000), SMC_OK);
    char saves[400], state[400];
    snprintf(saves, sizeof(saves), "%s/saves", ss[0]);
    snprintf(state, sizeof(state), "%s/state", ss[0]);
    struct smc_sync_cfg cfg = {saves, state, smc_console_systems, SMC_HOST_TEST, slog};
    CHECK(strcmp(vcard_browse(), "SNES=2;") == 0);
    CHECK_EQ(smc_clear_card(&vbus, &cfg, "SNES", 1), 2);
    CHECK(strcmp(vcard_browse(), "") == 0); /* the card's Saves on card: nothing left to show */
    struct smc_game_info gi;
    CHECK_EQ(smc_game_info(&vbus, "SNES", 1, "Test Only", &gi), SMC_OK);
    CHECK_EQ(gi.files, 0);
    /* the other SuperStation keeps its copy and puts it back; the card-only save is gone and comes nowhere */
    struct smc_sync_report r = run_sync(1);
    CHECK_EQ(r.deleted, 0);
    CHECK_EQ(r.pushed, 1);
    CHECK_EQ(r.pulled, 0);
    CHECK_EQ(get_save(1, "SNES/Mine.srm", got, sizeof(got)), 8192);
    CHECK(memcmp(got, sA, 8192) == 0);
    CHECK_EQ(get_save(1, "SNES/Test Only.srm", got, sizeof(got)), -1);
    /* and the first, which cleared it, still has its own */
    r = run_sync(0);
    CHECK_EQ(r.deleted, 0); /* (the copy the other put back may come in: the same bytes) */
    CHECK_EQ(get_save(0, "SNES/Mine.srm", got, sizeof(got)), 8192);
    CHECK(memcmp(got, sA, 8192) == 0);
    CHECK_EQ(get_save(0, "SNES/Test Only.srm", got, sizeof(got)), -1);
}

static void t_sync_like_a_ps1(void) {
    /* the card is read or written only when a game's save is loaded or saved */
    sync_setup();
    put_save(0, "SNES/Super Metroid (USA).sav", sA, sizeof(sA));
    put_save(0, "N64/Super Mario 64 (USA).eep", eep, sizeof(eep));
    /* a game played and left: its save goes; the other one isn't touched */
    struct smc_sync_report r = one_game(0, "SNES", "Super Metroid (USA)");
    CHECK_EQ(r.pushed, 1);
    CHECK_EQ(file_size("SuperMemCard/N64/Card1/Super Mario 64 (USA)/save.eep"), -1);
    /* started again, nothing changed: nothing written */
    r = one_game(0, "SNES", "Super Metroid (USA)");
    CHECK_EQ(r.pushed + r.pulled, 0);
    CHECK_EQ(r.unchanged, 1);
    /* the memory manager's "read card": everything */
    CHECK_EQ(run_sync(0).pushed, 1);
    /* a game started from the core's own file browser: that system's changed saves, after it */
    sleep(1);
    put_save(0, "SNES/Super Metroid (USA).sav", sB, sizeof(sB));
    frames = 0;
    struct smc_sync_cfg c0 = cfg_of(0);
    CHECK_EQ(smc_sync_changed(&cbus, &c0, "N64", &r), 0);
    CHECK_EQ(r.pushed, 0);
    CHECK(frames <= 2); /* only the Hello */
    CHECK_EQ(smc_sync_changed(&cbus, &c0, "SNES", &r), 0);
    CHECK_EQ(r.pushed, 1);
    /* another SuperStation's newer save comes in when the game is started here, not before */
    CHECK_EQ(run_sync(1).pulled, 2);
    put_save(1, "N64/Super Mario 64 (USA).eep", sA, sizeof(eep));
    CHECK_EQ(one_game(1, "N64", "Super Mario 64 (USA)").pushed, 1);
    CHECK_EQ(quick(0).pulled, 0);
    r = one_game(0, "N64", "Super Mario 64 (USA)");
    CHECK_EQ(r.pulled, 1);
    CHECK_EQ(get_save(0, "N64/Super Mario 64 (USA).eep", got, sizeof(got)), sizeof(eep));
    CHECK(memcmp(got, sA, sizeof(eep)) == 0);
    CHECK_EQ(quick(0).pushed + quick(0).pulled, 0); /* just pulled: nothing to save after */
    /* a reinstall: a game's save comes back when it's started */
    wipe(0);
    r = one_game(0, "SNES", "Super Metroid (USA)");
    CHECK_EQ(r.pulled, 1);
    CHECK_EQ(get_save(0, "SNES/Super Metroid (USA).sav", got, sizeof(got)), sizeof(sB));
    CHECK(memcmp(got, sB, sizeof(sB)) == 0);
}

#define SYNC_TESTS \
    {"sync: OSD Saves: a game's saves on each card, switching to one", t_sync_osd_pick_a_save, true, NULL}, \
    {"sync: card buttons during a game: the next game", t_sync_buttons_mid_game_wait_for_the_next, true, NULL}, \
    {"sync: a save's name on a card (Rename): kept on the card only", t_sync_save_names, true, NULL}, \
    {"sync: the first switch here: the saves stay on the old card", t_sync_first_switch_keeps_saves_on_the_old_card, true, NULL}, \
    {"sync: a Saturn game's discs: one save, Main's name", t_sync_saturn_disc_save_name, true, NULL}, \
    {"sync: the memory manager's card picker: a system's cards, a blank one", t_sync_card_picker, true, NULL}, \
    {"sync: a PSX card image's own PS1 icon on the card", t_sync_psx_own_icon, true, NULL}, \
    {"sync: saves to the card, then nothing to do, then a change", t_sync_to_card_then_unchanged, true, NULL}, \
    {"sync: a reinstall gets every save back", t_sync_reinstall_gets_everything_back, true, NULL}, \
    {"sync: one card, two SuperStations", t_sync_between_two_superstations, true, NULL}, \
    {"sync: both changed: both kept, nothing overwritten", t_sync_both_changed_keeps_both, true, NULL}, \
    {"sync: game names over 64 characters", t_sync_long_game_name, true, NULL}, \
    {"sync: another card gets the saves too", t_sync_another_card, true, NULL}, \
    {"sync: no Super MemCard card: nothing changes", t_sync_no_card, true, NULL}, \
    {"Core carries the card's screen", t_core_screen, true, NULL}, \
    {"sync: another virtual card picked: saves go to the old one first", t_sync_virtual_card_switch, true, NULL}, \
    {"sync: no switch until the old card has every save", t_sync_switch_waits_when_old_card_fails, true, NULL}, \
    {"sync: a core's save folder from its .rbf", t_sync_core_system_names, true, NULL}, \
    {"sync: an MGL's setname is the save folder (Console Mode GBC, Game Gear)", t_sync_mgl_setname, true, NULL}, \
    {"sync: a deleted save is deleted on every SuperStation", t_sync_delete_reaches_every_superstation, true, NULL}, \
    {"sync: deleted on one, played on another: the save stays", t_sync_delete_vs_played_since, true, NULL}, \
    {"sync: both changed, settled either way", t_sync_conflict_settled, true, NULL}, \
    {"game IDs from the games' own headers", t_game_ids_from_headers, true, NULL}, \
    {"sync: one save set per game, filed by game ID, with its time", t_sync_by_game_id, true, NULL}, \
    {"sync: two saves here for one game: two sets, both kept", t_sync_two_saves_one_game, true, NULL}, \
    {"sync: two card sets for one save file: the newer comes in", t_sync_two_sets_one_save_file, true, NULL}, \
    {"sync: a card cleared: off the card only, no SuperStation loses a save", t_sync_clear_card_only, true, NULL}, \
    {"sync: like a PS1: a game's save in at its start, out after it", t_sync_like_a_ps1, true, NULL},
