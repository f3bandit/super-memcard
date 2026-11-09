/* The card's SD card (ext/ESP8266SdFatWrapper/include/sd.h) as a folder on the PC. */
#define _GNU_SOURCE
#include "sd.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_FD 32

static char root[PATH_MAX];
static char card_path[256];

struct handle {
    bool used, is_dir;
    int fd;        /* file */
    DIR *dir;      /* directory */
    char rel[256]; /* path from the SD root */
    char name[256];
};
static struct handle h[MAX_FD];

void host_sd_set_root(const char *r) { snprintf(root, sizeof(root), "%s", r); }
const char *host_sd_card_path(void) { return card_path; }

static void full(const char *rel, char *out, size_t n) {
    while (*rel == '/')
        rel++;
    snprintf(out, n, "%s/%s", root, rel);
}

static int alloc(void) {
    for (int i = 0; i < MAX_FD; i++)
        if (!h[i].used) {
            memset(&h[i], 0, sizeof(h[i]));
            h[i].used = true;
            h[i].fd = -1;
            return i;
        }
    return -1;
}

static int open_rel(const char *rel, int oflag) {
    char p[PATH_MAX];
    full(rel, p, sizeof(p));
    struct stat st;
    int i = alloc();
    if (i < 0)
        return -1;
    snprintf(h[i].rel, sizeof(h[i].rel), "%s", rel);
    const char *slash = strrchr(rel, '/');
    snprintf(h[i].name, sizeof(h[i].name), "%s", slash ? slash + 1 : rel);
    if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
        h[i].is_dir = true;
        h[i].dir = opendir(p);
        if (!h[i].dir) {
            h[i].used = false;
            return -1;
        }
        return i;
    }
    h[i].fd = open(p, oflag, 0644);
    if (h[i].fd < 0) {
        h[i].used = false;
        return -1;
    }
    return i;
}

void sd_init(void) {}

int sd_open(const char *path, int oflag) {
    int i = open_rel(path, oflag);
    size_t n = strlen(path);
    if (i >= 0 && !h[i].is_dir && n > 4 && strcmp(path + n - 4, ".mcd") == 0 && (oflag & O_ACCMODE) == O_RDWR)
        snprintf(card_path, sizeof(card_path), "%s", path);
    return i;
}

static struct handle *get(int fd) { return (fd >= 0 && fd < MAX_FD && h[fd].used) ? &h[fd] : NULL; }

int sd_close(int fd) {
    struct handle *x = get(fd);
    if (!x)
        return -1;
    if (x->dir)
        closedir(x->dir);
    if (x->fd >= 0) {
        close(x->fd);
        if (strcmp(x->rel, card_path) == 0)
            card_path[0] = 0;
    }
    x->used = false;
    return 0;
}

void sd_flush(int fd) {
    struct handle *x = get(fd);
    if (x && x->fd >= 0)
        fsync(x->fd);
}

int sd_read(int fd, void *buf, size_t count) {
    struct handle *x = get(fd);
    return (x && x->fd >= 0) ? (int)read(x->fd, buf, count) : -1;
}

int sd_write(int fd, void *buf, size_t count) {
    struct handle *x = get(fd);
    return (x && x->fd >= 0) ? (int)write(x->fd, buf, count) : -1;
}

int sd_seek(int fd, int32_t offset, int whence) {
    struct handle *x = get(fd);
    return (x && x->fd >= 0 && lseek(x->fd, offset, whence) >= 0) ? 0 : -1;
}
int sd_seek64(int fd, int64_t offset, int whence) {
    struct handle *x = get(fd);
    return (x && x->fd >= 0 && lseek(x->fd, offset, whence) >= 0) ? 0 : -1;
}
uint32_t sd_tell(int fd) {
    struct handle *x = get(fd);
    return (x && x->fd >= 0) ? (uint32_t)lseek(x->fd, 0, SEEK_CUR) : 0;
}
uint64_t sd_tell64(int fd) { return sd_tell(fd); }

int sd_filesize(int fd) {
    struct handle *x = get(fd);
    struct stat st;
    return (x && x->fd >= 0 && fstat(x->fd, &st) == 0) ? (int)st.st_size : -1;
}
uint64_t sd_filesize64(int fd) { int s = sd_filesize(fd); return s < 0 ? 0 : (uint64_t)s; }

int sd_mkdir(const char *path) {
    char p[PATH_MAX];
    full(path, p, sizeof(p));
    return (mkdir(p, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

int sd_exists(const char *path) {
    char p[PATH_MAX];
    full(path, p, sizeof(p));
    return access(p, F_OK) == 0;
}

int sd_remove(const char *path) {
    char p[PATH_MAX];
    full(path, p, sizeof(p));
    return unlink(p);
}
int sd_rmdir(const char *path) {
    char p[PATH_MAX];
    full(path, p, sizeof(p));
    return rmdir(p);
}
int sd_rename(const char *a, const char *b) {
    char p[PATH_MAX], q[PATH_MAX];
    full(a, p, sizeof(p));
    full(b, q, sizeof(q));
    return rename(p, q);
}

/* Directory iteration as in the SdFat wrapper: each call closes the previous entry and opens
 * the next one; -1 at the end. */
int sd_iterate_dir(int dir, int it) {
    struct handle *d = get(dir);
    if (it >= 0)
        sd_close(it);
    if (!d || !d->dir)
        return -1;
    struct dirent *e;
    while ((e = readdir(d->dir))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char rel[512];
        snprintf(rel, sizeof(rel), "%s/%s", d->rel, e->d_name);
        int i = open_rel(rel, O_RDONLY);
        if (i >= 0)
            return i;
    }
    return -1;
}

size_t sd_get_name(int fd, char *name, size_t size) {
    struct handle *x = get(fd);
    if (!x || size == 0)
        return 0;
    snprintf(name, size, "%s", x->name);
    return strlen(name);
}

bool sd_is_dir(int fd) {
    struct handle *x = get(fd);
    return x && x->is_dir;
}

int sd_fd_is_open(int fd) { return get(fd) != NULL; }

int sd_getStat(int fd, sd_file_stat_t *const s) {
    struct handle *x = get(fd);
    struct stat st;
    if (!x || x->fd < 0 || fstat(x->fd, &st) != 0)
        return -1;
    memset(s, 0, sizeof(*s));
    s->size = (size_t)st.st_size;
    s->writable = true;
    return 0;
}

int sd_get_stat(int fd, ps2_fileio_stat_t *const s) {
    (void)fd;
    memset(s, 0, sizeof(*s));
    return -1;
}

sd_cid_t sd_get_CID(void) {
    sd_cid_t c;
    memset(&c, 0, sizeof(c));
    return c;
}
