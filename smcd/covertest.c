#include "smc_sync.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
/* smcd/test.sh: the icons' pictures (<base>.cover) kept, fetched for icons made before, .nocover when none.
 * Its folder: /tmp/covt (cover.png there: a test picture) */
static int fails;
static int fake(const char *url, const char *to) {
    if (!strstr(url, "libretro") || fails) return 1;
    char c[600]; snprintf(c, sizeof c, "cp /tmp/covt/cover.png '%s'", to); return system(c) ? 1 : 0;
}
static int ex(const char *p) { return access(p, F_OK) == 0; }
static void queue(void) {
    FILE *q = fopen("/tmp/covt/st/icons/queue.txt", "w");
    fprintf(q, "NES\t1\tTest Game\tTest Game\tTest Game\t\tTest Game\n");
    fclose(q);
}
int main(void) {
    system("rm -rf /tmp/covt/st; mkdir -p /tmp/covt/st/icons");
    struct smc_sync_cfg k = {0};
    k.state_dir = "/tmp/covt/st"; k.saves_dir = "/tmp/covt/saves";
    queue();
    int m = smc_fetch_icons(&k, fake);
    int ok1 = m == 1 && ex("/tmp/covt/st/icons/NES/Test Game.bin") && ex("/tmp/covt/st/icons/NES/Test Game.cover");
    printf("%s new game: icon and its picture kept (%d)\n", ok1 ? "ok  " : "FAIL", m);
    unlink("/tmp/covt/st/icons/NES/Test Game.cover");
    system("cp '/tmp/covt/st/icons/NES/Test Game.bin' /tmp/covt/bin.before");
    queue();
    smc_fetch_icons(&k, fake);
    int ok2 = ex("/tmp/covt/st/icons/NES/Test Game.cover") && !system("cmp -s '/tmp/covt/st/icons/NES/Test Game.bin' /tmp/covt/bin.before");
    printf("%s icon from before: its picture fetched, the icon unchanged\n", ok2 ? "ok  " : "FAIL");
    unlink("/tmp/covt/st/icons/NES/Test Game.cover");
    fails = 1;
    queue();
    smc_fetch_icons(&k, fake);
    int ok3 = ex("/tmp/covt/st/icons/NES/Test Game.nocover") && ex("/tmp/covt/st/icons/NES/Test Game.bin");
    printf("%s no picture anywhere: .nocover, the icon kept\n", ok3 ? "ok  " : "FAIL");
    return !(ok1 && ok2 && ok3);
}
