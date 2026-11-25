"""Phosphor's in-game link: its hooks put into a Main_MiSTer tree (after the Super MemCard shim). Usage: hooks.py <tree>"""
import os, re, sys, shutil
d = sys.argv[1]
src = os.path.dirname(os.path.abspath(__file__))
for f in ('phosphor.cpp', 'phosphor.h', 'phosphor_disc.h', 'phosphor_link.cpp'):
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
# the save written as a game is left (the shim's smc_shim_leave_game) and a game's loading box (menu.cpp's
# ProgressMessage): Phosphor's own cards in their place, as in Phosphor's Main (phosphor_saving / phosphor_loading,
# phosphor.cpp); MiSTer's OSD boxes only where the frontend doesn't draw them (a CRT's OSD text, no frontend)
def leave(s):
    s = once(s, """	busy = 1;
	OsdSetTitle("Saving", 0);
	int rows = OsdGetSize();
	for (int i = 0; i < rows; i++) OsdWrite(i, i == rows / 2 - 1 ? "     Saving the game..." : "", 0, 0);
	OsdUpdate();
	OsdEnable(DISABLE_KEYBOARD);
	unsigned long hard = GetTimer(5000), quiet = GetTimer(500);
	while (!CheckTimer(hard))
	{
""", """	busy = 1;
	// Phosphor draws it in its own style (its loading card, "Saving"); MiSTer's OSD box where it can't
	int phosphor_saving(int on);
	int fe = phosphor_saving(1);
	unsigned long osd_at = fe ? 0 : GetTimer(0);
	unsigned long hard = GetTimer(5000), quiet = GetTimer(500);
	while (!CheckTimer(hard))
	{
		if (fe) phosphor_saving(1);       // (it shows after 350 ms: a quick save shows nothing)
		else if (osd_at)
		{
			osd_at = 0;
			OsdSetTitle("Saving", 0);
			int rows = OsdGetSize();
			for (int i = 0; i < rows; i++) OsdWrite(i, i == rows / 2 - 1 ? "     Saving the game..." : "", 0, 0);
			OsdUpdate();
			OsdEnable(DISABLE_KEYBOARD);
		}
""")
    s = once(s, """		usleep(1000);
	}
	OsdDisable();
	busy = 0;
}""", """		usleep(1000);
	}
	if (fe) phosphor_saving(0);
	else OsdDisable();
	busy = 0;
}""")
    return s
edit('smc_shim.cpp', leave)

def progress(s):
    return re.sub(r'(void ProgressMessage\(const char\* ?title, const char\* ?text, int current, int max\)\n\{\n\tstatic int progress;\n)',
                  lambda m: m.group(1) + '\tint phosphor_loading(const char *, const char *, int, int);\n'
                  '\tif (phosphor_loading(title, text, current, max)) { progress = -1; return; }   // Phosphor draws it (phosphor.cpp)\n',
                  s, count=1)
edit('menu.cpp', progress)

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

# Coreshell's handoff (ported from corehost.patch / Phosphor's Main): the settings Coreshell's MGL hands this launch,
# <status mask value> over the core's .CFG and <setting key value> over the INI, in memory only (neither file
# written). Coreshell sends them (SuperCard among them) only to a Main whose binary says "Coreshell handoff: ".
def ho_cfg(s):
    s = once(s, '\nvoid cfg_parse()\n{\n', '''
// Coreshell's handoff (ported from Phosphor's Main): INI settings for this launch, read after the INI (never written)
static char cfg_overrides[2048];

void cfg_set_overrides(const char *lines)
{
	snprintf(cfg_overrides, sizeof(cfg_overrides), "%s", lines ? lines : "");
}

static void cfg_apply_overrides()
{
	char line[512];
	for (const char *p = cfg_overrides; *p; )
	{
		const char *e = strchr(p, '\\n');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		if (n && n < sizeof(line))
		{
			memcpy(line, p, n);
			line[n] = 0;
			ini_parse_var(line);
		}
		p += n + (e ? 1 : 0);
	}
}

void cfg_parse()
{
''')
    return once(s, '\t\tini_parse(altcfg(), video_get_core_mode_name(0));\n\t}\n',
                '\t\tini_parse(altcfg(), video_get_core_mode_name(0));\n\t}\n\tcfg_apply_overrides();   // Coreshell\'s handoff\n')
def ho_cfgh(s):
    return once(s, '#endif // __CFG_H__', 'void cfg_set_overrides(const char *lines);   // Coreshell: "key=value\\n" lines read after the INI, this launch only\n\n#endif // __CFG_H__')
def ho_mra(s):
    nomra = 'mgl_from_mra' not in s
    s = once(s, '''		if (!strcasecmp(node->tag, "mistergamedescription")) inside_mgl = 1;
''', '''		if (!strcasecmp(node->tag, "mistergamedescription")) inside_mgl = 1;
		else if (inside_mgl && !mgl_from_mra && !strcasecmp(node->tag, "setting"))
		{
			// Coreshell: an INI setting for this launch (over the active INI, in memory)
			const char *k = NULL, *v = "";
			for (int i = 0; i < node->n_attributes; i++)
			{
				if (!strcasecmp(node->attributes[i].name, "key")) k = node->attributes[i].value;
				else if (!strcasecmp(node->attributes[i].name, "value")) v = node->attributes[i].value;
			}
			size_t len = strlen(mgl.cs_ini);
			if (k && *k && !strpbrk(k, "=\\n") && !strchr(v, '\\n') && len + strlen(k) + strlen(v) + 3 < sizeof(mgl.cs_ini))
			{
				snprintf(mgl.cs_ini + len, sizeof(mgl.cs_ini) - len, "%s=%s\\n", k, v);
				printf("Coreshell handoff: INI %s=%s\\n", k, v);
			}
		}
		else if (inside_mgl && !mgl_from_mra && !strcasecmp(node->tag, "status"))
		{
			// Coreshell: the core's settings for this launch: these status bits (hex, byte 0 first)
			const char *m = NULL, *v = NULL;
			for (int i = 0; i < node->n_attributes; i++)
			{
				if (!strcasecmp(node->attributes[i].name, "mask")) m = node->attributes[i].value;
				else if (!strcasecmp(node->attributes[i].name, "value")) v = node->attributes[i].value;
			}
			if (m && v && strlen(m) <= 32 && strlen(v) <= 32)
			{
				memset(mgl.cs_mask, 0, sizeof(mgl.cs_mask));
				memset(mgl.cs_value, 0, sizeof(mgl.cs_value));
				for (int i = 0; m[2 * i] && m[2 * i + 1]; i++) { char b[3] = { m[2 * i], m[2 * i + 1], 0 }; mgl.cs_mask[i] = (unsigned char)strtoul(b, NULL, 16); }
				for (int i = 0; v[2 * i] && v[2 * i + 1]; i++) { char b[3] = { v[2 * i], v[2 * i + 1], 0 }; mgl.cs_value[i] = (unsigned char)strtoul(b, NULL, 16); }
				mgl.cs_status = 1;
				printf("Coreshell handoff: status mask=%s value=%s\\n", m, v);
			}
		}
''')
    if nomra: s = s.replace('inside_mgl && !mgl_from_mra && !strcasecmp(node->tag, "setting")', 'inside_mgl && !strcasecmp(node->tag, "setting")').replace('inside_mgl && !mgl_from_mra && !strcasecmp(node->tag, "status")', 'inside_mgl && !strcasecmp(node->tag, "status")')
    return once(s, '\nmgl_struct* mgl_get()\n{\n\treturn &mgl;\n}\n', '''
mgl_struct* mgl_get()
{
	return &mgl;
}

// ---- Coreshell's handoff (ported from Phosphor's Main) ----
void mgl_apply_coreshell_status(unsigned char *status, int size)
{
	if (!mgl.cs_status) return;
	for (int i = 0; i < size && i < (int)sizeof(mgl.cs_mask); i++)
		status[i] = (unsigned char)((status[i] & ~mgl.cs_mask[i]) | (mgl.cs_value[i] & mgl.cs_mask[i]));
	printf("Coreshell handoff: the core's settings applied (its .CFG untouched)\\n");
}
''')
def ho_mrah(s):
    s = re.sub(r'(struct mgl_struct\n\{\n(?:[^}]*\n))\};\n', lambda m: m.group(1) + '''	char cs_ini[2048];          // Coreshell: <setting key="..." value="..."/>: "key=value\\n" lines, over the active INI
	unsigned char cs_mask[16];  // Coreshell: <status mask="hex" value="hex"/>: these status bits, over the core's .CFG
	unsigned char cs_value[16];
	int  cs_status;
};
''', s, count=1)
    return once(s, 'mgl_struct* mgl_get();\n', 'mgl_struct* mgl_get();\nvoid mgl_apply_coreshell_status(unsigned char *status, int size);   // Coreshell\'s handoff\n')
def ho_uio(s):
    s = once(s, '\tcfg_parse();\n\tcfg_print();\n', '''	// Coreshell: the INI settings its MGL hands this launch, over the user's INI (never written)
	if (xml && isXmlName(xml) == 2) cfg_set_overrides(mgl_parse(xml)->cs_ini);
	else cfg_set_overrides(NULL);
	cfg_parse();
	cfg_print();
''')
    # after the core's .CFG is read (CORE_TYPE_8BIT), before the status goes to the core
    return re.sub(r'(printf\("Loading config %s\\n", name\);.*?\n)(\t+)(user_io_status_set\("\[0\]", 1\);)',
                  lambda m: m.group(1) + m.group(2) + '// Coreshell: the core\'s settings it hands this launch, over its .CFG (never written)\n'
                  + m.group(2) + 'if (xml && isXmlName(xml) == 2) mgl_apply_coreshell_status((unsigned char *)cur_status, sizeof(cur_status));\n'
                  + m.group(2) + m.group(3), s, count=1, flags=re.S)
if 'Coreshell handoff' in open(os.path.join(d, 'support/arcade/mra_loader.cpp'), errors='surrogateescape').read():
    print('handoff already in', d)
else:
    edit('cfg.cpp', ho_cfg); edit('cfg.h', ho_cfgh)
    edit('support/arcade/mra_loader.cpp', ho_mra); edit('support/arcade/mra_loader.h', ho_mrah)
    edit('user_io.cpp', ho_uio)
    print('handoff in', d)
