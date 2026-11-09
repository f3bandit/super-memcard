/* Super MemCard phase 1 sync (see smc_sync.h). */
#define _GNU_SOURCE
#include "smc_sync.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

const char *const smc_console_systems[] = {
    "NES", "SNES", "N64", "GAMEBOY", "GBA", "SMS", "Genesis", "MegaDrive", "MegaCD", "S32X", "Saturn",
    "TGFX16", "Jaguar", "NEOGEO", "PSX", NULL,
};

/* core (the .rbf's name before its date) -> its save folder (cores/cores.tsv; Main's names, 08) */
static const struct { const char *core, *sys; } core_systems[] = {
    {"NES", "NES"}, {"SNES", "SNES"}, {"N64", "N64"}, {"Gameboy", "GAMEBOY"}, {"GBA", "GBA"}, {"SMS", "SMS"},
    {"Genesis", "Genesis"}, {"MegaDrive", "MegaDrive"}, {"MegaCD", "MegaCD"}, {"S32X", "S32X"},
    {"Saturn", "Saturn"}, {"TurboGrafx16", "TGFX16"}, {"Jaguar", "Jaguar"}, {"NeoGeo", "NEOGEO"},
};

const char *smc_core_system(const char *rbf) {
    const char *b = strrchr(rbf, '/');
    b = b ? b + 1 : rbf;
    size_t n = strcspn(b, "_.");
    for (size_t i = 0; i < sizeof(core_systems) / sizeof(core_systems[0]); i++)
        if (strlen(core_systems[i].core) == n && !strncasecmp(b, core_systems[i].core, n))
            return core_systems[i].sys;
    return NULL;
}

#define MAX_ENTRIES 4096
#define MAX_SAVE (1024 * 1024)
#define NAME_TXT "name.txt"

/* ---- small helpers ---- */

uint32_t smc_crc32(const uint8_t *p, size_t n, uint32_t crc) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

void smc_game_id(const char *game, char id[65]) {
    size_t n = strlen(game);
    if (n <= SMC_GAME_MAX) {
        memcpy(id, game, n + 1);
        return;
    }
    memcpy(id, game, 55);
    snprintf(id + 55, 10, "~%08x", (unsigned)smc_crc32((const uint8_t *)game, n, 0));
}

static const struct smc_sync_cfg *C;
static char state_dir[1024]; /* <state>/<card ID>: what was synced is per card */
static void logf_(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (C && C->log)
        C->log(line);
}

static int mkdirs(const char *dir) {
    char p[1024];
    snprintf(p, sizeof(p), "%s", dir);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            if (mkdir(p, 0755) != 0 && errno != EEXIST)
                return -1;
            *s = '/';
        }
    return (mkdir(p, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

static long read_file(const char *path, uint8_t *buf, size_t max) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, max, f);
    int more = fgetc(f) != EOF;
    fclose(f);
    return more ? -2 : (long)n;
}

/* write whole: <path>.smc-new, fsync, rename */
static int write_file(const char *path, const uint8_t *buf, size_t n) {
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.smc-new", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return -1;
    bool ok = fwrite(buf, 1, n, f) == n && fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* ---- what was last synced ---- */

struct entry {
    char sys[SMC_SYS_MAX + 1];
    int card;         /* the system's virtual card on the card (1..99) */
    char game[256];   /* the local save name (without the extension) */
    char file[17];    /* the card's file name: save.sav, save.eep, save_1.cpk ... */
    uint32_t gen, crc;
    bool seen;    /* synced before (with this card) */
    bool visited; /* handled in this run */
};
static struct entry *ents;
static int nents;

static struct entry *find(const char *sys, int card, const char *game, const char *file) {
    for (int i = 0; i < nents; i++)
        if (!strcmp(ents[i].sys, sys) && ents[i].card == card && !strcmp(ents[i].game, game) &&
            !strcmp(ents[i].file, file))
            return &ents[i];
    return NULL;
}

static struct entry *add(const char *sys, int card, const char *game, const char *file) {
    struct entry *e = find(sys, card, game, file);
    if (e || nents == MAX_ENTRIES)
        return e;
    e = &ents[nents++];
    memset(e, 0, sizeof(*e));
    e->card = card;
    snprintf(e->sys, sizeof(e->sys), "%s", sys);
    snprintf(e->game, sizeof(e->game), "%s", game);
    snprintf(e->file, sizeof(e->file), "%s", file);
    return e;
}

static void load_state(void) {
    char p[1024], line[600];
    snprintf(p, sizeof(p), "%s/synced.txt", state_dir);
    FILE *f = fopen(p, "r");
    if (!f)
        return;
    /* sys card game file gen crc (before virtual cards: no card column, card 1) */
    while (fgets(line, sizeof(line), f)) {
        char *t[6];
        int k = 0;
        for (char *s = strtok(line, "\t\n"); s && k < 6; s = strtok(NULL, "\t\n"))
            t[k++] = s;
        if (k < 5)
            continue;
        int card = k == 6 ? atoi(t[1]) : 1;
        char **u = k == 6 ? t + 2 : t + 1;
        if (card < 1 || card > SMC_CARDS_MAX)
            continue;
        struct entry *e = add(t[0], card, u[0], u[1]);
        if (e) {
            e->gen = (uint32_t)strtoul(u[2], NULL, 10);
            e->crc = (uint32_t)strtoul(u[3], NULL, 16);
            e->seen = true;
        }
    }
    fclose(f);
}

/* which virtual card each system's local saves belong to: <state>/<card ID>/cards.txt, "sys<TAB>card" */
static struct { char sys[SMC_SYS_MAX + 1]; int card; } locals[64];
static int nlocals;

static int local_card(const char *sys) {
    for (int i = 0; i < nlocals; i++)
        if (!strcmp(locals[i].sys, sys))
            return locals[i].card;
    return 0;
}

static void set_local_card(const char *sys, int card) {
    for (int i = 0; i < nlocals; i++)
        if (!strcmp(locals[i].sys, sys)) {
            locals[i].card = card;
            return;
        }
    if (nlocals < 64) {
        snprintf(locals[nlocals].sys, sizeof(locals[nlocals].sys), "%s", sys);
        locals[nlocals++].card = card;
    }
}

static void load_locals(void) {
    char p[1024], line[128];
    nlocals = 0;
    snprintf(p, sizeof(p), "%s/cards.txt", state_dir);
    FILE *f = fopen(p, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = 0;
        int card = atoi(tab + 1);
        if (card >= 1 && card <= SMC_CARDS_MAX && strlen(line) <= SMC_SYS_MAX)
            set_local_card(line, card);
    }
    fclose(f);
}

static int save_locals(void) {
    char p[1024], tmp[1100];
    snprintf(p, sizeof(p), "%s/cards.txt", state_dir);
    snprintf(tmp, sizeof(tmp), "%s.new", p);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return -1;
    for (int i = 0; i < nlocals; i++)
        fprintf(f, "%s\t%d\n", locals[i].sys, locals[i].card);
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    return ok && rename(tmp, p) == 0 ? 0 : -1;
}

static int save_state(void) {
    char p[1024], tmp[1100];
    snprintf(p, sizeof(p), "%s/synced.txt", state_dir);
    snprintf(tmp, sizeof(tmp), "%s.new", p);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return -1;
    for (int i = 0; i < nents; i++)
        if (ents[i].seen)
            fprintf(f, "%s\t%d\t%s\t%s\t%u\t%08x\n", ents[i].sys, ents[i].card, ents[i].game, ents[i].file,
                    (unsigned)ents[i].gen, (unsigned)ents[i].crc);
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    return ok && rename(tmp, p) == 0 ? 0 : -1;
}

/* ---- local save files <-> card file names ---- */

/* "Mario 64_1.cpk" -> game "Mario 64", card file "save_1.cpk"; "Zelda.sav" -> "Zelda", "save.sav" */
static bool split_name(const char *fname, char *game, size_t gsz, char *cfile, size_t csz) {
    size_t n = strlen(fname);
    if (n >= 6 && fname[n - 6] == '_' && fname[n - 5] >= '1' && fname[n - 5] <= '4' &&
        (!strcmp(fname + n - 4, ".cpk") || !strcmp(fname + n - 4, ".tpk"))) {
        if (n - 6 == 0 || n - 6 >= gsz)
            return false;
        memcpy(game, fname, n - 6);
        game[n - 6] = 0;
        snprintf(cfile, csz, "save%s", fname + n - 6);
        return true;
    }
    const char *dot = strrchr(fname, '.');
    if (!dot || dot == fname || strlen(dot) > 11 || (size_t)(dot - fname) >= gsz)
        return false;
    if (strstr(fname, ".smc-new"))
        return false;
    memcpy(game, fname, (size_t)(dot - fname));
    game[dot - fname] = 0;
    snprintf(cfile, csz, "save%s", dot);
    return true;
}

static void local_path(char *out, size_t n, const char *sys, const char *game, const char *cfile) {
    /* card "save.sav" -> "<game>.sav", "save_1.cpk" -> "<game>_1.cpk" */
    snprintf(out, n, "%s/%s/%s%s", C->saves_dir, sys, game, cfile + 4);
}

/* ---- one save file ---- */

static uint8_t *bufA, *bufB;

static int push(const struct smc_bus *b, struct entry *e, const char *id, const uint8_t *data, long n, uint32_t gen,
                struct smc_sync_report *rep) {
    if (smc_select_card(b, e->sys, id, e->card, NULL) < 0)
        return -1;
    if (strcmp(id, e->game) != 0) {
        /* shortened ID: keep the full save name on the card */
        int rc = smc_write_file(b, NAME_TXT, (const uint8_t *)e->game, (uint32_t)strlen(e->game), 1);
        if (rc)
            return rc;
    }
    int rc = smc_write_file(b, e->file, data, (uint32_t)n, gen);
    if (rc)
        return rc;
    e->gen = gen;
    e->crc = smc_crc32(data, (size_t)n, 0);
    e->seen = true;
    rep->pushed++;
    logf_("to card %d: %s/%s %s (generation %u, %ld bytes)", e->card, e->sys, e->game, e->file, (unsigned)gen, n);
    return 0;
}

static int pull(struct entry *e, const uint8_t *data, uint32_t n, uint32_t gen, struct smc_sync_report *rep) {
    char p[1024], dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s", C->saves_dir, e->sys);
    local_path(p, sizeof(p), e->sys, e->game, e->file);
    if (mkdirs(dir) != 0 || write_file(p, data, n) != 0) {
        logf_("couldn't write %s", p);
        return -1;
    }
    e->gen = gen;
    e->crc = smc_crc32(data, n, 0);
    e->seen = true;
    rep->pulled++;
    logf_("from card %d: %s/%s %s (generation %u)", e->card, e->sys, e->game, e->file, (unsigned)gen);
    return 0;
}

static int keep_both(struct entry *e, const uint8_t *card, uint32_t n, uint32_t gen, struct smc_sync_report *rep) {
    char dir[1024], p[1200];
    snprintf(dir, sizeof(dir), "%s/conflicts/%s/Card%d/%s", state_dir, e->sys, e->card, e->game);
    snprintf(p, sizeof(p), "%s/%s.gen%u", dir, e->file, (unsigned)gen);
    rep->conflicts++;
    if (access(p, F_OK) == 0)
        return 0; /* already kept */
    if (mkdirs(dir) != 0 || write_file(p, card, n) != 0)
        return -1;
    logf_("both changed: %s/%s %s: local kept in place, the card's (generation %u) in %s", e->sys, e->game, e->file,
          (unsigned)gen, p);
    return 0;
}

/* Bring one save file in step. card_idx: its index in the selected set on the card, -1 if the card
 * doesn't have it (the set must be selected when card_idx >= 0). */
static int sync_one(const struct smc_bus *b, struct entry *e, const char *id, int card_idx, const struct smc_file *cf,
                    struct smc_sync_report *rep) {
    char p[1024];
    e->visited = true;
    local_path(p, sizeof(p), e->sys, e->game, e->file);
    long ln = read_file(p, bufA, MAX_SAVE);
    if (ln == -2) {
        logf_("skipped %s: over 1 MB", p);
        rep->skipped++;
        return 0;
    }
    bool have_local = ln > 0, have_card = card_idx >= 0;
    uint32_t lcrc = have_local ? smc_crc32(bufA, (size_t)ln, 0) : 0;
    bool local_changed = have_local && (!e->seen || lcrc != e->crc);
    bool card_changed = have_card && (!e->seen || cf->gen != e->gen);

    if (have_local && !have_card)
        return push(b, e, id, bufA, ln, (e->seen ? e->gen : 0) + 1, rep);
    if (!have_local && have_card) {
        if (smc_read_file(b, card_idx, cf->size, bufB) != SMC_OK)
            return -1;
        return pull(e, bufB, cf->size, cf->gen, rep);
    }
    if (!have_local)
        return 0;
    if (!local_changed && !card_changed) {
        rep->unchanged++;
        return 0;
    }
    if (local_changed && !card_changed)
        return push(b, e, id, bufA, ln, cf->gen + 1, rep);
    if (smc_read_file(b, card_idx, cf->size, bufB) != SMC_OK)
        return -1;
    if (!local_changed)
        return pull(e, bufB, cf->size, cf->gen, rep);
    /* both changed, or first sync with both sides there */
    if ((uint32_t)ln == cf->size && memcmp(bufA, bufB, (size_t)ln) == 0) {
        e->gen = cf->gen;
        e->crc = lcrc;
        e->seen = true;
        rep->unchanged++;
        return 0;
    }
    return keep_both(e, bufB, cf->size, cf->gen, rep);
}

/* ---- the whole sync ---- */

/* everything the card has for one game ID, then the local files of that game */
static int sync_game(const struct smc_bus *b, const char *sys, int card, const char *game, const char *id,
                     struct smc_sync_report *rep) {
    struct smc_file files[16];
    int n = smc_select_card(b, sys, id, card, NULL);
    if (n < 0)
        return n;
    for (int i = 0; i < n; i++)
        if (smc_info(b, i, &files[i]) != SMC_OK)
            return -1;
    char local_game[256];
    snprintf(local_game, sizeof(local_game), "%s", game);
    for (int i = 0; i < n; i++)
        if (!strcmp(files[i].name, NAME_TXT) && files[i].size < sizeof(local_game)) {
            if (smc_read_file(b, i, files[i].size, (uint8_t *)local_game) == SMC_OK)
                local_game[files[i].size] = 0;
        }

    /* the card's files */
    for (int i = 0; i < n; i++) {
        if (!strcmp(files[i].name, NAME_TXT))
            continue;
        struct entry *e = add(sys, card, local_game, files[i].name);
        if (!e)
            return -1;
        if (smc_select_card(b, sys, id, card, NULL) < 0) /* pushes in between select other sets */
            return -1;
        if (sync_one(b, e, id, i, &files[i], rep) != 0) {
            rep->failed++;
            logf_("failed: %s/%s %s", sys, local_game, files[i].name);
        }
    }

    /* local files of this game the card doesn't have */
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s", C->saves_dir, sys);
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        char g[256], cfile[17];
        if (de->d_name[0] == '.' || !split_name(de->d_name, g, sizeof(g), cfile, sizeof(cfile)) || strcmp(g, local_game))
            continue;
        bool on_card = false;
        for (int i = 0; i < n; i++)
            on_card |= !strcmp(files[i].name, cfile);
        if (on_card)
            continue;
        struct entry *e = add(sys, card, local_game, cfile);
        if (!e || sync_one(b, e, id, -1, NULL, rep) != 0) {
            rep->failed++;
            logf_("failed: %s/%s %s", sys, local_game, cfile);
        }
    }
    closedir(d);
    return 0;
}

/* one system on one of its virtual cards: every game the card has, then local games it doesn't */
static void sync_system(const struct smc_bus *b, const char *sys, int card, struct smc_sync_report *rep) {
    for (int i = 0; i < nents; i++)
        if (!strcmp(ents[i].sys, sys))
            ents[i].visited = false;
    for (int gi = 0;; gi++) {
        char id[65];
        if (smc_list_card(b, sys, card, gi, id) != SMC_OK)
            break;
        if (sync_game(b, sys, card, id, id, rep) < 0) {
            rep->failed++;
            logf_("failed: %s/%s: the card didn't answer", sys, id);
        }
    }
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s", C->saves_dir, sys);
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *de;
    static char done[256][256];
    int ndone = 0;
    while ((de = readdir(d))) {
        char g[256], cfile[17], id[65];
        if (de->d_name[0] == '.' || !split_name(de->d_name, g, sizeof(g), cfile, sizeof(cfile)))
            continue;
        bool already = false;
        for (int i = 0; i < nents && !already; i++)
            already = !strcmp(ents[i].sys, sys) && ents[i].card == card && !strcmp(ents[i].game, g) && ents[i].visited;
        for (int i = 0; i < ndone && !already; i++)
            already = !strcmp(done[i], g);
        if (already)
            continue;
        if (ndone < 256)
            snprintf(done[ndone++], 256, "%s", g);
        smc_game_id(g, id);
        if (sync_game(b, sys, card, g, id, rep) < 0) {
            rep->failed++;
            logf_("failed: %s/%s: the card didn't answer", sys, g);
        }
    }
    closedir(d);
}

/* Moving a system to another virtual card: its local saves (all on the old card now) are moved out of
 * the saves folder into <state>/<card ID>/cards/<sys>/Card<old>/, so the new card's saves come in alone
 * and nothing is deleted. */
static int set_aside(const char *sys, int old) {
    char dir[1024], to[1024];
    snprintf(dir, sizeof(dir), "%s/%s", C->saves_dir, sys);
    snprintf(to, sizeof(to), "%s/cards/%s/Card%d", state_dir, sys, old);
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    if (mkdirs(to) != 0) {
        closedir(d);
        return -1;
    }
    int rc = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        char g[256], cfile[17], from[1300], dst[1300];
        if (de->d_name[0] == '.' || !split_name(de->d_name, g, sizeof(g), cfile, sizeof(cfile)))
            continue;
        snprintf(from, sizeof(from), "%s/%s", dir, de->d_name);
        snprintf(dst, sizeof(dst), "%s/%s", to, de->d_name);
        if (rename(from, dst) != 0) {
            long n = read_file(from, bufA, MAX_SAVE);
            if (n < 0 || write_file(dst, bufA, (size_t)n) != 0 || unlink(from) != 0) {
                logf_("couldn't move %s aside", from);
                rc = -1;
            }
        }
    }
    closedir(d);
    return rc;
}

int smc_sync(const struct smc_bus *b, const struct smc_sync_cfg *cfg, struct smc_sync_report *rep) {
    memset(rep, 0, sizeof(*rep));
    C = cfg;
    struct smc_card card;
    if (smc_hello(b, cfg->host, 0, &card) != 1) {
        logf_("no Super MemCard card");
        return 0;
    }
    rep->card = true;
    memcpy(rep->card_id, card.id, sizeof(rep->card_id));
    logf_("Super MemCard card %s (version %u)", card.id, card.version);
    if (card.version < SMC_PROTOCOL) {
        logf_("the card's firmware is too old (Super MemCard %u, needs %u): not synced", card.version, SMC_PROTOCOL);
        return 0;
    }

    snprintf(state_dir, sizeof(state_dir), "%s/%s", cfg->state_dir, card.id);
    if (mkdirs(state_dir) != 0)
        return -1;
    ents = calloc(MAX_ENTRIES, sizeof(*ents));
    bufA = malloc(MAX_SAVE);
    bufB = malloc(MAX_SAVE);
    if (!ents || !bufA || !bufB) {
        free(ents), free(bufA), free(bufB);
        return -1;
    }
    nents = 0;
    load_state();

    load_locals();
    for (const char *const *s = cfg->systems; *s; s++) {
        int now = smc_core(b, *s, false);
        if (now < 1) {
            rep->failed++;
            logf_("failed: %s: the card didn't say which of its cards is in use", *s);
            continue;
        }
        int was = local_card(*s);
        if (was && was != now) {
            /* the card's buttons picked another card for this system: the local saves belong to the old
             * one, so they go to it first; only then are they replaced by the new card's */
            int failed = rep->failed, conflicts = rep->conflicts;
            sync_system(b, *s, was, rep);
            if (rep->failed != failed || rep->conflicts != conflicts || set_aside(*s, was) != 0) {
                logf_("%s stays on card %d: its saves couldn't all be put on it first", *s, was);
                continue;
            }
            logf_("%s: card %d -> card %d", *s, was, now);
            rep->switched++;
        }
        set_local_card(*s, now);
        sync_system(b, *s, now, rep);
    }

    int rc = save_state() | save_locals();
    logf_("sync done: %d to card, %d from card, %d unchanged, %d both changed, %d skipped, %d failed", rep->pushed,
          rep->pulled, rep->unchanged, rep->conflicts, rep->skipped, rep->failed);
    free(ents), free(bufA), free(bufB);
    ents = NULL;
    return rc;
}
