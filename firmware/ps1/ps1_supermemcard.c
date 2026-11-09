/* Super MemCard for sd2psXtd's PS1 mode: the SD card side (core 0).
 *
 * Core 1 (ps1_supermemcard_bus.inc) posts each complete request here; ps1_supermemcard_task(),
 * called from the main loop, carries it out on the SD card and posts the answer back.
 *
 *   SuperMemCard/card.ini                              id=<16 hex digits>, made once
 *   SuperMemCard/<system>/card.ini                     card=<N>: the system's current virtual card (1..99)
 *   SuperMemCard/<system>/Card<N>/<game ID>/<file>     a save file (system: Main's save folder name)
 *   SuperMemCard/<system>/Card<N>/<game ID>/<file>.new a save being written, until Commit
 *   SuperMemCard/<system>/Card<N>/<game ID>/meta.ini   <file>=<generation>
 *
 * A save is written to <file>.new and only replaces <file> at Commit, so a card pulled or
 * powered off mid-save keeps the previous save whole. */
#include "ps1_supermemcard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/rand.h"
#include "pico/time.h"
#include "sd.h"

#define ROOT "SuperMemCard"

/* ---- mailbox between the cores ---- */

static uint8_t mb_cmd, mb_req[SMC_MAX_REQ], mb_ans[SMC_MAX_ANS];
static int mb_len, mb_alen;
static uint32_t posted, done; /* sequence numbers, accessed with __atomic */
static bool active;           /* a Hello since power-up */

bool ps1_supermemcard_active(void) { return __atomic_load_n(&active, __ATOMIC_ACQUIRE); }

int ps1_supermemcard_req_len(uint8_t cmd, const uint8_t *r, int have) {
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
    case SMC_COMMIT: return 5;
    case SMC_STATUS: return 0;
    case SMC_LIST:
        if (have < 1) return 1;
        if (r[0] == 0 || r[0] > SMC_SYS_LEN) return -1;
        return 1 + r[0] + 1 + 2;
    case SMC_INFO: return 1;
    case SMC_CORE:
        if (have < 1) return 1;
        if (r[0] == 0 || r[0] > SMC_SYS_LEN) return -1;
        return 1 + r[0] + 1;
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
    uint32_t size, gen;
    bool exists;   /* committed copy on the SD card */
    bool writing;  /* <name>.new is open for writing */
    uint32_t new_size;
};

static char card_id[17];
static char set_dir[24 + SMC_SYS_LEN + SMC_ID_LEN];
static bool have_set;
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

/* read a small text file into buf (NUL-terminated); returns its length or -1 */
static int read_text(const char *path, char *buf, int max) {
    int fd = sd_open(path, O_RDONLY);
    if (fd < 0)
        return -1;
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

static void save_meta(void) {
    char text[SMC_MAX_FILES * (SMC_NAME_LEN + 16)], line[48], p[192];
    text[0] = 0;
    for (int i = 0; i < nfiles; i++)
        if (files[i].exists) {
            snprintf(line, sizeof(line), "%s=%lu\n", files[i].name, (unsigned long)files[i].gen);
            strcat(text, line);
        }
    snprintf(p, sizeof(p), "%s/meta.ini", set_dir);
    write_text(p, text);
}

static uint32_t meta_gen(const char *meta, const char *name) {
    size_t n = strlen(name);
    for (const char *l = meta; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
        if (strncmp(l, name, n) == 0 && l[n] == '=')
            return (uint32_t)strtoul(l + n + 1, NULL, 10);
    return 0;
}

static bool is_save_name(const char *n) {
    size_t l = strlen(n);
    if (l == 0 || l > SMC_NAME_LEN || strcmp(n, "meta.ini") == 0 || n[0] == '.')
        return false;
    return !(l > 4 && strcmp(n + l - 4, ".new") == 0);
}

/* ---- virtual cards, and the system whose core is running ---- */

static char mode_sys[SMC_SYS_LEN + 1]; /* "" : none, the buttons are the PS1 card's */
static int mode_card;                  /* its current card */
static uint32_t mode_seq;
static bool mode_dirty;                /* mode_card changed by the buttons, not yet in card.ini */
static uint64_t mode_save_at;
static bool ps1_used;                  /* set by core 1 */

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

static void save_mode_card(void) {
    char p[96], buf[32];
    if (!mode_dirty)
        return;
    mode_dirty = false;
    snprintf(p, sizeof(p), ROOT "/%s", mode_sys);
    mkdirs(p);
    snprintf(p, sizeof(p), ROOT "/%s/card.ini", mode_sys);
    snprintf(buf, sizeof(buf), "card=%d\n", mode_card);
    write_text(p, buf);
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
    mode_card = sys[0] ? read_current_card(sys) : 0;
    mode_seq++;
}

void ps1_supermemcard_ps1_used(void) { __atomic_store_n(&ps1_used, true, __ATOMIC_RELEASE); }
const char *ps1_supermemcard_mode_system(void) { return mode_sys; }
int ps1_supermemcard_mode_card(void) { return mode_card; }
uint32_t ps1_supermemcard_mode_seq(void) { return mode_seq; }

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

static void select_set(const char *sys, const char *id, int card) {
    close_open();
    /* .new files of the previous set stay on the SD card unfinished: never committed, never read */
    nfiles = 0;
    have_set = true;
    snprintf(set_dir, sizeof(set_dir), ROOT "/%s/Card%d/%s", sys, card, id);

    static char meta[SMC_MAX_FILES * (SMC_NAME_LEN + 16)];
    char p[192];
    snprintf(p, sizeof(p), "%s/meta.ini", set_dir);
    if (read_text(p, meta, sizeof(meta)) < 0)
        meta[0] = 0;

    int dir = sd_open(set_dir, O_RDONLY);
    if (dir < 0)
        return;
    int it = sd_iterate_dir(dir, -1);
    while (it != -1 && nfiles < SMC_MAX_FILES) {
        char name[SMC_NAME_LEN + 2];
        if (!sd_is_dir(it) && sd_get_name(it, name, sizeof(name)) && is_save_name(name)) {
            struct file *f = &files[nfiles++];
            memset(f, 0, sizeof(*f));
            snprintf(f->name, sizeof(f->name), "%s", name);
            f->size = (uint32_t)sd_filesize(it);
            f->gen = meta_gen(meta, name);
            f->exists = true;
        }
        it = sd_iterate_dir(dir, it);
    }
    if (it != -1)
        sd_close(it);
    sd_close(dir);
    /* stable order: by name */
    for (int i = 1; i < nfiles; i++)
        for (int j = i; j > 0 && strcmp(files[j - 1].name, files[j].name) > 0; j--) {
            struct file t = files[j];
            files[j] = files[j - 1];
            files[j - 1] = t;
        }
}

static bool file_ok(uint8_t f) { return have_set && f < nfiles; }

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
        a[3] = 0x07; /* features: save files, list, virtual cards (Core) */
        for (int i = 0; i < 8; i++) {
            unsigned v;
            sscanf(card_id + i * 2, "%2x", &v);
            a[4 + i] = (uint8_t)v;
        }
        put32(a + 12, 0xFFFFFFFFu); /* free space: not known through the SD wrapper */
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
        a[0] = SMC_GOOD;
        a[1] = none ? 0 : (uint8_t)current_card(sys);
        return 2;
    }

    case SMC_INFO:
        memset(a, 0, 25);
        if (!file_ok(r[0]) || !files[r[0]].exists) {
            a[0] = have_set ? SMC_NOT_FOUND : SMC_NO_SET;
            return 25;
        }
        a[0] = SMC_GOOD;
        memcpy(a + 1, files[r[0]].name, strlen(files[r[0]].name));
        put32(a + 17, files[r[0]].size);
        put32(a + 21, files[r[0]].gen);
        return 25;

    case SMC_READ: {
        uint8_t f = r[0];
        uint32_t off = ((uint32_t)r[1] << 8 | r[2]) * 128u;
        memset(a, 0, 130);
        a[129] = SMC_FAIL;
        if (!file_ok(f) || !files[f].exists || off >= files[f].size)
            return 130;
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
        a[0] = SMC_GOOD;
        a[1] = (uint8_t)f;
        return 2;
    }

    case SMC_COMMIT: {
        uint8_t f = r[0];
        a[0] = SMC_FAIL;
        if (!file_ok(f) || !files[f].writing)
            return 1;
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
        if (sd_exists(p))
            sd_remove(p);
        if (sd_rename(np, p) != 0)
            return 1;
        files[f].writing = false;
        files[f].exists = true;
        files[f].size = files[f].new_size;
        files[f].gen = be32(r + 1);
        save_meta();
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
        else if (take_name(sys, sizeof(sys), r + 1, sl, false) && card <= SMC_MAX_CARDS)
            snprintf(dir, sizeof(dir), ROOT "/%s/Card%d", sys, card ? card : current_card(sys));
        else
            return 1 + SMC_ID_LEN;
        int d = sd_open(dir, O_RDONLY);
        if (d < 0)
            return 1 + SMC_ID_LEN;
        int it = sd_iterate_dir(d, -1), i = 0;
        while (it != -1) {
            char name[SMC_ID_LEN + 2];
            if (sd_is_dir(it) && sd_get_name(it, name, sizeof(name)) && strlen(name) <= SMC_ID_LEN && name[0] != '.' &&
                i++ == want) {
                a[0] = SMC_GOOD;
                memcpy(a + 1, name, strlen(name));
                break;
            }
            it = sd_iterate_dir(d, it);
        }
        if (it != -1)
            sd_close(it);
        sd_close(d);
        return 1 + SMC_ID_LEN;
    }
    }
    return 0;
}

void ps1_supermemcard_task(void) {
    /* a PS1 game is using the card: the buttons are the PS1 card's again */
    if (mode_sys[0] && __atomic_load_n(&ps1_used, __ATOMIC_ACQUIRE))
        set_mode("");
    if (mode_dirty && time_us_64() >= mode_save_at)
        save_mode_card();

    uint32_t seq = __atomic_load_n(&posted, __ATOMIC_ACQUIRE);
    if (seq == __atomic_load_n(&done, __ATOMIC_ACQUIRE))
        return;
    mb_alen = do_command(mb_cmd, mb_req, mb_len, mb_ans);
    __atomic_store_n(&done, seq, __ATOMIC_RELEASE);
}
