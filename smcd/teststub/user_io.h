// Test stand-in for Main's user_io.h (shimtest only)
#pragma once
char *user_io_get_core_name(int orig = 0);
char is_menu();
char user_io_osd_is_visible();
void user_io_poll();
