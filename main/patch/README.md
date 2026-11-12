# Super MemCard for a MiSTer Main: the whole change as one patch

`main-mister-super-memcard.patch`: everything a Main needs for Super MemCard, against MiSTer-devel/Main_MiSTer
(`8329c5a`, Nov 2026), for porting into another Main (Console Mode's `MiSTer_ConsoleMode`):

| file | change |
|---|---|
| `smc_shim.cpp/.h`, `smc_frontport.cpp/.h`, `smc_osd.cpp/.h` | new: copied from `main/` (GPL-3.0, as Main) |
| `fpga_io.cpp` | `fpga_load_rbf()`: `smc_shim_core_start(name, xml)` before a core that isn't the menu loads |
| `main.cpp` | after `user_io_init()`: `smc_shim_main_start(argv[1], argv[2])`; in the loop: `smc_shim_poll()` |
| `scheduler.cpp` | `smc_shim_poll()` beside `user_io_poll()` (Mains built with the scheduler) |
| `menu.cpp` | the OSD's " Saves" row and its two states (`MENU_SMC_SAVES1/2`); `smc_shim_rom_load()` where the file browser hands Main a game (`MENU_GENERIC_FILE_SELECTED`, and the CD systems' disc in `MENU_GENERIC_IMAGE_SELECTED`) |

```
cd Main_MiSTer && git apply main-mister-super-memcard.patch && make
```

A Main this differs from: apply what fits, then the rest by hand from the table (docs/10, section 3, has why each
call is where it is). With no `smcd` running, nothing changes: Main works as without it. The same hooks run on the
SS1 in the `test-main` branch's Main (tested Nov 12 2026: every launch path, docs/08).
