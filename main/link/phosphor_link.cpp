// Phosphor's in-game link in another Main (the Super MemCard / Coreshell shim). phosphor.cpp is Phosphor's own Main's
// (phosphor-main), unchanged: Phosphor's shell talks to whichever Main runs the game the same way (/tmp/phosphor-main.sock:
// the menu button, its in-game menu over the game, the core's options in its own screens). This file is what that
// needs from Main beyond its hooks, written once for any Main_MiSTer: the core's options as the OSD runs them, its
// DIP switches and settings, built only on Main's public functions. What only Phosphor's own Main does (the menu core's
// screen, the dock's disc drive, the controller mapping, scripts) never happens here: a game runs here, the menu runs on
// Phosphor's Main. Weak: a Main that has its own keeps it.
// Copyright (C) 2026 f3bandit. GPL-3.0 (see LICENSE).
#include "phosphor_disc.h"
#include "user_io.h"
#include "spi.h"
#include "menu.h"
#include "support.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

// ---- the menu core's, the dock's, the mapping's: none here
int phosphor_disc_command(const char *, char *, size_t) { return 0; }
void phosphor_disc_poll(void (*)(const char *)) {}
void phosphor_disc_status(char *out, size_t n, int) { if (n) snprintf(out, n, "{\"ok\":true,\"drive\":false}"); }
int phosphor_disc_busy(void) { return 0; }
int phosphor_disc_eject(void) { return -1; }
__attribute__((weak)) void menu_phosphor_screen(int) {}
__attribute__((weak)) int menu_select_ini_pending(void) { return 0; }   // (the boot video rescue: Phosphor's Main only)
__attribute__((weak)) void menu_select_ini_pick(int) {}
__attribute__((weak)) int menu_foreign_console(void) { return 0; }
__attribute__((weak)) void video_fb_console() {}
__attribute__((weak)) void video_fb_console_done() {}
__attribute__((weak)) int mgl_run_here(const char *) { return -1; }
__attribute__((weak)) int menu_mouse_map = 0;
__attribute__((weak)) void set_menu_mouse_map(int) {}
__attribute__((weak)) int step_back_map_setting() { return 0; }
__attribute__((weak)) int poll_map_hold_action() { return 0; }
__attribute__((weak)) int input_players(char *out, size_t n) { if (n) out[0] = 0; return 0; }
__attribute__((weak)) int phosphor_mapping = 0;
__attribute__((weak)) uint32_t phosphor_map_vidpid = 0;
__attribute__((weak)) int phosphor_map_cancel = 0;
__attribute__((weak)) int phosphor_map_filter(struct input_event *, int) { return 0; }
__attribute__((weak)) int phosphor_map_sel_start(int, int, int) { return 0; }
__attribute__((weak)) int input_map_running(void) { return 0; }
__attribute__((weak)) void input_map_jump(int) {}
__attribute__((weak)) uint32_t input_map_dev_vidpid(void) { return 0; }
__attribute__((weak)) const char *input_map_file(void) { return ""; }

// ---- the core's options (phosphor-main's menu.cpp, the same code)

// A config string T/R/t/r entry (a trigger, or a reset) as the OSD runs it. Returns -1: not a one-bit option; 1: the
// menu closes (a reset); 0: it stays. (The systems every Main_MiSTer has; newer ones' own resets are their Main's.)
__attribute__((weak)) int menu_status_action(const char *p)
{
	int bit;
	int ex = (p[0] == 't') || (p[0] == 'r');
	if (user_io_status_bits(p + 1, &bit, 0, ex) != 1) return -1;
	const char *opt = p + 1;
	if (is_megacd() && bit == 1) opt = "[0]";
	user_io_status_set(opt, 1, ex);
	user_io_status_set(opt, 0, ex);
	return (p[0] == 'R' || p[0] == 'r') ? 1 : 0;
}

// The config string line n with its H/h/D/d prefixes weighed as the OSD weighs them: 'h' hidden, 'd' disabled, '.' shown
__attribute__((weak)) char menu_conf_state(int n)
{
	const char *p = user_io_get_confstr(n);
	if (!p) return 'h';
	uint32_t hdmask = spi_uio_cmd16(UIO_GET_OSDMASK, 0);
	int h = 0, d = 0;
	while ((p[0] == 'H' || p[0] == 'D' || p[0] == 'h' || p[0] == 'd') && strlen(p) > 2)
	{
		int flg = (hdmask & (1 << user_io_hd_mask(p + 1))) ? 1 : 0;
		if (p[0] == 'H') h |= flg;
		if (p[0] == 'h') h |= (flg ^ 1);
		if (p[0] == 'D') d |= flg;
		if (p[0] == 'd') d |= (flg ^ 1);
		p += 2;
	}
	return h ? 'h' : d ? 'd' : '.';
}

static const char *core_reset_entry(int n)
{
	char *p = user_io_get_confstr(n);
	if (!p) return NULL;
	while ((p[0] == 'H' || p[0] == 'D' || p[0] == 'h' || p[0] == 'd') && strlen(p) > 2) p += 2;
	if (p[0] == 'P' && p[1] >= '0' && p[1] <= '9' && p[2] != ',') p += 2;
	return p;
}

// The core's own Reset entry, as the OSD runs it (0: the core has none)
__attribute__((weak)) int menu_core_reset()
{
	const char *p = NULL, *named = NULL;
	int bit;
	for (int i = 1; user_io_get_confstr(i); i++)
	{
		const char *e = core_reset_entry(i);
		if (!e[0] || (e[0] != 'R' && e[0] != 'T')) continue;
		if (user_io_status_bits(e + 1, &bit, 0, 0) == 1 && bit == 0) { p = e; break; }
		const char *label = strchr(e, ',');
		if (!named && label && !strncasecmp(label + 1, "Reset", 5) && (!label[6] || label[6] == ',')) named = e;
	}
	if (!p) p = named;
	return p && menu_status_action(p) >= 0;
}

__attribute__((weak)) int menu_settings_save(void)
{
	if (is_menu()) return -1;
	char *filename = user_io_create_config_name(1);
	user_io_status_save(filename);
	if (is_arcade()) arcade_nvm_save();
	return 0;
}

__attribute__((weak)) int menu_dip_count(void) { return is_menu() ? 0 : arcade_sw()->dip_num; }
__attribute__((weak)) int menu_dip_get(int i, const char **name, int *num, int *cur)
{
	sw_struct *sw = arcade_sw();
	if (is_menu() || i < 0 || i >= sw->dip_num) return 0;
	uint64_t status = sw->dip_cur & sw->dip[i].mask;
	int m = 0;
	for (int n = 0; n < sw->dip[i].num; n++) if (status == sw->dip[i].val[n]) { m = n; break; }
	*name = sw->dip[i].name; *num = sw->dip[i].num; *cur = m;
	return 1;
}
__attribute__((weak)) const char *menu_dip_choice(int i, int m) { return arcade_sw()->dip[i].id[m]; }
__attribute__((weak)) int menu_dip_set(int i, int m)
{
	sw_struct *sw = arcade_sw();
	if (is_menu() || i < 0 || i >= sw->dip_num || m < 0 || m >= sw->dip[i].num) return -1;
	sw->dip_cur = (sw->dip_cur & ~sw->dip[i].mask) | sw->dip[i].val[m];
	arcade_sw_send();
	arcade_sw_save();
	return 0;
}
__attribute__((weak)) int menu_dip_apply(void)
{
	if (is_menu() || !arcade_sw()->dip_num) return -1;
	arcade_sw_save();
	user_io_status_set("[0]", 1);
	user_io_status_set("[0]", 0);
	return 0;
}

__attribute__((weak)) int menu_settings_reset(void)
{
	if (is_menu()) return -1;
	user_io_status_reset();
	char *filename = user_io_create_config_name(1);
	user_io_status_save(filename);
	if (arcade_sw()->dip_num)
	{
		arcade_sw()->dip_cur = arcade_sw()->dip_def;
		arcade_sw_send();
		user_io_status_set("[0]", 1);
		user_io_status_set("[0]", 0);
		arcade_sw_save();
	}
	return 0;
}

// (the test bench's "osdtext": the OSD's rows as text, kept by phosphor-main's osd.cpp; not here)
__attribute__((weak)) char osd_text[32][40];
