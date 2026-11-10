# Super MemCard in a MiSTer Main

The reference integration for a Main (Console Mode's `MiSTer_ConsoleMode`, or any fork of Main_MiSTer), built and
tested on an SS1 in a fork of Main_MiSTer. Copy these with `host/` into the Main's source:

| file | what |
|---|---|
| `smc_frontport.cpp/.h` | the front ports through the driver in the loaded core (`core/sys/smc_port.sv`): raw frames, holding the ports |
| `supermemcard.cpp/.h` | the three calls: `supermemcard_core_start(rbf, mgl)` before a game core loads, `supermemcard_menu_start()` back in the menu, `supermemcard_poll()` from Main's loop (phase 2) |

Where they're called in Main:
- `supermemcard_core_start(name, xml)`: in `fpga_load_rbf()`, before a core that isn't the menu is loaded.
- `supermemcard_menu_start()`: in `main()`, after `user_io_init()`, when Main starts with the menu core.
- `supermemcard_poll()`: in Main's loop.

docs/10-console-mode-guide.md has the whole picture.
