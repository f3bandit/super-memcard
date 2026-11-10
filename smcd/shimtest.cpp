// Test harness: main/smc_shim.cpp as a MiSTer Main would run it, with the front ports stood in for by the virtual card
// (vcardd), so smcd and the shim are tested together on a PC. Not part of anything shipped.
//   shimtest <vcardd socket> launch <rbf> [<mgl>] | menu | serve <seconds>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "smc_frontport.h"
#include "smc_shim.h"
extern "C" {
#include "smc_vcard.h"
}

static struct smc_bus vbus;
static int menu = 1;
static int held;

// stand-ins for Main
char *user_io_get_core_name(int) { return (char *)(menu ? "MENU" : "GAME"); }
char is_menu() { return menu; }

int smc_frontport_raw(int port, const uint8_t *out, uint8_t *in, int len, int ack_ms, int speed) {
    (void)ack_ms; (void)speed;
    if (port != 1) return len == 0 ? 0 : 0;   // the virtual card is in port 1; port 2 is empty
    if (len == 0) return 0;
    vbus.select(vbus.ctx, true);
    int n = 0;
    for (int k = 0; k < len; k++) {
        bool ack = false;
        in[k] = vbus.xfer(vbus.ctx, out[k], k == len - 1, &ack);
        n++;
        if (!ack && k < len - 1) break;
    }
    vbus.select(vbus.ctx, false);
    return n;
}
void smc_frontport_hold(int on) { held = on; }
void smc_frontport_reset() {}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 3 || smc_vcard_open(argv[1], &vbus)) { fprintf(stderr, "usage: shimtest <vcardd socket> launch|menu|serve ...\n"); return 2; }
    if (!strcmp(argv[2], "launch") && argc >= 4) {
        smc_shim_core_start(argv[3], argc >= 5 ? argv[4] : "");
        printf("launch returned\n");
        menu = 0;
    } else if (!strcmp(argv[2], "menu")) {
        smc_shim_menu_start();
        printf("menu told\n");
    }
    int secs = !strcmp(argv[2], "serve") && argc >= 4 ? atoi(argv[3]) : 20;
    for (time_t end = time(0) + secs; time(0) < end;) { smc_shim_poll(); usleep(5000); }
    smc_vcard_close(&vbus);
    return 0;
}
