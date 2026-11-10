TEST ONLY. Retro Remake's test signing key for Super MemCard card identities (smccert key).
It signs the simulated cards in the tests; it is not secret and must never sign a real card.
Retro Remake makes its own with smccert key, keeps it offline, and puts its public key in host/smc_rrkey.h and
firmware/ps1/ps1_supermemcard_rrkey.h.
