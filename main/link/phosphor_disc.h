// Phosphor's in-game link in another Main (the Super MemCard / Coreshell shim): phosphor.cpp as in Phosphor's own
// Main (phosphor-main), with what only that Main has (the pair button's disc tray; the controller mapping Phosphor runs
// in the menu core) answered here as "none" (phosphor_link.cpp). GPL-3.0.
// Copyright (C) 2026 f3bandit. GPL-3.0 (see LICENSE).
#ifndef PHOSPHOR_DISC_H
#define PHOSPHOR_DISC_H
#include <stddef.h>
#include <stdint.h>

int phosphor_disc_eject(void);   // the pair button's "Open the disc tray": -1 here (no drive in this Main)
int menu_select_ini_pending(void);   // the boot video rescue: Phosphor's Main only (phosphor_link.cpp)
void menu_select_ini_pick(int n);

// the mapping's steps (phosphor-main's input.h): the menu core's, never run in a game
#ifndef SYS_MAP_POS_OSD
#define SYS_MAP_POS_OSD 16

// phosphor-main's menu.h, video.h and mra_loader.h additions (phosphor_link.cpp here; menu.cpp for the OSD text menu)
int phosphor_osd_menu(const char *title, const char *text, int sel);
int phosphor_osd_own(void);
void phosphor_osd_menu_off(void);
int phosphor_osd_menu_shown(void);
int menu_status_action(const char *p);
char menu_conf_state(int n);
int menu_core_reset();
int menu_settings_save(void);
int menu_settings_reset(void);
int menu_dip_count(void);
int menu_dip_get(int i, const char **name, int *num, int *cur);
const char *menu_dip_choice(int i, int m);
int menu_dip_set(int i, int m);
int menu_dip_apply(void);
void menu_phosphor_screen(int phosphor);
int menu_foreign_console(void);
extern int menu_mouse_map;
void video_fb_console();
void video_fb_console_done();
int mgl_run_here(const char *xml);
#endif
void set_menu_mouse_map(int enable);
int step_back_map_setting();
int poll_map_hold_action();
int input_players(char *out, size_t n);
extern int phosphor_mapping;
extern uint32_t phosphor_map_vidpid;
extern int phosphor_map_cancel;
struct input_event;
int phosphor_map_filter(struct input_event *ev, int dev);
int phosphor_map_sel_start(int code, int value, int reset);
#endif
