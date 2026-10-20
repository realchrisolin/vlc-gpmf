/*
 * Rasterize Segoe UI into the HUD panel.
 * Linux and macOS have no Segoe UI here, so prepare fails and the HUD
 * draws its bitmap glyphs instead.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#if !defined(_WIN32)
#include "hud_font.h"

int hud_font_prepare(int label_cap, int value_cap, int time_cap, int tab_cap,
                     int date_cap)
{
    (void)label_cap;
    (void)value_cap;
    (void)time_cap;
    (void)tab_cap;
    (void)date_cap;
    return -1;
}

int hud_font_set_credit(int cap)
{
    (void)cap;
    return -1;
}

int hud_font_ok(void)
{
    return 0;
}

int hud_font_cap(int face)
{
    (void)face;
    return 0;
}

int hud_font_span(int face)
{
    (void)face;
    return 0;
}

int hud_font_digit(int face)
{
    (void)face;
    return 0;
}

int hud_font_width(int face, const char *text)
{
    (void)face;
    (void)text;
    return 0;
}

void hud_font_draw(unsigned char *pixels, int pitch, int pix, int bw, int bh,
                   int x, int y, int face, const char *text,
                   int r, int g, int b, int a)
{
    (void)pixels;
    (void)pitch;
    (void)pix;
    (void)bw;
    (void)bh;
    (void)x;
    (void)y;
    (void)face;
    (void)text;
    (void)r;
    (void)g;
    (void)b;
    (void)a;
}
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hud_font.h"

#define FACE_N 5

typedef struct {
    HFONT font;
    int cap;
    int lead;
    int cell;
    int digit;
    int extra;
    int tabular;
    HFONT big;
    int big_cell;
} face_t;

/* Draw the outline this many times larger, then average it down to the
 * HUD pixel. A one-pixel fringe still leaves steps on the curves. */
#define DRAW_SCALE 3

static face_t faces[FACE_N];
static face_t credit_face;
static int credit_cap_set;
static int ready;
static int caps_set[FACE_N];
static HDC measure_dc;
static HBITMAP measure_bm;
static HDC draw_dc;
static HBITMAP draw_bm;
static HBITMAP draw_dummy;
static void *draw_bits;
static int draw_w, draw_h;

/* Room for antialiasing that hangs off the advance. */
#define GLYPH_PAD 2

static int imax(int a, int b)
{
    return a > b ? a : b;
}

static HFONT make_font(int em, int semibold)
{
    LOGFONTW lf;
    memset(&lf, 0, sizeof lf);
    lf.lfHeight = em > 0 ? -em : -1;
    lf.lfWeight = semibold ? FW_SEMIBOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lf.lfQuality = ANTIALIASED_QUALITY;
    lf.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
    lstrcpyW(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

static int face_is_segoe(HFONT font)
{
    HFONT old;
    WCHAR name[64];
    int n;
    old = (HFONT)SelectObject(measure_dc, font);
    n = GetTextFaceW(measure_dc, 64, name);
    SelectObject(measure_dc, old);
    return n > 0 && wcsncmp(name, L"Segoe UI", 8) == 0;
}

static void read_metrics(face_t *f)
{
    HFONT old;
    TEXTMETRICW tm;
    SIZE sz;
    old = (HFONT)SelectObject(measure_dc, f->font);
    GetTextMetricsW(measure_dc, &tm);
    SetTextCharacterExtra(measure_dc, 0);
    GetTextExtentPoint32W(measure_dc, L"0", 1, &sz);
    SelectObject(measure_dc, old);
    f->lead = (int)tm.tmInternalLeading;
    f->cell = (int)tm.tmHeight;
    f->cap = (int)tm.tmAscent - f->lead;
    if (f->cap < 1)
        f->cap = 1;
    f->digit = sz.cx > 0 ? (int)sz.cx : 1;
}

static int em_for_cap(int want, int semibold)
{
    HFONT probe;
    face_t tmp;
    int em;
    probe = make_font(100, semibold);
    if (!probe)
        return want > 0 ? want : 1;
    tmp.font = probe;
    read_metrics(&tmp);
    DeleteObject(probe);
    if (tmp.cap < 1)
        return want;
    em = want * 100 / tmp.cap;
    if (em < 8)
        em = 8;
    return em;
}

static void drop_one(face_t *f)
{
    if (!f)
        return;
    if (f->font) {
        DeleteObject(f->font);
        f->font = NULL;
    }
    if (f->big) {
        DeleteObject(f->big);
        f->big = NULL;
    }
    f->big_cell = 0;
}

static int ensure_dc(void)
{
    HDC screen;
    if (measure_dc && draw_dc && measure_bm && draw_dummy)
        return 0;
    screen = GetDC(NULL);
    if (!measure_dc)
        measure_dc = CreateCompatibleDC(screen);
    if (!measure_bm)
        measure_bm = CreateCompatibleBitmap(screen, 8, 8);
    if (!draw_dc)
        draw_dc = CreateCompatibleDC(screen);
    if (!draw_dummy)
        draw_dummy = CreateCompatibleBitmap(screen, 1, 1);
    if (screen)
        ReleaseDC(NULL, screen);
    if (!measure_dc || !draw_dc || !measure_bm || !draw_dummy)
        return -1;
    SelectObject(measure_dc, measure_bm);
    SelectObject(draw_dc, draw_dummy);
    SetBkMode(draw_dc, TRANSPARENT);
    SetBkColor(draw_dc, RGB(0, 0, 0));
    SetTextColor(draw_dc, RGB(255, 255, 255));
    return 0;
}

static int font_cell(HFONT font);

static int make_face(face_t *f, int cap, int semibold, int extra)
{
    int em;
    drop_one(f);
    memset(f, 0, sizeof *f);
    em = em_for_cap(cap, semibold);
    f->font = make_font(em, semibold);
    f->tabular = 0;
    f->extra = extra;
    if (!f->font || !face_is_segoe(f->font)) {
        drop_one(f);
        return -1;
    }
    f->big = make_font(em * DRAW_SCALE, semibold);
    if (!f->big || !face_is_segoe(f->big)) {
        drop_one(f);
        return -1;
    }
    f->big_cell = font_cell(f->big);
    read_metrics(f);
    return 0;
}

static face_t *face_of(int face)
{
    if (face == HUD_FACE_CREDIT)
        return credit_face.font ? &credit_face : NULL;
    if (face < 0 || face >= FACE_N || !faces[face].font)
        return NULL;
    return &faces[face];
}

static void drop_faces(void)
{
    int i;
    for (i = 0; i < FACE_N; i++) {
        drop_one(&faces[i]);
        memset(&faces[i], 0, sizeof faces[i]);
    }
    ready = 0;
}

static int font_cell(HFONT font)
{
    HFONT old;
    TEXTMETRICW tm;
    old = (HFONT)SelectObject(measure_dc, font);
    GetTextMetricsW(measure_dc, &tm);
    SelectObject(measure_dc, old);
    return (int)tm.tmHeight;
}

int hud_font_prepare(int label_cap, int value_cap, int time_cap, int tab_cap,
                     int date_cap)
{
    int want[FACE_N];
    int semi[FACE_N];
    int i;
    want[HUD_FACE_LABEL] = label_cap;
    want[HUD_FACE_VALUE] = value_cap;
    want[HUD_FACE_TIME] = time_cap;
    want[HUD_FACE_TAB] = tab_cap;
    want[HUD_FACE_DATE] = date_cap;
    semi[HUD_FACE_LABEL] = 0;
    semi[HUD_FACE_VALUE] = 1;
    semi[HUD_FACE_TIME] = 1;
    semi[HUD_FACE_TAB] = 1;
    semi[HUD_FACE_DATE] = 0;
    if (ready) {
        int same = 1;
        for (i = 0; i < FACE_N; i++)
            if (caps_set[i] != want[i])
                same = 0;
        if (same)
            return 0;
    }
    if (ensure_dc() != 0) {
        ready = 0;
        return -1;
    }
    drop_faces();
    for (i = 0; i < FACE_N; i++) {
        int em = em_for_cap(want[i], semi[i]);
        faces[i].font = make_font(em, semi[i]);
        faces[i].tabular = (i == HUD_FACE_VALUE || i == HUD_FACE_TIME);
        faces[i].extra = (i == HUD_FACE_LABEL) ? imax(1, want[i] / 14) : 0;
        if (!faces[i].font || !face_is_segoe(faces[i].font)) {
            drop_faces();
            return -1;
        }
        faces[i].big = make_font(em * DRAW_SCALE, semi[i]);
        if (!faces[i].big || !face_is_segoe(faces[i].big)) {
            drop_faces();
            return -1;
        }
        faces[i].big_cell = font_cell(faces[i].big);
        read_metrics(&faces[i]);
        caps_set[i] = want[i];
    }
    ready = 1;
    return 0;
}

int hud_font_ok(void)
{
    return ready;
}

int hud_font_set_credit(int cap)
{
    if (cap < 6)
        cap = 6;
    if (cap > 64)
        cap = 64;
    if (credit_face.font && credit_cap_set == cap)
        return 0;
    if (ensure_dc() != 0)
        return -1;
    if (make_face(&credit_face, cap, 0, 0) != 0) {
        credit_cap_set = 0;
        return -1;
    }
    credit_cap_set = cap;
    return 0;
}

int hud_font_cap(int face)
{
    face_t *f = face_of(face);
    if (!f)
        return 0;
    return f->cap;
}

int hud_font_span(int face)
{
    face_t *f = face_of(face);
    int span;
    if (!f)
        return 0;
    span = f->cell - f->lead;
    return span > 0 ? span : f->cap;
}

int hud_font_digit(int face)
{
    face_t *f = face_of(face);
    if (!f)
        return 0;
    return f->digit;
}

static int text_width(face_t *f, const char *text)
{
    const char *p;
    SIZE sz;
    int w = 0;
    HFONT old;
    if (!text || !text[0] || !f->font)
        return 0;
    old = (HFONT)SelectObject(measure_dc, f->font);
    SetTextCharacterExtra(measure_dc, f->tabular ? 0 : f->extra);
    if (!f->tabular) {
        GetTextExtentPoint32A(measure_dc, text, (int)strlen(text), &sz);
        w = (int)sz.cx;
    } else {
        for (p = text; *p; p++) {
            if (*p >= '0' && *p <= '9') {
                w += f->digit;
            } else {
                GetTextExtentPoint32A(measure_dc, p, 1, &sz);
                w += (int)sz.cx;
            }
        }
    }
    SelectObject(measure_dc, old);
    return w;
}

int hud_font_width(int face, const char *text)
{
    face_t *f = face_of(face);
    if (!f)
        return 0;
    return text_width(f, text);
}

static int ensure_dib(int w, int h)
{
    BITMAPINFO bi;
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    if (draw_bm && w <= draw_w && h <= draw_h)
        return 0;
    if (draw_bm) {
        SelectObject(draw_dc, draw_dummy);
        DeleteObject(draw_bm);
        draw_bm = NULL;
        draw_bits = NULL;
    }
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    draw_bm = CreateDIBSection(draw_dc, &bi, DIB_RGB_COLORS, &draw_bits, NULL, 0);
    if (!draw_bm || !draw_bits)
        return -1;
    SelectObject(draw_dc, draw_bm);
    draw_w = w;
    draw_h = h;
    return 0;
}

static void over(unsigned char *dst, int cov, int r, int g, int b, int alpha)
{
    int sa, da, inv, out_a, dr, dg, db;
    if (cov <= 0)
        return;
    sa = cov * alpha / 255;
    if (sa <= 0)
        return;
    da = dst[3];
    inv = 255 - sa;
    out_a = sa + da * inv / 255;
    if (out_a <= 0)
        return;
    /* Straight alpha. A glyph on a faint chip has to become opaque,
     * or the label is composited away with the chip. */
    if (da <= 0 || sa >= 255) {
        dr = r;
        dg = g;
        db = b;
    } else {
        dr = (r * sa + dst[0] * da * inv / 255) / out_a;
        dg = (g * sa + dst[1] * da * inv / 255) / out_a;
        db = (b * sa + dst[2] * da * inv / 255) / out_a;
    }
    if (dr > 255)
        dr = 255;
    if (dg > 255)
        dg = 255;
    if (db > 255)
        db = 255;
    dst[0] = (unsigned char)dr;
    dst[1] = (unsigned char)dg;
    dst[2] = (unsigned char)db;
    dst[3] = (unsigned char)out_a;
}

static void blend(unsigned char *pixels, int pitch, int pix, int bw, int bh,
                  int dx, int dy, int dw, int dh, int r, int g, int b, int alpha)
{
    int y, x;
    for (y = 0; y < dh; y++) {
        unsigned char *row = (unsigned char *)draw_bits + (size_t)y * (size_t)draw_w * 4;
        int py = dy + y;
        if (py < 0 || py >= bh)
            continue;
        for (x = 0; x < dw; x++) {
            int px = dx + x;
            if (px < 0 || px >= bw)
                continue;
            over(pixels + (size_t)py * (size_t)pitch + (size_t)px * (size_t)pix,
                 row[x * 4 + 1], r, g, b, alpha);
        }
    }
}

static void blend_mask(unsigned char *pixels, int pitch, int pix, int bw, int bh,
                       int dx, int dy, int dw, int dh, const unsigned char *mask,
                       int r, int g, int b, int alpha)
{
    int y, x;
    for (y = 0; y < dh; y++) {
        int py = dy + y;
        if (py < 0 || py >= bh)
            continue;
        for (x = 0; x < dw; x++) {
            int px = dx + x;
            if (px < 0 || px >= bw)
                continue;
            over(pixels + (size_t)py * (size_t)pitch + (size_t)px * (size_t)pix,
                 mask[(size_t)y * (size_t)dw + (size_t)x], r, g, b, alpha);
        }
    }
}

/* Average the large grayscale raster into one coverage sample per HUD pixel. */
static void area_mask(const unsigned char *src, int src_stride, int src_w, int src_h,
                      unsigned char *dst, int dst_w, int dst_h)
{
    int y, x;
    if (dst_w < 1 || dst_h < 1 || src_w < 1 || src_h < 1)
        return;
    for (y = 0; y < dst_h; y++) {
        int64_t y0 = (int64_t)y * src_h * 256 / dst_h;
        int64_t y1 = (int64_t)(y + 1) * src_h * 256 / dst_h;
        int iy0 = (int)(y0 / 256);
        int iy1 = (int)((y1 + 255) / 256);
        int iy;
        if (iy0 < 0)
            iy0 = 0;
        if (iy1 > src_h)
            iy1 = src_h;
        for (x = 0; x < dst_w; x++) {
            int64_t x0 = (int64_t)x * src_w * 256 / dst_w;
            int64_t x1 = (int64_t)(x + 1) * src_w * 256 / dst_w;
            int ix0 = (int)(x0 / 256);
            int ix1 = (int)((x1 + 255) / 256);
            int ix;
            int64_t sum = 0;
            int64_t area = 0;
            if (ix0 < 0)
                ix0 = 0;
            if (ix1 > src_w)
                ix1 = src_w;
            for (iy = iy0; iy < iy1; iy++) {
                int64_t sy0 = (int64_t)iy * 256;
                int64_t sy1 = sy0 + 256;
                int64_t top = y0 > sy0 ? y0 : sy0;
                int64_t bot = y1 < sy1 ? y1 : sy1;
                int64_t wy = bot - top;
                const unsigned char *row;
                if (wy <= 0)
                    continue;
                row = src + (size_t)iy * (size_t)src_stride;
                for (ix = ix0; ix < ix1; ix++) {
                    int64_t sx0 = (int64_t)ix * 256;
                    int64_t sx1 = sx0 + 256;
                    int64_t left = x0 > sx0 ? x0 : sx0;
                    int64_t right = x1 < sx1 ? x1 : sx1;
                    int64_t wx = right - left;
                    if (wx <= 0)
                        continue;
                    sum += (int64_t)row[(size_t)ix * 4 + 1] * wx * wy;
                    area += wx * wy;
                }
            }
            dst[(size_t)y * (size_t)dst_w + (size_t)x] =
                area > 0 ? (unsigned char)((sum + area / 2) / area) : 0;
        }
    }
}

/* Returns 0 when the averaged raster was blended. */
static int draw_smooth(unsigned char *pixels, int pitch, int pix, int bw, int bh,
                       int x, int y, face_t *f, const char *text,
                       int r, int g, int b, int a)
{
    int scale = DRAW_SCALE;
    int n, tw, th, dst_w, dst_h, src_w, src_h, target, extra, got, pen, i;
    HFONT old;
    unsigned char *mask;
    n = (int)strlen(text);
    tw = text_width(f, text);
    th = f->cell;
    if (n < 1 || tw < 1 || th < 1 || !f->big)
        return -1;
    dst_w = tw + GLYPH_PAD * 2;
    dst_h = th;
    target = tw * scale;
    src_h = th * scale;
    if (f->big_cell + 2 > src_h)
        src_h = f->big_cell + 2;
    old = (HFONT)SelectObject(draw_dc, f->big);
    SetBkMode(draw_dc, TRANSPARENT);
    SetBkColor(draw_dc, RGB(0, 0, 0));
    SetTextColor(draw_dc, RGB(255, 255, 255));
    extra = 0;
    got = target;
    if (!f->tabular) {
        SIZE sz;
        int natural;
        SetTextCharacterExtra(draw_dc, 0);
        GetTextExtentPoint32A(draw_dc, text, n, &sz);
        natural = (int)sz.cx;
        if (n > 1)
            extra = (target - natural) / (n - 1);
        SetTextCharacterExtra(draw_dc, extra);
        GetTextExtentPoint32A(draw_dc, text, n, &sz);
        got = (int)sz.cx;
    } else {
        SetTextCharacterExtra(draw_dc, 0);
    }
    src_w = GLYPH_PAD * scale * 2 + (got > target ? got : target);
    if (src_w < 1 || src_h < 1 || ensure_dib(src_w, src_h) != 0) {
        SelectObject(draw_dc, old);
        return -1;
    }
    memset(draw_bits, 0, (size_t)draw_w * (size_t)draw_h * 4);
    pen = GLYPH_PAD * scale;
    if (!f->tabular) {
        TextOutA(draw_dc, pen, 0, text, n);
    } else {
        for (i = 0; i < n; i++) {
            char ch = text[i];
            SIZE sz1, szs;
            int slot, cx;
            SelectObject(draw_dc, f->font);
            SetTextCharacterExtra(draw_dc, 0);
            GetTextExtentPoint32A(draw_dc, &ch, 1, &sz1);
            SelectObject(draw_dc, f->big);
            SetTextCharacterExtra(draw_dc, 0);
            GetTextExtentPoint32A(draw_dc, &ch, 1, &szs);
            if (ch >= '0' && ch <= '9')
                slot = f->digit * scale;
            else
                slot = (int)sz1.cx * scale;
            if (slot < 1)
                slot = 1;
            cx = pen + (slot - (int)szs.cx) / 2;
            if (cx < pen)
                cx = pen;
            TextOutA(draw_dc, cx, 0, &ch, 1);
            pen += slot;
        }
    }
    mask = (unsigned char *)malloc((size_t)dst_w * (size_t)dst_h);
    if (!mask) {
        SelectObject(draw_dc, old);
        return -1;
    }
    area_mask((const unsigned char *)draw_bits, draw_w * 4, src_w, src_h,
              mask, dst_w, dst_h);
    blend_mask(pixels, pitch, pix, bw, bh, x - GLYPH_PAD, y - f->lead,
               dst_w, dst_h, mask, r, g, b, a);
    free(mask);
    SelectObject(draw_dc, old);
    return 0;
}

void hud_font_draw(unsigned char *pixels, int pitch, int pix, int bw, int bh,
                   int x, int y, int face, const char *text,
                   int r, int g, int b, int a)
{
    face_t *f;
    int tw, th, i, n, pen;
    HFONT old;
    if (!ready || !pixels || !text || !text[0])
        return;
    f = face_of(face);
    if (!f)
        return;
    if (f->big &&
        draw_smooth(pixels, pitch, pix, bw, bh, x, y, f, text, r, g, b, a) == 0)
        return;
    tw = text_width(f, text);
    th = f->cell;
    if (tw < 1 || th < 1)
        return;
    if (ensure_dib(tw + GLYPH_PAD * 2, th + 2) != 0)
        return;
    memset(draw_bits, 0, (size_t)draw_w * (size_t)draw_h * 4);
    old = (HFONT)SelectObject(draw_dc, f->font);
    SetBkMode(draw_dc, TRANSPARENT);
    SetBkColor(draw_dc, RGB(0, 0, 0));
    SetTextColor(draw_dc, RGB(255, 255, 255));
    SetTextCharacterExtra(draw_dc, f->tabular ? 0 : f->extra);
    n = (int)strlen(text);
    pen = GLYPH_PAD;
    if (!f->tabular) {
        /* One draw keeps the antialiasing between letters. */
        TextOutA(draw_dc, pen, 0, text, n);
    } else {
        for (i = 0; i < n; i++) {
            char ch = text[i];
            SIZE sz;
            int cx = pen;
            GetTextExtentPoint32A(draw_dc, &ch, 1, &sz);
            if (ch >= '0' && ch <= '9') {
                cx = pen + (f->digit - (int)sz.cx) / 2;
                TextOutA(draw_dc, cx, 0, &ch, 1);
                pen += f->digit;
            } else {
                TextOutA(draw_dc, pen, 0, &ch, 1);
                pen += (int)sz.cx;
            }
        }
    }
    SelectObject(draw_dc, old);
    blend(pixels, pitch, pix, bw, bh, x - GLYPH_PAD, y - f->lead,
          tw + GLYPH_PAD * 2, th, r, g, b, a);
}
#endif
