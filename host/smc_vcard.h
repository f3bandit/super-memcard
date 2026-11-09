#pragma once
#include "smc.h"

#define SMC_VCARD_SOCKET "/tmp/supermemcard.sock"

/* Connect to a running vcardd (NULL: the default socket). 0 on success. */
int smc_vcard_open(const char *path, struct smc_bus *bus);
void smc_vcard_close(struct smc_bus *bus);
