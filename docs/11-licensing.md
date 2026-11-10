# Licensing and the agreement with Retro Remake

**Draft term summary for a lawyer to turn into the agreement. Not legal advice.**

## Parties

- **Licensor:** f3bandit (the author and owner of Super MemCard).
- **Licensee:** Retro Remake (Taki Udon).

## What is licensed

`smcd`, the Super MemCard host program, and everything else in this repository that isn't listed as
GPL in [LICENSE](../LICENSE): the sync, game IDs and names, icons, virtual cards, the memory-manager
commands, the documentation, the guide and the tools.

## Terms

| | |
|---|---|
| Grant | Use, modify, ship and sell the licensed work in Retro Remake products and services, worldwide, without end date |
| Exclusivity | Exclusive: f3bandit licenses it to no one else |
| Ownership | f3bandit keeps ownership; this is a licence, not an assignment |
| Source | Retro Remake receives the source; products ship it in binary form only |
| End users | Binary only, on Retro Remake hardware; no right to copy, modify, redistribute or reverse-engineer |
| Hardware | The features run only with genuine Retro Remake hardware (the card's certificate; the SuperStation check) |
| Signing keys | Held by Retro Remake (card certificates, update signatures) |
| Credit | f3bandit is credited for his contributions in Console Mode's credits, in `smcd`'s about screen, and in the documentation of products that use his work |
| Consideration | Retro Remake sends f3bandit, free of charge, one unit of every product Retro Remake releases from now on, as it's released, so he can keep testing and building features for Retro Remake products. No other payment |
| If it ends | (for the lawyer) e.g. if Retro Remake stops shipping it or doesn't meet the terms, the rights return to f3bandit |

## The open parts

These stay GPL-3.0-or-later, **copyright f3bandit**, because they are built into GPL works. Retro
Remake uses and ships them under the GPL like anyone, publishing their source with what it ships:

| part | why GPL |
|---|---|
| `firmware/`: the card firmware's Super MemCard additions | sd2psXtd is GPL-3.0 |
| `core/sys/`: the card driver in the cores' `sys_top` (public: f3bandit/super-memcard-cores) | the MiSTer cores are GPL |
| `main/smc_frontport.*` and the Main shim | Main_MiSTer is GPL-3.0 |

They hold no Super MemCard logic: the firmware stores files, the driver and shim pass bytes.

## Why `smcd` isn't touched by the GPL

`smcd` is a separate program. It talks to the GPL Main through a local socket (the shim passes card
frames), the same way Console Mode's own frontend talks to its Main through a command channel. It
isn't compiled or linked into any GPL program.
