# Patches

Applied on top of the pinned upstream cores at build time (`tools/cores.sh patch <core>`):

- `sys/*.patch`: changes to the `sys/` framework, applied to every core (paths relative to `sys/`);
- `<core>/*.patch`: changes to one core (paths relative to the core's root).

Phase 1 needs none (it changes Main only). Phase 2's card driver goes in `sys/` (05 design).
