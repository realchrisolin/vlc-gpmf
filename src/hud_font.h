#ifndef HUD_FONT_H
#define HUD_FONT_H
/*
 * Segoe UI for the HUD. The date, labels, and units are Regular. The time
 * and the readings are Semibold. Digits are drawn in the width of "0" so a
 * changing 1 does not shift the string. Semibold's own figures are proportional.
 *
 * Copyright (C) 2026 Chris Olin
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
enum {
    HUD_FACE_LABEL = 0,
    HUD_FACE_VALUE = 1,
    HUD_FACE_TIME = 2,
    HUD_FACE_TAB = 3,
    HUD_FACE_DATE = 4,
    /* Map attribution. Independent of the HUD and the settings sheet. */
    HUD_FACE_CREDIT = 5
};

/* Caps are the desired capital heights in pixels. Returns 0 when Segoe UI is ready. */
int hud_font_prepare(int label_cap, int value_cap, int time_cap, int tab_cap,
                     int date_cap);
/* Capital height for the map credit only. Does not resize the HUD faces. */
int hud_font_set_credit(int cap);
int hud_font_ok(void);
int hud_font_cap(int face);
/* Distance from the capital top to the bottom of the em box, descent included. */
int hud_font_span(int face);
int hud_font_digit(int face);
int hud_font_width(int face, const char *text);
/* y is the capital top. Coverage is blended over the existing pixel. */
void hud_font_draw(unsigned char *pixels, int pitch, int pix, int bw, int bh,
                   int x, int y, int face, const char *text,
                   int r, int g, int b, int a);
#endif
