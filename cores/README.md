# Console cores

The MiSTer-devel console cores Super MemCard builds against, as submodules pinned to an upstream commit.
Our changes are patches in [patches/](../patches/README.md), applied at build time, so taking a new upstream
version is moving a pin, never a merge inside a fork.

| core | systems |
|---|---|
| NES | NES, Famicom, Famicom Disk System |
| SNES | SNES, Super Famicom |
| N64 | Nintendo 64 |
| Gameboy | Game Boy, Game Boy Color |
| GBA | Game Boy Advance |
| SMS | Master System, Game Gear |
| Genesis | Genesis / Mega Drive (older core) |
| MegaDrive | Mega Drive / Genesis (newer core) |
| MegaCD | Sega CD / Mega CD |
| S32X | 32X |
| Saturn | Saturn |
| TurboGrafx16 | PC Engine, TurboGrafx-16, CD-ROM², SuperGrafx |
| Jaguar | Atari Jaguar |
| NeoGeo | Neo Geo MVS/AES, Neo Geo CD |
| PSX | PlayStation |

The list is [cores.tsv](cores.tsv) (core, Quartus project, systems). The cores belong to their authors and keep
their own licences; this repo only points at them.

## Working with them

```
git submodule update --init --depth 1 cores/PSX   # check one out (all of them is ~1 GB)
tools/cores.sh status                              # which cores upstream has moved past our pin
tools/cores.sh update PSX                          # move a pin to upstream
tools/cores.sh sys                                 # which cores share a sys/ version
tools/cores.sh patch PSX                           # apply our patches to cores/PSX
```

**Watching upstream**: every day [upstream.yml](../.github/workflows/upstream.yml) checks each core's upstream
branch. For a core that moved it pushes `upstream/<core>` with the new pin, opens a pull request with the
compare link, and starts that core's build with our patches. Merging the PR takes the new version; a failing
build means our patches need updating for it first.

**Building**: Actions → *Build cores* → a core name or `all`. The .rbf is kept as a build artifact.

## sys/ versions (Nov 2026)

Our main change goes into `sys/`, the framework every core is built with, so the patch has to apply to each
version in use. At the pinned commits the 15 cores have 7:

| sys/ | cores |
|---|---|
| A | GBA, Gameboy, Jaguar, MegaCD, MegaDrive, NeoGeo, S32X, TurboGrafx16 |
| B | NES, SMS |
| C | SNES |
| D | N64 |
| E | Saturn |
| F | Genesis |
| G | PSX |

(`tools/cores.sh sys` prints the current grouping.)
