/* Stand-in for the parts of the Raspberry Pi Pico SDK that sd2psXtd's PS1 card code uses, so the
 * firmware's own files build and run on a PC. Each core is a thread; PIO state machines are byte
 * FIFOs fed by the virtual bus (host/vcard.c); the GPIO interrupt is called by the bus on SEL edges. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef unsigned int uint;

#define __time_critical_func(f) f
#define __not_in_flash_func(f) f
#define __no_inline_not_in_flash_func(f) f
#define NUM_CORES 2
#define NUM_BANK0_GPIOS 30

/* PIN_PSX_* come from the board variant in CMake; SD2PSX values. */
#ifndef PIN_PSX_ACK
#define PIN_PSX_ACK 16
#define PIN_PSX_SEL 17
#define PIN_PSX_CLK 18
#define PIN_PSX_CMD 19
#define PIN_PSX_DAT 20
#endif

uint get_core_num(void);
void host_set_core_num(uint n);

/* time */
uint64_t time_us_64(void);
void sleep_ms(uint32_t ms);
void sleep_us(uint64_t us);
typedef struct { volatile uint32_t timerawh, timerawl; } host_timer_hw_t;
host_timer_hw_t *host_timer_hw(void);
#define timer_hw (host_timer_hw())

/* multicore */
void multicore_launch_core1(void (*entry)(void));
void multicore_reset_core1(void);
void multicore_lockout_victim_init(void);

/* spin locks (hardware/sync.h) */
typedef struct host_spin_lock spin_lock_t;
uint spin_lock_claim_unused(bool required);
spin_lock_t *spin_lock_init(uint num);
void spin_lock_unsafe_blocking(spin_lock_t *l);
void spin_unlock_unsafe(spin_lock_t *l);

/* GPIO */
enum { GPIO_IRQ_LEVEL_LOW = 1, GPIO_IRQ_LEVEL_HIGH = 2, GPIO_IRQ_EDGE_FALL = 4, GPIO_IRQ_EDGE_RISE = 8 };
enum { GPIO_SLEW_RATE_SLOW = 0, GPIO_SLEW_RATE_FAST = 1 };
enum { GPIO_DRIVE_STRENGTH_2MA, GPIO_DRIVE_STRENGTH_4MA, GPIO_DRIVE_STRENGTH_8MA, GPIO_DRIVE_STRENGTH_12MA };
typedef void (*gpio_irq_callback_t)(uint gpio, uint32_t event_mask);
typedef void (*irq_handler_t)(void);
#define IO_IRQ_BANK0 13
#define GPIO_IRQ_CALLBACK_ORDER_PRIORITY 0x40
static inline void gpio_set_dir(uint g, bool out) { (void)g; (void)out; }
static inline void gpio_disable_pulls(uint g) { (void)g; }
static inline void gpio_set_slew_rate(uint g, int r) { (void)g; (void)r; }
static inline void gpio_set_drive_strength(uint g, int s) { (void)g; (void)s; }
static inline void gpio_set_irq_enabled(uint g, uint32_t ev, bool en) { (void)g; (void)ev; (void)en; }
static inline void check_gpio_param(uint g) { (void)g; }
void irq_add_shared_handler(uint num, irq_handler_t h, uint8_t prio);
void irq_remove_handler(uint num, irq_handler_t h);
static inline void irq_set_enabled(uint num, bool en) { (void)num; (void)en; }
typedef struct { volatile uint32_t inte[4], intf[4], ints[4]; } io_bank0_irq_ctrl_hw_t;
typedef struct {
    volatile uint32_t intr[4];
    io_bank0_irq_ctrl_hw_t proc0_irq_ctrl, proc1_irq_ctrl;
} host_iobank0_hw_t;
extern host_iobank0_hw_t host_iobank0;
#define iobank0_hw (&host_iobank0)

/* PIO */
typedef struct host_pio *PIO;
extern PIO pio0;
typedef struct { const uint16_t *instructions; uint8_t length; int8_t origin; } pio_program_t;
typedef struct { uint32_t unused; } pio_sm_config;
uint pio_add_program(PIO pio, const pio_program_t *p);
void pio_remove_program(PIO pio, const pio_program_t *p, uint offset);
int pio_claim_unused_sm(PIO pio, bool required);
void pio_sm_unclaim(PIO pio, uint sm);
bool pio_sm_is_rx_fifo_empty(PIO pio, uint sm);
uint32_t pio_sm_get(PIO pio, uint sm);
void pio_sm_put_blocking(PIO pio, uint sm, uint32_t v);
void pio_sm_clear_fifos(PIO pio, uint sm);
void pio_sm_drain_tx_fifo(PIO pio, uint sm);
static inline void pio_set_sm_mask_enabled(PIO p, uint32_t m, bool e) { (void)p; (void)m; (void)e; }
static inline void pio_restart_sm_mask(PIO p, uint32_t m) { (void)p; (void)m; }
static inline void pio_enable_sm_mask_in_sync(PIO p, uint32_t m) { (void)p; (void)m; }
static inline void pio_sm_exec(PIO p, uint sm, uint instr) { (void)p; (void)sm; (void)instr; }
static inline uint pio_encode_jmp(uint addr) { return addr; }
