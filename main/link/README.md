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
| `phosphor_link_ra.cpp` | MiSTer_RA only (when the tree has `achievements.cpp`): its client's results, read-only, as `ra` / `ra list` and `/tmp/raengine.events`; its pop-ups drawn as Phosphor's cards while Phosphor is the frontend (same queue, timing, sounds and corner as its own) |
| `phosphor_osd_card.cpp`, `phosphor_osd_font.h` | the cards: a 1-bit bitmap in the OSD, Pixel Operator (CC0, `fonts/`) and icons; `tools/gen_osd_card.py` makes the header |
| `hooks.py` | the hooks: `phosphor_poll()` in the loops, the keys and the file picked in `menu.cpp`, the pad in `input.cpp`, the cheats' API, and for MiSTer_RA its events, login, game load and pop-up drawing step |

Nothing here talks to RetroAchievements or changes what MiSTer_RA sends: MiSTer_RA is the client and does all of it.
