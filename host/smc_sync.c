/* Super MemCard phase 1 sync (see smc_sync.h). */
#define _GNU_SOURCE
#include "smc_sync.h"
#include "smc_ident.h"
#include "smc_gamedb.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

const char *const smc_console_systems[] = {
    "NES", "SNES", "N64", "GAMEBOY", "GBC", "GBA", "SMS", "Genesis", "MegaDrive", "MegaCD", "S32X", "Saturn",
    "TGFX16", "Jaguar", "NEOGEO", "PSX", "GameGear", NULL,
};

/* a save folder's name for a screen */
const char *smc_system_title(const char *sys) {
    static const struct { const char *sys, *title; } t[] = {
        {"NES", "NES"}, {"SNES", "SNES"}, {"N64", "Nintendo 64"}, {"GAMEBOY", "Game Boy"}, {"GBC", "Game Boy Color"},
        {"GBA", "Game Boy Advance"}, {"SMS", "Master System"}, {"GameGear", "Game Gear"}, {"Genesis", "Genesis"},
        {"MegaDrive", "Mega Drive"}, {"MegaCD", "Mega-CD"}, {"S32X", "32X"}, {"Saturn", "Saturn"},
        {"TGFX16", "TurboGrafx-16"}, {"Jaguar", "Jaguar"}, {"NEOGEO", "Neo Geo"}, {"PSX", "PlayStation"},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (!strcmp(t[i].sys, sys))
            return t[i].title;
    return sys;
}

/* core (the .rbf's name before its date) -> its save folder (cores/cores.tsv; Main's names, 08) */
static const struct { const char *core, *sys; } core_systems[] = {
    {"NES", "NES"}, {"SNES", "SNES"}, {"N64", "N64"}, {"Gameboy", "GAMEBOY"}, {"GBA", "GBA"}, {"SMS", "SMS"},
    {"Genesis", "Genesis"}, {"MegaDrive", "MegaDrive"}, {"MegaCD", "MegaCD"}, {"S32X", "S32X"},
    {"Saturn", "Saturn"}, {"TurboGrafx16", "TGFX16"}, {"Jaguar", "Jaguar"}, {"NeoGeo", "NEOGEO"}, {"PSX", "PSX"},
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

const char *smc_launch_system(const char *rbf, const char *setname, bool same_dir) {
    if (!setname || !setname[0] || same_dir)
        return smc_core_system(rbf);
    /* Main's CoreName2: the save folder is the MGL's <setname> (Console Mode's GameboyColor.mgl: the Gameboy core,
     * saves/GBC; Game Gear.mgl: the SMS core, saves/GameGear) */
    if (!smc_core_system(rbf))
        return NULL;
    for (int i = 0; smc_console_systems[i]; i++)
        if (!strcasecmp(setname, smc_console_systems[i]))
            return smc_console_systems[i];
    return NULL; /* Mega Duck, Atari 2600 ...: not a system the card keeps */
}

/* the text of <tag ...>text</tag>, and the tag's attributes in attr */
static int xml_tag(const char *x, const char *tag, char *out, size_t on, char *attr, size_t an) {
    char open[40];
    snprintf(open, sizeof(open), "<%s", tag);
    for (const char *p = x; (p = strcasestr(p, open)); p++) {
        const char *q = p + strlen(open);
        if (*q != '>' && *q != ' ' && *q != '\t' && *q != '/')
            continue;
        const char *e = strchr(q, '>');
        if (!e)
            return -1;
        if (attr)
            snprintf(attr, an, "%.*s", (int)(e - q), q);
        if (e[-1] == '/') { /* <tag ... /> */
            if (out)
                out[0] = 0;
            return 0;
        }
        const char *c = strchr(e + 1, '<');
        if (out)
            snprintf(out, on, "%.*s", c ? (int)(c - e - 1) : 0, e + 1);
        return 0;
    }
    return -1;
}

static void xml_unescape(char *s) {
    static const char *const ent[][2] = {{"&amp;", "&"}, {"&apos;", "'"}, {"&quot;", "\""}, {"&lt;", "<"}, {"&gt;", ">"}};
    for (int k = 0; k < 5; k++)
        for (char *a; (a = strstr(s, ent[k][0]));) {
            *a = ent[k][1][0];
            memmove(a + 1, a + strlen(ent[k][0]), strlen(a + strlen(ent[k][0])) + 1);
        }
}

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && strchr(" \t\r\n", s[n - 1]))
        s[--n] = 0;
    size_t k = strspn(s, " \t\r\n");
    memmove(s, s + k, strlen(s + k) + 1);
}

int smc_mgl_read(const char *path, struct smc_mgl *m) {
    memset(m, 0, sizeof(*m));
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f)
        return -1;
    static char buf[16384];
    size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = 0;
    char attr[512];
    if (xml_tag(buf, "rbf", m->rbf, sizeof(m->rbf), NULL, 0) == 0) {
        trim(m->rbf);
        xml_unescape(m->rbf);
    }
    if (xml_tag(buf, "setname", m->setname, sizeof(m->setname), attr, sizeof(attr)) == 0) {
        trim(m->setname);
        xml_unescape(m->setname);
        char *a = strcasestr(attr, "same_dir");
        if (a && (a = strchr(a, '=')))
            m->same_dir = atoi(a + 1 + (a[1] == '"' || a[1] == '\'')) == 1;
    }
    /* <file ... path="..."/>: Main names the save after the game file, without its folder and extension */
    if (xml_tag(buf, "file", NULL, 0, attr, sizeof(attr)) == 0) {
        char *p = strstr(attr, "path=");
        if (p) {
            p += 5;
            char qc = *p == '"' || *p == '\'' ? *p++ : ' ';
            char *e = strchr(p, qc);
            if (e)
                *e = 0;
            xml_unescape(p);
            char *base = strrchr(p, '/');
            base = base ? base + 1 : p;
            snprintf(m->game, sizeof(m->game), "%s", base);
            char *dot = strrchr(m->game, '.');
            if (dot && dot != m->game)
                *dot = 0;
        }
    }
    return 0;
}

#define MAX_ENTRIES 4096
#define MAX_SAVE (1024 * 1024)
#define NAME_TXT "name.txt"
#define ICON_BIN "icon.bin" /* the game's save icon (docs/05-design.md): not a save, never copied back */
#define GAME_INI "game.ini" /* the game: id=, title= (docs/09-game-database.md) */
static bool not_a_save(const char *n) { return !strcmp(n, NAME_TXT) || !strcmp(n, ICON_BIN) || !strcmp(n, GAME_INI); }

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
    long mtime, size; /* the local file when last synced: unchanged since means nothing to do (quick sync) */
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
    /* sys card game file gen crc mtime size (before virtual cards: no card column, card 1; before the quick sync: no
     * mtime and size) */
    while (fgets(line, sizeof(line), f)) {
        char *t[8];
        int k = 0;
        for (char *s = strtok(line, "\t\n"); s && k < 8; s = strtok(NULL, "\t\n"))
            t[k++] = s;
        if (k < 5)
            continue;
        int card = k >= 6 ? atoi(t[1]) : 1;
        char **u = k >= 6 ? t + 2 : t + 1;
        if (card < 1 || card > SMC_CARDS_MAX)
            continue;
        struct entry *e = add(t[0], card, u[0], u[1]);
        if (e) {
            e->gen = (uint32_t)strtoul(u[2], NULL, 10);
            e->crc = (uint32_t)strtoul(u[3], NULL, 16);
            e->mtime = k == 8 ? atol(t[6]) : -1;
            e->size = k == 8 ? atol(t[7]) : -1;
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
            fprintf(f, "%s\t%d\t%s\t%s\t%u\t%08x\t%ld\t%ld\n", ents[i].sys, ents[i].card, ents[i].game, ents[i].file,
                    (unsigned)ents[i].gen, (unsigned)ents[i].crc, ents[i].mtime, ents[i].size);
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

/* ---- the game a save belongs to (docs/09-game-database.md) ---- */

/* the games found in the system's games folders: Main names a save after its ROM, so a save "X.sav" belongs to the
 * game file named X (any extension) */
struct rom {
    char sys[SMC_SYS_MAX + 1], base[256], path[1024];
    long size, mtime;
    char id[65], title[65];
    int known; /* 0 not looked at, 1 id found, -1 can't be read */
};
static struct rom *roms;
static int nroms, maxroms;
static char indexed[64][SMC_SYS_MAX + 1];
static int nindexed;

static bool not_a_game(const char *n) {
    /* not .md: that's a Mega Drive ROM (a Genesis game was missed for it on the SS1, 08) */
    static const char *const skip[] = {".txt", ".png", ".jpg", ".xml", ".ini", ".cfg", ".pdf", ".nfo",
                                       ".sav", ".srm", ".mgl", ".json", NULL};
    const char *dot = strrchr(n, '.');
    if (n[0] == '.' || !dot)
        return true;
    for (const char *const *e = skip; *e; e++)
        if (!strcasecmp(dot, *e))
            return true;
    return false;
}

static void index_dir(const char *sys, const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d))) {
        char p[1024];
        struct stat st;
        if (de->d_name[0] == '.')
            continue;
        snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
        if (stat(p, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 3)
                index_dir(sys, p, depth + 1);
            continue;
        }
        if (not_a_game(de->d_name))
            continue;
        if (nroms == maxroms) {
            int m = maxroms ? maxroms * 2 : 256;
            struct rom *r = realloc(roms, (size_t)m * sizeof(*r));
            if (!r)
                break;
            roms = r, maxroms = m;
        }
        struct rom *r = &roms[nroms++];
        memset(r, 0, sizeof(*r));
        snprintf(r->sys, sizeof(r->sys), "%s", sys);
        snprintf(r->base, sizeof(r->base), "%.*s", (int)(strrchr(de->d_name, '.') - de->d_name), de->d_name);
        snprintf(r->path, sizeof(r->path), "%s", p);
        r->size = (long)st.st_size;
        r->mtime = (long)st.st_mtime;
    }
    closedir(d);
}

static void load_ids(void);

static void index_system(const char *sys) {
    for (int i = 0; i < nindexed; i++)
        if (!strcmp(indexed[i], sys))
            return;
    if (nindexed < 64)
        snprintf(indexed[nindexed++], SMC_SYS_MAX + 1, "%s", sys);
    for (const char *const *g = C->games_dirs; g && *g; g++) {
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s/%s", *g, sys);
        index_dir(sys, dir, 0);
    }
    load_ids(); /* the IDs worked out before, for files that haven't changed */
}

/* game IDs already worked out, kept between runs: <state>/ids.txt, path size time id title */
static void load_ids(void) {
    char p[1100], line[1500];
    snprintf(p, sizeof(p), "%s/ids.txt", C->state_dir);
    FILE *f = fopen(p, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        char *t[5] = {0};
        int k = 0;
        for (char *q = strtok(line, "\t\n"); q && k < 5; q = strtok(NULL, "\t\n"))
            t[k++] = q;
        if (k < 4)
            continue;
        for (int i = 0; i < nroms; i++)
            if (!roms[i].known && !strcmp(roms[i].path, t[0]) && roms[i].size == atol(t[1]) &&
                roms[i].mtime == atol(t[2])) {
                snprintf(roms[i].id, sizeof(roms[i].id), "%s", t[3]);
                snprintf(roms[i].title, sizeof(roms[i].title), "%s", k > 4 ? t[4] : "");
                roms[i].known = 1;
            }
    }
    fclose(f);
}

static void save_ids(void) {
    char p[1100], tmp[1200];
    snprintf(p, sizeof(p), "%s/ids.txt", C->state_dir);
    snprintf(tmp, sizeof(tmp), "%s.new", p);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    /* the ones known now, then the older ones not looked at this time (other systems' games) */
    for (int i = 0; i < nroms; i++)
        if (roms[i].known == 1)
            fprintf(f, "%s\t%ld\t%ld\t%s\t%s\n", roms[i].path, roms[i].size, roms[i].mtime, roms[i].id, roms[i].title);
    FILE *old = fopen(p, "r");
    char line[1500];
    while (old && fgets(line, sizeof(line), old)) {
        char path[1024];
        const char *tab = strchr(line, '\t');
        if (!tab || tab - line >= (long)sizeof(path))
            continue;
        snprintf(path, sizeof(path), "%.*s", (int)(tab - line), line);
        bool have = false;
        for (int i = 0; i < nroms && !have; i++)
            have = !strcmp(roms[i].path, path);
        if (!have)
            fputs(line, f);
    }
    if (old)
        fclose(old);
    fclose(f);
    rename(tmp, p);
}

static struct rom *rom_for(const char *sys, const char *game) {
    if (!C->games_dirs)
        return NULL;
    index_system(sys);
    for (int i = 0; i < nroms; i++)
        if (!strcmp(roms[i].sys, sys) && !strcmp(roms[i].base, game))
            return &roms[i];
    return NULL;
}

/* ---- starting a game from the memory manager (smcd PLAY) ---- */

/* how Console Mode's own MGLs load each system's game (its _@Homebrew MGLs): the core, the save folder when it isn't
 * the core's (<setname>), the file slot. Systems not here (CDs, arcade, and the ones without an MGL seen) start only
 * from the MGL of an earlier launch */
static const struct { const char *sys, *core, *setname; int index, delay; } mgl_slots[] = {
    {"NES", "NES", NULL, 1, 2},          {"SNES", "SNES", NULL, 0, 2},       {"GAMEBOY", "Gameboy", NULL, 1, 2},
    {"GBC", "Gameboy", "GBC", 1, 2},     {"GBA", "GBA", NULL, 1, 2},          {"MegaDrive", "MegaDrive", NULL, 1, 1},
    {"N64", "N64", NULL, 1, 1},          {"SMS", "SMS", NULL, 1, 1},          {"TGFX16", "TurboGrafx16", NULL, 0, 1},
};

static void xml_escape(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (; *in && o + 7 < n; in++) {
        const char *e = *in == '&' ? "&amp;" : *in == '<' ? "&lt;" : *in == '>' ? "&gt;" : *in == '"' ? "&quot;" : NULL;
        if (e)
            o += (size_t)snprintf(out + o, n - o, "%s", e);
        else
            out[o++] = *in;
    }
    out[o] = 0;
}

int smc_game_mgl(const struct smc_sync_cfg *cfg, const char *sys, const char *game, char *out, size_t n) {
    C = cfg;
    if (!out || !n)
        return -1;
    out[0] = 0;
    for (size_t i = 0; i < sizeof(mgl_slots) / sizeof(mgl_slots[0]); i++) {
        if (strcmp(mgl_slots[i].sys, sys))
            continue;
        struct rom *r = rom_for(sys, game);
        if (!r) {
            /* not in the index made earlier: the folders read again (a game copied in since) */
            int w = 0;
            for (int j = 0; j < nroms; j++)
                if (strcmp(roms[j].sys, sys))
                    roms[w++] = roms[j];
            nroms = w;
            w = 0;
            for (int j = 0; j < nindexed; j++)
                if (strcmp(indexed[j], sys))
                    memmove(indexed[w++], indexed[j], sizeof(indexed[j]));
            nindexed = w;
            r = rom_for(sys, game);
        }
        if (!r)
            return -1;
        char path[2100], set[96] = "";
        xml_escape(r->path, path, sizeof(path));
        if (mgl_slots[i].setname)
            snprintf(set, sizeof(set), "\t<setname>%s</setname>\n", mgl_slots[i].setname);
        int k = snprintf(out, n,
                         "<mistergamedescription>\n\t<rbf>_Console/%s</rbf>\n%s\t<file delay=\"%d\" type=\"f\" "
                         "index=\"%d\" path=\"%s\"/>\n</mistergamedescription>\n",
                         mgl_slots[i].core, set, mgl_slots[i].delay, mgl_slots[i].index, path);
        return k > 0 && (size_t)k < n ? 0 : -1;
    }
    return -1;
}

static int id_of(struct rom *r) {
    if (!r->known)
        r->known = smc_game_id_of(r->sys, r->path, r->id, r->title) == 0 ? 1 : -1;
    return r->known == 1 ? 0 : -1;
}

/* the card's folder for a local save: the game's ID when its ROM is found, else (as before) the save's name */
static bool card_id_for(const char *sys, const char *game, char id[65], char title[65]) {
    struct rom *r = rom_for(sys, game);
    if (r && id_of(r) == 0) {
        snprintf(id, 65, "%s", r->id);
        if (title)
            snprintf(title, 65, "%s", r->title);
        return true;
    }
    smc_game_id(game, id);
    if (title)
        title[0] = 0;
    return false;
}

/* this SuperStation's name for a game on the card: the ROM here with that game ID (so Main finds the save) */
static const char *local_for_id(const char *sys, const char *id) {
    if (!C->games_dirs)
        return NULL;
    index_system(sys);
    for (int i = 0; i < nroms; i++) {
        if (strcmp(roms[i].sys, sys))
            continue;
        /* a cartridge with a code: only ROMs with that code are hashed (reading a header is quick, a whole ROM isn't) */
        const char *dash = strrchr(id, '-');
        if (!roms[i].known && dash && strncmp(id, "CRC-", 4) && dash - id <= 20) {
            char hid[65], t[65];
            if (smc_game_header_id(sys, roms[i].path, hid, t) != 0 || strlen(hid) != (size_t)(dash - id) ||
                strncmp(hid, id, (size_t)(dash - id)))
                continue;
        }
        if (id_of(&roms[i]) == 0 && !strcmp(roms[i].id, id))
            return roms[i].base;
    }
    return NULL;
}

/* a local save of that game name (any of its files) in saves/<sys> */
static bool local_save_exists(const char *sys, const char *game) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s", C->saves_dir, sys);
    DIR *d = opendir(dir);
    struct dirent *de;
    bool found = false;
    while (d && !found && (de = readdir(d))) {
        char g[256], cfile[17];
        found = de->d_name[0] != '.' && split_name(de->d_name, g, sizeof(g), cfile, sizeof(cfile)) && !strcmp(g, game);
    }
    if (d)
        closedir(d);
    return found;
}

/* The card's set for a local save. Its game's ID; but when this SuperStation has two saves for one game (two copies of
 * a ROM under different names, each with its own progress, or two hacks of a disc that keep its product code; seen on
 * Console Mode's SD card, 08), each keeps its own set: the one the card's set already belongs to (its name.txt) keeps
 * the ID, the other is <id>~<CRC of its save name>. Nothing is merged or dropped. */
static void card_id_of_save(const struct smc_bus *b, const char *sys, int card, const char *game, char id[65]) {
    if (!card_id_for(sys, game, id, NULL) || card < 1)
        return;
    char own[65];
    snprintf(own, sizeof(own), "%.55s~%08X", id, (unsigned)smc_crc32((const uint8_t *)game, strlen(game), 0));
    if (smc_select_card(b, sys, own, card, NULL) > 0) { /* it has its own set already: it stays there */
        snprintf(id, 65, "%s", own);
        return;
    }
    int n = smc_select_card(b, sys, id, card, NULL);
    char owner[256] = "";
    for (int i = 0; i < n; i++) {
        struct smc_file f;
        if (smc_info(b, i, &f) == SMC_OK && !strcmp(f.name, NAME_TXT) && !f.deleted && f.size < sizeof(owner)) {
            if (smc_read_file(b, i, f.size, (uint8_t *)owner) == SMC_OK)
                owner[f.size] = 0;
            else
                owner[0] = 0;
            break;
        }
    }
    if (owner[0] && strcmp(owner, game) && local_save_exists(sys, owner))
        snprintf(id, 65, "%s", own);
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
    /* the save's time: when Main wrote it (its file's time, the SuperStation's clock) */
    char lp[1024];
    struct stat st;
    local_path(lp, sizeof(lp), e->sys, e->game, e->file);
    uint32_t t = stat(lp, &st) == 0 && st.st_mtime > 0 ? (uint32_t)st.st_mtime : 0;
    int rc = smc_write_file_at(b, e->file, data, (uint32_t)n, gen, t);
    if (rc)
        return rc;
    e->gen = gen;
    e->crc = smc_crc32(data, (size_t)n, 0);
    e->seen = true;
    rep->pushed++;
    logf_("to card %d: %s/%s %s (generation %u, %ld bytes)", e->card, e->sys, e->game, e->file, (unsigned)gen, n);
    return 0;
}

static int pull(struct entry *e, const uint8_t *data, uint32_t n, uint32_t gen, uint32_t time,
                struct smc_sync_report *rep) {
    char p[1024], dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s", C->saves_dir, e->sys);
    local_path(p, sizeof(p), e->sys, e->game, e->file);
    if (mkdirs(dir) != 0 || write_file(p, data, n) != 0) {
        logf_("couldn't write %s", p);
        return -1;
    }
    if (time) {
        /* the time the save was made, not when it was copied here */
        struct timeval tv[2] = {{(time_t)time, 0}, {(time_t)time, 0}};
        utimes(p, tv);
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

/* A deleted save's local file goes to <state>/<card ID>/deleted/<sys>/Card<N>/, never deleted outright */
static int trash_local(const struct entry *e, const char *path) {
    char dir[1024], to[1300];
    snprintf(dir, sizeof(dir), "%s/deleted/%s/Card%d", state_dir, e->sys, e->card);
    const char *base = strrchr(path, '/');
    snprintf(to, sizeof(to), "%s/%s", dir, base ? base + 1 : path);
    if (mkdirs(dir) != 0)
        return -1;
    if (rename(path, to) == 0)
        return 0;
    long n = read_file(path, bufB, MAX_SAVE);
    if (n < 0 || write_file(to, bufB, (size_t)n) != 0 || unlink(path) != 0) {
        logf_("couldn't move %s to %s", path, to);
        return -1;
    }
    return 0;
}

/* Bring one save file in step. card_idx: its index in the selected set on the card, -1 if the card
 * doesn't have it (the set must be selected when card_idx >= 0). */
static int sync_one_(const struct smc_bus *b, struct entry *e, const char *id, int card_idx, const struct smc_file *cf,
                     struct smc_sync_report *rep);

/* the local file as it is now (after the sync): the quick sync skips it while it stays so */
static void note_local(struct entry *e) {
    char p[1024];
    struct stat st;
    local_path(p, sizeof(p), e->sys, e->game, e->file);
    if (stat(p, &st) == 0)
        e->mtime = (long)st.st_mtime, e->size = (long)st.st_size;
    else
        e->mtime = e->size = -1;
}

static int sync_one(const struct smc_bus *b, struct entry *e, const char *id, int card_idx, const struct smc_file *cf,
                    struct smc_sync_report *rep) {
    int rc = sync_one_(b, e, id, card_idx, cf, rep);
    if (rc == 0)
        note_local(e);
    return rc;
}

static int sync_one_(const struct smc_bus *b, struct entry *e, const char *id, int card_idx, const struct smc_file *cf,
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
    bool have_local = ln > 0, have_card = card_idx >= 0 && !cf->deleted;
    uint32_t lcrc = have_local ? smc_crc32(bufA, (size_t)ln, 0) : 0;
    bool local_changed = have_local && (!e->seen || lcrc != e->crc);

    if (card_idx >= 0 && cf->deleted) {
        /* deleted on the card (by another SuperStation, or by this one) */
        if (have_local && (local_changed || e->gen >= cf->gen))
            return push(b, e, id, bufA, ln, cf->gen + 1, rep); /* played since: the save comes back */
        if (have_local && trash_local(e, p) != 0)
            return -1;
        if (have_local) {
            rep->deleted++;
            logf_("deleted on the card: %s/%s %s (kept in the state folder's deleted/)", e->sys, e->game, e->file);
        }
        e->gen = cf->gen;
        e->crc = 0;
        e->seen = true;
        return 0;
    }
    bool card_changed = have_card && (!e->seen || cf->gen != e->gen);

    if (have_local && !have_card)
        return push(b, e, id, bufA, ln, (e->seen ? e->gen : 0) + 1, rep);
    if (!have_local && have_card) {
        if (smc_read_file(b, card_idx, cf->size, bufB) != SMC_OK)
            return -1;
        return pull(e, bufB, cf->size, cf->gen, cf->time, rep);
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
        return pull(e, bufB, cf->size, cf->gen, cf->time, rep);
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

/* Systems whose saves carry the game's own icon, and how to read it into icon.bin's layout (docs/05-design.md). A save
 * with its own icon shows that one in Console Mode and SS1 Tool; every other save its system's icon. A new system
 * whose saves carry an icon is one more entry here. */
static const struct {
    const char *sys;
    size_t (*read)(const uint8_t *save, size_t len, uint8_t *out, size_t out_max);
} icon_readers[] = {
    {"PSX", smc_ps1_icon}, /* a PS1 card image: its first save's icon */
};

/* The game's own icon from its save file in the saves folder, as icon.bin: its length, 0 if none */
static size_t own_icon(const char *sys, const char *dir, const char *game, uint8_t *out, size_t out_max) {
    size_t (*read)(const uint8_t *, size_t, uint8_t *, size_t) = NULL;
    for (size_t i = 0; i < sizeof(icon_readers) / sizeof(icon_readers[0]); i++)
        if (!strcmp(icon_readers[i].sys, sys))
            read = icon_readers[i].read;
    if (!read)
        return 0;
    DIR *d = opendir(dir);
    struct dirent *de;
    size_t got = 0;
    while (d && !got && (de = readdir(d))) {
        char g[256], f[17], path[1300];
        if (de->d_name[0] == '.' || !split_name(de->d_name, g, sizeof(g), f, sizeof(f)) || strcmp(g, game))
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        FILE *fp = fopen(path, "rb");
        if (!fp)
            continue;
        static uint8_t img[256 * 1024 + 1];   /* the biggest save with an icon read whole */
        size_t n = fread(img, 1, sizeof(img), fp);
        fclose(fp);
        got = read(img, n, out, out_max);
    }
    if (d)
        closedir(d);
    return got;
}

/* ---- Save icons for games whose saves carry none (docs/05-design.md) ----
 * The icon database, on the SuperStation: <state>/icons/<system>/<game ID>.bin, each game's 32x32 icon made once from
 * its picture; <game ID>.none: no picture anywhere (not asked for again for 30 days, except at a manual card read).
 * Where a game's picture comes from, in order (smc_fetch_icons, in the background: a sync never waits on the net):
 *   1. ours: SMC_ICON_REPO/<system>/<game ID>.png (mods, patched games, homebrew; the public super-memcard-cores repo)
 *   2. libretro's cover, by the game's official name from the game lists
 *   3. Console Mode's own art beside the game (media/<game>.png), offline
 * A game without one yet shows its system's icon; the download list is <state>/icons/queue.txt, and icons made but not
 * yet on the card <state>/icons/ready.txt (smc_apply_icons puts them there). PSX saves always use their own icon. */
#define ICON_RETRY_DAYS 30
#define SMC_ICON_REPO "https://raw.githubusercontent.com/f3bandit/super-memcard-cores/main/icons"
#define SMC_LIBRETRO_THUMBS "https://thumbnails.libretro.com"

static bool icon_retry; /* a manual card read: ask again for games marked .none */
void smc_icons_retry(bool on) { icon_retry = on; }

static void icon_db_path(char *out, size_t n, const char *sys, const char *base, const char *ext) {
    snprintf(out, n, "%s/icons/%s/%s.%s", C->state_dir, sys, base, ext);
}

static size_t read_small(const char *path, uint8_t *out, size_t max) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t n = fread(out, 1, max, f);
    fclose(f);
    return n;
}

static bool put_small(const char *path, const void *d, size_t len) {
    char tmp[1300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return false;
    bool ok = fwrite(d, 1, len, f) == len;
    ok &= fclose(f) == 0;
    if (ok)
        ok = rename(tmp, path) == 0;
    else
        unlink(tmp);
    return ok;
}

static void icons_line(const char *file, const char *line) {
    char dir[1100], path[1200];
    snprintf(dir, sizeof(dir), "%s/icons", C->state_dir);
    mkdirs(dir);
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    FILE *f = fopen(path, "a");
    if (f) {
        fputs(line, f);
        fclose(f);
    }
}

/* the game's icon from the database; none yet: the game goes on the download list. Its length, 0 for none yet. */
static size_t db_icon(const char *sys, int card, const char *id, const char *base, const char *game, uint8_t *out,
                      size_t out_max) {
    char path[1400];
    struct stat st;
    icon_db_path(path, sizeof(path), sys, base, "bin");
    size_t l = read_small(path, out, out_max);
    if (l >= 8 && !memcmp(out, "SMIC", 4))
        return l;
    icon_db_path(path, sizeof(path), sys, base, "none");
    if (!icon_retry && stat(path, &st) == 0 && time(NULL) - st.st_mtime < ICON_RETRY_DAYS * 86400)
        return 0;
    const char *crc = NULL, *dash = strrchr(base, '-');
    if (!strncmp(base, "CRC-", 4))
        crc = base + 4;
    else if (dash && strlen(dash + 1) == 8 && strspn(dash + 1, "0123456789ABCDEF") == 8)
        crc = dash + 1;
    const char *name = C->gamedb_dir ? smc_gamedb_name(C->gamedb_dir, sys, crc, crc ? NULL : base) : NULL;
    struct rom *r = rom_for(sys, game);
    char line[2600];
    snprintf(line, sizeof(line), "%s\t%d\t%s\t%s\t%s\t%s\n", sys, card, id, base, name ? name : "", r ? r->path : "");
    icons_line("queue.txt", line);
    return 0;
}

/* libretro's file name for a game (these characters become _), then the URL's escapes */
static void url_name(char *out, size_t n, const char *name) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && o + 4 < n; p++) {
        unsigned char c = *p;
        if (strchr("&*/:`<>?\\|\"", c))
            c = '_';
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || strchr("-._~", c))
            out[o++] = (char)c;
        else
            o += (size_t)snprintf(out + o, n - o, "%%%02X", c);
    }
    out[o] = 0;
}

/* one picture, downloaded and made into an icon */
static size_t fetch_icon(int (*fetch)(const char *url, const char *to), const char *url, const char *tmp, uint8_t *out,
                         size_t out_max) {
    size_t l = 0;
    if (fetch(url, tmp) == 0)
        l = smc_art_icon(tmp, out, out_max);
    unlink(tmp);
    return l;
}

int smc_fetch_icons(const struct smc_sync_cfg *cfg, int (*fetch)(const char *url, const char *to)) {
    C = cfg;
    char q[1100], work[1100];
    snprintf(q, sizeof(q), "%s/icons/queue.txt", C->state_dir);
    snprintf(work, sizeof(work), "%s/icons/queue.work", C->state_dir);
    bool have = rename(q, work) == 0;
    /* games set aside because their game list wasn't here yet: tried again with this run */
    char later[1100];
    snprintf(later, sizeof(later), "%s/icons/queue.later", C->state_dir);
    FILE *lf = fopen(later, "r");
    if (lf) {
        FILE *wf = fopen(work, "a");
        char buf[2600];
        while (wf && fgets(buf, sizeof(buf), lf))
            fputs(buf, wf);
        if (wf)
            fclose(wf);
        fclose(lf);
        unlink(later);
        have = true;
    }
    if (!have)
        return 0;
    FILE *f = fopen(work, "r");
    char line[2600];
    int made = 0;
    static char done[512][100];
    int nd = 0;
    while (f && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *v[6];
        int k = 0;
        for (char *p = line; k < 6; k++) {
            v[k] = p;
            char *t = strchr(p, '\t');
            if (!t) {
                k++;
                break;
            }
            *t = 0;
            p = t + 1;
        }
        if (k < 6)
            continue;
        const char *sys = v[0], *card = v[1], *id = v[2], *base = v[3], *name = v[4], *rom = v[5];
        char key[100], bin[1300], none[1300], tmp[1300], ready[1200];
        snprintf(key, sizeof(key), "%s/%s", sys, base);
        bool seen = false;
        for (int i = 0; i < nd; i++)
            seen |= !strcmp(done[i], key);
        snprintf(ready, sizeof(ready), "%s\t%s\t%s\t%s\n", sys, card, id, base);
        icon_db_path(bin, sizeof(bin), sys, base, "bin");
        if (seen || access(bin, F_OK) == 0) {
            if (access(bin, F_OK) == 0)
                icons_line("ready.txt", ready); /* another card set of a game already done */
            continue;
        }
        if (nd < 512)
            snprintf(done[nd++], sizeof(done[0]), "%s", key);
        char dir[1100];
        snprintf(dir, sizeof(dir), "%s/icons/%s", C->state_dir, sys);
        mkdirs(dir);
        snprintf(tmp, sizeof(tmp), "%s/.fetch", dir);
        static uint8_t icon[8 + 32 + 512];
        size_t l = 0;
        char url[1400], en[600], el[200];
        /* 1. ours, by game ID */
        url_name(en, sizeof(en), base);
        url_name(el, sizeof(el), sys);
        snprintf(url, sizeof(url), "%s/%s/%s.png", SMC_ICON_REPO, el, en);
        l = fetch_icon(fetch, url, tmp, icon, sizeof(icon));
        /* 2. libretro's cover, by official name, in each of the system's lists (looked up again here: the lists
         * may have come since the game was listed) */
        char fresh_name[400] = "";
        bool lists_missing = false;
        if (!*name && C->gamedb_dir) {
            const char *crc = NULL, *dash = strrchr(base, '-');
            if (!strncmp(base, "CRC-", 4))
                crc = base + 4;
            else if (dash && strlen(dash + 1) == 8 && strspn(dash + 1, "0123456789ABCDEF") == 8)
                crc = dash + 1;
            const char *nm = smc_gamedb_name(C->gamedb_dir, sys, crc, crc ? NULL : base);
            if (nm)
                snprintf(fresh_name, sizeof(fresh_name), "%s", nm);
            const char *lists[3];
            int disc, nl = smc_gamedb_lists(sys, lists, &disc), have = 0;
            for (int li = 0; li < nl; li++) {
                char lp[1300];
                snprintf(lp, sizeof(lp), "%s/%s.dat", C->gamedb_dir, lists[li]);
                have += access(lp, R_OK) == 0;
            }
            lists_missing = nl > 0 && have == 0;   /* none of the system's lists here yet */
            if (*fresh_name)
                name = fresh_name;
        }
        if (!l && *name) {
            const char *lists[3];
            int disc, nl = smc_gamedb_lists(sys, lists, &disc);
            url_name(en, sizeof(en), name);
            for (int li = 0; li < nl && !l; li++) {
                url_name(el, sizeof(el), lists[li]);
                snprintf(url, sizeof(url), "%s/%s/Named_Boxarts/%s.png", SMC_LIBRETRO_THUMBS, el, en);
                l = fetch_icon(fetch, url, tmp, icon, sizeof(icon));
            }
        }
        /* 3. Console Mode's art beside the game: media/<game>.png in its folder or one above */
        if (!l && *rom) {
            char d2[1024], romname[256], path[1400];
            const char *sl = strrchr(rom, '/');
            snprintf(romname, sizeof(romname), "%s", sl ? sl + 1 : rom);
            char *dot = strrchr(romname, '.');
            if (dot)
                *dot = 0;
            snprintf(d2, sizeof(d2), "%s", rom);
            for (int up = 0; up < 3 && !l; up++) {
                char *cut = strrchr(d2, '/');
                if (!cut)
                    break;
                *cut = 0;
                static const char *const ext[] = {"png", "jpg", "jpeg", "PNG", "JPG"};
                for (size_t e = 0; e < sizeof(ext) / sizeof(ext[0]) && !l; e++) {
                    snprintf(path, sizeof(path), "%s/media/%s.%s", d2, romname, ext[e]);
                    if (access(path, R_OK) == 0)
                        l = smc_art_icon(path, icon, sizeof(icon));
                }
            }
        }
        icon_db_path(none, sizeof(none), sys, base, "none");
        if (l && put_small(bin, icon, l)) {
            unlink(none);
            icons_line("ready.txt", ready);
            made++;
        } else if (lists_missing && !*name) {
            char again[2600];   /* its game list isn't here yet (offline, or no gamedb module): ask again later */
            snprintf(again, sizeof(again), "%s\t%s\t%s\t%s\t\t%s\n", sys, card, id, base, rom);
            icons_line("queue.later", again);
        } else {
            put_small(none, "", 0); /* no picture anywhere */
        }
    }
    if (f)
        fclose(f);
    unlink(work);
    return made;
}

int smc_icon_lists_needed(const struct smc_sync_cfg *cfg, char names[][96], int max) {
    C = cfg;
    if (!C->gamedb_dir)
        return 0;
    int n = 0;
    const char *files[] = {"queue.txt", "queue.later"};
    for (int fi = 0; fi < 2; fi++) {
        char q[1100], line[2600];
        snprintf(q, sizeof(q), "%s/icons/%s", C->state_dir, files[fi]);
        FILE *f = fopen(q, "r");
        while (f && fgets(line, sizeof(line), f)) {
            char *tab = strchr(line, '\t');
            if (!tab)
                continue;
            *tab = 0;
            const char *lists[3];
            int disc, nl = smc_gamedb_lists(line, lists, &disc);
            for (int li = 0; li < nl; li++) {
                char lp[1300];
                snprintf(lp, sizeof(lp), "%s/%s.dat", C->gamedb_dir, lists[li]);
                if (!access(lp, R_OK))
                    continue;
                bool have = false;
                for (int k = 0; k < n; k++)
                    have |= !strcmp(names[k], lists[li]);
                if (!have && n < max)
                    snprintf(names[n++], 96, "%s", lists[li]);
            }
        }
        if (f)
            fclose(f);
    }
    return n;
}

int smc_apply_icons(const struct smc_bus *b, const struct smc_sync_cfg *cfg) {
    C = cfg;
    char r[1100], work[1100];
    snprintf(r, sizeof(r), "%s/icons/ready.txt", C->state_dir);
    snprintf(work, sizeof(work), "%s/icons/ready.work", C->state_dir);
    if (rename(r, work))
        return 0;
    FILE *f = fopen(work, "r");
    char line[600];
    int put = 0;
    while (f && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char sys[40], id[80], base[80];
        int card;
        if (sscanf(line, "%39[^\t]\t%d\t%79[^\t]\t%79s", sys, &card, id, base) != 4)
            continue;
        static uint8_t icon[8 + 32 + 512];
        char bin[1300];
        icon_db_path(bin, sizeof(bin), sys, base, "bin");
        size_t l = read_small(bin, icon, sizeof(icon));
        if (l < 8 || memcmp(icon, "SMIC", 4))
            continue;
        /* only over the system's placeholder (or no icon): never over a game's own */
        int n = smc_select_card(b, sys, id, card, NULL);
        if (n < 0)
            continue;
        int at = -1;
        struct smc_file fi;
        for (int i = 0; i < n; i++)
            if (smc_info(b, i, &fi) == SMC_OK && !strcmp(fi.name, ICON_BIN) && !fi.deleted) {
                at = i;
                break;
            }
        uint32_t gen = 1;
        if (at >= 0) {
            size_t dl;
            const uint8_t *def = smc_system_icon(sys, &dl);
            uint8_t cur[256];
            if (!def || fi.size != dl || smc_read_file(b, at, (uint32_t)dl, cur) != SMC_OK || memcmp(cur, def, dl))
                continue;
            gen = fi.gen + 1;
        }
        if (smc_select_card(b, sys, id, card, NULL) >= 0 && smc_write_file(b, ICON_BIN, icon, (uint32_t)l, gen) == SMC_OK)
            put++;
        else
            logf_("couldn't put the icon on the card: %s/%s", sys, id);
    }
    if (f)
        fclose(f);
    unlink(work);
    return put;
}

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
        if (!strcmp(files[i].name, NAME_TXT) && !files[i].deleted && files[i].size < sizeof(local_game)) {
            if (smc_read_file(b, i, files[i].size, (uint8_t *)local_game) == SMC_OK)
                local_game[files[i].size] = 0;
        }

    bool has_icon = false, has_ini = false;
    int pushed = rep->pushed;
    for (int i = 0; i < n; i++) {
        has_icon |= !strcmp(files[i].name, ICON_BIN);
        has_ini |= !strcmp(files[i].name, GAME_INI);
    }
    /* this SuperStation's file name for the game: its own ROM with the same game ID, when name.txt names a ROM
     * that isn't here. Only for a set filed under a game ID (game.ini): looking for a ROM by ID reads the system's
     * ROMs, which for a save without a known game (no ROM anywhere) found nothing after hashing them all (08) */
    if (C->games_dirs && has_ini && !strchr(id, '~') && !rom_for(sys, local_game)) {
        const char *l = local_for_id(sys, id);
        if (l)
            snprintf(local_game, sizeof(local_game), "%s", l);
    }

    /* the card's files */
    for (int i = 0; i < n; i++) {
        if (not_a_save(files[i].name))
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
    struct dirent *de;
    while (d && (de = readdir(d))) {
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
    if (d)
        closedir(d);
    /* the game's icon: its save's own (icon_readers: PSX), else the icon database's (made from its picture), else its
     * system's until the database has one (db_icon lists it for the download; smc_apply_icons puts it on later) */
    size_t ilen = 0;
    const uint8_t *def = smc_system_icon(sys, &ilen), *icon = def;
    char base_id[65];
    snprintf(base_id, sizeof(base_id), "%s", id); /* a second save set (<id>~<crc>) shows the game's icon too */
    if (strchr(base_id, '~'))
        *strchr(base_id, '~') = 0;
    static uint8_t own[8 + 32 + 32 * 32 / 2];
    int icon_at = -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(files[i].name, ICON_BIN) && !files[i].deleted)
            icon_at = i;
    bool is_default = false;
    if (icon_at >= 0 && def && files[icon_at].size == ilen) {
        uint8_t cur[256];
        is_default = ilen <= sizeof(cur) && smc_select_card(b, sys, id, card, NULL) >= 0 &&
                     smc_read_file(b, icon_at, (uint32_t)ilen, cur) == SMC_OK && !memcmp(cur, def, ilen);
    }
    bool better = false;
    if (!has_icon || is_default) {
        size_t l = own_icon(sys, dir, local_game, own, sizeof(own));
        bool carries = false;
        for (size_t k = 0; k < sizeof(icon_readers) / sizeof(icon_readers[0]); k++)
            carries |= !strcmp(icon_readers[k].sys, sys);
        if (!l && !carries)
            l = db_icon(sys, card, id, base_id, local_game, own, sizeof(own));
        if (l) {
            icon = own;
            ilen = l;
            better = true;
        }
    }
    if (icon && ((!has_icon && (rep->pushed > pushed || better)) || (is_default && better))) {
        uint32_t gen = icon_at >= 0 ? files[icon_at].gen + 1 : 1;
        if (smc_select_card(b, sys, id, card, NULL) < 0 || smc_write_file(b, ICON_BIN, icon, (uint32_t)ilen, gen) != SMC_OK)
            logf_("couldn't put the icon on the card: %s/%s", sys, local_game);
    }
    /* and what the game is, for a frontend's save list (game name, ID) */
    char gid[65], title[65], ini[320], base[65];
    snprintf(base, sizeof(base), "%s", id); /* a second save set of the game (card_id_of_save): <id>~<crc> */
    if (strchr(base, '~') && strlen(strchr(base, '~')) == 9)
        *strchr(base, '~') = 0;
    if (!has_ini && rep->pushed > pushed && card_id_for(sys, local_game, gid, title) && !strcmp(gid, base)) {
        /* the game ID shown is the game's own code (NSME), not the folder's code-CRC */
        char hid[65], t2[65];
        struct rom *r = rom_for(sys, local_game);
        if (r && smc_game_header_id(sys, r->path, hid, t2) == 0)
            snprintf(gid, sizeof(gid), "%s", hid);
        /* the official name, from the game lists when the frontend has them: by the ROM's CRC (the end of the folder
         * name), or a disc's product code (the folder name) */
        const char *crc = NULL, *dash = strrchr(base, '-');
        if (!strncmp(base, "CRC-", 4))
            crc = base + 4;
        else if (dash && strlen(dash + 1) == 8 && strspn(dash + 1, "0123456789ABCDEF") == 8)
            crc = dash + 1;
        const char *name = smc_gamedb_name(C->gamedb_dir, sys, crc, crc ? NULL : base);
        snprintf(ini, sizeof(ini), "id=%s\ntitle=%s\nname=%.120s\n", gid, title, name ? name : "");
        if (smc_select_card(b, sys, id, card, NULL) < 0 ||
            smc_write_file(b, GAME_INI, (const uint8_t *)ini, (uint32_t)strlen(ini), 1) != SMC_OK)
            logf_("couldn't put game.ini on the card: %s/%s", sys, local_game);
    }
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
        card_id_of_save(b, sys, card, g, id);
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

/* A session with the card: Hello, the card's state loaded. 1: ready, 0: no (usable) card, <0: error. */
static int begin(const struct smc_bus *b, const struct smc_sync_cfg *cfg, struct smc_sync_report *rep) {
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
        ents = NULL;
        return -1;
    }
    nents = 0;
    nroms = nindexed = 0;
    load_state();
    load_locals();
    return 1;
}

static int end(void) {
    smc_gamedb_close();
    if (nroms)
        save_ids();
    free(roms);
    roms = NULL;
    nroms = maxroms = nindexed = 0;
    int rc = save_state() | save_locals();
    free(ents), free(bufA), free(bufB);
    ents = NULL;
    return rc;
}

/* A system on the virtual card the card has in use for it. When the card's buttons picked another one, the local saves
 * go to the old card first; only then are they replaced by the new card's (the whole system: a switch is rare).
 * game: one game only (NULL: every game). Returns the card in use, 0 if the system couldn't be synced. */
static bool hold_card; /* a game is running: a card picked meanwhile is for the next game (smc_sync_hold_card) */
void smc_sync_hold_card(bool on) { hold_card = on; }

static int sync_system_now(const struct smc_bus *b, const char *sys, const char *game, struct smc_sync_report *rep) {
    int now = smc_core(b, sys, false);
    if (now < 1) {
        rep->failed++;
        logf_("failed: %s: the card didn't say which of its cards is in use", sys);
        return 0;
    }
    int was = local_card(sys);
    if (hold_card && was && was != now) {
        /* the buttons picked another card while the game runs: the game has its save from card `was` in memory, so its
         * saves keep going there; the switch happens when the game ends (or a save is picked: smc_use_card) */
        static char said[80];
        char k[80];
        snprintf(k, sizeof(k), "%s %d %d", sys, was, now);
        if (strcmp(said, k))
            logf_("%s: card %d picked during the game: from the next game on (this one stays on card %d)", sys, now, was);
        snprintf(said, sizeof(said), "%s", k);
        now = was;
    }
    if (was && was != now) {
        int failed = rep->failed, conflicts = rep->conflicts;
        sync_system(b, sys, was, rep);
        if (rep->failed != failed || rep->conflicts != conflicts || set_aside(sys, was) != 0) {
            logf_("%s stays on card %d: its saves couldn't all be put on it first", sys, was);
            return 0;
        }
        logf_("%s: card %d -> card %d", sys, was, now);
        rep->switched++;
        game = NULL; /* everything of the new card comes in */
    }
    set_local_card(sys, now);
    if (!game)
        sync_system(b, sys, now, rep);
    else {
        char id[65];
        card_id_of_save(b, sys, now, game, id);
        if (sync_game(b, sys, now, game, id, rep) < 0) {
            rep->failed++;
            logf_("failed: %s/%s: the card didn't answer", sys, game);
        }
    }
    return now;
}

static void done_log(const char *what, const struct smc_sync_report *rep) {
    logf_("%s done: %d to card, %d from card, %d unchanged, %d both changed, %d deleted, %d skipped, %d failed", what,
          rep->pushed, rep->pulled, rep->unchanged, rep->conflicts, rep->deleted, rep->skipped, rep->failed);
}

int smc_sync(const struct smc_bus *b, const struct smc_sync_cfg *cfg, struct smc_sync_report *rep) {
    int ok = begin(b, cfg, rep);
    if (ok <= 0)
        return ok;
    for (const char *const *s = cfg->systems; *s; s++)
        sync_system_now(b, *s, NULL, rep);
    done_log("sync", rep);
    return end();
}

int smc_sync_game(const struct smc_bus *b, const struct smc_sync_cfg *cfg, const char *sys, const char *game,
                  struct smc_sync_report *rep) {
    int ok = begin(b, cfg, rep);
    if (ok <= 0)
        return ok;
    sync_system_now(b, sys, game, rep);
    done_log("game sync", rep);
    return end();
}

int smc_game_saves(const struct smc_bus *b, const struct smc_sync_cfg *cfg, const char *sys, const char *game,
                   void (*cb)(int card, int files, uint32_t time, bool playing, void *ctx), void *ctx) {
    struct smc_sync_report rep;
    memset(&rep, 0, sizeof(rep));
    int ok = begin(b, cfg, &rep);
    if (ok <= 0)
        return ok;
    int playing = local_card(sys);
    if (!playing)
        playing = smc_core(b, sys, false);
    int found = 0;
    for (int card = 1; card <= SMC_CARDS_MAX; card++) {
        char id[65] = "";
        card_id_of_save(b, sys, card, game, id);
        if (!id[0])
            smc_game_id(game, id);
        int n = smc_select_card(b, sys, id, card, NULL);
        int saves = 0;
        uint32_t newest = 0;
        for (int i = 0; i < n; i++) {
            struct smc_file f;
            if (smc_info(b, i, &f) != SMC_OK || f.deleted || not_a_save(f.name))
                continue;
            saves++;
            if (f.time > newest)
                newest = f.time;
        }
        if (saves || card == playing) {
            cb(card, saves, newest, card == playing, ctx);
            found++;
        }
    }
    end();
    return found;
}

int smc_use_card(const struct smc_bus *b, const struct smc_sync_cfg *cfg, const char *sys, const char *game, int card,
                 struct smc_sync_report *rep) {
    int ok = begin(b, cfg, rep);
    if (ok <= 0)
        return ok;
    int now = -1;
    /* the card first says this system uses `card`; the switch then puts this SuperStation's saves on the old card and
     * brings the new card's in, as when the buttons pick a card (sync_system_now), however a game is running */
    bool held = hold_card;
    hold_card = false;
    if (smc_core_set_card(b, sys, card) == card)
        now = sync_system_now(b, sys, NULL, rep);
    hold_card = held;
    (void)game;
    done_log("save picked", rep);
    int e = end();
    return now == card ? e : (e < 0 ? e : -1);
}

int smc_sync_changed(const struct smc_bus *b, const struct smc_sync_cfg *cfg, const char *sys,
                     struct smc_sync_report *rep) {
    int ok = begin(b, cfg, rep);
    if (ok <= 0)
        return ok;
    for (const char *const *s = cfg->systems; *s; s++) {
        if (sys && strcmp(sys, *s))
            continue;
        /* only local saves new or changed since they were last synced: the folder is read, no file, no ROM, and the
         * card isn't asked anything unless one changed */
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s/%s", cfg->saves_dir, *s);
        DIR *d = opendir(dir);
        if (!d)
            continue;
        static char changed[64][256];
        int nchanged = 0;
        struct dirent *de;
        while ((de = readdir(d))) {
            char g[256], cfile[17], p[1300];
            struct stat st;
            if (de->d_name[0] == '.' || !split_name(de->d_name, g, sizeof(g), cfile, sizeof(cfile)))
                continue;
            snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
            if (stat(p, &st) != 0 || !S_ISREG(st.st_mode))
                continue;
            int card = local_card(*s);
            struct entry *e = card ? find(*s, card, g, cfile) : NULL;
            if (e && e->seen && e->mtime == (long)st.st_mtime && e->size == (long)st.st_size)
                continue;
            bool have = false;
            for (int i = 0; i < nchanged && !have; i++)
                have = !strcmp(changed[i], g);
            if (!have && nchanged < 64)
                snprintf(changed[nchanged++], 256, "%s", g);
        }
        closedir(d);
        int switched = rep->switched;
        for (int i = 0; i < nchanged; i++)
            if (!sync_system_now(b, *s, changed[i], rep) || rep->switched != switched)
                break; /* (a switched system was synced whole) */
    }
    done_log("quick sync", rep);
    return end();
}

/* the card's file for a local save: selects its game on the system's card; returns its index (or -1 when the card
 * hasn't it, f->deleted for a tombstone), <-1 on error */
static int find_on_card(const struct smc_bus *b, const char *sys, int card, const char *id, const char *cfile,
                        struct smc_file *f) {
    int n = smc_select_card(b, sys, id, card, NULL);
    if (n < 0)
        return -2;
    for (int i = 0; i < n; i++) {
        if (smc_info(b, i, f) != SMC_OK)
            return -2;
        if (!strcmp(f->name, cfile))
            return i;
    }
    return -1;
}

static int system_card(const struct smc_bus *b, const char *sys) {
    int card = local_card(sys);
    return card ? card : smc_core(b, sys, false);
}

int smc_delete_save(const struct smc_bus *b, const struct smc_sync_cfg *cfg, const char *sys, const char *save) {
    struct smc_sync_report rep;
    char game[256], cfile[17], id[65], path[1024];
    if (!split_name(save, game, sizeof(game), cfile, sizeof(cfile)))
        return SMC_E_BAD;
    int ok = begin(b, cfg, &rep);
    if (ok <= 0)
        return ok < 0 ? ok : SMC_E_NO_REPLY;
    int rc = SMC_OK, card = system_card(b, sys);
    card_id_of_save(b, sys, card, game, id);
    struct entry *e = card > 0 ? add(sys, card, game, cfile) : NULL;
    struct smc_file f;
    int idx = e ? find_on_card(b, sys, card, id, cfile, &f) : -2;
    if (idx < -1)
        rc = SMC_E_NO_REPLY;
    else {
        uint32_t gen = idx >= 0 ? f.gen + 1 : (e->seen ? e->gen + 1 : 1);
        if (idx >= 0 && !f.deleted)
            rc = smc_delete(b, idx, gen, (uint32_t)time(NULL));
        snprintf(path, sizeof(path), "%s/%s/%s", cfg->saves_dir, sys, save);
        if (rc == SMC_OK && access(path, F_OK) == 0 && trash_local(e, path) != 0)
            rc = SMC_E_BAD;
        if (rc == SMC_OK) {
            e->gen = gen;
            e->crc = 0;
            e->seen = true;
            logf_("deleted: %s/%s %s on card %d (generation %u)", sys, game, cfile, card, (unsigned)gen);
        }
    }
    int erc = end();
    return rc ? rc : (erc ? SMC_E_BAD : SMC_OK);
}

/* conflicts/<sys>/Card<N>/<game>/<file>.gen<G>: the card's copy kept when both sides changed */
int smc_conflicts(const struct smc_sync_cfg *cfg, const char *card_id,
                  void (*cb)(const char *sys, int card, const char *save, uint32_t gen, void *ctx), void *ctx) {
    char root[1100];
    int n = 0;
    snprintf(root, sizeof(root), "%s/%s/conflicts", cfg->state_dir, card_id);
    DIR *ds = opendir(root);
    if (!ds)
        return 0;
    struct dirent *s, *c, *g, *f;
    while ((s = readdir(ds))) {
        char p1[1400];
        if (s->d_name[0] == '.')
            continue;
        snprintf(p1, sizeof(p1), "%s/%s", root, s->d_name);
        DIR *dc = opendir(p1);
        while (dc && (c = readdir(dc))) {
            char p2[1700];
            int card = 0;
            if (sscanf(c->d_name, "Card%d", &card) != 1)
                continue;
            snprintf(p2, sizeof(p2), "%s/%s", p1, c->d_name);
            DIR *dg = opendir(p2);
            while (dg && (g = readdir(dg))) {
                char p3[2000];
                if (g->d_name[0] == '.')
                    continue;
                snprintf(p3, sizeof(p3), "%s/%s", p2, g->d_name);
                DIR *df = opendir(p3);
                while (df && (f = readdir(df))) {
                    const char *gp = strstr(f->d_name, ".gen");
                    size_t l = strlen(f->d_name);
                    if (!gp || (l > 4 && !strcmp(f->d_name + l - 4, ".old")))
                        continue;
                    char save[300], cfile[17];
                    snprintf(cfile, sizeof(cfile), "%.*s", (int)(gp - f->d_name), f->d_name);
                    /* card "save.sav" -> "<game>.sav", "save_1.cpk" -> "<game>_1.cpk" */
                    snprintf(save, sizeof(save), "%s%s", g->d_name, strlen(cfile) > 4 ? cfile + 4 : "");
                    cb(s->d_name, card, save, (uint32_t)strtoul(gp + 4, NULL, 10), ctx);
                    n++;
                }
                if (df)
                    closedir(df);
            }
            if (dg)
                closedir(dg);
        }
        if (dc)
            closedir(dc);
    }
    closedir(ds);
    return n;
}

int smc_resolve(const struct smc_bus *b, const struct smc_sync_cfg *cfg, const char *sys, const char *save, int keep) {
    struct smc_sync_report rep;
    char game[256], cfile[17], id[65], path[1024];
    if (!split_name(save, game, sizeof(game), cfile, sizeof(cfile)) || (keep != SMC_KEEP_LOCAL && keep != SMC_KEEP_CARD))
        return SMC_E_BAD;
    int ok = begin(b, cfg, &rep);
    if (ok <= 0)
        return ok < 0 ? ok : SMC_E_NO_REPLY;
    int rc = SMC_E_NOT_FOUND, card = system_card(b, sys);
    card_id_of_save(b, sys, card, game, id);
    struct entry *e = card > 0 ? add(sys, card, game, cfile) : NULL;
    struct smc_file f;
    int idx = e ? find_on_card(b, sys, card, id, cfile, &f) : -2;
    snprintf(path, sizeof(path), "%s/%s/%s", cfg->saves_dir, sys, save);
    long ln = read_file(path, bufA, MAX_SAVE);
    if (idx < -1)
        rc = SMC_E_NO_REPLY;
    else if (idx >= 0 && !f.deleted && ln > 0) {
        char cdir[1300], keepdir[1300];
        snprintf(cdir, sizeof(cdir), "%s/conflicts/%s/Card%d/%s", state_dir, sys, card, game);
        if (keep == SMC_KEEP_LOCAL) {
            rc = push(b, e, id, bufA, ln, f.gen + 1, &rep); /* the card's copy stays in conflicts/ */
        } else if ((rc = smc_read_file(b, idx, f.size, bufB)) == SMC_OK) {
            /* the local one is kept beside the card's copy before it's replaced */
            char kp[1500];
            snprintf(keepdir, sizeof(keepdir), "%s", cdir);
            snprintf(kp, sizeof(kp), "%s/%s.local", keepdir, cfile);
            if (mkdirs(keepdir) != 0 || write_file(kp, bufA, (size_t)ln) != 0 || write_file(path, bufB, f.size) != 0)
                rc = SMC_E_BAD;
            else {
                e->gen = f.gen;
                e->crc = smc_crc32(bufB, f.size, 0);
                e->seen = true;
            }
        }
        if (rc == SMC_OK) {
            /* the conflict is settled: its copies stay, renamed .old */
            DIR *d = opendir(cdir);
            struct dirent *de;
            while (d && (de = readdir(d))) {
                size_t l = strlen(de->d_name), cl = strlen(cfile);
                if (!strncmp(de->d_name, cfile, cl) && strncmp(de->d_name + cl, ".gen", 4) == 0 &&
                    !(l > 4 && !strcmp(de->d_name + l - 4, ".old"))) {
                    char from[1700], to[1800];
                    snprintf(from, sizeof(from), "%s/%s", cdir, de->d_name);
                    snprintf(to, sizeof(to), "%s.old", from);
                    rename(from, to);
                }
            }
            if (d)
                closedir(d);
            logf_("both changed, settled: %s/%s %s: the %s copy kept", sys, game, cfile,
                  keep == SMC_KEEP_LOCAL ? "SuperStation's" : "card's");
        }
    }
    int erc = end();
    return rc ? rc : (erc ? SMC_E_BAD : SMC_OK);
}

int smc_game_info(const struct smc_bus *b, const char *sys, int card, const char *id, struct smc_game_info *gi) {
    struct smc_file f;
    memset(gi, 0, sizeof(*gi));
    snprintf(gi->id, sizeof(gi->id), "%s", id);
    int n = smc_select_card(b, sys, id, card, NULL);
    if (n < 0)
        return n;
    char text[256];
    for (int i = 0; i < n; i++) {
        if (smc_info(b, i, &f) != SMC_OK)
            return SMC_E_NO_REPLY;
        if (f.deleted)
            continue;
        bool txt = !strcmp(f.name, NAME_TXT) || !strcmp(f.name, GAME_INI);
        if (txt && f.size < sizeof(text)) {
            if (smc_read_file(b, i, f.size, (uint8_t *)text) != SMC_OK)
                return SMC_E_NO_REPLY;
            text[f.size] = 0;
            if (!strcmp(f.name, NAME_TXT))
                snprintf(gi->save_name, sizeof(gi->save_name), "%s", text);
            else {
                char *g = strstr(text, "id=");
                if (g == text || (g && g[-1] == '\n')) {
                    g += 3;
                    snprintf(gi->id, sizeof(gi->id), "%.*s", (int)strcspn(g, "\n"), g);
                }
                char *nm = strstr(text, "\nname=");
                if (nm) {
                    nm += 6;
                    snprintf(gi->name, sizeof(gi->name), "%.*s", (int)strcspn(nm, "\n"), nm);
                }
                char *t = strstr(text, "title=");
                if (t) {
                    t += 6;
                    snprintf(gi->title, sizeof(gi->title), "%.*s", (int)strcspn(t, "\n"), t);
                }
            }
            continue;
        }
        if (!strcmp(f.name, ICON_BIN)) {
            gi->has_icon = true;
            continue;
        }
        gi->files++;
        gi->bytes += f.size;
        if (f.time > gi->time)
            gi->time = f.time;
    }
    if (!gi->save_name[0])
        snprintf(gi->save_name, sizeof(gi->save_name), "%s", id); /* named after the save before game IDs */
    return SMC_OK;
}

