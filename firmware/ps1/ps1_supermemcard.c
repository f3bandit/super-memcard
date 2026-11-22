/* Copyright (C) 2026 f3bandit. Super MemCard additions to sd2psXtd: GPL-3.0-or-later, as sd2psXtd.
 * This program is distributed WITHOUT ANY WARRANTY; see the GNU General Public License. */
/* Super MemCard for sd2psXtd's PS1 mode: the SD card side (core 0).
 *
 * Core 1 (ps1_supermemcard_bus.inc) posts each complete request here; ps1_supermemcard_task(),
 * called from the main loop, carries it out on the SD card and posts the answer back.
 *
 *   SuperMemCard/card.ini                              id=<16 hex digits>, made once
 *   SuperMemCard/settings.ini                          saves=0: Super MemCard off (the card menu)
 *   SuperMemCard/<system>/card.ini                     card=<N>: the system's current virtual card (1..99)
 *   SuperMemCard/<system>/Card<N>/<game ID>/<file>     a save file (system: Main's save folder name)
 *   SuperMemCard/<system>/Card<N>/<game ID>/<file>.new a save being written, until Commit
 *   SuperMemCard/<system>/Card<N>/<game ID>/meta.ini   <file>=<generation>,<time>, or <file>=-<generation>,<time>: deleted
 *   SuperMemCard/<system>/Card<N>/<game ID>/.deleted/  deleted saves, kept
 *   MemoryCards/PS1/<game ID>/<game ID>-<n>.mcd        the system "PS1CARD" (SMC_PS1CARD): PS1 mode's own per-game cards,
 *                                                      as files (Select, List, Read, Create, Commit, Delete), for the
 *                                                      SuperStation's memory manager. The card PS1 mode has open is
 *                                                      busy (SMC_BUSY): its copy in memory is the newer one
 *
 * A save is written to <file>.new and only replaces <file> at Commit, so a card pulled or
 * powered off mid-save keeps the previous save whole. */
#include "ps1_supermemcard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "pico/rand.h"
#ifndef SMC_SIM
bool stdio_usb_connected(void);   /* pico_stdio_usb's (linked into the firmware; its header isn't on this library's path) */
#endif
#include "pico/time.h"
#include "sd.h"

#define ROOT "SuperMemCard"
#define PS1_ROOT "MemoryCards/PS1"
#include "ps1_cardman.h"

/* ---- mailbox between the cores ---- */

static uint8_t mb_cmd, mb_req[SMC_MAX_REQ], mb_ans[SMC_MAX_ANS];
static int mb_len, mb_alen;
static uint32_t posted, done; /* sequence numbers, accessed with __atomic */
static bool active;           /* a Hello since power-up */
static int enabled;           /* 0: not read yet, 1: on, 2: off (settings.ini); read by core 1 */
static bool clock24;          /* settings.ini clock=24: the screen's times in 24 h (12 h otherwise) */
static int tz_min;            /* the SuperStation's UTC offset in minutes (settings.ini tz=), from Core */
static bool tz_known;

/* ---- the log on the USB serial port (/dev/ttyACM0 on an SS1), only while something there reads it: with nothing
 * reading, a full USB buffer would hold the card up. Block transfers are summed, one line per file. ---- */
#ifndef SMC_SIM
#define smc_log(...) do { if (stdio_usb_connected()) printf("smc: " __VA_ARGS__); } while (0)
#else
#define smc_log(...) do { } while (0)
#endif
static void write_settings(void);
static uint32_t log_rblocks, log_wblocks;
static uint64_t log_xfer_from, log_xfer_us;

static void log_transfer_end(void) {
    if (!log_rblocks && !log_wblocks)
        return;
    uint32_t ms = (uint32_t)(log_xfer_us / 1000), bytes = (log_rblocks + log_wblocks) * 128;
    smc_log("%lu blocks read, %lu written (%lu bytes) in %lu ms busy, %lu ms in all: %lu B/s\n",
            (unsigned long)log_rblocks, (unsigned long)log_wblocks, (unsigned long)bytes, (unsigned long)ms,
            (unsigned long)((time_us_64() - log_xfer_from) / 1000),
            (unsigned long)(time_us_64() > log_xfer_from ? bytes * 1000000ull / (time_us_64() - log_xfer_from) : 0));
    log_rblocks = log_wblocks = 0;
    log_xfer_us = 0;
}

static void log_command(uint8_t cmd, const uint8_t *r, int rlen, const uint8_t *a, int alen, uint64_t us) {
    if (cmd == SMC_READ || cmd == SMC_WRITE) {
        if (!log_rblocks && !log_wblocks)
            log_xfer_from = time_us_64() - us;
        if (cmd == SMC_READ)
            log_rblocks++;
        else
            log_wblocks++;
        log_xfer_us += us;
        return;
    }
    log_transfer_end();
    static const char *const names[] = {"Hello", "Select", "Read", "Write", "Create", "Commit", "Status", "List",
                                        "Info", "Core", "Delete", "Cert", "Auth", "AuthSig", "Progress"};
    const char *nm = cmd >= SMC_HELLO && cmd <= SMC_PROGRESS ? names[cmd - SMC_HELLO] : "?";
    smc_log("%s (%02x) %d bytes in, %d out, status %02x, %lu us\n", nm, cmd, rlen, alen, alen > 0 ? a[0] : 0,
            (unsigned long)us);
    if (cmd == SMC_SELECT && rlen > 1) {
        int sl = r[0], il = rlen > 1 + sl ? r[1 + sl] : 0;
        smc_log("  %.*s / %.*s\n", sl, (const char *)r + 1, il, (const char *)r + 2 + sl);
    }
}
static uint32_t free_kb = 0xFFFFFFFFu, written_kb; /* counted in the menu; KB written since */
/* the SS1's progress bar (SMC_PROGRESS): -1 none */
static int prog_pct = -1;
static char prog_text[SMC_PROGRESS_TEXT + 1];
static uint64_t prog_at;
static uint64_t last_cmd_at;

bool ps1_supermemcard_active(void) { return __atomic_load_n(&active, __ATOMIC_ACQUIRE); }
/* core 1: a Hello is only collected while Super MemCard is on: off, 81 60 isn't ACKed, as stock firmware */
static bool hello_allowed(void) { return __atomic_load_n(&enabled, __ATOMIC_ACQUIRE) == 1; }

int ps1_supermemcard_req_len(uint8_t cmd, const uint8_t *r, int have) {
    if (cmd == SMC_HELLO && !hello_allowed())
        return -1; /* off (or settings not read yet): not a command, nothing ACKed */
    switch (cmd) {
    case SMC_HELLO: return 5;
    case SMC_SELECT:
        if (have < 1) return 1;
        if (r[0] == 0 || r[0] > SMC_SYS_LEN) return -1;
        if (have < 2 + r[0]) return 2 + r[0];
        if (r[1 + r[0]] == 0 || r[1 + r[0]] > SMC_ID_LEN) return -1;
        return 2 + r[0] + r[1 + r[0]] + 1;
    case SMC_READ: return 3;
    case SMC_WRITE: return 3 + 128 + 1;
    case SMC_CREATE:
        if (have < 5) return 5;
        if (r[4] == 0 || r[4] > SMC_NAME_LEN) return -1;
        return 5 + r[4];
    case SMC_COMMIT: return 9;
    case SMC_STATUS: return 0;
    case SMC_LIST:
        if (have < 1) return 1;
        if (r[0] == 0 || r[0] > SMC_SYS_LEN) return -1;
        return 1 + r[0] + 1 + 2;
    case SMC_INFO: return 1;
    case SMC_DELETE: return 9;
    case SMC_PROGRESS:
        if (have < 2) return 2;
        if (r[1] > SMC_PROGRESS_TEXT) return -1;
        return 2 + r[1];
    case SMC_CERT:
    case SMC_AUTH:
    case SMC_AUTHSIG: return ps1_supermemcard_id_req_len(cmd);
    case SMC_UPDATE: return ps1_supermemcard_update_req_len(r, have);
    case SMC_CORE: {
        if (have < 1) return 1;
        if (r[0] == 0 || r[0] > SMC_SYS_LEN) return -1;
        int base = 1 + r[0] + 1;   /* with SMC_CORE_SET_CARD in its flags, the card's number follows */
        if (have < base)
            return base;
        int flags = r[base - 1], n = base + (flags & SMC_CORE_SET_CARD ? 1 : 0);
        if (flags & SMC_CORE_SCREEN)
            for (int i = 0; i < SMC_SCREEN_LINES; i++) {   /* then the screen's lines: each its length, then it */
                if (have < n + 1)
                    return n + 1;
                if (r[n] > (i == SMC_SCREEN_GAME ? SMC_SCREEN_LEN : SMC_SCREEN_COLS)) return -1;
                n += 1 + r[n];
            }
        return flags & SMC_CORE_TZ ? n + 2 : n;   /* then the UTC offset */
    }
    default: return -1;
    }
}

int ps1_supermemcard_exec(uint8_t cmd, const uint8_t *req, int len, uint8_t *ans, volatile int *abort) {
    /* a request abandoned earlier may still be running on core 0 */
    while (__atomic_load_n(&done, __ATOMIC_ACQUIRE) != __atomic_load_n(&posted, __ATOMIC_ACQUIRE))
        if (*abort)
            return 0;
    mb_cmd = cmd;
    memcpy(mb_req, req, (size_t)len);
    mb_len = len;
    uint32_t seq = __atomic_add_fetch(&posted, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&done, __ATOMIC_ACQUIRE) != seq)
        if (*abort)
            return 0;
    memcpy(ans, mb_ans, (size_t)mb_alen);
    return mb_alen;
}

/* ---- the selected save set ---- */

struct file {
    char name[SMC_NAME_LEN + 1];
    uint32_t size, gen, time;
    bool exists;   /* committed copy on the SD card */
    bool deleted;  /* a tombstone: deleted at generation gen */
    bool writing;  /* <name>.new is open for writing */
    uint32_t new_size;
};

static char card_id[17];
static char set_dir[24 + SMC_SYS_LEN + SMC_ID_LEN];
static bool have_set;
static bool set_ps1;                         /* the set is a PS1 mode card folder (PS1CARD) */
static char set_id[SMC_ID_LEN + 1];
static struct file files[SMC_MAX_FILES];
static int nfiles;
static int open_fd = -1, open_file = -1; /* one file kept open */
static bool open_new;

/* File names: letters, digits, - _ . only. System and game IDs: anything FAT allows (game IDs are
 * Main's save names, e.g. "Super Metroid (USA)"), but no path separators. */
static bool ok_char(char c, bool name) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
        return true;
    if (name || (unsigned char)c < 0x20 || c == 0x7F)
        return false;
    return strchr("/\\:*?\"<>|", c) == NULL;
}

/* copy a length-prefixed name, refusing anything that could leave its folder */
static bool take_name(char *out, size_t outsz, const uint8_t *p, int len, bool name) {
    if (len <= 0 || (size_t)len >= outsz || p[0] == '.' || p[0] == ' ')
        return false;
    for (int i = 0; i < len; i++) {
        if (!ok_char((char)p[i], name))
            return false;
        out[i] = (char)p[i];
    }
    out[len] = 0;
    /* FAT drops trailing dots and spaces, which could make two names the same folder */
    return strstr(out, "..") == NULL && out[len - 1] != '.' && out[len - 1] != ' ';
}

static void close_open(void) {
    if (open_fd >= 0) {
        sd_flush(open_fd);
        sd_close(open_fd);
    }
    open_fd = open_file = -1;
}

static void path_of(char *out, size_t n, int f, bool new_copy) {
    snprintf(out, n, "%s/%s%s", set_dir, files[f].name, new_copy ? ".new" : "");
}

static int fd_for(int f, bool new_copy) {
    if (open_file == f && open_new == new_copy)
        return open_fd;
    close_open();
    char p[192];
    path_of(p, sizeof(p), f, new_copy);
    open_fd = sd_open(p, new_copy ? O_RDWR : O_RDONLY);
    if (open_fd >= 0) {
        open_file = f;
        open_new = new_copy;
    }
    return open_fd;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }

static void mkdirs(const char *dir) {
    char p[192];
    snprintf(p, sizeof(p), "%s", dir);
    for (char *s = p; *s; s++)
        if (*s == '/') {
            *s = 0;
            sd_mkdir(p);
            *s = '/';
        }
    sd_mkdir(p);
}

/* read a small text file into buf (NUL-terminated); returns its length or -1. Only <path>.new there: write_text was
 * cut off after the new copy was whole and the old one gone (a power cut, the card pulled): the new one is it */
static int read_text(const char *path, char *buf, int max) {
    int fd = sd_open(path, O_RDONLY);
    if (fd < 0) {
        char tmp[200];
        snprintf(tmp, sizeof(tmp), "%s.new", path);
        if (!sd_exists(tmp) || sd_rename(tmp, path) != 0)
            return -1;
        fd = sd_open(path, O_RDONLY);
        if (fd < 0)
            return -1;
    }
    int n = sd_read(fd, buf, (size_t)max - 1);
    sd_close(fd);
    if (n < 0)
        return -1;
    buf[n] = 0;
    return n;
}

/* write a small text file whole: to <path>.new, then rename over <path> */
static bool write_text(const char *path, const char *text) {
    char tmp[200];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    int fd = sd_open(tmp, O_RDWR | O_CREAT | O_TRUNC);
    if (fd < 0)
        return false;
    int len = (int)strlen(text);
    bool ok = sd_write(fd, (void *)text, (size_t)len) == len;
    sd_flush(fd);
    sd_close(fd);
    if (!ok)
        return false;
    if (sd_exists(path))
        sd_remove(path);
    return sd_rename(tmp, path) == 0;
}

static void load_card_id(void) {
    char buf[64];
    if (card_id[0])
        return;
    if (read_text(ROOT "/card.ini", buf, sizeof(buf)) > 0) {
        char *p = strstr(buf, "id=");
        if (p && strlen(p) >= 19) {
            memcpy(card_id, p + 3, 16);
            card_id[16] = 0;
            return;
        }
    }
    snprintf(card_id, sizeof(card_id), "%016llx", (unsigned long long)get_rand_64());
    sd_mkdir(ROOT);
    snprintf(buf, sizeof(buf), "id=%s\n", card_id);
    write_text(ROOT "/card.ini", buf);
}

#define META_MAX (SMC_MAX_FILES * 48)   /* meta.ini: a line a file, name=-gen,time (up to 40 bytes) */
static void save_meta(void) {
    char text[META_MAX], p[192];
    int k = 0;
    text[0] = 0;
    for (int i = 0; i < nfiles; i++)
        if ((files[i].exists || files[i].deleted) && k < (int)sizeof(text))
            k += snprintf(text + k, sizeof(text) - (size_t)k, "%s=%s%lu,%lu\n", files[i].name, files[i].deleted ? "-" : "",
                          (unsigned long)files[i].gen, (unsigned long)files[i].time);
    snprintf(p, sizeof(p), "%s/meta.ini", set_dir);
    write_text(p, text);
}

/* "<gen>,<time>" or "-<gen>,<time>" (time missing: 0) */
static uint32_t parse_gen(const char *v, uint32_t *time) {
    char *e;
    uint32_t g = (uint32_t)strtoul(v + (*v == '-'), &e, 10);
    *time = *e == ',' ? (uint32_t)strtoul(e + 1, NULL, 10) : 0;
    return g;
}

static uint32_t meta_gen(const char *meta, const char *name, uint32_t *time) {
    size_t n = strlen(name);
    *time = 0;
    for (const char *l = meta; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
        if (strncmp(l, name, n) == 0 && l[n] == '=')
            return parse_gen(l + n + 1, time);
    return 0;
}

static bool is_save_name(const char *n) {
    size_t l = strlen(n);
    if (l == 0 || l > SMC_NAME_LEN || strcmp(n, "meta.ini") == 0 || n[0] == '.')
        return false;
    if (set_ps1)                             /* a PS1 card folder: its cards only (<id>-<n>.mcd) */
        return l > 4 && strcasecmp(n + l - 4, ".mcd") == 0;
    return !(l > 4 && (strcmp(n + l - 4, ".new") == 0 || strcmp(n + l - 4, ".old") == 0));
}

/* the selected PS1 card folder is the one PS1 mode has open (in memory, written back as it goes): busy */
static bool ps1_set_busy(void) {
    return set_ps1 && strcasecmp(set_id, ps1_cardman_get_folder_name()) == 0;
}

/* a save of the game, not the SuperStation's notes beside it (its name, game, icon, label) */
static bool is_game_save(const char *n) {
    return is_save_name(n) && strcmp(n, "name.txt") && strcmp(n, "game.ini") && strcmp(n, "icon.bin") &&
           strcmp(n, "label.txt");
}

/* ---- virtual cards, and the system whose core is running ---- */

static char mode_sys[SMC_SYS_LEN + 1]; /* "" : none, the buttons are the PS1 card's */
static int mode_card;                  /* its current card */
static uint32_t mode_seq;
static bool mode_dirty;                /* mode_card changed by the buttons, not yet in card.ini */
static uint64_t mode_save_at;
static bool ps1_used;                  /* set by core 1 */
static char mode_text[SMC_SCREEN_LINES][SMC_SCREEN_LEN + 1]; /* the system mode's screen, from Core */

static int read_current_card(const char *sys) {
    char p[96], buf[32];
    snprintf(p, sizeof(p), ROOT "/%s/card.ini", sys);
    if (read_text(p, buf, sizeof(buf)) > 0) {
        char *c = strstr(buf, "card=");
        int n = c ? atoi(c + 5) : 0;
        if (n >= 1 && n <= SMC_MAX_CARDS)
            return n;
    }
    return 1;
}

static void write_card_ini(const char *sys, int card) {
    char p[96], buf[32];
    snprintf(p, sizeof(p), ROOT "/%s", sys);
    mkdirs(p);
    snprintf(p, sizeof(p), ROOT "/%s/card.ini", sys);
    snprintf(buf, sizeof(buf), "card=%d\n", card);
    write_text(p, buf);
}

static void save_mode_card(void) {
    if (!mode_dirty)
        return;
    mode_dirty = false;
    write_card_ini(mode_sys, mode_card);
}

/* a system's current card: the running system's may not be saved yet */
static int current_card(const char *sys) {
    if (mode_sys[0] && strcmp(sys, mode_sys) == 0)
        return mode_card;
    return read_current_card(sys);
}

static void set_mode(const char *sys) {
    if (strcmp(sys, mode_sys) == 0)
        return;
    save_mode_card();
    snprintf(mode_sys, sizeof(mode_sys), "%s", sys);
    memset(mode_text, 0, sizeof(mode_text));
    smc_log("mode: %s\n", sys[0] ? sys : "PS1 card");
    mode_card = sys[0] ? read_current_card(sys) : 0;
    mode_seq++;
}

void ps1_supermemcard_ps1_used(void) { __atomic_store_n(&ps1_used, true, __ATOMIC_RELEASE); }
const char *ps1_supermemcard_mode_system(void) { return mode_sys; }
int ps1_supermemcard_mode_card(void) { return mode_card; }
uint32_t ps1_supermemcard_mode_seq(void) { return mode_seq; }
const char *ps1_supermemcard_mode_line(int line) {
    return line >= 0 && line < SMC_SCREEN_LINES ? mode_text[line] : "";
}

bool ps1_supermemcard_card_key(int delta) {
    if (!mode_sys[0])
        return false;
    int n = mode_card + (delta > 0 ? 1 : -1);
    if (n >= 1 && n <= SMC_MAX_CARDS && n != mode_card) {
        mode_card = n;
        mode_seq++;
        mode_dirty = true;
        mode_save_at = time_us_64() + 1500 * 1000;
    }
    return true;
}

/* a file's CRC-32 (zlib's): ~0.15 s for a 128 KB card. A PS1 mode card's "generation" (Info): the SS1 keeps a copy
 * of each card and reads one again only when this changed */
static uint32_t file_crc(const char *dir, const char *name) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    char p[192];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    int fd = sd_open(p, O_RDONLY);
    if (fd < 0)
        return 0;
    static uint8_t buf[4096];
    uint32_t crc = 0xFFFFFFFFu;
    int n;
    while ((n = sd_read(fd, buf, sizeof(buf))) > 0)
        for (int i = 0; i < n; i++)
            crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    sd_close(fd);
    return ~crc;
}

static void select_set(const char *sys, const char *id, int card) {
    close_open();
    /* .new files of the previous set stay on the SD card unfinished: never committed, never read */
    nfiles = 0;
    have_set = true;
    set_ps1 = strcmp(sys, SMC_PS1CARD) == 0;
    snprintf(set_id, sizeof(set_id), "%s", id);
    if (set_ps1)
        snprintf(set_dir, sizeof(set_dir), PS1_ROOT "/%s", id);
    else
        snprintf(set_dir, sizeof(set_dir), ROOT "/%s/Card%d/%s", sys, card, id);

    static char meta[META_MAX];
    char p[192];
    snprintf(p, sizeof(p), "%s/meta.ini", set_dir);
    if (read_text(p, meta, sizeof(meta)) < 0)
        meta[0] = 0;

    int dir = sd_open(set_dir, O_RDONLY);
    if (dir < 0)
        return;
    /* a Commit cut off between its renames: <file>.old without <file> is the previous save, put back */
    if (!set_ps1) {
        char olds[4][SMC_NAME_LEN + 1];
        int nold = 0;
        int it = sd_iterate_dir(dir, -1);
        while (it != -1) {
            char name[SMC_NAME_LEN + 6];
            size_t l;
            if (!sd_is_dir(it) && sd_get_name(it, name, sizeof(name)) && (l = strlen(name)) > 4 && l - 4 <= SMC_NAME_LEN &&
                !strcmp(name + l - 4, ".old") && nold < 4) {
                name[l - 4] = 0;
                snprintf(olds[nold++], sizeof(olds[0]), "%s", name);
            }
            it = sd_iterate_dir(dir, it);
        }
        if (it != -1)
            sd_close(it);
        sd_close(dir);
        for (int i = 0; i < nold; i++) {
            char a[200], b[200];
            snprintf(a, sizeof(a), "%s/%s", set_dir, olds[i]);
            snprintf(b, sizeof(b), "%s/%s.old", set_dir, olds[i]);
            if (sd_exists(a))
                sd_remove(b);   /* the new save has its name: the old one wasn't removed yet */
            else
                sd_rename(b, a);
        }
        dir = sd_open(set_dir, O_RDONLY);
        if (dir < 0)
            return;
    }
    int it = sd_iterate_dir(dir, -1);
    while (it != -1 && nfiles < SMC_MAX_FILES) {
        char name[SMC_NAME_LEN + 2];
        if (!sd_is_dir(it) && sd_get_name(it, name, sizeof(name)) && is_save_name(name)) {
            struct file *f = &files[nfiles++];
            memset(f, 0, sizeof(*f));
            snprintf(f->name, sizeof(f->name), "%s", name);
            f->size = (uint32_t)sd_filesize(it);
            f->gen = meta_gen(meta, name, &f->time);
            f->exists = true;
        }
        it = sd_iterate_dir(dir, it);
    }
    if (it != -1)
        sd_close(it);
    sd_close(dir);
    /* a PS1 mode card: PS1 mode writes it without meta.ini, so its generation is its CRC-32 (the folder closed first) */
    if (set_ps1)
        for (int i = 0; i < nfiles; i++)
            files[i].gen = file_crc(set_dir, files[i].name);
    /* tombstones: deleted saves, in meta.ini only */
    for (const char *l = meta; *l && nfiles < SMC_MAX_FILES; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : "") {
        const char *eq = strchr(l, '=');
        const char *nl = strchr(l, '\n');
        if (!eq || (nl && eq > nl) || eq[1] != '-' || eq - l > SMC_NAME_LEN)
            continue;
        char name[SMC_NAME_LEN + 1];
        memcpy(name, l, (size_t)(eq - l));
        name[eq - l] = 0;
        bool have = false;
        for (int i = 0; i < nfiles; i++)
            have |= strcmp(files[i].name, name) == 0;
        if (have || !is_save_name(name))
            continue; /* a save written again since is there itself */
        struct file *f = &files[nfiles++];
        memset(f, 0, sizeof(*f));
        snprintf(f->name, sizeof(f->name), "%s", name);
        f->gen = parse_gen(eq + 1, &f->time);
        f->deleted = true;
    }
    /* stable order: by name */
    for (int i = 1; i < nfiles; i++)
        for (int j = i; j > 0 && strcmp(files[j - 1].name, files[j].name) > 0; j--) {
            struct file t = files[j];
            files[j] = files[j - 1];
            files[j - 1] = t;
        }
}

static bool file_ok(uint8_t f) { return have_set && f < nfiles; }

/* ---- folders: the systems, a system's games (List, and the card's Browse menu) ---- */

/* calls cb for each folder in dir (not .hidden ones, names up to SMC_ID_LEN) until it returns false; returns how many */
static int each_dir(const char *dir, bool (*cb)(const char *name, int i, void *ctx), void *ctx) {
    int d = sd_open(dir, O_RDONLY);
    if (d < 0)
        return 0;
    int it = sd_iterate_dir(d, -1), i = 0;
    while (it != -1) {
        char name[SMC_ID_LEN + 2];
        if (sd_is_dir(it) && sd_get_name(it, name, sizeof(name)) && strlen(name) <= SMC_ID_LEN && name[0] != '.') {
            if (!cb(name, i++, ctx))
                break;
        }
        it = sd_iterate_dir(d, it);
    }
    if (it != -1)
        sd_close(it);
    sd_close(d);
    return i;
}

struct nth { int want; uint8_t *out; };
static bool nth_cb(const char *name, int i, void *ctx) {
    struct nth *n = ctx;
    if (i != n->want)
        return true;
    memcpy(n->out, name, strlen(name));
    return false;
}

struct names { char (*out)[SMC_ID_LEN + 1]; int max; };
static bool names_cb(const char *name, int i, void *ctx) {
    struct names *n = ctx;
    if (i >= n->max)
        return false;
    snprintf(n->out[i], SMC_ID_LEN + 1, "%s", name);
    return true;
}

/* a game's folder with a save in it (a set whose saves were all deleted or cleared keeps only its tombstones) */
static bool dir_has_save(const char *dir) {
    int d = sd_open(dir, O_RDONLY);
    if (d < 0)
        return false;
    bool found = false;
    int it = sd_iterate_dir(d, -1);
    while (it != -1 && !found) {
        char name[SMC_NAME_LEN + 2];
        found = !sd_is_dir(it) && sd_get_name(it, name, sizeof(name)) && is_game_save(name);
        it = sd_iterate_dir(d, it);
    }
    if (it != -1)
        sd_close(it);
    sd_close(d);
    return found;
}

/* the games on a system's card that have a save, kept in place in ids; returns how many */
static int only_with_saves(const char *dir, char (*ids)[SMC_ID_LEN + 1], int got) {
    int k = 0;
    for (int i = 0; i < got; i++) {
        char p[200];
        snprintf(p, sizeof(p), "%s/%s", dir, ids[i]);
        if (dir_has_save(p)) {
            if (k != i)
                memcpy(ids[k], ids[i], SMC_ID_LEN + 1);
            k++;
        }
    }
    return k;
}

int ps1_supermemcard_browse_systems(char (*out)[SMC_ID_LEN + 1], int max) {
    struct names n = {out, max};
    int got = each_dir(ROOT, names_cb, &n);
    if (got > max)
        got = max;
    /* only the systems with a save on their card now */
    static char games[64][SMC_ID_LEN + 1];
    int k = 0;
    for (int i = 0; i < got; i++) {
        char dir[24 + SMC_SYS_LEN];
        snprintf(dir, sizeof(dir), ROOT "/%s/Card%d", out[i], current_card(out[i]));
        struct names g = {games, 64};
        int ng = each_dir(dir, names_cb, &g);
        if (only_with_saves(dir, games, ng < 64 ? ng : 64) > 0) {
            if (k != i)
                memcpy(out[k], out[i], SMC_ID_LEN + 1);
            k++;
        }
    }
    return k;
}

int ps1_supermemcard_browse_games(const char *sys, int *card, char (*ids)[SMC_ID_LEN + 1],
                                  char (*names)[SMC_BROWSE_NAME], int max) {
    char dir[24 + SMC_SYS_LEN];
    *card = current_card(sys);
    snprintf(dir, sizeof(dir), ROOT "/%s/Card%d", sys, *card);
    struct names n = {ids, max};
    int got = each_dir(dir, names_cb, &n);
    if (got > max)
        got = max;
    got = only_with_saves(dir, ids, got);
    for (int i = 0; i < got; i++) {
        /* game.ini's official name, else the name in the game's header, else its folder */
        char p[200], ini[320], *v = NULL;
        snprintf(p, sizeof(p), "%s/%s/game.ini", dir, ids[i]);
        if (read_text(p, ini, sizeof(ini)) > 0) {
            v = strstr(ini, "\nname=");
            v = v && v[6] != '\n' && v[6] ? v + 6 : NULL;
            if (!v && (v = strstr(ini, "title=")) != NULL)
                v = v[6] != '\n' && v[6] ? v + 6 : NULL;
        }
        if (v)
            snprintf(names[i], SMC_BROWSE_NAME, "%.*s", (int)strcspn(v, "\n"), v);
        else {
            /* no official name: the save's own name, which the SuperStation keeps beside it (name.txt), never the
             * game's ID (a CRC means nothing to a player: IDs are for the SuperStation's side) */
            char nm[SMC_BROWSE_NAME];
            snprintf(p, sizeof(p), "%s/%s/name.txt", dir, ids[i]);
            int l = read_text(p, nm, sizeof(nm));
            if (l > 0) {
                nm[strcspn(nm, "\r\n")] = 0;
                while (l > 0 && (nm[strlen(nm) - 1] == ' ' || nm[strlen(nm) - 1] == '.'))   /* "Zelda, The " */
                    nm[strlen(nm) - 1] = 0, l--;
            }
            snprintf(names[i], SMC_BROWSE_NAME, "%s", l > 0 && nm[0] ? nm : ids[i]);
        }
    }
    return got;
}

/* days from 1970-01-01 to y-m-d (proleptic Gregorian): the date back from a Unix time without the C library's tz */
static void civil(int64_t days, int *y, int *m, int *d) {
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

int ps1_supermemcard_browse_saves(const char *sys, int card, const char *id, char (*names)[SMC_NAME_LEN + 1],
                                  char (*when)[24], int max) {
    char dir[192], p[208];
    static char meta[META_MAX];
    snprintf(dir, sizeof(dir), ROOT "/%s/Card%d/%s", sys, card, id);
    snprintf(p, sizeof(p), "%s/meta.ini", dir);
    if (read_text(p, meta, sizeof(meta)) < 0)
        meta[0] = 0;
    int n = 0, d = sd_open(dir, O_RDONLY);
    if (d < 0)
        return 0;
    int it = sd_iterate_dir(d, -1);
    while (it != -1 && n < max) {
        char name[SMC_NAME_LEN + 2];
        if (!sd_is_dir(it) && sd_get_name(it, name, sizeof(name)) && is_game_save(name)) {
            uint32_t t = 0;
            meta_gen(meta, name, &t);
            snprintf(names[n], SMC_NAME_LEN + 1, "%s", name);
            when[n][0] = 0;
            if (t) {
                int64_t lt = (int64_t)t + (int64_t)tz_min * 60;
                int y, mo, da;
                civil(lt / 86400, &y, &mo, &da);
                int s = (int)(lt % 86400);
                char t24[16];
                snprintf(t24, sizeof(t24), "%02d/%02d/%02d %02d:%02d", mo, da, y % 100, s / 3600, s / 60 % 60);
                ps1_supermemcard_format_time(t24, when[n], sizeof(when[n]));
            }
            n++;
        }
        it = sd_iterate_dir(d, it);
    }
    if (it != -1)
        sd_close(it);
    sd_close(d);
    for (int a = 1; a < n; a++)   /* by name */
        for (int b = a; b > 0 && strcmp(names[b - 1], names[b]) > 0; b--) {
            char tn[SMC_NAME_LEN + 1], tw[24];
            memcpy(tn, names[b], sizeof(tn)); memcpy(names[b], names[b - 1], sizeof(tn)); memcpy(names[b - 1], tn, sizeof(tn));
            memcpy(tw, when[b], sizeof(tw)); memcpy(when[b], when[b - 1], sizeof(tw)); memcpy(when[b - 1], tw, sizeof(tw));
        }
    return n;
}

/* ---- the commands ---- */

static int do_command(uint8_t cmd, const uint8_t *r, int len, uint8_t *a) {
    (void)len;
    switch (cmd) {
    case SMC_HELLO:
        if (r[0] != 'S' || r[1] != 'M')
            return 0;
        load_card_id();
        __atomic_store_n(&active, true, __ATOMIC_RELEASE);
        a[0] = 'S';
        a[1] = 'M';
        a[2] = SMC_VERSION;
        /* features: save files, list, virtual cards (Core), delete, identity (CERT/AUTH); the firmware update through
         * the front port when the card has the resident updater */
        a[3] = 0x1F | (ps1_supermemcard_update_available() ? SMC_FEATURE_UPDATE : 0);
        for (int i = 0; i < 8; i++) {
            unsigned v = 0;
            sscanf(card_id + i * 2, "%2x", &v);
            a[4 + i] = (uint8_t)v;
        }
        /* free space: as counted in the card's menu, less what Super MemCard wrote since (FFFFFFFF: not counted) */
        put32(a + 12, free_kb == 0xFFFFFFFFu ? free_kb : (written_kb < free_kb ? free_kb - written_kb : 0));
        return 16;

    case SMC_SELECT: {
        char sys[SMC_SYS_LEN + 1], id[SMC_ID_LEN + 1];
        int sl = r[0], il = r[1 + sl], card = r[2 + sl + il];
        a[0] = SMC_FAIL;
        a[1] = a[2] = 0;
        if (!take_name(sys, sizeof(sys), r + 1, sl, false) || !take_name(id, sizeof(id), r + 2 + sl, il, false) ||
            card > SMC_MAX_CARDS) {
            have_set = false;
            return 3;
        }
        if (card == 0)
            card = current_card(sys);
        select_set(sys, id, card);
        a[0] = SMC_GOOD;
        a[1] = (uint8_t)nfiles;
        a[2] = (uint8_t)card;
        return 3;
    }

    case SMC_CORE: {
        char sys[SMC_SYS_LEN + 1];
        int sl = r[0], flags = r[1 + sl];
        bool none = sl == 1 && r[1] == '.';
        a[0] = SMC_FAIL;
        a[1] = 0;
        if (!none && !take_name(sys, sizeof(sys), r + 1, sl, false))
            return 2;
        if (flags & SMC_CORE_RUNNING) {
            set_mode(none ? "" : sys);
            __atomic_store_n(&ps1_used, false, __ATOMIC_RELEASE); /* only PS1 use from now on ends it */
        }
        if ((flags & SMC_CORE_SCREEN) && (flags & SMC_CORE_RUNNING) && !none) {
            /* the screen's lines: printable ASCII only (the font), anything else a space */
            const uint8_t *g = r + 2 + sl + (flags & SMC_CORE_SET_CARD ? 1 : 0);
            for (int l = 0; l < SMC_SCREEN_LINES; l++) {
                int gl = *g++;
                for (int i = 0; i < gl; i++)
                    mode_text[l][i] = g[i] >= 0x20 && g[i] < 0x7F ? (char)g[i] : ' ';
                mode_text[l][gl] = 0;
                g += gl;
            }
            mode_seq++;
            smc_log("screen: %s | %s | %s | %s | %s\n", mode_text[0], mode_text[1], mode_text[2], mode_text[3],
                    mode_text[4]);
        }
        if (flags & SMC_CORE_TZ) {
            const uint8_t *g = r + 2 + sl + (flags & SMC_CORE_SET_CARD ? 1 : 0);
            if (flags & SMC_CORE_SCREEN)
                for (int l = 0; l < SMC_SCREEN_LINES; l++)
                    g += 1 + *g;
            int tz = (int16_t)((g[0] << 8) | g[1]);
            if (tz >= -14 * 60 && tz <= 14 * 60 && (!tz_known || tz != tz_min)) {
                tz_min = tz;
                tz_known = true;
                write_settings();   /* kept: the card's screens show local time on their own after */
            }
        }
        if ((flags & SMC_CORE_SET_CARD) && !none) {
            /* the host picks the system's card (a save chosen on the SuperStation): as the buttons do, saved at once */
            int card = r[2 + sl];
            if (card < 1 || card > SMC_MAX_CARDS)
                return 2;
            if (mode_sys[0] && !strcmp(mode_sys, sys)) {
                mode_card = card;
                mode_seq++;
                mode_dirty = false;
            }
            write_card_ini(sys, card);
        }
        a[0] = SMC_GOOD;
        a[1] = none ? 0 : (uint8_t)current_card(sys);
        return 2;
    }

    case SMC_INFO:
        memset(a, 0, 29);
        if (!file_ok(r[0]) || !(files[r[0]].exists || files[r[0]].deleted)) {
            a[0] = have_set ? SMC_NOT_FOUND : SMC_NO_SET;
            return 29;
        }
        a[0] = files[r[0]].deleted ? SMC_DELETED : SMC_GOOD;
        memcpy(a + 1, files[r[0]].name, strlen(files[r[0]].name));
        put32(a + 17, files[r[0]].size);
        put32(a + 21, files[r[0]].gen);
        put32(a + 25, files[r[0]].time);
        return 29;

    case SMC_READ: {
        uint8_t f = r[0];
        uint32_t off = ((uint32_t)r[1] << 8 | r[2]) * 128u;
        memset(a, 0, 130);
        a[129] = SMC_FAIL;
        if (!file_ok(f) || !files[f].exists || off >= files[f].size)
            return 130;
        if (ps1_set_busy()) {
            a[129] = SMC_BUSY;
            return 130;
        }
        int fd = fd_for(f, false);
        if (fd < 0 || sd_seek(fd, (int32_t)off, SEEK_SET) != 0)
            return 130;
        uint32_t n = files[f].size - off < 128 ? files[f].size - off : 128;
        if (sd_read(fd, a, n) != (int)n)
            return 130;
        uint8_t chk = r[1] ^ r[2];
        for (int i = 0; i < 128; i++)
            chk ^= a[i];
        a[128] = chk;
        a[129] = SMC_GOOD;
        return 130;
    }

    case SMC_WRITE: {
        uint8_t f = r[0];
        uint32_t off = ((uint32_t)r[1] << 8 | r[2]) * 128u;
        uint8_t chk = r[1] ^ r[2];
        for (int i = 0; i < 128; i++)
            chk ^= r[3 + i];
        a[0] = SMC_FAIL;
        if (chk != r[131]) {
            a[0] = SMC_BAD_CHECKSUM;
            return 1;
        }
        if (!file_ok(f) || !files[f].writing || off >= files[f].new_size)
            return 1;
        int fd = fd_for(f, true);
        if (fd < 0 || sd_seek(fd, (int32_t)off, SEEK_SET) != 0)
            return 1;
        uint32_t n = files[f].new_size - off < 128 ? files[f].new_size - off : 128;
        if (sd_write(fd, (void *)(r + 3), n) != (int)n)
            return 1;
        a[0] = SMC_GOOD;
        return 1;
    }

    case SMC_CREATE: {
        uint32_t size = be32(r);
        char name[SMC_NAME_LEN + 1];
        a[0] = SMC_FAIL;
        a[1] = 0xFF;
        if (!have_set || size == 0 || size > SMC_MAX_FILE || !take_name(name, sizeof(name), r + 5, r[4], true) ||
            !is_save_name(name))
            return 2;
        if (ps1_set_busy()) {
            a[0] = SMC_BUSY;
            return 2;
        }
        int f = -1;
        for (int i = 0; i < nfiles; i++)
            if (strcmp(files[i].name, name) == 0)
                f = i;
        if (f < 0) {
            if (nfiles >= SMC_MAX_FILES)
                return 2;
            f = nfiles++;
            memset(&files[f], 0, sizeof(files[f]));
            snprintf(files[f].name, sizeof(files[f].name), "%s", name);
        }
        close_open();
        mkdirs(set_dir);
        char p[192];
        path_of(p, sizeof(p), f, true);
        /* only an empty file: the blocks fill it as they come, and Commit checks it's complete. Nothing here
         * may take time in proportion to the save's size: the host is waiting for this answer (an SD card
         * written synchronously takes ~3 ms per 128 bytes; filling 8 KB here took 180 ms on the SS1) */
        int fd = sd_open(p, O_RDWR | O_CREAT | O_TRUNC);
        if (fd < 0)
            return 2;
        sd_close(fd);
        files[f].writing = true;
        files[f].new_size = size;
        written_kb += (size + 1023) / 1024;
        a[0] = SMC_GOOD;
        a[1] = (uint8_t)f;
        return 2;
    }

    case SMC_COMMIT: {
        uint8_t f = r[0];
        a[0] = SMC_FAIL;
        if (!file_ok(f) || !files[f].writing)
            return 1;
        if (ps1_set_busy()) {                /* (switched to it since Create) */
            a[0] = SMC_BUSY;
            return 1;
        }
        close_open();
        char p[192], np[192];
        path_of(p, sizeof(p), f, false);
        path_of(np, sizeof(np), f, true);
        int nfd = sd_open(np, O_RDONLY);
        int got = nfd >= 0 ? sd_filesize(nfd) : -1;
        if (nfd >= 0)
            sd_close(nfd);
        if (got != (int)files[f].new_size)
            return 1; /* not every block arrived: the previous save stays */
        /* the previous save is renamed <file>.old, the new one takes its name, then .old goes: cut off between
         * those, select_set puts the previous save back (never a save lost, never half of one) */
        char op[200];
        snprintf(op, sizeof(op), "%s.old", p);
        if (sd_exists(op))
            sd_remove(op);
        if (sd_exists(p) && sd_rename(p, op) != 0)
            return 1;
        if (sd_rename(np, p) != 0) {
            sd_rename(op, p);
            return 1;
        }
        if (sd_exists(op))
            sd_remove(op);
        files[f].writing = false;
        files[f].exists = true;
        files[f].deleted = false;
        files[f].size = files[f].new_size;
        files[f].gen = be32(r + 1);
        files[f].time = be32(r + 5);
        save_meta();
        a[0] = SMC_GOOD;
        return 1;
    }

    case SMC_DELETE: {
        uint8_t f = r[0];
        a[0] = SMC_FAIL;
        if (!file_ok(f) || !files[f].exists || files[f].writing)
            return 1;
        if (ps1_set_busy()) {
            a[0] = SMC_BUSY;
            return 1;
        }
        close_open();
        char p[192], t[200];
        path_of(p, sizeof(p), f, false);
        if (set_ps1) {
            /* a PS1 mode card (the SS1 deletes a game's card with no saves on it): gone for good, and its folder with
             * its last card (no tombstone: PS1 mode makes a new blank one if the game is played again) */
            if (sd_remove(p) != 0)
                return 1;
            files[f].exists = false;
            bool left = false;
            for (int i = 0; i < nfiles; i++)
                left |= files[i].exists;
            if (!left) {
                snprintf(t, sizeof(t), "%s/meta.ini", set_dir);
                if (sd_exists(t))
                    sd_remove(t);
                sd_rmdir(set_dir);
            }
            a[0] = SMC_GOOD;
            return 1;
        }
        snprintf(t, sizeof(t), "%s/.deleted", set_dir);
        sd_mkdir(t);
        snprintf(t, sizeof(t), "%s/.deleted/%s", set_dir, files[f].name);
        if (sd_exists(t))
            sd_remove(t);
        if (sd_rename(p, t) != 0)
            return 1;
        files[f].exists = false;
        files[f].deleted = true;
        files[f].size = 0;
        files[f].gen = be32(r + 1);
        files[f].time = be32(r + 5);
        save_meta();
        a[0] = SMC_GOOD;
        return 1;
    }

    case SMC_CERT:
    case SMC_AUTH:
    case SMC_AUTHSIG:
        return ps1_supermemcard_id_command(cmd, r, a);

    case SMC_UPDATE:
        return ps1_supermemcard_update_command(r, len, a);

    case SMC_PROGRESS: {
        /* the SS1's progress bar: printable ASCII only (the font) */
        int pct = r[0], tl = r[1] > SMC_PROGRESS_TEXT ? SMC_PROGRESS_TEXT : r[1];
        for (int i = 0; i < tl; i++)
            prog_text[i] = r[2 + i] >= 0x20 && r[2 + i] < 0x7F ? (char)r[2 + i] : ' ';
        if (tl || pct == 0xFF)
            prog_text[tl] = 0;
        prog_pct = pct == 0xFF ? -1 : pct > 100 ? 100 : pct;
        prog_at = time_us_64();
#ifdef SMC_SIM
        fprintf(stderr, "smc progress %d %s\n", prog_pct, prog_text);   /* (the virtual card has no screen: its log) */
#endif
        a[0] = SMC_GOOD;
        return 1;
    }

    case SMC_STATUS:
        a[0] = a[1] = 0; /* every write above is on the SD card before it's answered */
        return 2;

    case SMC_LIST: {
        /* the idx-th game of a system, or with sys "." the idx-th system */
        char sys[SMC_SYS_LEN + 1], dir[24 + SMC_SYS_LEN];
        int sl = r[0], card = r[1 + sl], want = r[2 + sl] << 8 | r[3 + sl];
        bool systems = sl == 1 && r[1] == '.';
        memset(a, 0, 1 + SMC_ID_LEN);
        a[0] = SMC_NOT_FOUND;
        if (systems)
            snprintf(dir, sizeof(dir), ROOT);
        else if (sl == (int)strlen(SMC_PS1CARD) && !memcmp(r + 1, SMC_PS1CARD, (size_t)sl))
            snprintf(dir, sizeof(dir), PS1_ROOT);   /* PS1 mode's card folders */
        else if (take_name(sys, sizeof(sys), r + 1, sl, false) && card <= SMC_MAX_CARDS)
            snprintf(dir, sizeof(dir), ROOT "/%s/Card%d", sys, card ? card : current_card(sys));
        else
            return 1 + SMC_ID_LEN;
        struct nth n = {want, a + 1};
        if (each_dir(dir, nth_cb, &n) > want)
            a[0] = SMC_GOOD;
        return 1 + SMC_ID_LEN;
    }
    }
    return 0;
}

static void load_settings(void) {
    char buf[64];
    bool have = read_text(ROOT "/settings.ini", buf, sizeof(buf)) > 0;
    bool off = have && strstr(buf, "saves=0") != NULL;
    clock24 = have && strstr(buf, "clock=24") != NULL;
    const char *tz = have ? strstr(buf, "tz=") : NULL;
    tz_known = tz != NULL;
    tz_min = tz ? atoi(tz + 3) : 0;
    __atomic_store_n(&enabled, off ? 2 : 1, __ATOMIC_RELEASE);
}

static void write_settings(void) {
    char buf[64];
    int l = snprintf(buf, sizeof(buf), "saves=%d\nclock=%d\n", __atomic_load_n(&enabled, __ATOMIC_ACQUIRE) == 2 ? 0 : 1,
                     clock24 ? 24 : 12);
    if (tz_known)
        snprintf(buf + l, sizeof(buf) - (size_t)l, "tz=%d\n", tz_min);
    sd_mkdir(ROOT);
    write_text(ROOT "/settings.ini", buf);
}

bool ps1_supermemcard_clock24(void) { return clock24; }
void ps1_supermemcard_set_clock24(bool on) {
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE))
        load_settings();
    clock24 = on;
    write_settings();
    mode_seq++;   /* the screen's time again */
}

void ps1_supermemcard_format_time(const char *in, char *out, size_t n) {
    /* in: "MM/DD/YY HH:MM" (24 h, as the SuperStation sends it); out: as is, or "MM/DD/YY hh:MMAM" in 12 h */
    int h, m;
    if (clock24 || strlen(in) != 14 || sscanf(in + 9, "%2d:%2d", &h, &m) != 2 || h < 0 || h > 23) {
        snprintf(out, n, "%s", in);
        return;
    }
    snprintf(out, n, "%.9s%02d:%02d%s", in, h % 12 ? h % 12 : 12, m, h < 12 ? "AM" : "PM");
}

bool ps1_supermemcard_enabled(void) { return __atomic_load_n(&enabled, __ATOMIC_ACQUIRE) != 2; }

void ps1_supermemcard_set_enabled(bool on) {
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE))
        load_settings();   /* keep the clock setting */
    __atomic_store_n(&enabled, on ? 1 : 2, __ATOMIC_RELEASE);
    write_settings();
    smc_log("Super MemCard %s\n", on ? "on" : "off");
    if (!on) {
        __atomic_store_n(&active, false, __ATOMIC_RELEASE); /* until the next Hello, after it's on again */
        set_mode("");
    }
}

uint32_t ps1_supermemcard_count_free(void) {
    free_kb = sd_free_kb();
    written_kb = 0;
    return free_kb;
}

bool ps1_supermemcard_busy(void) { return last_cmd_at && time_us_64() < last_cmd_at + 1500 * 1000; }

bool ps1_supermemcard_gui_may_run(void) {
    /* drawing the screen (a scrolling line redrawn and sent to the OLED) holds this core ~20 ms, and the SS1's next
     * block waits for it: while blocks are coming (one in the last 30 ms), the screen is drawn every 250 ms only */
    static uint64_t drawn;
    uint64_t now = time_us_64();
    if (!last_cmd_at || now > last_cmd_at + 30 * 1000 || now > drawn + 250 * 1000) {
        drawn = now;
        return true;
    }
    return false;
}

bool ps1_supermemcard_progress(int *pct, const char **text) {
    if (prog_pct < 0 || time_us_64() > prog_at + SMC_PROGRESS_TIMEOUT_MS * 1000ull)
        return false;
    if (pct)
        *pct = prog_pct;
    if (text)
        *text = prog_text;
    return true;
}

void ps1_supermemcard_task(void) {
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE))
        load_settings();
    /* a PS1 game is using the card: the buttons are the PS1 card's again */
    if (mode_sys[0] && __atomic_load_n(&ps1_used, __ATOMIC_ACQUIRE))
        set_mode("");
    if (mode_dirty && time_us_64() >= mode_save_at)
        save_mode_card();

    uint32_t seq = __atomic_load_n(&posted, __ATOMIC_ACQUIRE);
    if (seq != __atomic_load_n(&done, __ATOMIC_ACQUIRE)) {
        uint64_t t0 = time_us_64();
        mb_alen = do_command(mb_cmd, mb_req, mb_len, mb_ans);
        last_cmd_at = time_us_64();
        __atomic_store_n(&done, seq, __ATOMIC_RELEASE);
        log_command(mb_cmd, mb_req, mb_len, mb_ans, mb_alen, last_cmd_at - t0);   /* after the answer: never slows it */
    } else if ((log_rblocks || log_wblocks) && time_us_64() > last_cmd_at + 500 * 1000)
        log_transfer_end();
    /* the identity: deployed from the SD card at the first pass, a challenge signed once its AUTH is answered */
    if (ps1_supermemcard_enabled())
        ps1_supermemcard_has_identity();
    ps1_supermemcard_id_task();
    ps1_supermemcard_update_task();
}
