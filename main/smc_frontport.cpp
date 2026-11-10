// Super MemCard: the front ports from a MiSTer Main (smc_frontport.h). Copyright (C) 2026 f3bandit.
// GPL-3.0-or-later, as Main_MiSTer.
#include "smc_frontport.h"

#include <unistd.h>

#include "hardware.h"   // GetTimer, CheckTimer
#include "spi.h"        // spi_uio_cmd_cont, spi_w, DisableIO

// the driver's commands (core/sys/smc_port.sv)
#define SMC_ID          0x80   // reply: 534D ('SM'), version, mode
#define SMC_MODE        0x81   // write: bit 0 = the front ports are the driver's
#define SMC_RAW         0x87   // write: {port 2, len}, {speed, ack wait in 10.24 us units}, the bytes as words
#define SMC_RAW_STATUS  0x88   // reply: {busy, done, count}, then the card's bytes as words

static int ver = -1;            // the driver's version in the loaded core; 0: none; -1: not asked yet
static unsigned long next_probe;
static int held;

void smc_frontport_reset() { ver = -1; }

// Asked again at most once a second while the answer is "no": right after Main loads a core it may not answer yet
static int present()
{
	if (ver == 0 && CheckTimer(next_probe)) ver = -1;
	if (ver < 0)
	{
		next_probe = GetTimer(1000);
		// the answer to a word comes back with that same word (as with hps_io's own commands): the command word
		// itself returns the first reply word
		uint16_t id = spi_uio_cmd_cont(SMC_ID);
		uint16_t v = spi_w(0);
		DisableIO();
		ver = (id == 0x534D && v) ? v : 0;
	}
	return ver > 0;
}

void smc_frontport_hold(int on)
{
	if (on == held) return;
	if (present())
	{
		spi_uio_cmd_cont(SMC_MODE); spi_w(on ? 1 : 0); DisableIO();
		if (on) usleep(50000);   // a card just switched onto the bus misses the first frame (seen on the SS1)
	}
	held = on;
}

int smc_frontport_raw(int port, const uint8_t *out, uint8_t *in, int len, int ack_ms, int speed)
{
	if (!present() || ver < 2) return -1;
	if (speed < 0 || speed > 3 || ver < 3) speed = 0;   // speeds: driver version 3
	if (len == 0) return 0;
	if (len < 0 || len > 255 || (port != 1 && port != 2)) return -1;
	int mine = !held;
	if (mine)
	{
		spi_uio_cmd_cont(SMC_MODE); spi_w(1); DisableIO();
		usleep(50000);
	}

	spi_uio_cmd_cont(SMC_RAW);
	spi_w((port == 2 ? 0x100 : 0) | len);
	spi_w((uint16_t)(speed << 14 | (ack_ms > 167 ? 167 : ack_ms) * 100000 / 1024));   // units of 10.24 us
	for (int k = 0; k < len; k += 2) spi_w((uint16_t)(out[k] | ((k + 1 < len ? out[k + 1] : 0) << 8)));
	DisableIO();

	int count = -3;
	unsigned long until = GetTimer(2000 + ack_ms * 4);
	while (!CheckTimer(until))
	{
		usleep(500);
		uint16_t st = spi_uio_cmd_cont(SMC_RAW_STATUS);
		if ((st & 0x8000) || !(st & 0x4000)) { DisableIO(); continue; }   // busy, or not done yet
		count = st & 0xFF;
		for (int k = 0; k < len; k += 2)
		{
			uint16_t w = spi_w(0);
			in[k] = w & 0xFF;
			if (k + 1 < len) in[k + 1] = w >> 8;
		}
		DisableIO();
		break;
	}
	if (mine) { spi_uio_cmd_cont(SMC_MODE); spi_w(0); DisableIO(); }
	return count;
}
