/* Stand-in for the header pioasm makes from src/ps1/ps1_mc_spi.pio. On the card, cmd_reader
 * shifts in each CMD byte, dat_writer ACKs and shifts out each reply byte, and the controller
 * reader shifts in DAT. Here they are the FIFOs in host/vcard.c. */
#pragma once
#include "pico_host.h"
extern const pio_program_t cmd_reader_program, dat_writer_program;
static inline void cmd_reader_program_init(PIO p, uint sm, uint off) { (void)p; (void)sm; (void)off; }
static inline void dat_writer_program_init(PIO p, uint sm, uint off) { (void)p; (void)sm; (void)off; }
static inline void controller_program_init(PIO p, uint sm, uint off) { (void)p; (void)sm; (void)off; }
