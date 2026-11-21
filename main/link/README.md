# Phosphor's in-game link in another Main

What lets a frontend (Phosphor, through Coreshell) run a game on a Main that isn't its own (odelot's MiSTer_RA for the
RetroAchievements cores, for one) and still be the only menu the user sees, with nothing in that Main changed beyond
these hooks. GPL-3.0, like Main. Applied after the shim (`../patch`), by Coreshell's shim builds
(`python3 -m coreshell mains build ra --link main/link`):

    python3 hooks.py <Main_MiSTer tree>      # copies the files below in and hooks them up, then make as usual

| file | what |
|---|---|
| `phosphor.cpp/.h` | Phosphor's own Main's local API (phosphor-main's, the same file): `/tmp/phosphor-main.sock`, the menu button, the in-game menu over the game |
| `phosphor_link.cpp`, `phosphor_disc.h` | what that API needs from a Main that isn't Phosphor's: the core's options, DIP switches and settings through Main's public functions; the rest answered as "none" |
| `hooks.py` | the hooks: `phosphor_poll()` in the loops, the keys and the file picked in `menu.cpp`, the pad in `input.cpp`, the cheats' API, and Phosphor's own cards for a game's loading and saving (`ProgressMessage`, the shim's `smc_shim_leave_game`) |

Nothing here touches RetroAchievements: no hook in MiSTer_RA's `achievements.cpp`, rcheevos or its pop-ups. MiSTer_RA
is the client and does all of it, as on any MiSTer; Phosphor shows the user's progress from the RA Viewer add-on.
