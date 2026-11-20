// Phosphor's in-game link in another Main (the Super MemCard / Coreshell shim): phosphor.cpp as in Phosphor's own
// Main (phosphor-main), with what only that Main has (the dock's disc drive; the controller mapping Phosphor runs in
// the menu core) answered here as "none" (phosphor_link.cpp). GPL-3.0.
// Copyright (C) 2026 f3bandit. GPL-3.0 (see LICENSE).
#ifndef PHOSPHOR_DISC_H
#define PHOSPHOR_DISC_H
#include <stddef.h>
#include <stdint.h>

void phosphor_disc_poll(void (*send_event)(const char *));     // from phosphor_poll
int phosphor_disc_command(const char *line, char *out, size_t n);  // 1: handled, reply in out
void phosphor_disc_status(char *out, size_t n, int event);
int phosphor_disc_busy(void);

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
