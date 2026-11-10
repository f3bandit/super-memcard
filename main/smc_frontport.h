// Super MemCard: the SuperStation One's front ports from a MiSTer Main, through the front-port driver in the loaded
// core (core/sys/smc_port.sv: in the menu core on hps_io's EXT_BUS, in a game core in sys_top). Copyright (C) 2026
// f3bandit.
#pragma once
#include <stdint.h>

// One raw frame on front port 1 or 2: out[0..len-1] in one select, the card's byte for each into in[], stopping after
// the first byte the card doesn't ACK. ack_ms: how long a byte's ACK may take (1..167). speed: 0 as a PS1 (250 kHz),
// 1 500 kHz, 2 1 MHz, 3 2 MHz. Returns the bytes exchanged (0: nothing answered); -1 no driver in this core (len 0
// only asks that), -2 busy, -3 no answer from the driver in time.
int smc_frontport_raw(int port, const uint8_t *out, uint8_t *in, int len, int ack_ms, int speed);

// Keep the front ports switched to the driver (1) across several frames, or give them back to the core (0).
void smc_frontport_hold(int on);

// Forget what the loaded core answered (call when another core is loaded).
void smc_frontport_reset();
