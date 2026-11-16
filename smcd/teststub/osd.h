// Test stand-in for Main's osd.h (shimtest only)
#pragma once
void OsdSetTitle(const char *s, int arrow = 0);
void OsdWrite(unsigned char n, const char *s = "", unsigned char inver = 0, unsigned char stipple = 0, char usebg = 0, int maxinv = 32, int mininv = 0);
int OsdGetSize();
void OsdUpdate();
void ScrollText(char n, const char *str, int off, int len, int max_len, unsigned char invert, int idx = 0);
void ScrollReset(int idx = 0);
#define DISABLE_KEYBOARD 0x02
void OsdEnable(unsigned char mode);
void OsdDisable();
