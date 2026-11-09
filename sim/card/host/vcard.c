/* The RP2040 underneath sd2psXtd's PS1 card code, on a PC: two cores as threads, the PIO
 * state machines as FIFOs, the SEL interrupt, the clock. And the bus the host drives. */
#define _GNU_SOURCE
#include "vcard_server.h"
#include "pico_host.h"
#include "ps1_mc_spi.pio.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/* firmware entry points (src/ps1.c, src/ps1/ps1_dirty.h, src/ps1/ps1_memory_card.h) */
void ps1_init(void);
bool ps1_task(void);
void ps1_memory_card_exit(void);
void ps1_mc_data_interface_flush(void);
void ps1_cardman_close(void);
void ps1_cardman_flush(void);
void ps1_mmce_next_ch(bool delay);
void ps1_mmce_prev_ch(bool delay);
void ps1_mmce_next_idx(bool delay);
void ps1_mmce_prev_idx(bool delay);
#if WITH_SUPERMEMCARD
bool ps1_supermemcard_card_key(int delta);
uint32_t ps1_supermemcard_count_free(void);
void ps1_supermemcard_set_enabled(bool on);
#endif
void game_db_init(void);
extern int ps1_dirty_activity;
void host_sd_set_root(const char *root);
const char *host_sd_card_path(void);

/* ---- cores and time ---- */

static _Thread_local uint core_num;
uint get_core_num(void) { return core_num; }
void host_set_core_num(uint n) { core_num = n; }

static uint64_t t0;
static uint64_t mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
/* The card's clock starts at boot, like the RP2040 timer. */
uint64_t time_us_64(void) { return mono_us() - t0; }
uint64_t get_rand_64(void) {
    uint64_t v = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(&v, sizeof(v), 1, f) != 1)
        v = mono_us() * 0x9E3779B97F4A7C15ull;
    if (f)
        fclose(f);
    return v;
}
void sleep_us(uint64_t us) { usleep((useconds_t)us); }
void sleep_ms(uint32_t ms) { usleep((useconds_t)ms * 1000u); }
static _Thread_local host_timer_hw_t timer_regs;
host_timer_hw_t *host_timer_hw(void) {
    uint64_t t = time_us_64();
    timer_regs.timerawh = (uint32_t)(t >> 32);
    timer_regs.timerawl = (uint32_t)t;
    return &timer_regs;
}

static pthread_t core1;
static void (*core1_entry)(void);
static void *core1_main(void *arg) {
    (void)arg;
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    core_num = 1;
    core1_entry();
    return NULL;
}
void multicore_launch_core1(void (*entry)(void)) {
    core1_entry = entry;
    pthread_create(&core1, NULL, core1_main, NULL);
}
void multicore_reset_core1(void) {
    pthread_cancel(core1);
    pthread_join(core1, NULL);
}
void multicore_lockout_victim_init(void) {}

struct host_spin_lock { atomic_flag f; };
static struct host_spin_lock locks[32];
static uint next_lock = 16; /* the SDK hands out the striped locks first */
uint spin_lock_claim_unused(bool required) { (void)required; return next_lock++; }
spin_lock_t *spin_lock_init(uint num) { atomic_flag_clear(&locks[num].f); return &locks[num]; }
void spin_lock_unsafe_blocking(spin_lock_t *l) { while (atomic_flag_test_and_set(&l->f)) sched_yield(); }
void spin_unlock_unsafe(spin_lock_t *l) { atomic_flag_clear(&l->f); }

/* ---- SEL interrupt ---- */

host_iobank0_hw_t host_iobank0;
static irq_handler_t bank0_handler;
void irq_add_shared_handler(uint num, irq_handler_t h, uint8_t prio) { (void)num; (void)prio; bank0_handler = h; }
void irq_remove_handler(uint num, irq_handler_t h) { (void)num; if (bank0_handler == h) bank0_handler = NULL; }

/* ---- PIO: cmd_reader (sm 0), dat_writer (sm 1), controller reader (sm 2) ---- */

struct host_pio { int unused; };
static struct host_pio pio_block;
PIO pio0 = &pio_block;
const pio_program_t cmd_reader_program = {0}, dat_writer_program = {0};

#define FIFO_LEN 64
struct fifo { uint32_t v[FIFO_LEN]; unsigned head, tail; };
static struct fifo fifos[4];
static pthread_mutex_t bus = PTHREAD_MUTEX_INITIALIZER;
static int sm_claimed;

uint pio_add_program(PIO p, const pio_program_t *prog) { (void)p; (void)prog; return 0; }
void pio_remove_program(PIO p, const pio_program_t *prog, uint off) { (void)p; (void)prog; (void)off; }
int pio_claim_unused_sm(PIO p, bool required) { (void)p; (void)required; return sm_claimed++; }
void pio_sm_unclaim(PIO p, uint sm) { (void)p; (void)sm; }

static bool fifo_empty(struct fifo *f) { return f->head == f->tail; }
static void fifo_put(struct fifo *f, uint32_t v) { f->v[f->tail++ % FIFO_LEN] = v; }
static uint32_t fifo_get(struct fifo *f) { return f->v[f->head++ % FIFO_LEN]; }

bool pio_sm_is_rx_fifo_empty(PIO p, uint sm) {
    (void)p;
    pthread_mutex_lock(&bus);
    bool e = fifo_empty(&fifos[sm]);
    pthread_mutex_unlock(&bus);
    if (e)
        sched_yield();
    return e;
}
uint32_t pio_sm_get(PIO p, uint sm) {
    (void)p;
    pthread_mutex_lock(&bus);
    uint32_t v = fifo_empty(&fifos[sm]) ? 0 : fifo_get(&fifos[sm]);
    pthread_mutex_unlock(&bus);
    return v;
}
void pio_sm_put_blocking(PIO p, uint sm, uint32_t v) {
    (void)p;
    pthread_mutex_lock(&bus);
    fifo_put(&fifos[sm], v);
    pthread_mutex_unlock(&bus);
}
void pio_sm_clear_fifos(PIO p, uint sm) {
    (void)p;
    pthread_mutex_lock(&bus);
    fifos[sm].head = fifos[sm].tail;
    pthread_mutex_unlock(&bus);
}
void pio_sm_drain_tx_fifo(PIO p, uint sm) { pio_sm_clear_fifos(p, sm); }

/* ---- the bus ---- */

static atomic_uint loops; /* passes of core 0's main loop */

enum { SM_CMD = 0, SM_DAT = 1, SM_CTRL = 2 };
static bool sel;
static bool have_reply; /* dat_writer pulled a byte (and ACKed): it goes out on the next byte */
static uint32_t reply;

static void sel_edge(uint32_t ev) {
    uint pin = PIN_PSX_SEL;
    host_iobank0.proc1_irq_ctrl.ints[pin / 8] = ev << (4 * (pin % 8));
    uint me = core_num;
    core_num = 1; /* the handler was installed on core 1 */
    if (bank0_handler)
        bank0_handler();
    core_num = me;
    host_iobank0.proc1_irq_ctrl.ints[pin / 8] = 0;
}

void vcs_select(bool selected) {
    if (selected == sel)
        return;
    sel = selected;
    have_reply = false;
    sel_edge(selected ? GPIO_IRQ_EDGE_FALL : GPIO_IRQ_EDGE_RISE);
}

static uint8_t xfer(uint8_t cmd, int dat, bool last, bool *ack) {
    pthread_mutex_lock(&bus);
    /* dat_writer drives DAT by switching the pin to output, so the firmware queues ~byte */
    uint8_t in = have_reply ? (uint8_t)~reply : 0xFF;
    have_reply = false;
    fifo_put(&fifos[SM_CMD], (uint32_t)cmd << 24);
    if (dat >= 0)
        fifo_put(&fifos[SM_CTRL], (uint32_t)dat << 24);
    pthread_mutex_unlock(&bus);

    /* The card ACKs a byte by queueing its next reply. The PS1 gives up after ~100 us;
     * a PC thread can be descheduled, so wait longer, and only briefly for the last byte.
     * And only once the card has had the CPU for a while (its main loop went round): on a busy
     * PC (CI, other simulations) 100 ms of wall time can pass with the card's threads never
     * running, and a byte the card would have ACKed looked unanswered (08). */
    uint64_t deadline = mono_us() + (last ? 2000 : 100000);
    unsigned start = atomic_load(&loops), passes = last ? 2 : 20;
    bool a = false;
    for (;;) {
        pthread_mutex_lock(&bus);
        if (!fifo_empty(&fifos[SM_DAT])) {
            reply = fifo_get(&fifos[SM_DAT]);
            have_reply = a = true;
        }
        pthread_mutex_unlock(&bus);
        if (a || (mono_us() > deadline && atomic_load(&loops) - start >= passes))
            break;
        sched_yield();
    }
    if (ack)
        *ack = a;
    return in;
}

uint8_t vcs_xfer(uint8_t cmd, bool last, bool *ack) { return xfer(cmd, -1, last, ack); }
uint8_t vcs_peek(void) {
    pthread_mutex_lock(&bus);
    uint8_t v = have_reply ? (uint8_t)~reply : 0xFF;
    pthread_mutex_unlock(&bus);
    return v;
}
uint8_t vcs_xfer_ctrl(uint8_t cmd, uint8_t dat, bool last, bool *ack) { return xfer(cmd, dat, last, ack); }

/* ---- core 0: the firmware's main loop (src/main.c runs ps1_task forever) ---- */

static pthread_t core0;
static atomic_bool booted, running;
static atomic_int key_pending = -1;

/* The card's main screen with the card in PS1 mode, as src/gui.c evt_scr_main (with the Super MemCard
 * patch) does it; run on core 0, where the GUI runs. */
static void main_screen_key(int key) {
    enum { PREV, NEXT, BACK, ENTER, MENU_FREE, MENU_SAVES_OFF, MENU_SAVES_ON };
#if WITH_SUPERMEMCARD
    /* the SuperStation menu's items, as src/gui.c's evt_smc_* call them */
    if (key == MENU_FREE) { ps1_supermemcard_count_free(); return; }
    if (key == MENU_SAVES_OFF || key == MENU_SAVES_ON) { ps1_supermemcard_set_enabled(key == MENU_SAVES_ON); return; }
#endif
#if WITH_SUPERMEMCARD
    if (!ps1_supermemcard_card_key((key == NEXT || key == ENTER) ? 1 : -1))
#endif
    switch (key) {
        case PREV: ps1_mmce_prev_ch(true); break;
        case NEXT: ps1_mmce_next_ch(true); break;
        case BACK: ps1_mmce_prev_idx(true); break;
        case ENTER: ps1_mmce_next_idx(true); break;
    }
}

void vcs_key(int key) {
    atomic_store(&key_pending, key);
    while (atomic_load(&key_pending) >= 0)
        usleep(200);
}
static void *core0_main(void *arg) {
    (void)arg;
    core_num = 0;
    game_db_init();
    ps1_init();
    atomic_store(&booted, true);
    while (atomic_load(&running)) {
        int k = atomic_load(&key_pending);
        if (k >= 0) {
            main_screen_key(k);
            atomic_store(&key_pending, -1);
        }
        ps1_task();
        atomic_fetch_add(&loops, 1);
        usleep(100);
    }
    /* power-off path, as in ps1_deinit() */
    ps1_memory_card_exit();
    ps1_mc_data_interface_flush();
    ps1_cardman_close();
    multicore_reset_core1();
    return NULL;
}

int vcs_start(const char *sd_root) {
    if (access(sd_root, W_OK) != 0)
        return -1;
    t0 = mono_us() - 1000000; /* booted a second ago, so lockout timers start expired */
    host_sd_set_root(sd_root);
    sel = false;
    atomic_store(&running, true);
    pthread_create(&core0, NULL, core0_main, NULL);
    while (!atomic_load(&booted))
        usleep(1000);
    return 0;
}

void vcs_settle(void) {
    /* switching waits 250 ms; the dirty lockout is 100 ms; then two clean passes of the loop */
    usleep(400 * 1000);
    for (int clean = 0; clean < 2;) {
        unsigned n = atomic_load(&loops);
        while (atomic_load(&loops) == n)
            usleep(200);
        clean = ps1_dirty_activity ? 0 : clean + 1;
    }
}

void vcs_stop(void) {
    atomic_store(&running, false);
    pthread_join(core0, NULL);
}

const char *vcs_card_path(void) { return host_sd_card_path(); }
