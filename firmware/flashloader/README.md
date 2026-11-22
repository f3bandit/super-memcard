# The card's resident updater

`flashloader.c`, `flashloader.h`, `memmap_default.ld`, `memmap_defines.ld`, `memmap_flashloader.ld` and `uf2tool.py`
are [pico-flashloader](https://github.com/rhulme/pico-flashloader) by Richard Hulme (BSD-3-Clause, see LICENSE),
commit 79817c8, unchanged. `CMakeLists.txt` builds it beside the card firmware (firmware/patches/0008).

It sits in the first 4 KB of the card's flash and starts the card firmware after it. When the firmware update
(`src/ps1/ps1_supermemcard_update.c`, SMC_UPDATE over the front port) has staged a new, checked firmware at 2 MB and
restarted, it checks it again, writes it over the old one (the first page last, so a power cut leaves nothing
half-written that would start), and tries again at the next start if it didn't finish. If nothing can be started,
the card comes up in the RP2040's USB boot mode, where it can always be flashed from a PC or the SS1.

A card gets the updater once, over USB: `<variant>-full.uf2` (the updater and the firmware). After that, every
update can come through the front port.
