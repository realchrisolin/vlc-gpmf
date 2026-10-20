/*
 * Dark map inset and the travel arrow. Tiles are painted by the caller
 * between the panel and the marker.
 * Copyright (C) 2026 Chris Olin
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "hud_map.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int box_side(const hud_map_box *box);

static void put_px(unsigned char *p, int pitch, int pix, int w, int h,
                   int x, int y, int r, int g, int b, int a)
{
    unsigned char *px;
    if (x < 0 || y < 0 || x >= w || y >= h || pix < 4)
        return;
    px = p + (size_t)y * (size_t)pitch + (size_t)x * (size_t)pix;
    px[0] = (unsigned char)r;
    px[1] = (unsigned char)g;
    px[2] = (unsigned char)b;
    px[3] = (unsigned char)a;
}

static void fill_round(unsigned char *p, int pitch, int pix, int bw, int bh,
                       int x, int y, int w, int h, int rad,
                       int r, int g, int b, int a)
{
    int iy, ix;
    if (w <= 0 || h <= 0)
        return;
    if (rad < 1)
        rad = 1;
    if (rad * 2 > w)
        rad = w / 2;
    if (rad * 2 > h)
        rad = h / 2;
    for (iy = 0; iy < h; iy++) {
        for (ix = 0; ix < w; ix++) {
            int dx = 0, dy = 0;
            if (ix < rad)
                dx = rad - ix;
            else if (ix >= w - rad)
                dx = ix - (w - 1 - rad);
            if (iy < rad)
                dy = rad - iy;
            else if (iy >= h - rad)
                dy = iy - (h - 1 - rad);
            if (dx * dx + dy * dy <= rad * rad)
                put_px(p, pitch, pix, bw, bh, x + ix, y + iy, r, g, b, a);
        }
    }
}

static void line(unsigned char *p, int pitch, int pix, int bw, int bh,
                 int x0, int y0, int x1, int y1, int thick,
                 int r, int g, int b, int a)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    int i, o;
    if (steps < 1)
        steps = 1;
    if (thick < 1)
        thick = 1;
    for (i = 0; i <= steps; i++) {
        int x = x0 + dx * i / steps;
        int y = y0 + dy * i / steps;
        for (o = -thick / 2; o <= thick / 2; o++)
            put_px(p, pitch, pix, bw, bh, x + o, y, r, g, b, a);
    }
}

static void fill_tri(unsigned char *p, int pitch, int pix, int bw, int bh,
                     float x0, float y0, float x1, float y1, float x2, float y2,
                     int r, int g, int b, int a)
{
    float den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    int minx, maxx, miny, maxy, x, y;
    if (den > -0.001f && den < 0.001f)
        return;
    minx = (int)floorf(x0);
    maxx = minx;
    miny = (int)floorf(y0);
    maxy = miny;
    if ((int)floorf(x1) < minx) minx = (int)floorf(x1);
    if ((int)floorf(x1) > maxx) maxx = (int)floorf(x1);
    if ((int)floorf(x2) < minx) minx = (int)floorf(x2);
    if ((int)floorf(x2) > maxx) maxx = (int)floorf(x2);
    if ((int)floorf(y1) < miny) miny = (int)floorf(y1);
    if ((int)floorf(y1) > maxy) maxy = (int)floorf(y1);
    if ((int)floorf(y2) < miny) miny = (int)floorf(y2);
    if ((int)floorf(y2) > maxy) maxy = (int)floorf(y2);
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= bw) maxx = bw - 1;
    if (maxy >= bh) maxy = bh - 1;
    for (y = miny; y <= maxy; y++) {
        for (x = minx; x <= maxx; x++) {
            float px = (float)x + 0.5f;
            float py = (float)y + 0.5f;
            float w0 = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / den;
            float w1 = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / den;
            float w2 = 1.f - w0 - w1;
            if (w0 >= 0.f && w1 >= 0.f && w2 >= 0.f)
                put_px(p, pitch, pix, bw, bh, x, y, r, g, b, a);
        }
    }
}

static void paint_arrow(unsigned char *p, int pitch, int pix, int w, int h,
                        float len, double course_deg, int r, int g, int b, int a)
{
    float rad = (float)(course_deg * 3.141592653589793 / 180.0);
    float fx = sinf(rad);
    float fy = -cosf(rad);
    float rx = -fy;
    float ry = fx;
    float cx = (float)w * 0.5f;
    float cy = (float)h * 0.5f;
    float x0 = cx + fx * len;
    float y0 = cy + fy * len;
    float x1 = cx - fx * len * 0.5f + rx * len * 0.55f;
    float y1 = cy - fy * len * 0.5f + ry * len * 0.55f;
    float x2 = cx - fx * len * 0.5f - rx * len * 0.55f;
    float y2 = cy - fy * len * 0.5f - ry * len * 0.55f;
    fill_tri(p, pitch, pix, w, h, x0, y0, x1, y1, x2, y2, r, g, b, a);
}

static const unsigned char glyph_m[7] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
static const unsigned char glyph_a[7] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
static const unsigned char glyph_p[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};

static void blit_glyph(unsigned char *p, int pitch, int pix, int bw, int bh,
                       int x, int y, int scale, const unsigned char *gl,
                       int r, int g, int b, int a)
{
    int row, col;
    for (row = 0; row < 7; row++) {
        for (col = 0; col < 5; col++) {
            if (gl[row] & (0x10 >> col)) {
                int py, px;
                for (py = 0; py < scale; py++)
                    for (px = 0; px < scale; px++)
                        put_px(p, pitch, pix, bw, bh,
                               x + col * scale + px, y + row * scale + py,
                               r, g, b, a);
            }
        }
    }
}

static void paint_map_word(unsigned char *p, int pitch, int pix, int w, int h)
{
    int scale = h / 14;
    int adv, total, x, y;
    if (scale < 2)
        scale = 2;
    if (scale > 8)
        scale = 8;
    adv = 6 * scale;
    total = adv * 3 - scale;
    x = (w - total) / 2;
    y = (h - 7 * scale) / 2;
    blit_glyph(p, pitch, pix, w, h, x, y, scale, glyph_m, 255, 196, 64, 255);
    blit_glyph(p, pitch, pix, w, h, x + adv, y, scale, glyph_a, 255, 196, 64, 255);
    blit_glyph(p, pitch, pix, w, h, x + adv * 2, y, scale, glyph_p, 255, 196, 64, 255);
}

static void paint_hide(unsigned char *p, int pitch, int pix, const hud_map_box *box)
{
    int cx = box->hide_x + box->hide_w / 2;
    int cy = box->hide_y + box->hide_h / 2;
    int rad = box->hide_w / 2 - 4;
    int arm, thick, y, x;
    if (rad < 8)
        rad = 8;
    thick = rad / 6;
    if (thick < 2)
        thick = 2;
    arm = rad * 2 / 3;
    for (y = -rad; y <= rad; y++) {
        for (x = -rad; x <= rad; x++) {
            if (x * x + y * y <= rad * rad)
                put_px(p, pitch, pix, box->w, box->h, cx + x, cy + y, 255, 255, 255, 36);
        }
    }
    line(p, pitch, pix, box->w, box->h, cx - arm, cy - arm, cx + arm, cy + arm,
         thick, 236, 238, 242, 255);
    line(p, pitch, pix, box->w, box->h, cx + arm, cy - arm, cx - arm, cy + arm,
         thick, 236, 238, 242, 255);
}

void hud_map_mercator(double lat, double lon, int zoom, double *x, double *y)
{
    double lat_r, n, s;
    if (zoom < 0)
        zoom = 0;
    if (zoom > 22)
        zoom = 22;
    n = ldexp(1.0, zoom);
    lat_r = lat * 3.141592653589793 / 180.0;
    if (lat_r > 1.484) /* about 85 degrees */
        lat_r = 1.484;
    if (lat_r < -1.484)
        lat_r = -1.484;
    s = sin(lat_r);
    *x = (lon + 180.0) / 360.0 * n;
    *y = (1.0 - log((1.0 + s) / (1.0 - s)) / (2.0 * 3.141592653589793)) * 0.5 * n;
}

double hud_map_bearing(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = lat1 * 3.141592653589793 / 180.0;
    double p2 = lat2 * 3.141592653589793 / 180.0;
    double dl = (lon2 - lon1) * 3.141592653589793 / 180.0;
    double y = sin(dl) * cos(p2);
    double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
    double deg = atan2(y, x) * 180.0 / 3.141592653589793;
    if (deg < 0)
        deg += 360.0;
    return deg;
}

double hud_map_distance_m(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = lat1 * 3.141592653589793 / 180.0;
    double p2 = lat2 * 3.141592653589793 / 180.0;
    double dp = p2 - p1;
    double dl = (lon2 - lon1) * 3.141592653589793 / 180.0;
    double a = sin(dp / 2) * sin(dp / 2) +
               cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    double c = 2 * atan2(sqrt(a), sqrt(1 - a));
    return 6371000.0 * c;
}

int hud_map_arrow_len(int px)
{
    if (px < 24)
        px = 24;
    if (px > 240)
        px = 240;
    return px;
}

void hud_map_ease(double *lat, double *lon, double *course,
                  double raw_lat, double raw_lon, double raw_course,
                  int have_course, double dt, double tau)
{
    double alpha, turn;
    if (!lat || !lon || !course)
        return;
    if (tau <= 0.0 || dt <= 0.0) {
        *lat = raw_lat;
        *lon = raw_lon;
        if (have_course)
            *course = raw_course;
        return;
    }
    alpha = 1.0 - exp(-dt / tau);
    if (alpha < 0.0)
        alpha = 0.0;
    if (alpha > 1.0)
        alpha = 1.0;
    *lat += alpha * (raw_lat - *lat);
    *lon += alpha * (raw_lon - *lon);
    if (!have_course)
        return;
    turn = raw_course - *course;
    while (turn > 180.0)
        turn -= 360.0;
    while (turn < -180.0)
        turn += 360.0;
    *course += alpha * turn;
    if (*course < 0.0)
        *course += 360.0;
    if (*course >= 360.0)
        *course -= 360.0;
}

static void clamp_wh(int video_w, int video_h, int *w, int *h)
{
    int minw = 240;
    int minh = 240;
    int maxw, maxh;
    if (video_w < 32)
        video_w = 32;
    if (video_h < 32)
        video_h = 32;
    maxw = video_w - 36;
    maxh = video_h - 36;
    if (maxw < 32)
        maxw = video_w;
    if (maxh < 32)
        maxh = video_h;
    if (minw > maxw)
        minw = maxw;
    if (minh > maxh)
        minh = maxh;
    if (*w < minw)
        *w = minw;
    if (*h < minh)
        *h = minh;
    if (*w > maxw)
        *w = maxw;
    if (*h > maxh)
        *h = maxh;
}

static void layout_controls(hud_map_box *box, int video_w, int video_h, int expanded)
{
    int side = box_side(box);
    int shortv = video_w < video_h ? video_w : video_h;
    int grip, btn, gap, pad, stack, y0, y1, reserve;
    if (shortv <= 0)
        shortv = video_h > 0 ? video_h : 1080;
    grip = shortv / 80;
    if (grip < 36)
        grip = 36;
    if (side > 20 && grip > side / 5)
        grip = side / 5;
    if (grip < 8)
        grip = 8;
    box->grip = grip;
    if (!expanded)
        return;
    btn = side / 8;
    if (btn < 40)
        btn = 40;
    if (btn > 72)
        btn = 72;
    if (btn > side / 4)
        btn = side / 4;
    if (btn < 16)
        btn = 16;
    gap = btn / 5;
    if (gap < 6)
        gap = 6;
    pad = 4;
    reserve = side / 5;
    if (reserve < 36)
        reserve = 36;
    box->zin_w = box->zin_h = btn;
    box->zout_w = box->zout_h = btn;
    box->zin_x = grip + pad;
    box->zout_x = box->zin_x;
    stack = btn * 2 + gap;
    y0 = grip + pad;
    y1 = box->h - reserve;
    if (y1 < y0 + stack)
        y1 = y0 + stack;
    box->zin_y = y0 + (y1 - y0 - stack) / 2;
    if (box->zin_y < y0)
        box->zin_y = y0;
    box->zout_y = box->zin_y + btn + gap;
    box->hide_w = btn;
    box->hide_h = btn;
    box->hide_x = box->w - grip - pad - btn;
    box->hide_y = grip + pad;
    if (box->hide_x < 0)
        box->hide_x = 0;
    if (box->hide_y < 0)
        box->hide_y = 0;
}

void hud_map_box_of(int video_w, int video_h, int expanded,
                    int user_w, int user_h, hud_map_box *box)
{
    int size;
    memset(box, 0, sizeof *box);
    if (video_h <= 0)
        video_h = 1080;
    if (video_w <= 0)
        video_w = 1920;
    size = video_h / 4;
    if (size < 240)
        size = 240;
    if (size > 720)
        size = 720;
    if (!expanded) {
        box->w = size / 3;
        if (box->w < 140)
            box->w = 140;
        box->h = box->w / 3;
        if (box->h < 52)
            box->h = 52;
        box->hide_w = box->w;
        box->hide_h = box->h;
        return;
    }
    if (user_w > 0 && user_h > 0) {
        box->w = user_w;
        box->h = user_h;
        clamp_wh(video_w, video_h, &box->w, &box->h);
    } else {
        box->w = size;
        box->h = size;
    }
    layout_controls(box, video_w, video_h, 1);
}

static void layout_dock(hud_map_box *box, int video_w, int video_h)
{
    int shortv = video_w < video_h ? video_w : video_h;
    int btn, gap, section, stack, gaps, avail, top, x;
    int lift = video_h / 8;
    if (shortv <= 0)
        shortv = video_h > 0 ? video_h : 1080;
    if (lift < 140)
        lift = 140;
    box->lift = lift;
    btn = 72;
    if (shortv / 80 + 36 > btn)
        btn = shortv / 80;
    if (btn < 36)
        btn = 36;
    if (btn > 72)
        btn = 72;
    gap = btn / 5;
    if (gap < 6)
        gap = 6;
    section = btn / 2;
    if (section < gap)
        section = gap;
    gaps = gap * 2 + section * 2;
    avail = video_h - lift - 36;
    if (avail < 5 * 16 + gaps)
        avail = 5 * 16 + gaps;
    if (5 * btn + gaps > avail)
        btn = (avail - gaps) / 5;
    if (btn < 16)
        btn = 16;
    gap = btn / 5;
    if (gap < 4)
        gap = 4;
    section = btn / 2;
    if (section < gap)
        section = gap;
    stack = btn * 5 + gap * 2 + section * 2;
    top = video_h - lift - stack;
    if (top < 36)
        top = 36;
    x = video_w - 36 - btn;
    if (x < 8)
        x = 8;
    box->zin_w = box->zin_h = btn;
    box->zout_w = box->zout_h = btn;
    box->oup_w = box->oup_h = btn;
    box->odn_w = box->odn_h = btn;
    box->hide_w = box->hide_h = btn;
    box->zin_x = box->zout_x = box->oup_x = box->odn_x = box->hide_x = x;
    box->zin_y = top;
    box->zout_y = box->zin_y + btn + gap;
    box->oup_y = box->zout_y + btn + section;
    box->odn_y = box->oup_y + btn + gap;
    box->hide_y = box->odn_y + btn + section;
}

void hud_map_immersive_box(int video_w, int video_h, int expanded, hud_map_box *box)
{
    int size;
    memset(box, 0, sizeof *box);
    if (video_h <= 0)
        video_h = 1080;
    if (video_w <= 0)
        video_w = 1920;
    if (!expanded) {
        size = video_h / 4;
        if (size < 240)
            size = 240;
        if (size > 720)
            size = 720;
        box->w = size / 3;
        if (box->w < 140)
            box->w = 140;
        box->h = box->w / 3;
        if (box->h < 52)
            box->h = 52;
        box->hide_w = box->w;
        box->hide_h = box->h;
        box->lift = video_h / 8;
        if (box->lift < 140)
            box->lift = 140;
        return;
    }
    box->full = 1;
    box->w = video_w;
    box->h = video_h;
    layout_dock(box, video_w, video_h);
}

int hud_map_opacity_step(int opacity, int dir)
{
    if (dir > 0)
        opacity += 10;
    else if (dir < 0)
        opacity -= 10;
    if (opacity < 10)
        opacity = 10;
    if (opacity > 100)
        opacity = 100;
    return opacity;
}

int hud_map_aim(const hud_map_box *box, int expanded, int ox, int oy,
                int sx, int sy, int sw, int sh,
                int hx, int hy, int hw, int hh,
                int x, int y, int *from_strip)
{
    if (from_strip)
        *from_strip = 0;
    if (sw > 0 && sh > 0 && x >= sx && y >= sy && x < sx + sw && y < sy + sh) {
        if (from_strip)
            *from_strip = 1;
        if (hw > 0 && hh > 0 && x >= hx && y >= hy && x < hx + hw && y < hy + hh)
            return HUD_MAP_HIT_HIDE;
        return HUD_MAP_HIT_MOVE;
    }
    if (!box)
        return HUD_MAP_HIT_NONE;
    return hud_map_hit(box, expanded, x - ox, y - oy);
}

static int in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
    return rw > 0 && rh > 0 && x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

int hud_map_hit(const hud_map_box *box, int expanded, int x, int y)
{
    int g;
    if (!box || x < 0 || y < 0 || x >= box->w || y >= box->h)
        return HUD_MAP_HIT_NONE;
    if (!expanded)
        return HUD_MAP_HIT_HIDE;
    if (box->full) {
        /* The gaps between the five buttons are part of the nearer button.
         * A click that lands on the padding still hits that control. */
        struct {
            int id, x, y, w, h;
        } b[5];
        int i, gap, pad, side, best, best_d;
        b[0].id = HUD_MAP_HIT_ZOOM_IN;
        b[0].x = box->zin_x;
        b[0].y = box->zin_y;
        b[0].w = box->zin_w;
        b[0].h = box->zin_h;
        b[1].id = HUD_MAP_HIT_ZOOM_OUT;
        b[1].x = box->zout_x;
        b[1].y = box->zout_y;
        b[1].w = box->zout_w;
        b[1].h = box->zout_h;
        b[2].id = HUD_MAP_HIT_OPACITY_UP;
        b[2].x = box->oup_x;
        b[2].y = box->oup_y;
        b[2].w = box->oup_w;
        b[2].h = box->oup_h;
        b[3].id = HUD_MAP_HIT_OPACITY_DOWN;
        b[3].x = box->odn_x;
        b[3].y = box->odn_y;
        b[3].w = box->odn_w;
        b[3].h = box->odn_h;
        b[4].id = HUD_MAP_HIT_HIDE;
        b[4].x = box->hide_x;
        b[4].y = box->hide_y;
        b[4].w = box->hide_w;
        b[4].h = box->hide_h;
        gap = 0;
        side = 0;
        for (i = 0; i < 5; i++) {
            int j;
            if (b[i].w > side)
                side = b[i].w;
            for (j = i + 1; j < 5; j++) {
                int g = 0;
                if (b[j].y >= b[i].y + b[i].h)
                    g = b[j].y - (b[i].y + b[i].h);
                else if (b[i].y >= b[j].y + b[j].h)
                    g = b[i].y - (b[j].y + b[j].h);
                if (g > gap)
                    gap = g;
            }
        }
        pad = gap / 2;
        if (pad < 3)
            pad = 3;
        if (side > 0 && pad > side / 2)
            pad = side / 2;
        best = HUD_MAP_HIT_NONE;
        best_d = 0;
        for (i = 0; i < 5; i++) {
            int cx, cy, dx, dy, d;
            if (b[i].w <= 0 || b[i].h <= 0)
                continue;
            if (!in_rect(x, y, b[i].x - pad, b[i].y - pad,
                         b[i].w + pad * 2, b[i].h + pad * 2))
                continue;
            cx = b[i].x + b[i].w / 2;
            cy = b[i].y + b[i].h / 2;
            dx = x - cx;
            dy = y - cy;
            d = dx * dx + dy * dy;
            if (best == HUD_MAP_HIT_NONE || d < best_d) {
                best = b[i].id;
                best_d = d;
            }
        }
        return best;
    }
    g = box->grip > 0 ? box->grip : 1;
    if (x < g && y < g)
        return HUD_MAP_HIT_NW;
    if (x >= box->w - g && y < g)
        return HUD_MAP_HIT_NE;
    if (x < g && y >= box->h - g)
        return HUD_MAP_HIT_SW;
    if (x >= box->w - g && y >= box->h - g)
        return HUD_MAP_HIT_SE;
    if (y < g)
        return HUD_MAP_HIT_N;
    if (y >= box->h - g)
        return HUD_MAP_HIT_S;
    if (x < g)
        return HUD_MAP_HIT_W;
    if (x >= box->w - g)
        return HUD_MAP_HIT_E;
    if (in_rect(x, y, box->hide_x, box->hide_y, box->hide_w, box->hide_h))
        return HUD_MAP_HIT_HIDE;
    if (in_rect(x, y, box->zin_x, box->zin_y, box->zin_w, box->zin_h))
        return HUD_MAP_HIT_ZOOM_IN;
    if (in_rect(x, y, box->zout_x, box->zout_y, box->zout_w, box->zout_h))
        return HUD_MAP_HIT_ZOOM_OUT;
    return HUD_MAP_HIT_MOVE;
}

void hud_map_resize(int hit, int l0, int t0, int r0, int b0,
                    int pointer_x, int pointer_y, int video_w, int video_h,
                    int *x, int *y, int *w, int *h)
{
    int move_l = 0, move_t = 0, move_r = 0, move_b = 0;
    int l = l0, t = t0, r = r0, b = b0;
    int minw = 240, minh = 240, maxw, maxh;
    if (!x || !y || !w || !h)
        return;
    if (video_w < 32)
        video_w = 32;
    if (video_h < 32)
        video_h = 32;
    switch (hit) {
    case HUD_MAP_HIT_SE: move_r = 1; move_b = 1; break;
    case HUD_MAP_HIT_SW: move_l = 1; move_b = 1; break;
    case HUD_MAP_HIT_NE: move_r = 1; move_t = 1; break;
    case HUD_MAP_HIT_NW: move_l = 1; move_t = 1; break;
    case HUD_MAP_HIT_E: move_r = 1; break;
    case HUD_MAP_HIT_W: move_l = 1; break;
    case HUD_MAP_HIT_S: move_b = 1; break;
    case HUD_MAP_HIT_N: move_t = 1; break;
    default:
        *x = l0;
        *y = t0;
        *w = r0 - l0;
        *h = b0 - t0;
        return;
    }
    if (move_l)
        l = pointer_x;
    if (move_t)
        t = pointer_y;
    if (move_r)
        r = pointer_x;
    if (move_b)
        b = pointer_y;
    if (move_l && l < 0)
        l = 0;
    if (move_t && t < 0)
        t = 0;
    if (move_r && r > video_w)
        r = video_w;
    if (move_b && b > video_h)
        b = video_h;
    maxw = video_w - 36;
    maxh = video_h - 36;
    if (maxw < 32)
        maxw = video_w;
    if (maxh < 32)
        maxh = video_h;
    if (minw > maxw)
        minw = maxw;
    if (minh > maxh)
        minh = maxh;
    if (r - l < minw) {
        if (move_l && !move_r)
            l = r - minw;
        else
            r = l + minw;
    }
    if (b - t < minh) {
        if (move_t && !move_b)
            t = b - minh;
        else
            b = t + minh;
    }
    if (r - l > maxw) {
        if (move_l && !move_r)
            l = r - maxw;
        else
            r = l + maxw;
    }
    if (b - t > maxh) {
        if (move_t && !move_b)
            t = b - maxh;
        else
            b = t + maxh;
    }
    *x = l;
    *y = t;
    *w = r - l;
    *h = b - t;
}

static void fill_rect(unsigned char *p, int pitch, int pix, int bw, int bh,
                      int x, int y, int w, int h, int r, int g, int b, int a)
{
    int iy, ix;
    for (iy = 0; iy < h; iy++)
        for (ix = 0; ix < w; ix++)
            put_px(p, pitch, pix, bw, bh, x + ix, y + iy, r, g, b, a);
}

static void paint_disc(unsigned char *p, int pitch, int pix, const hud_map_box *box,
                       int x, int y, int w, int h, int alpha)
{
    int cx = x + w / 2;
    int cy = y + h / 2;
    int rad = w / 2 - 2;
    int iy, ix;
    if (rad < 6)
        rad = 6;
    for (iy = -rad; iy <= rad; iy++)
        for (ix = -rad; ix <= rad; ix++)
            if (ix * ix + iy * iy <= rad * rad)
                put_px(p, pitch, pix, box->w, box->h, cx + ix, cy + iy,
                       255, 255, 255, alpha);
}

static void paint_plus(unsigned char *p, int pitch, int pix, const hud_map_box *box,
                       int x, int y, int w, int h, int alpha, int minus)
{
    int cx = x + w / 2;
    int cy = y + h / 2;
    int arm = w / 5;
    int thick = w / 14;
    if (arm < 6)
        arm = 6;
    if (thick < 2)
        thick = 2;
    fill_rect(p, pitch, pix, box->w, box->h, cx - arm, cy - thick / 2,
              arm * 2, thick, 236, 238, 242, alpha);
    if (!minus)
        fill_rect(p, pitch, pix, box->w, box->h, cx - thick / 2, cy - arm,
                  thick, arm * 2, 236, 238, 242, alpha);
}

static void paint_bracket(unsigned char *p, int pitch, int pix, const hud_map_box *box,
                          int x, int y, int sx, int sy, int len, int thick, int alpha)
{
    int i, k;
    for (i = 0; i < len; i++) {
        for (k = 0; k < thick; k++) {
            put_px(p, pitch, pix, box->w, box->h, x + sx * i, y + sy * k,
                   255, 255, 255, alpha);
            put_px(p, pitch, pix, box->w, box->h, x + sx * k, y + sy * i,
                   255, 255, 255, alpha);
        }
    }
}

void hud_map_controls(unsigned char *pixels, int pitch, int pix,
                      const hud_map_box *box, int expanded, int hot,
                      int zoom, int zoom_down)
{
    int len, thick, alpha, plus_a, minus_a;
    int side;
    if (!pixels || !box || !expanded || box->w < 8 || box->h < 8)
        return;
    side = box_side(box);
    len = box->grip * 2 / 3;
    if (len < 14)
        len = 14;
    if (len > 28)
        len = 28;
    if (len > side / 6)
        len = side / 6;
    thick = 3;
    alpha = hot ? 230 : 96;
    paint_bracket(pixels, pitch, pix, box, 6, 6, 1, 1, len, thick, alpha);
    paint_bracket(pixels, pitch, pix, box, box->w - 7, 6, -1, 1, len, thick, alpha);
    paint_bracket(pixels, pitch, pix, box, 6, box->h - 7, 1, -1, len, thick, alpha);
    paint_bracket(pixels, pitch, pix, box, box->w - 7, box->h - 7, -1, -1, len, thick, alpha);
    plus_a = zoom >= 18 ? 90 : 255;
    minus_a = zoom <= 2 ? 90 : 255;
    paint_disc(pixels, pitch, pix, box, box->zin_x, box->zin_y, box->zin_w, box->zin_h,
               zoom_down == 1 && zoom < 18 ? 70 : 36);
    paint_disc(pixels, pitch, pix, box, box->zout_x, box->zout_y, box->zout_w, box->zout_h,
               zoom_down == 2 && zoom > 2 ? 70 : 36);
    paint_plus(pixels, pitch, pix, box, box->zin_x, box->zin_y, box->zin_w, box->zin_h, plus_a, 0);
    paint_plus(pixels, pitch, pix, box, box->zout_x, box->zout_y, box->zout_w, box->zout_h, minus_a, 1);
}

static void paint_core(unsigned char *p, int pitch, int pix, const hud_map_box *box,
                       int x, int y, int w, int h, int alpha, int ring)
{
    int cx = x + w / 2;
    int cy = y + h / 2;
    int rad = w / 5;
    int thick, iy, ix;
    if (rad < 5)
        rad = 5;
    thick = rad / 3;
    if (thick < 2)
        thick = 2;
    for (iy = -rad; iy <= rad; iy++) {
        for (ix = -rad; ix <= rad; ix++) {
            int d = ix * ix + iy * iy;
            int hole = (rad - thick) * (rad - thick);
            if (d > rad * rad)
                continue;
            if (ring && d < hole)
                continue;
            put_px(p, pitch, pix, box->w, box->h, cx + ix, cy + iy, 236, 238, 242, alpha);
        }
    }
}

void hud_map_dock(unsigned char *pixels, int pitch, int pix,
                  const hud_map_box *box, int zoom, int zoom_down,
                  int opacity, int opacity_down)
{
    int plus_a, minus_a, up_a, down_a;
    if (!pixels || !box || !box->full)
        return;
    plus_a = zoom >= 18 ? 90 : 255;
    minus_a = zoom <= 2 ? 90 : 255;
    up_a = opacity >= 100 ? 90 : 255;
    down_a = opacity <= 10 ? 90 : 255;
    paint_disc(pixels, pitch, pix, box, box->zin_x, box->zin_y, box->zin_w, box->zin_h,
               zoom_down == 1 && zoom < 18 ? 70 : 36);
    paint_disc(pixels, pitch, pix, box, box->zout_x, box->zout_y, box->zout_w, box->zout_h,
               zoom_down == 2 && zoom > 2 ? 70 : 36);
    paint_disc(pixels, pitch, pix, box, box->oup_x, box->oup_y, box->oup_w, box->oup_h,
               opacity_down == 1 && opacity < 100 ? 70 : 36);
    paint_disc(pixels, pitch, pix, box, box->odn_x, box->odn_y, box->odn_w, box->odn_h,
               opacity_down == 2 && opacity > 10 ? 70 : 36);
    paint_plus(pixels, pitch, pix, box, box->zin_x, box->zin_y, box->zin_w, box->zin_h, plus_a, 0);
    paint_plus(pixels, pitch, pix, box, box->zout_x, box->zout_y, box->zout_w, box->zout_h, minus_a, 1);
    paint_core(pixels, pitch, pix, box, box->oup_x, box->oup_y, box->oup_w, box->oup_h, up_a, 0);
    paint_core(pixels, pitch, pix, box, box->odn_x, box->odn_y, box->odn_w, box->odn_h, down_a, 1);
    paint_hide(pixels, pitch, pix, box);
}

static void paint_cross(unsigned char *p, int pitch, int pix, int bw, int bh,
                        int x, int y, int w, int h)
{
    int cx = x + w / 2;
    int cy = y + h / 2;
    int rad = w / 2 - 4;
    int arm, thick;
    if (rad < 8)
        rad = 8;
    thick = rad / 6;
    if (thick < 2)
        thick = 2;
    arm = rad * 2 / 3;
    line(p, pitch, pix, bw, bh, cx - arm, cy - arm, cx + arm, cy + arm,
         thick, 236, 238, 242, 255);
    line(p, pitch, pix, bw, bh, cx + arm, cy - arm, cx - arm, cy + arm,
         thick, 236, 238, 242, 255);
}

void hud_map_dock_face(unsigned char *pixels, int pitch, int pix,
                       int w, int h, int hit, int pressed,
                       int zoom, int opacity)
{
    hud_map_box box;
    int disc, glyph;
    if (!pixels || w < 8 || h < 8)
        return;
    memset(&box, 0, sizeof box);
    box.full = 1;
    box.w = w;
    box.h = h;
    box.zin_w = box.zout_w = box.oup_w = box.odn_w = box.hide_w = w;
    box.zin_h = box.zout_h = box.oup_h = box.odn_h = box.hide_h = h;
    disc = pressed ? 168 : 108;
    glyph = 255;
    switch (hit) {
    case HUD_MAP_HIT_ZOOM_IN:
        if (zoom >= 18)
            glyph = 90;
        paint_disc(pixels, pitch, pix, &box, 0, 0, w, h, disc);
        paint_plus(pixels, pitch, pix, &box, 0, 0, w, h, glyph, 0);
        break;
    case HUD_MAP_HIT_ZOOM_OUT:
        if (zoom <= 2)
            glyph = 90;
        paint_disc(pixels, pitch, pix, &box, 0, 0, w, h, disc);
        paint_plus(pixels, pitch, pix, &box, 0, 0, w, h, glyph, 1);
        break;
    case HUD_MAP_HIT_OPACITY_UP:
        if (opacity >= 100)
            glyph = 90;
        paint_disc(pixels, pitch, pix, &box, 0, 0, w, h, disc);
        paint_core(pixels, pitch, pix, &box, 0, 0, w, h, glyph, 0);
        break;
    case HUD_MAP_HIT_OPACITY_DOWN:
        if (opacity <= 10)
            glyph = 90;
        paint_disc(pixels, pitch, pix, &box, 0, 0, w, h, disc);
        paint_core(pixels, pitch, pix, &box, 0, 0, w, h, glyph, 1);
        break;
    case HUD_MAP_HIT_HIDE:
        paint_disc(pixels, pitch, pix, &box, 0, 0, w, h, disc);
        paint_cross(pixels, pitch, pix, w, h, 0, 0, w, h);
        break;
    default:
        break;
    }
}

void hud_map_pill(unsigned char *pixels, int pitch, int pix,
                  const hud_map_box *box, int x, int y, int w, int h)
{
    int rad;
    if (!pixels || !box || w < 4 || h < 4)
        return;
    rad = h / 2;
    if (rad < 4)
        rad = 4;
    fill_round(pixels, pitch, pix, box->w, box->h, x, y, w, h, rad, 9, 11, 15, 210);
}

static int box_side(const hud_map_box *box)
{
    return box->w < box->h ? box->w : box->h;
}

static void inner_geom(const hud_map_box *box, int *ox, int *oy, int *w, int *h, int *rad)
{
    int edge = 2;
    int outer = box_side(box) / 16;
    if (outer < 12)
        outer = 12;
    *ox = edge;
    *oy = edge;
    *w = box->w - edge * 2;
    *h = box->h - edge * 2;
    *rad = outer - edge;
    if (*rad < 1)
        *rad = 1;
    if (*w < 1)
        *w = 1;
    if (*h < 1)
        *h = 1;
    if (*rad * 2 > *w)
        *rad = *w / 2;
    if (*rad * 2 > *h)
        *rad = *h / 2;
}

static int in_round(int lx, int ly, int w, int h, int rad)
{
    int dx = 0;
    int dy = 0;
    if (lx < 0 || ly < 0 || lx >= w || ly >= h)
        return 0;
    if (lx < rad)
        dx = rad - lx;
    else if (lx >= w - rad)
        dx = lx - (w - 1 - rad);
    if (ly < rad)
        dy = rad - ly;
    else if (ly >= h - rad)
        dy = ly - (h - 1 - rad);
    return dx * dx + dy * dy <= rad * rad;
}

void hud_map_panel(unsigned char *pixels, int pitch, int pix,
                   const hud_map_box *box, int expanded)
{
    int edge = 2;
    int rad;
    if (!pixels || !box || box->w < 8 || box->h < 8)
        return;
    rad = box_side(box) / 16;
    if (rad < 12)
        rad = 12;
    fill_round(pixels, pitch, pix, box->w, box->h, 0, 0, box->w, box->h, rad,
               255, 255, 255, 48);
    fill_round(pixels, pitch, pix, box->w, box->h, edge, edge,
               box->w - edge * 2, box->h - edge * 2, rad - edge,
               9, 11, 15, 210);
    if (!expanded)
        paint_map_word(pixels, pitch, pix, box->w, box->h);
}

void hud_map_marker(unsigned char *pixels, int pitch, int pix,
                    const hud_map_box *box, int expanded, int arrow_px,
                    double course_deg, int show_arrow)
{
    float whole, marker, rim;
    if (!pixels || !box || !expanded || box->w < 8 || box->h < 8)
        return;
    /* paint_arrow's length is the tip. The tail sits half that far behind.
     * The caller passes the drawn length. The preference is 24..240 picture
     * pixels; a display-sized map may pass a shorter on-screen length. */
    if (show_arrow) {
        if (arrow_px < 8)
            arrow_px = 8;
        if (arrow_px > 240)
            arrow_px = 240;
        whole = (float)arrow_px;
        marker = whole / 1.5f;
        rim = marker / 7.5f;
        if (rim < 2.f)
            rim = 2.f;
        paint_arrow(pixels, pitch, pix, box->w, box->h, marker + rim,
                    course_deg, 6, 8, 12, 230);
        paint_arrow(pixels, pitch, pix, box->w, box->h, marker,
                    course_deg, 220, 36, 42, 255);
    }
    if (!box->full)
        paint_hide(pixels, pitch, pix, box);
}

void hud_map_draw(unsigned char *pixels, int pitch, int pix,
                  const hud_map_box *box, int expanded, int arrow_px,
                  double course_deg)
{
    hud_map_panel(pixels, pitch, pix, box, expanded);
    hud_map_marker(pixels, pitch, pix, box, expanded, arrow_px, course_deg, 1);
}

int hud_map_content(const hud_map_box *box, int x, int y)
{
    int ox, oy, w, h, rad;
    if (!box)
        return 0;
    if (box->full)
        return x >= 0 && y >= 0 && x < box->w && y < box->h;
    inner_geom(box, &ox, &oy, &w, &h, &rad);
    return in_round(x - ox, y - oy, w, h, rad);
}

void hud_map_shade(unsigned char *pixels, int pitch, int pix,
                   const hud_map_box *box, int band)
{
    int ox, oy, w, h, rad, x, y;
    int y0;
    if (!pixels || !box || band < 1 || pix < 4)
        return;
    inner_geom(box, &ox, &oy, &w, &h, &rad);
    y0 = oy + h - band;
    if (y0 < oy)
        y0 = oy;
    for (y = y0; y < oy + h; y++) {
        for (x = ox; x < ox + w; x++) {
            unsigned char *px;
            int ia;
            if (!in_round(x - ox, y - oy, w, h, rad))
                continue;
            if (x < 0 || y < 0 || x >= box->w || y >= box->h)
                continue;
            px = pixels + (size_t)y * (size_t)pitch + (size_t)x * (size_t)pix;
            ia = 255 - 168;
            px[0] = (unsigned char)((px[0] * ia + 6 * 168) / 255);
            px[1] = (unsigned char)((px[1] * ia + 8 * 168) / 255);
            px[2] = (unsigned char)((px[2] * ia + 12 * 168) / 255);
        }
    }
}
