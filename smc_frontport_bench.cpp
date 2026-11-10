// Test bench only: the shim's front-port calls (smc_frontport.h) over this Main's raw frames, which work with the
// menu core and with the game cores' sys_top driver alike. A real Main uses super-memcard's main/smc_frontport.cpp.
#include "smc_frontport.h"
#include "phosphor.h"

int smc_frontport_raw(int port, const uint8_t *out, uint8_t *in, int len, int ack_ms, int speed)
{
	return phosphor_card_raw_at(port, out, in, len, ack_ms, speed);
}
void smc_frontport_hold(int on) { phosphor_ports_hold(on); }
void smc_frontport_reset() {}
