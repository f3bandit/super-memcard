"""Phosphor's in-game link: its hooks put into a Main_MiSTer tree (after the Super MemCard shim). Usage: hooks.py <tree>"""
import os, re, sys, shutil
d = sys.argv[1]
src = os.path.dirname(os.path.abspath(__file__))
for f in ('phosphor.cpp', 'phosphor.h', 'phosphor_version.h', 'phosphor_disc.h', 'phosphor_link.cpp'):
    shutil.copy(os.path.join(src, f), os.path.join(d, f))

def edit(f, fn):
    p = os.path.join(d, f)
    s = open(p, errors='surrogateescape').read()
    t = fn(s)
    if t == s: raise SystemExit('%s: no change' % f)
    open(p, 'w', errors='surrogateescape').write(t)

def once(s, a, b):
    assert s.count(a) >= 1, a
    return s.replace(a, b, 1)

# the UI loop (main.cpp, and scheduler.cpp's coroutine): phosphor_poll() beside the shim's
for f in ('main.cpp', 'scheduler.cpp'):
    def add_poll(s):
        s = re.sub(r'(?m)^#include "smc_shim.h".*$', lambda m: m.group(0) + '\n#include "phosphor.h"   // Phosphor\'s in-game link', s, count=1)
        return re.sub(r'(?m)^(\s*)(smc_shim_poll\(\);[^\n]*)$',
                      lambda m: m.group(1) + m.group(2) + '\n' + m.group(1) + 'phosphor_poll();   // Phosphor: its local API (phosphor.cpp), the in-game link', s, 1)
    edit(f, add_poll)

# menu.cpp: the keys through phosphor_filter_key; the game picked in MiSTer's file browser; the OSD text menu (CRT)
def menu(s):
    s = once(s, '#include "menu.h"\n', '#include "menu.h"\n#include "phosphor.h"   // Phosphor\'s in-game link\n')
    s = once(s, '\t\tc = menu_key_get();\n',
             '\t\tc = phosphor_filter_key(menu_key_get());   // Phosphor: the menu button to its in-game menu\n')
    s = re.sub(r'(?m)^(\s*)((?:if \(!?)?smc_shim_rom_load\()', lambda m: m.group(1) + 'phosphor_file_loaded(selPath);   // Phosphor: the game, for its in-game menu\n' + m.group(1) + m.group(2), s)
    s = s.rstrip('\n') + '''

/* Phosphor: its in-game menu as text in MiSTer's OSD, for a CRT (the analog port shows the core and the OSD,
   never Linux's framebuffer). Shown like an info message: the game keeps running and the keys stay with Phosphor;
   lines split on '\\n', the row sel inverted. (phosphor-main's menu.cpp, the same code) */
static int ph_osd;
int phosphor_osd_own(void) { return ph_osd && menustate == MENU_INFO; }
int phosphor_osd_menu(const char *title, const char *text, int sel)
{
	if (menustate > MENU_INFO) return -1;
	int lines = 1;
	for (const char *c = text; *c; c++) if (*c == '\\n') lines++;
	int want = lines <= 8 ? 8 : 16;
	OsdSetTitle(title, 0);
	if (!phosphor_osd_own() || want != OsdGetSize())
	{
		OsdSetSize(want);
		OsdEnable(OSD_MSG);
	}
	ph_osd = 1;
	char s[40];
	int l = 0;
	const char *p = text;
	while (l < OsdGetSize())
	{
		int n = 0;
		while (*p && *p != '\\n' && n < 30) s[n++] = *p++;
		while (*p && *p != '\\n') p++;
		s[n] = 0;
		OsdWrite(l, s, l == sel, 0);
		l++;
		if (!*p) break;
		p++;
	}
	while (l < OsdGetSize()) OsdWrite(l++, "", 0, 0);
	menu_timer = GetTimer(30 * 60 * 1000);
	menustate = MENU_INFO;
	OsdUpdate();
	return 0;
}
int phosphor_osd_menu_shown(void) { return phosphor_osd_own(); }
void phosphor_osd_menu_off(void)
{
	if (phosphor_osd_own()) MenuHide();
	ph_osd = 0;
}
'''
    return s
edit('menu.cpp', menu)

# input.cpp: the controllers to Phosphor while its screen is over the game (or its text menu is in the OSD)
def inp(s):
    s = once(s, '#include "input.h"\n', '#include "input.h"\n#include "phosphor.h"   // Phosphor\'s in-game link\n#include "phosphor_disc.h"\n')
    return once(s, '\t\telse if (video_fb_state())\n',
                '\t\telse if ((video_fb_state() || phosphor_ingame_text()) && phosphor_pad(mask, press))\n'
                '\t\t{\n\t\t\t// Phosphor\'s shell has the screen: the button went to it, mapped the way MiSTer has it\n\t\t}\n'
                '\t\telse if (video_fb_state())\n')
edit('input.cpp', inp)
print('hooks in', d)

# the game a file browser pick starts (before the shim's own, at the same two places)
def loaded(s):
    s = once(s, '\t\t\tif (selPath[0] && mgl->done && !store_name && !smc_shim_rom_load(selPath, \'f\', ioctl_index)) break;\n',
             '\t\t\tif (selPath[0] && mgl->done) phosphor_file_loaded(selPath);   // Phosphor: the game, for its in-game menu\n'
             '\t\t\tif (selPath[0] && mgl->done && !store_name && !smc_shim_rom_load(selPath, \'f\', ioctl_index)) break;\n')
    return once(s, '\t\t\t// Super MemCard: a CD game picked in the OSD',
                '\t\t\tif (selPath[0] && mgl->done) phosphor_file_loaded(selPath);   // Phosphor: the game, for its in-game menu\n'
                '\t\t\t// Super MemCard: a CD game picked in the OSD')
edit('menu.cpp', loaded)
print('file hooks in', d)

# cheats.cpp: a core's cheats for Phosphor's own screen (phosphor-main's, the same code: cheats[] is cheats.cpp's)
def cheats(s):
    return s.rstrip('\n') + '''

// Phosphor's frontend API ("cheats", "cheat N"): each cheat's name and state, toggled as the OSD does
int cheats_item(int i, char *name, int n, int *on)
{
	if (i < 0 || i >= cheats_available()) return 0;
	snprintf(name, n, "%s", cheats[i].name);
	int l = strlen(name);
	if (l > 3 && !strncasecmp(name + l - 3, ".gg", 3)) name[l - 3] = 0;
	*on = cheats[i].enabled ? 1 : 0;
	return 1;
}
int cheats_toggle_at(int i)
{
	if (i < 0 || i >= cheats_available()) return -1;
	iSelectedEntry = i;
	cheats_toggle();
	return cheats[i].enabled ? 1 : 0;
}
'''
edit('cheats.cpp', cheats)
print('cheats in', d)

# MiSTer_RA (odelot's): its client's results to the frontend (phosphor_link_ra.cpp), read-only
if os.path.exists(os.path.join(d, 'achievements.cpp')):
    for f in ('phosphor_link_ra.cpp', 'phosphor_osd_card.cpp', 'phosphor_osd_font.h'):
        shutil.copy(os.path.join(src, f), os.path.join(d, f))
    def ra(s):
        s = once(s, '#include "rc_client.h"\n', '#include "rc_client.h"\n'
                 '// Phosphor\'s in-game link (phosphor_link_ra.cpp): this client\'s results shown by the frontend, read-only\n'
                 'void phosphor_ra_event(const rc_client_event_t *e, rc_client_t *c);\n'
                 'void phosphor_ra_login(int result, const char *err, rc_client_t *c);\n'
                 'void phosphor_ra_game(int result, const char *err, rc_client_t *c);\n'
                 'void phosphor_ra_draw(const char *text, int ms, int align, int frame);\n')
        s = once(s, 'static rc_client_t *g_client = NULL;\n', 'static rc_client_t *g_client = NULL;\n'
                 'rc_client_t *achievements_rc_client(void) { return g_client; }   // Phosphor\'s in-game link: "ra", "ra list"\n')
        s = re.sub(r'(?m)^(static int g_popup_pos\b[^\n]*\n)', lambda m: m.group(1) + 'int achievements_popup_pos(void);   // Phosphor\'s in-game link: its pop-ups in the same corner\n', s, count=1)
        s = s.rstrip('\n') + '\n\nint achievements_popup_pos(void) { return g_popup_pos; }   // Phosphor\'s in-game link\n'
        s = once(s, '\t(void)client;\n\tRA_LOG("Event: type=%d", event->type);\n',
                 '\t(void)client;\n\tphosphor_ra_event(event, client);   // Phosphor\'s in-game link: the frontend\'s pop-ups\n'
                 '\tRA_LOG("Event: type=%d", event->type);\n')
        s = once(s, '\tg_login_pending = 0;\n', '\tg_login_pending = 0;\n\tphosphor_ra_login(result, error_message, client);   // Phosphor\'s in-game link\n')
        s = once(s, '\tg_game_load_pending = 0;\n', '\tg_game_load_pending = 0;\n\tphosphor_ra_game(result, error_message, client);   // Phosphor\'s in-game link\n')
        # its pop-ups' drawing step: Phosphor's card (phosphor_ra_draw); its queue, timings, sounds and corner unchanged
        head, body = s.split('int achievements_popup_pos(void);', 1)
        n = len(re.findall(r'\bInfoAligned\(', body))
        assert n >= 2, n
        s = head + 'int achievements_popup_pos(void);' + re.sub(r'\bInfoAligned\(', 'phosphor_ra_draw(', body)
        return s
    edit('achievements.cpp', ra)
    def ra_menu(s):   # menu.cpp's one RetroAchievements pop-up (cheats off in hardcore), at Info's place (top left)
        if 'void phosphor_ra_draw(' not in s:
            s = once(s, '#include "phosphor.h"   // Phosphor\'s in-game link\n',
                     '#include "phosphor.h"   // Phosphor\'s in-game link\nvoid phosphor_ra_draw(const char *text, int ms, int align, int frame);\n')
        s = once(s, '\t\t\t\tInfo("Hardcore Mode: cheats disabled", 3000);\n',
                 '\t\t\t\tphosphor_ra_draw("Hardcore Mode: cheats disabled", 3000, 0, 1);   // Phosphor\'s card (phosphor_link_ra.cpp)\n')
        return s.rstrip('\n') + '''

// Phosphor's cards over a game (phosphor_osd_card.cpp): a bitmap in the OSD, shown the way InfoAligned shows its box
void OSD_PrintBitmap(const uint8_t *cols, int w, int hrows);   // osd.cpp (the link's)
void PhosphorInfoBitmap(const uint8_t *cols, int wch, int hrows, int timeout, int align)
{
	if (menustate <= MENU_INFO)
	{
		OSD_PrintBitmap(cols, wch * 8, hrows);
		int x = INFO_MARGIN;
		if (align == INFO_ALIGN_CENTER) x = (INFO_SPAN - (wch * 8)) / 2;
		else if (align == INFO_ALIGN_RIGHT) x = INFO_SPAN - (wch * 8) - INFO_MARGIN;
		if (x < 0) x = 0;
		InfoEnable(x, (cfg.direct_video && get_vga_fb()) ? 30 : 10, wch, hrows);
		OsdSetSize(16);
		menu_timer = GetTimer(timeout);
		menustate = MENU_INFO;
		OsdUpdate();
	}
}
'''
    edit('menu.cpp', ra_menu)
    def osd(s):
        return s.rstrip('\n') + '''

// Phosphor's cards (phosphor_osd_card.cpp): <hrows> OSD rows of a 1-bit bitmap, a byte per column (bit 0 at the top)
void OSD_PrintBitmap(const uint8_t *cols, int w, int hrows)
{
	for (int y = 0; y < hrows; y++)
	{
		osd_start(y);
		memcpy(osdbuf + osdbufpos, cols + y * w, w);
	}
}
'''
    edit('osd.cpp', osd)
    print('RetroAchievements link in', d)
