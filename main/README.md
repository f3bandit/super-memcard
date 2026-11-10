# Super MemCard in a MiSTer Main

What a Main (Console Mode's `MiSTer_ConsoleMode`, or any fork of Main_MiSTer) adds for Super MemCard: the shim. All of
Super MemCard is `smcd`, a program of its own; Main passes its card frames to the front ports and tells it when a game
starts and when the menu is back. Copy into the Main's source (GPL-3.0, as Main):

| file | what |
|---|---|
| `smc_shim.cpp/.h` | the three calls, and the socket `smcd` sends its frames on |
| `smc_frontport.cpp/.h` | the front ports through the driver in the loaded core (`core/sys/smc_port.sv`): raw frames, holding the ports |

Where they're called in Main:
- `smc_shim_core_start(name, xml)`: in `fpga_load_rbf()`, before a core that isn't the menu is loaded.
- `smc_shim_menu_start()`: in `main()`, after `user_io_init()`, when Main starts with the menu core.
- `smc_shim_poll()`: in Main's loop.

With no `smcd` running, nothing happens and Main works as without Super MemCard. docs/10-console-mode-guide.md has the
whole picture.

`supermemcard.cpp/.h` is the earlier version, with Super MemCard's logic inside Main; it is replaced by `smcd` and the
shim, and kept only for reference.
