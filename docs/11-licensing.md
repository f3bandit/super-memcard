# Licensing

Not legal advice: a lawyer should review this before it's relied on.

## Super MemCard: open source

Super MemCard is f3bandit's (the author and owner; the card and its memory card manager are his idea and
design). Since Nov 15 2026 it's open source:

- **Apache License 2.0** ([LICENSE](../LICENSE), [NOTICE](../NOTICE)) for everything that isn't a GPL part:
  `smcd`, `host/`, the tools, the docs and the guide. Anyone, Retro Remake included, may use, change, sell
  and ship it, in their products too, keeping f3bandit's copyright lines and passing the NOTICE on.
- **GPL-3.0-or-later** for the parts built into GPL works (below), for everyone, with an **additional
  permission for Retro Remake and Taki Udon**: they may use f3bandit's own code in those parts in any
  product of theirs, closed-source software such as Console Mode and their hardware included, without the
  GPL's conditions, free of charge; the only condition is crediting f3bandit. The permission is personal to
  them (and people working on Retro Remake products for them). It covers f3bandit's code only, not the GPL
  works it's built into (sd2psXtd, Main_MiSTer and its forks, the cores). The text is in [NOTICE](../NOTICE).

There's no exclusive licence: the draft terms below never took effect for Super MemCard or Coreshell.

**Coreshell** (core_shell) is also open source under the Apache License 2.0 (its LICENSE and NOTICE). It
talks to `smcd` over smcd's socket as a separate program and contains none of its code.

## f3bandit's other SuperStation projects (draft, not in effect)

Draft terms for a lawyer, for f3bandit's code in these projects (each repository's LICENSE.md lists what is
f3bandit's and what isn't). Not agreed; not legal advice.

- **cm_modules** (and ss1_cm_modules): Console Mode's modules: the memory card manager, the controller
  tester, the drive repair, cmjobd, the app link (cmremote), the modules and CM Link.
- **phosphor_os** and **phosphor-modules**: Phosphor's TV interface, phosphord, apps, tools and modules,
  including the dock IR remote upgrade and the safe shutdown.

Not included: the GPL forks (phosphor-main, phosphor-menu, the cores), which stay GPL; and SS1 Tool,
f3bandit's support tool for users, which keeps its own licence and its existing permission for Retro
Remake.

### Draft terms

| | |
|---|---|
| Grant | Use, modify, ship and sell the licensed work in Retro Remake products and services, worldwide, without end date |
| Exclusivity | Exclusive: f3bandit licenses it to no one else |
| Ownership | f3bandit keeps ownership; this is a licence, not an assignment |
| Source | Retro Remake receives the source; products ship it in binary form only |
| End users | Binary only, on Retro Remake hardware; no right to copy, modify, redistribute or reverse-engineer |
| Hardware | The features run only with genuine Retro Remake hardware: the card's identity ([12](12-card-identity.md)); the SuperStation check (to come) |
| Signing keys | Held by Retro Remake (the key that signs card identities, update signatures) |
| Credit | f3bandit is credited for his contributions in Console Mode's credits, in `smcd`'s about screen, and in the documentation of products that use his work |
| Consideration | Retro Remake sends f3bandit, free of charge, one unit of every product Retro Remake releases from now on, as it's released, so he can keep testing and building features for Retro Remake products. No other payment |
| If it ends | (for the lawyer) e.g. if Retro Remake stops shipping it or doesn't meet the terms, the rights return to f3bandit |

## The GPL parts

These stay GPL-3.0-or-later, **copyright f3bandit**, because they are built into GPL works (plus the
additional permission for Retro Remake and Taki Udon above):

| part | why GPL |
|---|---|
| `firmware/`: the card firmware's Super MemCard additions | sd2psXtd is GPL-3.0 |
| `core/sys/`: the card driver in the cores' `sys_top` (public: f3bandit/super-memcard-cores) | the MiSTer cores are GPL |
| `main/smc_frontport.*`, the Main shim and `main/patch/` | Main_MiSTer is GPL-3.0 |

They hold no Super MemCard logic: the firmware stores files, the driver and shim pass bytes.

## Why `smcd` isn't touched by the GPL

`smcd` is a separate program. It talks to the GPL Main through a local socket (the shim passes card
frames), the same way Console Mode's own frontend talks to its Main through a command channel. It
isn't compiled or linked into any GPL program.
