# Patches

Applied on top of the pinned upstream cores at build time (`tools/cores.sh patch <core>`):

- `sys/*.patch`: changes to the `sys/` framework, applied to every core (paths relative to `sys/`);
- `<core>/*.patch`: changes to one core (paths relative to the core's root).

Phase 1 needs none (it changes Main only). Phase 2's card driver goes in `sys/`: `tools/sys-patch.sh` (run by
`cores.sh patch`) copies `core/sys/` into the core's `sys/` and adds it to `sys_top.v` at four lines every pinned
core's `sys_top.v` has (the cores carry 4 different versions of it, so a script rather than a `.patch`).
