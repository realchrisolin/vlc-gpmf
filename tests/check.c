/*
 * Reader, clock, and map checks. No VLC.
 *
 * The MP4s are built here: four gpmd payloads a couple of megabytes apart,
 * then the same track with only GPS9, then one whose clock is the MP4
 * creation time. A playhead window loaded from the end of the track must
 * sample GPS9 the same way as a full read, including the points that fall
 * between two payloads.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "gpmf_read.h"
#include "hud_map.h"
#include "hud_zone.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MP4_UNIX_EPOCH 2082844800LL
#define GAP_BYTES (2u * 1024u * 1024u)
#define PAYLOADS 4

static int failures;

static void fail(const char *fmt, ...)
{
    va_list ap;
    fputs("FAIL ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    failures++;
}

static int64_t civil_unix(int year, int month, int day, int hour, int minute, int second)
{
    int64_t y = year;
    int64_t era, yoe, doy, doe, days;
    y -= month <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    days = era * 146097 + doe - 719468;
    return days * 86400 + (int64_t)hour * 3600 + (int64_t)minute * 60 + second;
}

static void expect_i(const char *what, int got, int want)
{
    if (got != want)
        fail("%s: got %d, want %d", what, got, want);
}

static void expect_d(const char *what, double got, double want, double tol)
{
    if (!(fabs(got - want) <= tol))
        fail("%s: got %.8f, want %.8f", what, got, want);
}

static void expect_s(const char *what, const char *got, const char *want)
{
    if (!got || !want || strcmp(got, want) != 0)
        fail("%s: got \"%s\", want \"%s\"", what, got ? got : "(null)", want ? want : "(null)");
}

static void exe_dir(const char *argv0, char *out, size_t n)
{
    const char *s;
    const char *slash = NULL;
    size_t len;
    for (s = argv0; s && *s; s++) {
        if (*s == '/' || *s == '\\')
            slash = s;
    }
    if (!slash) {
        snprintf(out, n, ".");
        return;
    }
    len = (size_t)(slash - argv0);
    if (len >= n)
        len = n - 1;
    memcpy(out, argv0, len);
    out[len] = '\0';
    if (len == 0)
        snprintf(out, n, ".");
}

static void join_path(char *out, size_t n, const char *dir, const char *name)
{
    snprintf(out, n, "%s/%s", dir, name);
}

typedef struct {
    unsigned char *d;
    size_t n, cap;
} Buf;

static void bneed(Buf *b, size_t extra)
{
    size_t cap;
    unsigned char *p;
    if (b->n + extra <= b->cap)
        return;
    cap = b->cap ? b->cap * 2 : 256;
    while (cap < b->n + extra)
        cap *= 2;
    p = (unsigned char *)realloc(b->d, cap);
    if (!p) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    b->d = p;
    b->cap = cap;
}

static void bu8(Buf *b, unsigned v)
{
    bneed(b, 1);
    b->d[b->n++] = (unsigned char)v;
}

static void bu16(Buf *b, unsigned v)
{
    bu8(b, v >> 8);
    bu8(b, v);
}

static void bu32(Buf *b, uint32_t v)
{
    bu8(b, (unsigned)(v >> 24));
    bu8(b, (unsigned)(v >> 16));
    bu8(b, (unsigned)(v >> 8));
    bu8(b, (unsigned)v);
}

static void bu64(Buf *b, uint64_t v)
{
    bu32(b, (uint32_t)(v >> 32));
    bu32(b, (uint32_t)v);
}

static void braw(Buf *b, const void *p, size_t n)
{
    bneed(b, n);
    memcpy(b->d + b->n, p, n);
    b->n += n;
}

static void bfour(Buf *b, const char *s)
{
    braw(b, s, 4);
}

static void bf32(Buf *b, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    bu32(b, u);
}

static size_t atom_open(Buf *b, const char *typ)
{
    size_t at = b->n;
    bu32(b, 0);
    bfour(b, typ);
    return at;
}

static void atom_close(Buf *b, size_t at)
{
    uint32_t sz = (uint32_t)(b->n - at);
    b->d[at] = (unsigned char)(sz >> 24);
    b->d[at + 1] = (unsigned char)(sz >> 16);
    b->d[at + 2] = (unsigned char)(sz >> 8);
    b->d[at + 3] = (unsigned char)sz;
}

static void klv(Buf *b, const char *key, int type, int ssize, const void *data, int nbytes)
{
    int pad;
    if (ssize <= 0 || nbytes < ssize || (nbytes % ssize) != 0) {
        fprintf(stderr, "bad klv %s\n", key);
        exit(1);
    }
    bfour(b, key);
    bu8(b, (unsigned)type);
    bu8(b, (unsigned)ssize);
    bu16(b, (unsigned)(nbytes / ssize));
    if (data && nbytes)
        braw(b, data, (size_t)nbytes);
    pad = (4 - (nbytes & 3)) & 3;
    while (pad--)
        bu8(b, 0);
}

static size_t nest_open(Buf *b, const char *key)
{
    size_t at = b->n;
    bfour(b, key);
    bu32(b, 0);
    return at;
}

static void nest_close(Buf *b, size_t at)
{
    size_t inner = b->n - at - 8;
    int pad;
    if (inner > 65535) {
        fprintf(stderr, "nested gpmf too large\n");
        exit(1);
    }
    b->d[at + 4] = 0;
    b->d[at + 5] = 1;
    b->d[at + 6] = (unsigned char)(inner >> 8);
    b->d[at + 7] = (unsigned char)inner;
    pad = (int)((4 - (inner & 3)) & 3);
    while (pad--)
        bu8(b, 0);
}

static void gpsu_text(char out[17], int index)
{
    snprintf(out, 17, "2603080659%02d.500", index);
}

static void add_sample(Buf *raw, int k, float days, float secs)
{
    int i;
    float v[9];
    v[0] = 43.0f + 0.25f * (float)k;
    v[1] = -76.0f - 0.5f * (float)k;
    v[2] = 100.0f + (float)k;
    v[3] = 10.0f + (float)k;
    v[4] = 11.0f + (float)k;
    v[5] = days;
    v[6] = secs;
    v[7] = 1.0f;
    v[8] = 3.0f;
    for (i = 0; i < 9; i++)
        bf32(raw, v[i]);
}

/* One second of GPS9, two fixes. gpsu_index < 0 leaves the clock to GPS9 or the file. */
static Buf make_payload(int index, int gpsu_index, float days, float secs, int samples)
{
    Buf b;
    Buf raw;
    size_t dev, strm;
    char stamp[17];
    unsigned char id[4];
    const char *name = "HERO11 Black";
    int j;
    memset(&b, 0, sizeof b);
    memset(&raw, 0, sizeof raw);
    dev = nest_open(&b, "DEVC");
    id[0] = 0;
    id[1] = 0;
    id[2] = 0;
    id[3] = 1;
    klv(&b, "DVID", 'L', 4, id, 4);
    klv(&b, "DVNM", 'c', 1, name, (int)strlen(name));
    if (gpsu_index >= 0) {
        strm = nest_open(&b, "STRM");
        gpsu_text(stamp, gpsu_index);
        klv(&b, "GPSU", 'c', 16, stamp, 16);
        nest_close(&b, strm);
    }
    strm = nest_open(&b, "STRM");
    klv(&b, "STNM", 'c', 1, "GPS", 3);
    for (j = 0; j < samples; j++)
        add_sample(&raw, index * samples + j, days, secs);
    klv(&b, "GPS9", 'f', 36, raw.d, samples * 36);
    free(raw.d);
    nest_close(&b, strm);
    nest_close(&b, dev);
    return b;
}

static Buf make_empty_payload(void)
{
    Buf b;
    size_t dev;
    const char *name = "file clock";
    memset(&b, 0, sizeof b);
    dev = nest_open(&b, "DEVC");
    klv(&b, "DVNM", 'c', 1, name, (int)strlen(name));
    nest_close(&b, dev);
    return b;
}

static void bfull(Buf *b, int ver_flags)
{
    bu32(b, (uint32_t)ver_flags);
}

static int write_at(FILE *fp, long off, const void *p, size_t n)
{
    if (fseek(fp, off, SEEK_SET) != 0)
        return -1;
    return fwrite(p, 1, n, fp) == n ? 0 : -1;
}

static int write_mp4(const char *path, Buf *payload, int n, uint32_t gap, int64_t creation)
{
    Buf head;
    Buf moov;
    size_t at, trak, mdia, minf, stbl, stsd;
    uint64_t base, cursor, mdat_end;
    uint64_t off[8];
    FILE *fp;
    int i;
    if (n <= 0 || n > 8)
        return -1;
    memset(&head, 0, sizeof head);
    memset(&moov, 0, sizeof moov);
    at = atom_open(&head, "ftyp");
    bfour(&head, "isom");
    bu32(&head, 0);
    bfour(&head, "isom");
    atom_close(&head, at);

    base = (uint64_t)head.n + 8;
    cursor = base;
    for (i = 0; i < n; i++) {
        off[i] = cursor;
        cursor += payload[i].n;
        if (i + 1 < n)
            cursor += gap;
    }
    mdat_end = cursor;

    at = atom_open(&moov, "moov");
    {
        size_t mv = atom_open(&moov, "mvhd");
        if (creation > 0) {
            bfull(&moov, 0x01000000);
            bu64(&moov, (uint64_t)creation);
            bu64(&moov, (uint64_t)creation);
            bu32(&moov, 1000);
            bu64(&moov, (uint64_t)n * 1000);
        } else {
            bfull(&moov, 0);
            bu32(&moov, 0);
            bu32(&moov, 0);
            bu32(&moov, 1000);
            bu32(&moov, (uint32_t)n * 1000);
        }
        atom_close(&moov, mv);
    }
    trak = atom_open(&moov, "trak");
    mdia = atom_open(&moov, "mdia");
    {
        size_t md = atom_open(&moov, "mdhd");
        bfull(&moov, 0);
        bu32(&moov, 0);
        bu32(&moov, 0);
        bu32(&moov, 1000);
        bu32(&moov, (uint32_t)n * 1000);
        atom_close(&moov, md);
    }
    minf = atom_open(&moov, "minf");
    stbl = atom_open(&moov, "stbl");
    stsd = atom_open(&moov, "stsd");
    bfull(&moov, 0);
    bu32(&moov, 1);
    {
        size_t ent = atom_open(&moov, "gpmd");
        bu32(&moov, 0);
        bu32(&moov, 1);
        atom_close(&moov, ent);
    }
    atom_close(&moov, stsd);
    {
        size_t st = atom_open(&moov, "stts");
        bfull(&moov, 0);
        bu32(&moov, 1);
        bu32(&moov, (uint32_t)n);
        bu32(&moov, 1000);
        atom_close(&moov, st);
    }
    {
        size_t st = atom_open(&moov, "stsc");
        bfull(&moov, 0);
        bu32(&moov, 1);
        bu32(&moov, 1);
        bu32(&moov, 1);
        bu32(&moov, 1);
        atom_close(&moov, st);
    }
    {
        size_t st = atom_open(&moov, "stsz");
        bfull(&moov, 0);
        bu32(&moov, 0);
        bu32(&moov, (uint32_t)n);
        for (i = 0; i < n; i++)
            bu32(&moov, (uint32_t)payload[i].n);
        atom_close(&moov, st);
    }
    {
        size_t st = atom_open(&moov, "stco");
        bfull(&moov, 0);
        bu32(&moov, (uint32_t)n);
        for (i = 0; i < n; i++) {
            if (off[i] > 0xffffffffu) {
                free(head.d);
                free(moov.d);
                return -1;
            }
            bu32(&moov, (uint32_t)off[i]);
        }
        atom_close(&moov, st);
    }
    atom_close(&moov, stbl);
    atom_close(&moov, minf);
    atom_close(&moov, mdia);
    atom_close(&moov, trak);
    atom_close(&moov, at);

    fp = fopen(path, "wb");
    if (!fp) {
        free(head.d);
        free(moov.d);
        return -1;
    }
    {
        unsigned char mh[8];
        uint32_t mdat_sz = (uint32_t)(mdat_end - (uint64_t)head.n);
        mh[0] = (unsigned char)(mdat_sz >> 24);
        mh[1] = (unsigned char)(mdat_sz >> 16);
        mh[2] = (unsigned char)(mdat_sz >> 8);
        mh[3] = (unsigned char)mdat_sz;
        mh[4] = 'm';
        mh[5] = 'd';
        mh[6] = 'a';
        mh[7] = 't';
        if (write_at(fp, 0, head.d, head.n) != 0 ||
            write_at(fp, (long)head.n, mh, 8) != 0) {
            fclose(fp);
            free(head.d);
            free(moov.d);
            return -1;
        }
    }
    for (i = 0; i < n; i++) {
        if (write_at(fp, (long)off[i], payload[i].d, payload[i].n) != 0) {
            fclose(fp);
            free(head.d);
            free(moov.d);
            return -1;
        }
    }
    if (write_at(fp, (long)mdat_end, moov.d, moov.n) != 0) {
        fclose(fp);
        free(head.d);
        free(moov.d);
        return -1;
    }
    fclose(fp);
    free(head.d);
    free(moov.d);
    return 0;
}

static void free_payloads(Buf *p, int n)
{
    int i;
    for (i = 0; i < n; i++)
        free(p[i].d);
}

static const gpmf_series *gps9_of(gpmf_file *f)
{
    return gpmf_series_find(f, GPMF_FOURCC('G', 'P', 'S', '9'));
}

static void expect_clock(const char *what, gpmf_file *f, double t, int src,
                         int year, int month, int day, int hour, int minute, int second)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    char label[64];
    snprintf(label, sizeof label, "%s source", what);
    expect_i(label, f->clock_source, src);
    snprintf(label, sizeof label, "%s parts", what);
    if (gpmf_clock_parts(f, t, &y, &mo, &d, &h, &mi, &s) != 0)
        fail("%s: no clock", what);
    else {
        expect_i(label, y, year);
        expect_i(label, mo, month);
        expect_i(label, d, day);
        expect_i(label, h, hour);
        expect_i(label, mi, minute);
        expect_i(label, s, second);
    }
}

static void check_epoch(void)
{
    if (civil_unix(1970, 1, 1, 0, 0, 0) != 0)
        fail("civil 1970-01-01 is not unix 0");
    if (civil_unix(2000, 1, 1, 0, 0, 0) != 946684800)
        fail("civil 2000-01-01 is not unix 946684800");
}

static void check_seam(const char *path)
{
    Buf payload[PAYLOADS];
    gpmf_file *window = NULL;
    gpmf_file *full = NULL;
    const gpmf_series *sw;
    const gpmf_series *sf;
    char err[256];
    int i;
    double t;
    err[0] = '\0';
    for (i = 0; i < PAYLOADS; i++)
        payload[i] = make_payload(i, i, 0.0f, 0.0f, 2);
    if (write_mp4(path, payload, PAYLOADS, GAP_BYTES, 0) != 0) {
        fail("cannot write %s", path);
        free_payloads(payload, PAYLOADS);
        return;
    }
    free_payloads(payload, PAYLOADS);
    if (gpmf_open_index(&window, path, err, sizeof err) != 0) {
        fail("open_index %s: %s", path, err[0] ? err : "failed");
        remove(path);
        return;
    }
    expect_i("indexed payloads", window->payloads, PAYLOADS);
    expect_d("indexed duration", window->duration, 4.0, 1e-9);
    expect_i("ready before any read", gpmf_ready(window, 0), 0);

    /* Walk the payloads from the end. Each call's 1.5 s lookbehind stops on
     * the previous payload, so the four runs stay split. */
    expect_i("load payload 3", gpmf_ensure(window, 4.5, 0), 1);
    expect_i("start still unread", gpmf_ready(window, 0), 0);
    expect_i("payload 3 ready", gpmf_ready(window, 3.2), 1);
    expect_clock("anchor from the last payload", window, 0, GPMF_CLOCK_GPSU,
                 2026, 3, 8, 6, 59, 0);
    expect_i("load payload 2", gpmf_ensure(window, 3.5, 0), 1);
    expect_i("load payload 1", gpmf_ensure(window, 2.5, 0), 1);
    expect_i("load payload 0", gpmf_ensure(window, 0.5, 0), 1);
    expect_i("window already loaded", gpmf_ensure(window, 0, 3.0), 0);
    sw = gps9_of(window);
    if (!sw)
        fail("window has no GPS9");
    else
        expect_i("split runs", sw->run_count, PAYLOADS);

    if (gpmf_open(&full, path, err, sizeof err) != 0) {
        fail("open %s: %s", path, err[0] ? err : "failed");
        gpmf_close(window);
        remove(path);
        return;
    }
    sf = gps9_of(full);
    if (!sf)
        fail("full read has no GPS9");
    else {
        expect_i("merged runs", sf->run_count, 1);
        expect_i("sample count", sf->count, PAYLOADS * 2);
    }
    expect_clock("full GPSU", full, 0, GPMF_CLOCK_GPSU, 2026, 3, 8, 6, 59, 0);
    if (sw && sf) {
        for (t = 0.25; t <= 3.75 + 1e-9; t += 0.25) {
            float a[9], b[9];
            int na, nb, c;
            char label[64];
            na = gpmf_sample(sw, t, a);
            nb = gpmf_sample(sf, t, b);
            snprintf(label, sizeof label, "channels at %.2f", t);
            expect_i(label, na, 9);
            expect_i(label, nb, 9);
            if (na == 9 && nb == 9) {
                for (c = 0; c < 9; c++) {
                    snprintf(label, sizeof label, "GPS9[%d] at %.2f", c, t);
                    expect_d(label, (double)a[c], (double)b[c], 1e-5);
                }
            }
        }
        {
            float row[9];
            if (gpmf_sample(sf, 0.25, row) == 9) {
                expect_d("first lat", row[0], 43.0, 1e-5);
                expect_d("first lon", row[1], -76.0, 1e-5);
            }
            if (gpmf_sample(sf, 1.0, row) == 9)
                expect_d("seam lat", row[0], 43.375, 1e-4);
        }
    }
    gpmf_close(full);
    gpmf_close(window);
    remove(path);
}

static void check_gps9_clock(const char *path)
{
    Buf payload;
    gpmf_file *file = NULL;
    char err[256];
    double midnight;
    float days, secs;
    err[0] = '\0';
    midnight = (double)civil_unix(2026, 3, 8, 0, 0, 0);
    days = (float)((midnight - 946684800.0) / 86400.0);
    secs = (float)(6 * 3600 + 59 * 60);
    payload = make_payload(0, -1, days, secs, 1);
    if (write_mp4(path, &payload, 1, 0, 0) != 0) {
        fail("cannot write %s", path);
        free(payload.d);
        return;
    }
    free(payload.d);
    if (gpmf_open(&file, path, err, sizeof err) != 0) {
        fail("gps9 open: %s", err[0] ? err : "failed");
        remove(path);
        return;
    }
    expect_clock("GPS9", file, 0.5, GPMF_CLOCK_GPS9, 2026, 3, 8, 6, 59, 0);
    gpmf_close(file);
    remove(path);
}

static void check_file_clock(const char *path)
{
    Buf payload;
    gpmf_file *file = NULL;
    char err[256];
    int64_t unix_s;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    int off = 0;
    err[0] = '\0';
    unix_s = civil_unix(2026, 1, 9, 3, 18, 47);
    payload = make_empty_payload();
    if (write_mp4(path, &payload, 1, 0, unix_s + MP4_UNIX_EPOCH) != 0) {
        fail("cannot write %s", path);
        free(payload.d);
        return;
    }
    free(payload.d);
    if (gpmf_open(&file, path, err, sizeof err) != 0) {
        fail("file clock open: %s", err[0] ? err : "failed");
        remove(path);
        return;
    }
    expect_clock("FILE", file, 0, GPMF_CLOCK_FILE, 2026, 1, 9, 3, 18, 47);
    expect_clock("FILE plus one second", file, 1, GPMF_CLOCK_FILE, 2026, 1, 9, 3, 18, 48);
    if (hud_zone_in("America/New_York", unix_s, &y, &mo, &d, &h, &mi, &s, &off) != 0)
        fail("New York offset for the file clock");
    else if (h == 3 && mi == 18)
        fail("FILE clock matches a New York wall time; the stored digits were shifted");
    gpmf_close(file);
    remove(path);
}

static void expect_zone(const char *what, int64_t unix_s,
                        int year, int month, int day, int hour, int minute, int second, int utoff)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, off = 0;
    char label[80];
    if (hud_zone_in("America/New_York", unix_s, &y, &mo, &d, &h, &mi, &s, &off) != 0) {
        fail("%s: zone lookup failed", what);
        return;
    }
    snprintf(label, sizeof label, "%s civil", what);
    expect_i(label, y, year);
    expect_i(label, mo, month);
    expect_i(label, d, day);
    expect_i(label, h, hour);
    expect_i(label, mi, minute);
    expect_i(label, s, second);
    snprintf(label, sizeof label, "%s offset", what);
    expect_i(label, off, utoff);
}

static void check_zone(void)
{
    char tz[64];
    char label[32];
    if (hud_zone_open() != 0) {
        fail("timezone data was not found beside the test or in ../third_party/zonedetect");
        return;
    }
    tz[0] = '\0';
    if (hud_zone_lookup(43.09355, -76.08021, tz, (int)sizeof tz) != 0)
        fail("lookup at 43.09355, -76.08021 failed");
    else
        expect_s("East Henrietta Road", tz, "America/New_York");
    expect_zone("before the 2026 spring change", civil_unix(2026, 3, 8, 6, 59, 0),
                2026, 3, 8, 1, 59, 0, -5 * 3600);
    expect_zone("2026 spring change", civil_unix(2026, 3, 8, 7, 0, 0),
                2026, 3, 8, 3, 0, 0, -4 * 3600);
    expect_zone("before the 2026 fall change", civil_unix(2026, 11, 1, 5, 59, 0),
                2026, 11, 1, 1, 59, 0, -4 * 3600);
    expect_zone("2026 fall change", civil_unix(2026, 11, 1, 6, 0, 0),
                2026, 11, 1, 1, 0, 0, -5 * 3600);
    hud_zone_label(label, (int)sizeof label, -5 * 3600);
    expect_s("EST label", label, "UTC-5");
    hud_zone_label(label, (int)sizeof label, -4 * 3600);
    expect_s("EDT label", label, "UTC-4");
}

static void check_map(void)
{
    hud_map_box box;
    double lat, lon, course;
    double bear;
    lat = 1;
    lon = 2;
    course = 10;
    hud_map_ease(&lat, &lon, &course, 43.0, -76.0, 180.0, 1, 0.1, 0.0);
    expect_d("ease lat", lat, 43.0, 0);
    expect_d("ease lon", lon, -76.0, 0);
    expect_d("ease course", course, 180.0, 0);

    lat = 0;
    lon = 0;
    course = 350;
    hud_map_ease(&lat, &lon, &course, 0, 0, 10, 1, 1.0, 1.0);
    if (!(course > 0.0 && course < 30.0))
        fail("heading 350 toward 10 took the long way: course %.3f", course);

    bear = hud_map_bearing(40.0, -70.0, 39.0, -70.0);
    expect_d("bearing south", bear, 180.0, 1e-6);
    bear = hud_map_bearing(40.0, -70.0, 41.0, -70.0);
    expect_d("bearing north", bear, 0.0, 1e-6);
    bear = hud_map_bearing(0.0, 0.0, 0.0, 1.0);
    expect_d("bearing east", bear, 90.0, 1e-4);
    bear = hud_map_bearing(0.0, 0.0, 0.0, -1.0);
    expect_d("bearing west", bear, 270.0, 1e-4);
    if (!(bear >= 0.0 && bear < 360.0))
        fail("bearing out of range");

    expect_i("arrow floor", hud_map_arrow_len(0), 24);
    expect_i("arrow low", hud_map_arrow_len(23), 24);
    expect_i("arrow default", hud_map_arrow_len(64), 64);
    expect_i("arrow high", hud_map_arrow_len(240), 240);
    expect_i("arrow ceiling", hud_map_arrow_len(1000), 240);

    expect_i("opacity floor", hud_map_opacity_step(10, -1), 10);
    expect_i("opacity ceiling", hud_map_opacity_step(100, 1), 100);
    expect_i("opacity up", hud_map_opacity_step(70, 1), 80);
    expect_i("opacity down", hud_map_opacity_step(70, -1), 60);
    expect_i("opacity step below 20", hud_map_opacity_step(20, -1), 10);
    expect_i("opacity clamp down", hud_map_opacity_step(15, -1), 10);
    expect_i("opacity clamp up", hud_map_opacity_step(95, 1), 100);

    hud_map_immersive_box(1920, 1080, 1, &box);
    expect_i("immersive full", box.full, 1);
    expect_i("center is not a drag", hud_map_hit(&box, 1, box.w / 2, box.h / 2), HUD_MAP_HIT_NONE);
    expect_i("corner is not a grip", hud_map_hit(&box, 1, 0, 0), HUD_MAP_HIT_NONE);
    expect_i("dock still zooms",
             hud_map_hit(&box, 1, box.zin_x + 1, box.zin_y + 1), HUD_MAP_HIT_ZOOM_IN);
    expect_i("gap under zoom in still hits it",
             hud_map_hit(&box, 1, box.zin_x + box.zin_w / 2, box.zin_y + box.zin_h + 1),
             HUD_MAP_HIT_ZOOM_IN);
    expect_i("left of the dock misses",
             hud_map_hit(&box, 1, box.zin_x - box.zin_w, box.zin_y + box.zin_h / 2),
             HUD_MAP_HIT_NONE);
    {
        int mid = (box.zout_y + box.zout_h + box.oup_y) / 2;
        int hit = hud_map_hit(&box, 1, box.zout_x + box.zout_w / 2, mid);
        if (hit != HUD_MAP_HIT_ZOOM_OUT && hit != HUD_MAP_HIT_OPACITY_UP)
            fail("section gap hit %d", hit);
    }
}

static int contains_ci(const char *s, const char *part)
{
    size_t n = strlen(part);
    size_t i, len = strlen(s);
    if (n == 0 || len < n)
        return 0;
    for (i = 0; i + n <= len; i++) {
        size_t k;
        for (k = 0; k < n; k++) {
            char a = s[i + k];
            char b = part[k];
            if (a >= 'A' && a <= 'Z')
                a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z')
                b = (char)(b - 'A' + 'a');
            if (a != b)
                break;
        }
        if (k == n)
            return 1;
    }
    return 0;
}

static unsigned ru16le(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned ru32le(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

#ifdef _WIN32
static void check_dll(const char *path)
{
    FILE *fp;
    unsigned char *b;
    long sz;
    unsigned lfanew, magic, nsec, opt, dir_at, import_rva;
    unsigned i;
    int saw_vlc = 0;
    char name[256];
    fp = fopen(path, "rb");
    if (!fp) {
        fail("plugin DLL is missing: %s", path);
        return;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fail("cannot size %s", path);
        fclose(fp);
        return;
    }
    sz = ftell(fp);
    if (sz < 256 || sz > 32 * 1024 * 1024) {
        fail("plugin DLL size %ld is not usable", sz);
        fclose(fp);
        return;
    }
    b = (unsigned char *)malloc((size_t)sz);
    if (!b || fseek(fp, 0, SEEK_SET) != 0 || fread(b, 1, (size_t)sz, fp) != (size_t)sz) {
        fail("cannot read %s", path);
        free(b);
        fclose(fp);
        return;
    }
    fclose(fp);
    if (b[0] != 'M' || b[1] != 'Z') {
        fail("%s is not a PE DLL", path);
        free(b);
        return;
    }
    lfanew = ru32le(b + 0x3c);
    if (lfanew + 24 + 112 >= (unsigned)sz || memcmp(b + lfanew, "PE\0\0", 4) != 0) {
        fail("%s has no PE header", path);
        free(b);
        return;
    }
    nsec = ru16le(b + lfanew + 6);
    opt = ru16le(b + lfanew + 20);
    magic = ru16le(b + lfanew + 24);
    if (magic == 0x20b)
        dir_at = lfanew + 24 + 112;
    else if (magic == 0x10b)
        dir_at = lfanew + 24 + 96;
    else {
        fail("%s has an unknown optional header", path);
        free(b);
        return;
    }
    if (dir_at + 16 >= (unsigned)sz) {
        fail("%s import directory is out of range", path);
        free(b);
        return;
    }
    import_rva = ru32le(b + dir_at + 8);
    {
        unsigned sec = lfanew + 24 + opt;
        unsigned import_off = 0;
        int found = 0;
        for (i = 0; i < nsec; i++) {
            unsigned va, vsz, raw, rawsz;
            unsigned sh = sec + i * 40;
            if (sh + 40 > (unsigned)sz)
                break;
            va = ru32le(b + sh + 12);
            vsz = ru32le(b + sh + 8);
            rawsz = ru32le(b + sh + 16);
            raw = ru32le(b + sh + 20);
            if (vsz < rawsz)
                vsz = rawsz;
            if (import_rva >= va && import_rva < va + vsz) {
                import_off = raw + (import_rva - va);
                found = 1;
                break;
            }
        }
        if (!found || import_rva == 0) {
            fail("%s has no import table", path);
            free(b);
            return;
        }
        for (i = 0; i < 64; i++) {
            unsigned desc = import_off + i * 20;
            unsigned name_rva, name_off = 0;
            unsigned k;
            int name_found = 0;
            if (desc + 20 > (unsigned)sz)
                break;
            if (ru32le(b + desc) == 0 && ru32le(b + desc + 12) == 0 && ru32le(b + desc + 16) == 0)
                break;
            name_rva = ru32le(b + desc + 12);
            for (k = 0; k < nsec; k++) {
                unsigned va, vsz, raw, rawsz, sh;
                sh = sec + k * 40;
                va = ru32le(b + sh + 12);
                vsz = ru32le(b + sh + 8);
                rawsz = ru32le(b + sh + 16);
                raw = ru32le(b + sh + 20);
                if (vsz < rawsz)
                    vsz = rawsz;
                if (name_rva >= va && name_rva < va + vsz) {
                    name_off = raw + (name_rva - va);
                    name_found = 1;
                    break;
                }
            }
            if (!name_found || name_off >= (unsigned)sz)
                continue;
            for (k = 0; k + 1 < sizeof name && name_off + k < (unsigned)sz; k++) {
                name[k] = (char)b[name_off + k];
                if (name[k] == '\0')
                    break;
            }
            name[k] = '\0';
            if (contains_ci(name, "libgcc_s") || contains_ci(name, "winpthread"))
                fail("%s imports %s", path, name);
            if (contains_ci(name, "vlccore"))
                saw_vlc = 1;
        }
    }
    if (!saw_vlc)
        fail("%s does not import libvlccore", path);
    free(b);
}
#endif

int main(int argc, char **argv)
{
    char dir[1024];
    char seam[1200], gps9[1200], fileclk[1200], dll[1200];
    exe_dir(argc > 0 ? argv[0] : ".", dir, sizeof dir);
    join_path(seam, sizeof seam, dir, "check-seam.mp4");
    join_path(gps9, sizeof gps9, dir, "check-gps9.mp4");
    join_path(fileclk, sizeof fileclk, dir, "check-file.mp4");
    join_path(dll, sizeof dll, dir, "libgpmf_plugin.dll");

    check_epoch();
    check_map();
    check_seam(seam);
    check_gps9_clock(gps9);
    check_zone();
    check_file_clock(fileclk);
    hud_zone_close();
#ifdef _WIN32
    check_dll(dll);
#endif
    if (failures) {
        fprintf(stderr, "%d check%s failed\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("gpmf-check ok\n");
    return 0;
}
