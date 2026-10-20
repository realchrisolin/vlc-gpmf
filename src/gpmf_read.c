/*
 * gpmf_read.c — MP4 gpmd index and GPMF KLV decoder.
 *
 * Sample times are presentation times. One positive edit list maps
 * media time M to M - (elst.media_time / mdhd.timescale). The payload
 * window comes from stts. Samples sit on the center of equal slots
 * inside that window. SCAL is applied when the sample is stored.
 *
 * Copyright (C) 2026 Chris Olin
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "gpmf_read.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
# define WIN32_LEAN_AND_MEAN
# include <windows.h>
#else
# include <pthread.h>
#endif

#define ID_MOOV GPMF_FOURCC('m','o','o','v')
#define ID_MVHD GPMF_FOURCC('m','v','h','d')
#define ID_TRAK GPMF_FOURCC('t','r','a','k')
#define ID_MDIA GPMF_FOURCC('m','d','i','a')
#define ID_MDHD GPMF_FOURCC('m','d','h','d')
#define ID_MINF GPMF_FOURCC('m','i','n','f')
#define ID_STBL GPMF_FOURCC('s','t','b','l')
#define ID_STSD GPMF_FOURCC('s','t','s','d')
#define ID_STTS GPMF_FOURCC('s','t','t','s')
#define ID_STSC GPMF_FOURCC('s','t','s','c')
#define ID_STSZ GPMF_FOURCC('s','t','s','z')
#define ID_STCO GPMF_FOURCC('s','t','c','o')
#define ID_CO64 GPMF_FOURCC('c','o','6','4')
#define ID_EDTS GPMF_FOURCC('e','d','t','s')
#define ID_ELST GPMF_FOURCC('e','l','s','t')
#define ID_UDTA GPMF_FOURCC('u','d','t','a')
#define ID_TKHD GPMF_FOURCC('t','k','h','d')
#define ID_GPMD GPMF_FOURCC('g','p','m','d')

#define ID_DEVC GPMF_FOURCC('D','E','V','C')
#define ID_DVID GPMF_FOURCC('D','V','I','D')
#define ID_DVNM GPMF_FOURCC('D','V','N','M')
#define ID_STRM GPMF_FOURCC('S','T','R','M')
#define ID_STNM GPMF_FOURCC('S','T','N','M')
#define ID_SIUN GPMF_FOURCC('S','I','U','N')
#define ID_UNIT GPMF_FOURCC('U','N','I','T')
#define ID_MTRX GPMF_FOURCC('M','T','R','X')
#define ID_ORIN GPMF_FOURCC('O','R','I','N')
#define ID_ORIO GPMF_FOURCC('O','R','I','O')
#define ID_SCAL GPMF_FOURCC('S','C','A','L')
#define ID_TYPE GPMF_FOURCC('T','Y','P','E')
#define ID_TSMP GPMF_FOURCC('T','S','M','P')
#define ID_TICK GPMF_FOURCC('T','I','C','K')
#define ID_TOCK GPMF_FOURCC('T','O','C','K')
#define ID_TIMO GPMF_FOURCC('T','I','M','O')
#define ID_STMP GPMF_FOURCC('S','T','M','P')
#define ID_STPS GPMF_FOURCC('S','T','P','S')
#define ID_PFRM GPMF_FOURCC('P','F','R','M')
#define ID_TMPC GPMF_FOURCC('T','M','P','C')
#define ID_EMPT GPMF_FOURCC('E','M','P','T')
#define ID_QUAN GPMF_FOURCC('Q','U','A','N')
#define ID_VERS GPMF_FOURCC('V','E','R','S')
#define ID_FREE GPMF_FOURCC('F','R','E','E')
#define ID_RMRK GPMF_FOURCC('R','M','R','K')
#define ID_ACCL GPMF_FOURCC('A','C','C','L')
#define ID_GYRO GPMF_FOURCC('G','Y','R','O')
#define ID_GRAV GPMF_FOURCC('G','R','A','V')
#define ID_CORI GPMF_FOURCC('C','O','R','I')
#define ID_IORI GPMF_FOURCC('I','O','R','I')
#define ID_GPS5 GPMF_FOURCC('G','P','S','5')
#define ID_GPS9 GPMF_FOURCC('G','P','S','9')
#define ID_GPSU GPMF_FOURCC('G','P','S','U')
#define ID_GPSF GPMF_FOURCC('G','P','S','F')
#define ID_GPSP GPMF_FOURCC('G','P','S','P')

/* Seconds from 1904-01-01 to 1970-01-01. MP4 creation_time uses the first. */
#define GPMF_MP4_UNIX_EPOCH 2082844800LL

typedef struct {
    double pres_start;
    double pres_dur;
    double media_start;
    double rate;
} edit_t;

typedef struct {
    uint64_t off;
    uint32_t size;
    double t_in;
    double t_out;
    int ready;
} payload_t;

#define GPMF_SERIES_CAP 32

typedef struct gpmf_load {
    FILE *fp;
    payload_t *pays;
    uint32_t n;
    uint8_t *buf;
    uint32_t bufcap;
#ifdef _WIN32
    CRITICAL_SECTION mu;
#else
    pthread_mutex_t mu;
#endif
    int mu_init;
} gpmf_load;

typedef struct {
    int timescale;
    int is_gpmd;
    uint32_t sample_count;
    uint32_t const_size;
    uint32_t *sample_size;
    uint32_t chunk_count;
    uint64_t *chunk_off;
    uint32_t stsc_count;
    uint32_t *stsc_first;
    uint32_t *stsc_per;
    uint32_t stts_count;
    uint32_t *stts_n;
    uint32_t *stts_d;
    int nedit;
    edit_t edits[8];
    uint8_t *elst;
    size_t elst_len;
    int width;
    int height;
} track_t;

typedef struct {
    uint32_t fourcc;
    uint8_t type;
    uint8_t ssize;
    uint16_t repeat;
    const uint8_t *data;
    uint32_t nbytes;
} klv_t;

static void warn_set(gpmf_file *f, const char *msg)
{
    if (f->warning[0] == '\0')
        snprintf(f->warning, sizeof f->warning, "%s", msg);
}

static void fourcc_str(uint32_t id, char out[5])
{
    out[0] = (char)((id >> 24) & 0xff);
    out[1] = (char)((id >> 16) & 0xff);
    out[2] = (char)((id >> 8) & 0xff);
    out[3] = (char)(id & 0xff);
    out[4] = '\0';
}

static uint16_t ru16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static int16_t rs16(const uint8_t *p)
{
    return (int16_t)ru16(p);
}

static uint32_t ru32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int32_t rs32(const uint8_t *p)
{
    return (int32_t)ru32(p);
}

static uint64_t ru64(const uint8_t *p)
{
    return ((uint64_t)ru32(p) << 32) | ru32(p + 4);
}

static int64_t rs64(const uint8_t *p)
{
    return (int64_t)ru64(p);
}

static float rf32(const uint8_t *p)
{
    uint32_t u = ru32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static double rf64(const uint8_t *p)
{
    uint64_t u = ru64(p);
    double d;
    memcpy(&d, &u, 8);
    return d;
}

static int seek_at(FILE *fp, uint64_t off)
{
#ifdef _WIN32
    return _fseeki64(fp, (long long)off, SEEK_SET);
#else
    return fseeko(fp, (off_t)off, SEEK_SET);
#endif
}

static void file_lock(gpmf_file *f)
{
    gpmf_load *L;
    if (!f)
        return;
    L = (gpmf_load *)f->load;
    if (!L || !L->mu_init)
        return;
#ifdef _WIN32
    EnterCriticalSection(&L->mu);
#else
    pthread_mutex_lock(&L->mu);
#endif
}

static void file_unlock(gpmf_file *f)
{
    gpmf_load *L;
    if (!f)
        return;
    L = (gpmf_load *)f->load;
    if (!L || !L->mu_init)
        return;
#ifdef _WIN32
    LeaveCriticalSection(&L->mu);
#else
    pthread_mutex_unlock(&L->mu);
#endif
}

static int64_t file_end(FILE *fp)
{
#ifdef _WIN32
    if (_fseeki64(fp, 0, SEEK_END) != 0)
        return -1;
    return _ftelli64(fp);
#else
    if (fseeko(fp, 0, SEEK_END) != 0)
        return -1;
    return (int64_t)ftello(fp);
#endif
}

static FILE *open_bin(const char *path)
{
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (wlen > 0) {
        wchar_t *w = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
        FILE *fp = NULL;
        if (w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, w, wlen) > 0)
            fp = _wfopen(w, L"rb");
        free(w);
        if (fp)
            return fp;
    }
#endif
    return fopen(path, "rb");
}

static int is_container(uint32_t typ)
{
    return typ == ID_MOOV || typ == ID_TRAK || typ == ID_MDIA || typ == ID_MINF ||
           typ == ID_STBL || typ == ID_EDTS || typ == ID_UDTA;
}

static int is_meta_key(uint32_t id)
{
    switch (id) {
    case ID_DEVC: case ID_DVID: case ID_DVNM: case ID_STRM:
    case ID_STNM: case ID_SIUN: case ID_UNIT: case ID_MTRX:
    case ID_ORIN: case ID_ORIO: case ID_SCAL: case ID_TYPE:
    case ID_TSMP: case ID_TICK: case ID_TOCK: case ID_TIMO:
    case ID_STMP: case ID_STPS: case ID_PFRM: case ID_TMPC:
    case ID_EMPT: case ID_QUAN: case ID_VERS: case ID_FREE:
    case ID_RMRK:
        return 1;
    default:
        return 0;
    }
}

static int keep_key(uint32_t id)
{
    return id == ID_ACCL || id == ID_GYRO || id == ID_GRAV || id == ID_CORI ||
           id == ID_IORI || id == ID_GPS5 || id == ID_GPS9 || id == ID_GPSF ||
           id == ID_GPSP;
}

static int elem_size(char type)
{
    switch (type) {
    case 'b': case 'B': case 'c': return 1;
    case 's': case 'S': return 2;
    case 'f': case 'F': case 'l': case 'L': case 'q': return 4;
    case 'j': case 'J': case 'd': case 'Q': return 8;
    case 'U': case 'G': return 16;
    default: return 0;
    }
}

static void track_clear(track_t *t)
{
    free(t->sample_size);
    free(t->chunk_off);
    free(t->stsc_first);
    free(t->stsc_per);
    free(t->stts_n);
    free(t->stts_d);
    free(t->elst);
    memset(t, 0, sizeof *t);
}

static int parse_mdhd(const uint8_t *p, size_t n, track_t *t)
{
    if (n < 4)
        return -1;
    uint8_t ver = p[0];
    if (ver == 0) {
        if (n < 20)
            return -1;
        t->timescale = (int)ru32(p + 12);
    } else {
        if (n < 28)
            return -1;
        t->timescale = (int)ru32(p + 20);
    }
    return t->timescale > 0 ? 0 : -1;
}

static int parse_elst(const uint8_t *p, size_t n, track_t *t, int movie_ts)
{
    if (n < 8 || movie_ts <= 0)
        return -1;
    uint8_t ver = p[0];
    uint32_t count = ru32(p + 4);
    size_t pos = 8;
    double pres = 0;
    t->nedit = 0;
    for (uint32_t i = 0; i < count && t->nedit < 8; i++) {
        edit_t e;
        memset(&e, 0, sizeof e);
        e.pres_start = pres;
        if (ver == 0) {
            if (pos + 12 > n)
                break;
            uint32_t dur = ru32(p + pos);
            int32_t mt = rs32(p + pos + 4);
            int16_t ri = rs16(p + pos + 8);
            uint16_t rf = ru16(p + pos + 10);
            e.pres_dur = (double)dur / (double)movie_ts;
            e.media_start = mt < 0 ? -1.0 : (double)mt / (double)t->timescale;
            e.rate = (double)ri + (double)rf / 65536.0;
            pos += 12;
        } else {
            if (pos + 20 > n)
                break;
            uint64_t dur = ru64(p + pos);
            int64_t mt = rs64(p + pos + 8);
            int16_t ri = rs16(p + pos + 16);
            uint16_t rf = ru16(p + pos + 18);
            e.pres_dur = (double)dur / (double)movie_ts;
            e.media_start = mt < 0 ? -1.0 : (double)mt / (double)t->timescale;
            e.rate = (double)ri + (double)rf / 65536.0;
            pos += 20;
        }
        if (!(e.rate > 0.0))
            e.rate = 1.0;
        pres += e.pres_dur;
        t->edits[t->nedit++] = e;
    }
    return 0;
}

static int parse_stsd(const uint8_t *p, size_t n, track_t *t)
{
    if (n < 8)
        return -1;
    uint32_t entries = ru32(p + 4);
    size_t pos = 8;
    for (uint32_t i = 0; i < entries && pos + 8 <= n; i++) {
        uint32_t sz = ru32(p + pos);
        uint32_t fmt = ru32(p + pos + 4);
        if (sz < 8 || pos + sz > n)
            break;
        if (fmt == ID_GPMD)
            t->is_gpmd = 1;
        pos += sz;
    }
    return 0;
}

static int parse_stts(const uint8_t *p, size_t n, track_t *t)
{
    if (n < 8)
        return -1;
    uint32_t count = ru32(p + 4);
    if (count > 1000000 || 8 + (size_t)count * 8 > n)
        return -1;
    t->stts_n = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
    t->stts_d = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
    if (!t->stts_n || !t->stts_d)
        return -1;
    t->stts_count = count;
    for (uint32_t i = 0; i < count; i++) {
        t->stts_n[i] = ru32(p + 8 + i * 8);
        t->stts_d[i] = ru32(p + 12 + i * 8);
    }
    return 0;
}

static int parse_stsc(const uint8_t *p, size_t n, track_t *t)
{
    if (n < 8)
        return -1;
    uint32_t count = ru32(p + 4);
    if (count > 1000000 || 8 + (size_t)count * 12 > n)
        return -1;
    t->stsc_first = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
    t->stsc_per = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
    if (!t->stsc_first || !t->stsc_per)
        return -1;
    t->stsc_count = count;
    for (uint32_t i = 0; i < count; i++) {
        t->stsc_first[i] = ru32(p + 8 + i * 12);
        t->stsc_per[i] = ru32(p + 12 + i * 12);
    }
    return 0;
}

static int parse_stsz(const uint8_t *p, size_t n, track_t *t)
{
    if (n < 12)
        return -1;
    t->const_size = ru32(p + 4);
    t->sample_count = ru32(p + 8);
    if (t->sample_count > 2000000)
        return -1;
    if (t->const_size == 0) {
        if (12 + (size_t)t->sample_count * 4 > n)
            return -1;
        t->sample_size = (uint32_t *)malloc((size_t)t->sample_count * sizeof(uint32_t));
        if (!t->sample_size)
            return -1;
        for (uint32_t i = 0; i < t->sample_count; i++)
            t->sample_size[i] = ru32(p + 12 + i * 4);
    }
    return 0;
}

static int parse_stco(const uint8_t *p, size_t n, track_t *t, int wide)
{
    if (n < 8)
        return -1;
    uint32_t count = ru32(p + 4);
    size_t need = wide ? 8 : 4;
    if (count > 2000000 || 8 + (size_t)count * need > n)
        return -1;
    t->chunk_off = (uint64_t *)malloc((size_t)count * sizeof(uint64_t));
    if (!t->chunk_off)
        return -1;
    t->chunk_count = count;
    for (uint32_t i = 0; i < count; i++) {
        if (wide)
            t->chunk_off[i] = ru64(p + 8 + i * 8);
        else
            t->chunk_off[i] = ru32(p + 8 + i * 4);
    }
    return 0;
}

static int parse_mvhd(gpmf_file *file, const uint8_t *p, size_t n, int *movie_ts)
{
    uint64_t creation = 0;
    if (n < 4)
        return -1;
    uint8_t ver = p[0];
    if (ver == 0) {
        if (n < 16)
            return -1;
        creation = ru32(p + 4);
        *movie_ts = (int)ru32(p + 12);
    } else {
        if (n < 24)
            return -1;
        creation = ru64(p + 4);
        *movie_ts = (int)ru32(p + 20);
    }
    if (file && creation > (uint64_t)GPMF_MP4_UNIX_EPOCH && !file->clock_valid) {
        file->clock_unix0 = (double)creation - (double)GPMF_MP4_UNIX_EPOCH;
        file->clock_valid = 1;
        file->clock_source = GPMF_CLOCK_FILE;
    }
    return *movie_ts > 0 ? 0 : -1;
}

static double media_to_pres(const track_t *t, double media_sec)
{
    int i;
    if (t->nedit == 0)
        return media_sec;
    for (i = 0; i < t->nedit; i++) {
        const edit_t *e = &t->edits[i];
        double rate;
        double m1;
        if (e->media_start < 0)
            continue;
        rate = e->rate > 0 ? e->rate : 1.0;
        m1 = e->media_start + e->pres_dur * rate;
        if (media_sec >= e->media_start && media_sec <= m1)
            return e->pres_start + (media_sec - e->media_start) / rate;
    }
    for (i = 0; i < t->nedit; i++) {
        const edit_t *e = &t->edits[i];
        double rate;
        if (e->media_start < 0)
            continue;
        rate = e->rate > 0 ? e->rate : 1.0;
        return e->pres_start + (media_sec - e->media_start) / rate;
    }
    return media_sec;
}

static uint32_t samples_in_chunk(const track_t *t, uint32_t chunk_1based)
{
    uint32_t per = 0;
    uint32_t i;
    for (i = 0; i < t->stsc_count; i++) {
        if (t->stsc_first[i] <= chunk_1based)
            per = t->stsc_per[i];
        else
            break;
    }
    return per;
}

static void note_seen(gpmf_file *f, uint32_t id, int samples, int stored)
{
    int i;
    for (i = 0; i < f->seen_count; i++) {
        if (f->seen[i].fourcc == id) {
            f->seen[i].samples += samples;
            if (stored)
                f->seen[i].stored = 1;
            return;
        }
    }
    if (f->seen_count >= 32)
        return;
    f->seen[f->seen_count].fourcc = id;
    fourcc_str(id, f->seen[f->seen_count].fourcc_s);
    f->seen[f->seen_count].samples = samples;
    f->seen[f->seen_count].stored = stored;
    f->seen_count++;
}

static gpmf_series *series_get(gpmf_file *f, uint32_t id, int dvid, int channels)
{
    int i;
    gpmf_series *s;
    for (i = 0; i < f->series_count; i++) {
        if (f->series[i].fourcc == id && f->series[i].dvid == dvid)
            return &f->series[i];
    }
    if (!f->series) {
        f->series = (gpmf_series *)calloc(GPMF_SERIES_CAP, sizeof(gpmf_series));
        if (!f->series)
            return NULL;
    }
    if (f->series_count >= GPMF_SERIES_CAP) {
        warn_set(f, "too many telemetry streams; later ones were skipped");
        return NULL;
    }
    s = &f->series[f->series_count];
    memset(s, 0, sizeof *s);
    s->fourcc = id;
    fourcc_str(id, s->fourcc_s);
    s->dvid = dvid;
    s->owner = f;
    s->channels = channels;
    s->fix_col = -1;
    if (id == ID_GPS9 && channels > 8)
        s->fix_col = 8;
    if (id == ID_GPSF && channels > 0)
        s->fix_col = 0;
    f->series_count++;
    return s;
}

static int series_reserve(gpmf_series *s, int extra)
{
    int need;
    int cap;
    float *nv;
    if (extra < 0 || s->count > 200000000 - extra)
        return -1;
    need = s->count + extra;
    if (need <= s->capacity)
        return 0;
    cap = s->capacity > 0 ? s->capacity : 4096;
    while (cap < need) {
        if (cap > 100000000)
            return -1;
        cap *= 2;
    }
    if ((size_t)cap > SIZE_MAX / ((size_t)s->channels * sizeof(float)))
        return -1;
    nv = (float *)realloc(s->values, (size_t)cap * (size_t)s->channels * sizeof(float));
    if (!nv)
        return -1;
    s->values = nv;
    s->capacity = cap;
    return 0;
}

static int series_add_run(gpmf_series *s, double t0, double dt, int count)
{
    gpmf_run *r;
    int i, place, begin;
    if (count <= 0)
        return 0;
    begin = s->count - count;
    place = s->run_count;
    for (i = 0; i < s->run_count; i++) {
        if (t0 < s->runs[i].t0) {
            place = i;
            break;
        }
    }
    /* Samples were just appended. Merge only when they sit directly after
     * the previous run's values, which is the in-order case. */
    if (place > 0) {
        gpmf_run *prev = &s->runs[place - 1];
        double expect = prev->t0 + (double)prev->count * prev->dt;
        if (fabs(prev->dt - dt) < 1e-9 && fabs(expect - t0) < 1e-4 &&
            prev->begin + prev->count == begin) {
            prev->count += count;
            return 0;
        }
    }
    if (s->run_count + 1 > s->run_capacity) {
        int cap = s->run_capacity > 0 ? s->run_capacity * 2 : 64;
        r = (gpmf_run *)realloc(s->runs, (size_t)cap * sizeof(gpmf_run));
        if (!r)
            return -1;
        s->runs = r;
        s->run_capacity = cap;
    }
    if (place < s->run_count) {
        memmove(&s->runs[place + 1], &s->runs[place],
                (size_t)(s->run_count - place) * sizeof(gpmf_run));
    }
    r = &s->runs[place];
    r->t0 = t0;
    r->dt = dt;
    r->begin = begin;
    r->count = count;
    s->run_count++;
    return 0;
}

static int read_number(char type, const uint8_t *p, double *out)
{
    switch (type) {
    case 'b': *out = (double)(int8_t)p[0]; return 1;
    case 'B': *out = (double)p[0]; return 1;
    case 's': *out = (double)rs16(p); return 2;
    case 'S': *out = (double)ru16(p); return 2;
    case 'l': *out = (double)rs32(p); return 4;
    case 'L': *out = (double)ru32(p); return 4;
    case 'f': *out = (double)rf32(p); return 4;
    case 'q': *out = (double)rs32(p) / 65536.0; return 4;
    case 'j': *out = (double)rs64(p); return 8;
    case 'J': *out = (double)ru64(p); return 8;
    case 'd': *out = rf64(p); return 8;
    case 'Q': *out = (double)rs64(p) / 4294967296.0; return 8;
    default: return 0;
    }
}

static int decode_struct(char type, const char *typestr, const uint8_t *p, int ssize,
                         float *dst, int maxch)
{
    int ch = 0;
    int off = 0;
    if (type == '?') {
        int i;
        if (!typestr || !typestr[0])
            return -1;
        for (i = 0; typestr[i] && ch < maxch; i++) {
            double v;
            int es = elem_size(typestr[i]);
            int n;
            if (es <= 0 || off + es > ssize)
                return -1;
            n = read_number(typestr[i], p + off, &v);
            if (n <= 0)
                return -1;
            dst[ch++] = (float)v;
            off += es;
        }
        return ch;
    }
    {
        int es = elem_size(type);
        if (es <= 0 || ssize < es || (ssize % es) != 0)
            return -1;
        ch = ssize / es;
        if (ch > maxch)
            ch = maxch;
        for (int i = 0; i < ch; i++) {
            double v;
            if (read_number(type, p + i * es, &v) <= 0)
                return -1;
            dst[i] = (float)v;
        }
        return ch;
    }
}

static void copy_name(char *dst, size_t dstlen, const uint8_t *p, uint32_t n)
{
    size_t i, w = 0;
    if (dstlen == 0)
        return;
    for (i = 0; i < n && w + 1 < dstlen; i++) {
        unsigned char c = p[i];
        if (c == 0)
            break;
        if (c < 32 || c > 126)
            c = ' ';
        dst[w++] = (char)c;
    }
    while (w > 0 && dst[w - 1] == ' ')
        w--;
    dst[w] = '\0';
}

static int read_scale(const klv_t *k, double *scale, int maxn)
{
    int es = elem_size((char)k->type);
    int count;
    int i;
    if (es <= 0 || k->ssize < es || (k->ssize % es) != 0)
        return 0;
    count = (k->ssize / es) * (int)k->repeat;
    if (count > maxn)
        count = maxn;
    for (i = 0; i < count; i++) {
        double v;
        if (read_number((char)k->type, k->data + i * es, &v) <= 0)
            return i;
        scale[i] = v;
    }
    return count;
}

static void remember_stream(gpmf_file *f, gpmf_series *s, int dvid, const char *name,
                            const char *orin, double scale0)
{
    (void)dvid;
    if (s->stnm[0] == '\0' && name && name[0])
        snprintf(s->stnm, sizeof s->stnm, "%s", name);
    if (s->orin[0] == '\0' && orin && orin[0])
        snprintf(s->orin, sizeof s->orin, "%.3s", orin);
    if (s->scale0 == 0 && scale0 != 0)
        s->scale0 = scale0;
    (void)f;
}

static int store_samples(gpmf_file *f, int dvid, const klv_t *sample, const char *typestr,
                         const double *scale, int nscale, const char *orin, double timo,
                         const char *stnm, double t_in, double t_out)
{
    float row[GPMF_MAX_CH];
    int ch;
    int apply_orin;
    gpmf_series *s;
    int base;
    double dt;
    double t0;
    int i;
    if (sample->repeat == 0 || sample->type == 0 || sample->type == '#')
        return 0;
    if (t_out <= t_in)
        return 0;
    ch = decode_struct((char)sample->type, typestr, sample->data, sample->ssize, row, GPMF_MAX_CH);
    if (ch <= 0)
        return 0;
    s = series_get(f, sample->fourcc, dvid, ch);
    if (!s)
        return -1;
    if (s->channels != ch) {
        warn_set(f, "a stream changed channel count; later samples were skipped");
        return 0;
    }
    if (series_reserve(s, (int)sample->repeat) != 0)
        return -1;
    apply_orin = (orin && orin[0] && ch == 3);
    base = s->count;
    for (i = 0; i < (int)sample->repeat; i++) {
        float tmp[GPMF_MAX_CH];
        int c;
        const uint8_t *p = sample->data + (size_t)i * sample->ssize;
        if (decode_struct((char)sample->type, typestr, p, sample->ssize, tmp, GPMF_MAX_CH) != ch)
            return 0;
        for (c = 0; c < ch; c++) {
            double sc = 1.0;
            float v = tmp[c];
            if (nscale > 0) {
                sc = scale[c < nscale ? c : 0];
                if (sc == 0)
                    sc = 1.0;
                v = (float)((double)v / sc);
            }
            if (apply_orin && c < 3) {
                char ax = orin[c];
                if (ax >= 'a' && ax <= 'z')
                    v = -v;
            }
            s->values[(size_t)(base + i) * (size_t)ch + c] = v;
        }
    }
    s->count = base + (int)sample->repeat;
    dt = (t_out - t_in) / (double)sample->repeat;
    t0 = (t_in - timo) + 0.5 * dt;
    if (series_add_run(s, t0, dt, (int)sample->repeat) != 0)
        return -1;
    remember_stream(f, s, dvid, stnm, orin, nscale > 0 ? scale[0] : 0);
    note_seen(f, sample->fourcc, (int)sample->repeat, 1);
    return 0;
}

static int take_digits(const uint8_t *s, int n, int *out)
{
    int v = 0;
    int i;
    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return 0;
}

/* Days from civil y-m-d to 1970-01-01, then the clock time. Howard Hinnant. */
static int civil_to_unix(int y, int mo, int d, int h, int mi, double sec, double *out)
{
    int64_t y2, m2, era, yoe, doy, doe, days;
    if (y < 1970 || y > 2100 || mo < 1 || mo > 12 || d < 1 || d > 31)
        return -1;
    if (h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec >= 62)
        return -1;
    y2 = y;
    m2 = mo;
    y2 -= m2 <= 2;
    era = (y2 >= 0 ? y2 : y2 - 399) / 400;
    yoe = y2 - era * 400;
    doy = (153 * (m2 + (m2 > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    days = era * 146097 + doe - 719468;
    *out = (double)days * 86400.0 + (double)h * 3600.0 + (double)mi * 60.0 + sec;
    return 0;
}

/* GPSU is 16 ASCII bytes: YYMMDDHHMMSS.sss, UTC. One sample anchors the file. */
static void take_gpsu(gpmf_file *f, const klv_t *k, double t_in, double t_out, double timo)
{
    int yy, mo, d, h, mi, ss, ms;
    double unix_s, pres, dt;
    const uint8_t *s;
    if (!f || f->clock_source == GPMF_CLOCK_GPSU || f->clock_source == GPMF_CLOCK_GPS9)
        return;
    if (k->repeat < 1 || k->nbytes < 16)
        return;
    s = k->data;
    if (s[12] != '.')
        return;
    if (take_digits(s, 2, &yy) || take_digits(s + 2, 2, &mo) ||
        take_digits(s + 4, 2, &d) || take_digits(s + 6, 2, &h) ||
        take_digits(s + 8, 2, &mi) || take_digits(s + 10, 2, &ss) ||
        take_digits(s + 13, 3, &ms))
        return;
    if (civil_to_unix(2000 + yy, mo, d, h, mi, (double)ss + (double)ms / 1000.0, &unix_s) != 0)
        return;
    dt = (k->repeat > 0 && t_out > t_in) ? (t_out - t_in) / (double)k->repeat : 0;
    pres = (t_in - timo) + 0.5 * dt;
    f->clock_unix0 = unix_s - pres;
    f->clock_valid = 1;
    f->clock_source = GPMF_CLOCK_GPSU;
}

static int parse_stream(gpmf_file *f, int dvid, const uint8_t *buf, uint32_t len,
                        double t_in, double t_out)
{
    klv_t keys[48];
    int nkeys = 0;
    uint32_t pos = 0;
    int sample_i = -1;
    double scale[GPMF_MAX_CH];
    int nscale = 0;
    char typestr[32];
    char orin[4];
    char stnm[64];
    double timo = 0;
    int i;
    memset(typestr, 0, sizeof typestr);
    memset(orin, 0, sizeof orin);
    memset(stnm, 0, sizeof stnm);
    while (pos + 8 <= len && nkeys < 48) {
        klv_t k;
        uint32_t padded;
        k.fourcc = ru32(buf + pos);
        k.type = buf[pos + 4];
        k.ssize = buf[pos + 5];
        k.repeat = ru16(buf + pos + 6);
        k.nbytes = (uint32_t)k.ssize * (uint32_t)k.repeat;
        padded = (k.nbytes + 3u) & ~3u;
        if (pos + 8 + padded > len)
            break;
        k.data = buf + pos + 8;
        keys[nkeys++] = k;
        pos += 8 + padded;
    }
    for (i = 0; i < nkeys; i++) {
        if (keys[i].type != 0 && !is_meta_key(keys[i].fourcc))
            sample_i = i;
    }
    for (i = 0; i < nkeys; i++) {
        klv_t *k = &keys[i];
        if (k->fourcc == ID_SCAL)
            nscale = read_scale(k, scale, GPMF_MAX_CH);
        else if (k->fourcc == ID_TYPE && k->nbytes > 0)
            copy_name(typestr, sizeof typestr, k->data, k->nbytes);
        else if (k->fourcc == ID_ORIN && k->nbytes >= 3)
            copy_name(orin, sizeof orin, k->data, 3);
        else if (k->fourcc == ID_STNM && k->nbytes > 0)
            copy_name(stnm, sizeof stnm, k->data, k->nbytes);
        else if (k->fourcc == ID_TIMO && k->nbytes >= 4 && k->type == 'f')
            timo = (double)rf32(k->data);
    }
    for (i = 0; i < nkeys; i++) {
        if (keys[i].fourcc == ID_GPSU)
            take_gpsu(f, &keys[i], t_in, t_out, timo);
    }
    if (sample_i < 0)
        return 0;
    if (keys[sample_i].type == '#') {
        warn_set(f, "Huffman payload skipped");
        note_seen(f, keys[sample_i].fourcc, (int)keys[sample_i].repeat, 0);
        return 0;
    }
    if (!keep_key(keys[sample_i].fourcc)) {
        note_seen(f, keys[sample_i].fourcc, (int)keys[sample_i].repeat, 0);
        return 0;
    }
    return store_samples(f, dvid, &keys[sample_i], typestr, scale, nscale, orin, timo,
                         stnm, t_in, t_out);
}

static int parse_sequence(gpmf_file *f, int *dvid, char *dvnm, size_t dvnm_len,
                          const uint8_t *buf, uint32_t len, int depth,
                          double t_in, double t_out)
{
    uint32_t pos = 0;
    if (depth > 16)
        return 0;
    while (pos + 8 <= len) {
        uint8_t type = buf[pos + 4];
        uint8_t ssize = buf[pos + 5];
        uint16_t repeat = ru16(buf + pos + 6);
        uint32_t nbytes = (uint32_t)ssize * (uint32_t)repeat;
        uint32_t padded = (nbytes + 3u) & ~3u;
        uint32_t fourcc;
        if (pos + 8 + padded > len)
            break;
        fourcc = ru32(buf + pos);
        if (type == 0 && fourcc == ID_DEVC) {
            int local_id = 0;
            char local_name[64];
            local_name[0] = '\0';
            if (parse_sequence(f, &local_id, local_name, sizeof local_name,
                               buf + pos + 8, nbytes, depth + 1, t_in, t_out) != 0)
                return -1;
            if (f->device[0] == '\0' && local_name[0]) {
                snprintf(f->device, sizeof f->device, "%s", local_name);
                f->dvid = local_id;
            }
        } else if (type == 0 && fourcc == ID_STRM) {
            if (parse_stream(f, *dvid, buf + pos + 8, nbytes, t_in, t_out) != 0)
                return -1;
        } else if (type == 0) {
            if (parse_sequence(f, dvid, dvnm, dvnm_len, buf + pos + 8, nbytes,
                               depth + 1, t_in, t_out) != 0)
                return -1;
        } else if (fourcc == ID_DVID && nbytes >= 1) {
            double v = 0;
            if (read_number((char)type, buf + pos + 8, &v) > 0)
                *dvid = (int)v;
        } else if (fourcc == ID_DVNM && nbytes > 0) {
            copy_name(dvnm, dvnm_len, buf + pos + 8, nbytes);
        }
        pos += 8 + padded;
    }
    return 0;
}

typedef struct {
    gpmf_file *file;
    int movie_ts;
    int committed;
    char *err;
    size_t errlen;
} mp4_ctx;

static void set_err(mp4_ctx *ctx, const char *msg)
{
    if (ctx->err && ctx->errlen)
        snprintf(ctx->err, ctx->errlen, "%s", msg);
}

static int atom_at(const uint8_t *b, size_t len, size_t pos,
                   uint32_t *typ, uint64_t *size, size_t *hdr)
{
    uint32_t size32;
    if (pos + 8 > len)
        return -1;
    size32 = ru32(b + pos);
    *typ = ru32(b + pos + 4);
    *hdr = 8;
    *size = size32;
    if (size32 == 1) {
        if (pos + 16 > len)
            return -1;
        *size = ru64(b + pos + 8);
        *hdr = 16;
    } else if (size32 == 0) {
        *size = (uint64_t)(len - pos);
    }
    if (*size < *hdr || pos + (size_t)*size > len)
        return -1;
    return 0;
}

static void parse_tkhd(const uint8_t *p, size_t n, track_t *trak)
{
    size_t off;
    uint32_t w, h;
    if (n < 4)
        return;
    if (p[0] == 0)
        off = 76;
    else if (p[0] == 1)
        off = 88;
    else
        return;
    if (n < off + 8)
        return;
    w = ru32(p + off);
    h = ru32(p + off + 4);
    trak->width = (int)(w >> 16);
    trak->height = (int)(h >> 16);
}

static int on_leaf(uint32_t typ, const uint8_t *p, size_t n, track_t *trak)
{
    if (!trak)
        return 0;
    if (typ == ID_TKHD) {
        parse_tkhd(p, n, trak);
        return 0;
    }
    if (typ == ID_MDHD)
        return parse_mdhd(p, n, trak);
    if (typ == ID_ELST) {
        free(trak->elst);
        trak->elst = (uint8_t *)malloc(n);
        if (!trak->elst)
            return -1;
        memcpy(trak->elst, p, n);
        trak->elst_len = n;
        return 0;
    }
    if (typ == ID_STSD)
        return parse_stsd(p, n, trak);
    if (typ == ID_STTS)
        return parse_stts(p, n, trak);
    if (typ == ID_STSC)
        return parse_stsc(p, n, trak);
    if (typ == ID_STSZ)
        return parse_stsz(p, n, trak);
    if (typ == ID_STCO)
        return parse_stco(p, n, trak, 0);
    if (typ == ID_CO64)
        return parse_stco(p, n, trak, 1);
    return 0;
}

static int commit_track(mp4_ctx *ctx, track_t *t)
{
    payload_t *pays;
    uint32_t n;
    uint32_t produced = 0;
    uint64_t tick = 0;
    uint32_t si = 0;
    uint32_t c;
    uint32_t e;
    double tscale;
    gpmf_load *load;

    if (!t->is_gpmd)
        return 0;
    if (t->timescale <= 0 || t->sample_count == 0 || t->chunk_count == 0 || t->stsc_count == 0) {
        set_err(ctx, "gpmd track is missing its sample index");
        return -1;
    }
    if (t->elst && t->elst_len)
        parse_elst(t->elst, t->elst_len, t, ctx->movie_ts);

    n = t->sample_count;
    pays = (payload_t *)calloc(n, sizeof(payload_t));
    if (!pays) {
        set_err(ctx, "out of memory");
        return -1;
    }
    tscale = (double)t->timescale;
    for (e = 0; e < t->stts_count && produced < n; e++) {
        uint32_t cnt = t->stts_n[e];
        uint32_t delta = t->stts_d[e];
        uint32_t j;
        for (j = 0; j < cnt && produced < n; j++) {
            double m_in = (double)tick / tscale;
            tick += delta;
            pays[produced].t_in = media_to_pres(t, m_in);
            pays[produced].t_out = media_to_pres(t, (double)tick / tscale);
            produced++;
        }
    }
    for (c = 1; c <= t->chunk_count && si < n; c++) {
        uint32_t per = samples_in_chunk(t, c);
        uint64_t off = t->chunk_off[c - 1];
        uint32_t k;
        for (k = 0; k < per && si < n; k++) {
            uint32_t sz = t->const_size ? t->const_size : t->sample_size[si];
            pays[si].off = off;
            pays[si].size = sz;
            off += sz;
            si++;
        }
    }
    if (produced == 0 || si == 0) {
        free(pays);
        set_err(ctx, "gpmd track has no samples");
        return -1;
    }
    if (produced != t->sample_count || si != t->sample_count)
        warn_set(ctx->file, "gpmd index does not cover every sample");
    n = produced < si ? produced : si;
    ctx->file->payloads = (int)n;
    ctx->file->t_first = pays[0].t_in;
    ctx->file->duration = pays[n - 1].t_out;
    load = (gpmf_load *)ctx->file->load;
    if (!load) {
        free(pays);
        set_err(ctx, "out of memory");
        return -1;
    }
    free(load->pays);
    load->pays = pays;
    load->n = n;
    ctx->committed = 1;
    return 0;
}

static int walk_atoms(mp4_ctx *ctx, const uint8_t *b, size_t len, track_t *trak)
{
    size_t pos = 0;
    while (pos + 8 <= len) {
        uint32_t typ;
        uint64_t size;
        size_t hdr;
        const uint8_t *payload;
        size_t plen;
        if (atom_at(b, len, pos, &typ, &size, &hdr) != 0)
            return -1;
        payload = b + pos + hdr;
        plen = (size_t)(size - hdr);
        if (typ == ID_TRAK && !trak) {
            track_t child;
            memset(&child, 0, sizeof child);
            if (walk_atoms(ctx, payload, plen, &child) != 0) {
                track_clear(&child);
                return -1;
            }
            if ((long long)child.width * child.height >
                (long long)ctx->file->video_w * ctx->file->video_h) {
                ctx->file->video_w = child.width;
                ctx->file->video_h = child.height;
            }
            if (child.is_gpmd && !ctx->committed) {
                if (commit_track(ctx, &child) != 0) {
                    track_clear(&child);
                    return -1;
                }
            }
            track_clear(&child);
        } else if (is_container(typ)) {
            if (walk_atoms(ctx, payload, plen, trak) != 0)
                return -1;
        } else if (on_leaf(typ, payload, plen, trak) != 0) {
            return -1;
        }
        pos += (size_t)size;
    }
    return 0;
}

static int find_mvhd(gpmf_file *file, const uint8_t *b, size_t len, int *movie_ts)
{
    size_t pos = 0;
    while (pos + 8 <= len) {
        uint32_t typ;
        uint64_t size;
        size_t hdr;
        if (atom_at(b, len, pos, &typ, &size, &hdr) != 0)
            return -1;
        if (typ == ID_MVHD)
            return parse_mvhd(file, b + pos + hdr, (size_t)(size - hdr), movie_ts);
        if (is_container(typ)) {
            if (find_mvhd(file, b + pos + hdr, (size_t)(size - hdr), movie_ts) == 0 && *movie_ts > 0)
                return 0;
        }
        pos += (size_t)size;
    }
    return -1;
}

static double sample_pres(const gpmf_series *s, int index)
{
    int i;
    for (i = 0; i < s->run_count; i++) {
        const gpmf_run *r = &s->runs[i];
        if (index >= r->begin && index < r->begin + r->count)
            return r->t0 + (double)(index - r->begin) * r->dt;
    }
    return 0;
}

/* GPS9 columns: lat, lon, alt, 2D speed, 3D speed, days since 2000-01-01,
 * seconds since midnight, DOP, fix. UTC. */
static void clock_from_gps9(gpmf_file *f)
{
    const gpmf_series *s;
    int i;
    if (!f || f->clock_source == GPMF_CLOCK_GPSU || f->clock_source == GPMF_CLOCK_GPS9)
        return;
    s = gpmf_series_find(f, ID_GPS9);
    if (!s || s->channels < 7 || s->count < 1)
        return;
    for (i = 0; i < s->count; i++) {
        const float *v = s->values + (size_t)i * (size_t)s->channels;
        double days, secs;
        if (s->fix_col >= 0 && s->fix_col < s->channels && v[s->fix_col] < 0.5f)
            continue;
        days = (double)v[5];
        secs = (double)v[6];
        if (days < 0 || days > 60000 || secs < 0 || secs >= 86400)
            continue;
        f->clock_unix0 = 946684800.0 + days * 86400.0 + secs - sample_pres(s, i);
        f->clock_valid = 1;
        f->clock_source = GPMF_CLOCK_GPS9;
        return;
    }
}

const gpmf_series *gpmf_series_find(const gpmf_file *file, uint32_t fourcc)
{
    int i;
    const gpmf_series *found = NULL;
    if (!file)
        return NULL;
    file_lock((gpmf_file *)file);
    for (i = 0; i < file->series_count; i++) {
        if (file->series[i].fourcc == fourcc) {
            found = &file->series[i];
            break;
        }
    }
    file_unlock((gpmf_file *)file);
    return found;
}

static int sample_unlocked(const gpmf_series *s, double t, float *out)
{
    int lo, hi, ans;
    const gpmf_run *r;
    int i0, i1, a, b, c;
    double frac;
    if (!s || !out || s->count <= 0 || s->run_count <= 0 || s->channels <= 0)
        return -1;
    lo = 0;
    hi = s->run_count - 1;
    ans = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (s->runs[mid].t0 <= t) {
            ans = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (ans < 0) {
        r = &s->runs[0];
        if (!(r->dt > 0) || t < r->t0 - 0.5 * r->dt)
            return -1;
        ans = 0;
    }
    r = &s->runs[ans];
    if (!(r->dt > 0)) {
        if (fabs(t - r->t0) > 0.05)
            return -1;
        i0 = 0;
        i1 = 0;
        frac = 0;
    } else {
        double end = r->t0 + (double)(r->count - 1) * r->dt;
        double x;
        if (t > end + 0.5 * r->dt) {
            if (ans + 1 < s->run_count) {
                const gpmf_run *next = &s->runs[ans + 1];
                double span = next->t0 - end;
                if (span > 0 && t < next->t0 && span < (r->dt + next->dt) * 1.5) {
                    frac = (t - end) / span;
                    a = r->begin + r->count - 1;
                    b = next->begin;
                    goto mix;
                }
            }
            if (t > end + r->dt)
                return -1;
            i0 = r->count - 1;
            i1 = i0;
            frac = 0;
        } else {
            x = (t - r->t0) / r->dt;
            if (x < 0)
                x = 0;
            i0 = (int)x;
            if (i0 >= r->count)
                i0 = r->count - 1;
            if (i0 < 0)
                i0 = 0;
            i1 = i0 + 1;
            if (i1 >= r->count) {
                /* A seek can store the next payload before this one, so a
                 * continuous series is two runs. Blend into that neighbor
                 * the same way a merged run would. */
                if (ans + 1 < s->run_count) {
                    const gpmf_run *next = &s->runs[ans + 1];
                    double expect = end + r->dt;
                    if (next->count > 0 && fabs(next->dt - r->dt) < 1e-9 &&
                        fabs(next->t0 - expect) < 1e-4) {
                        a = r->begin + i0;
                        b = next->begin;
                        frac = x - (double)i0;
                        if (frac < 0)
                            frac = 0;
                        if (frac > 1)
                            frac = 1;
                        goto mix;
                    }
                }
                i1 = i0;
            }
            frac = x - (double)i0;
            if (frac < 0)
                frac = 0;
            if (frac > 1)
                frac = 1;
        }
    }
    a = r->begin + i0;
    b = r->begin + i1;
mix:
    if (a < 0 || b < 0 || a >= s->count || b >= s->count)
        return -1;
    if (s->fix_col >= 0 && s->fix_col < s->channels) {
        float fa = s->values[(size_t)a * (size_t)s->channels + s->fix_col];
        float fb = s->values[(size_t)b * (size_t)s->channels + s->fix_col];
        if (fa < 0.5f || fb < 0.5f)
            return -1;
    }
    for (c = 0; c < s->channels; c++) {
        float va = s->values[(size_t)a * (size_t)s->channels + (size_t)c];
        float vb = s->values[(size_t)b * (size_t)s->channels + (size_t)c];
        out[c] = va + (float)((double)(vb - va) * frac);
    }
    return s->channels;
}

int gpmf_sample(const gpmf_series *s, double t, float *out)
{
    int n;
    if (!s)
        return -1;
    file_lock((gpmf_file *)s->owner);
    n = sample_unlocked(s, t, out);
    file_unlock((gpmf_file *)s->owner);
    return n;
}

/* GRAV on this camera is not stored in the accelerometer's ORIN order.
 * Pick the signed permutation that makes the two agree, once per file. */
static void align_grav(gpmf_file *file)
{
    static const int perms[6][3] = {
        {0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}
    };
    const gpmf_series *ac = gpmf_series_find(file, ID_ACCL);
    const gpmf_series *gr = gpmf_series_find(file, ID_GRAV);
    double best = 0.75;
    int p, s;
    file->grav_aligned = 0;
    file->grav_perm[0] = 0;
    file->grav_perm[1] = 1;
    file->grav_perm[2] = 2;
    file->grav_sign[0] = file->grav_sign[1] = file->grav_sign[2] = 1;
    if (!ac || !gr || ac->channels < 3 || gr->channels < 3)
        return;
    for (p = 0; p < 6; p++) {
        for (s = 0; s < 8; s++) {
            int sign[3];
            double sum = 0;
            int n = 0;
            int k;
            sign[0] = (s & 1) ? -1 : 1;
            sign[1] = (s & 2) ? -1 : 1;
            sign[2] = (s & 4) ? -1 : 1;
            for (k = 0; k < 9; k++) {
                double t = file->t_first + (file->duration - file->t_first) * ((k + 0.5) / 9.0);
                float a[3], g[3], m[3];
                float am, gm, dot;
                int c;
                if (t < 0)
                    t = 0;
                if (gpmf_sample(ac, t, a) != 3 || gpmf_sample(gr, t, g) != 3)
                    continue;
                for (c = 0; c < 3; c++)
                    m[c] = (float)sign[c] * g[perms[p][c]];
                am = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
                gm = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
                if (am < 1.0f || gm < 0.2f)
                    continue;
                dot = (a[0] * m[0] + a[1] * m[1] + a[2] * m[2]) / (am * gm);
                sum += (double)dot;
                n++;
            }
            if (n >= 4 && sum / n > best) {
                best = sum / n;
                file->grav_perm[0] = perms[p][0];
                file->grav_perm[1] = perms[p][1];
                file->grav_perm[2] = perms[p][2];
                file->grav_sign[0] = sign[0];
                file->grav_sign[1] = sign[1];
                file->grav_sign[2] = sign[2];
                file->grav_aligned = 1;
            }
        }
    }
}

int gpmf_gforce(const gpmf_file *file, double t, float *total_g, float *lat_x, float *lat_y)
{
    const gpmf_series *ac;
    const gpmf_series *gr;
    float a[3];
    float graw[3];
    float g[3];
    float am, gm, d;
    float gx, gy, gz;
    float lx, ly, lz;
    float rx, ry, rz;
    float xx, xy, xz, xn;
    float yx, yy, yz;
    int c;
    if (!total_g || !lat_x || !lat_y)
        return -1;
    *total_g = 0;
    *lat_x = 0;
    *lat_y = 0;
    ac = gpmf_series_find(file, ID_ACCL);
    if (!ac || gpmf_sample(ac, t, a) != 3)
        return -1;
    am = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    *total_g = am / (float)GPMF_GRAVITY;
    gr = gpmf_series_find(file, ID_GRAV);
    if (!file->grav_aligned || !gr || gpmf_sample(gr, t, graw) != 3)
        return 0;
    for (c = 0; c < 3; c++)
        g[c] = (float)file->grav_sign[c] * graw[file->grav_perm[c]];
    gm = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    if (gm < 0.2f)
        return 0;
    gx = g[0] / gm;
    gy = g[1] / gm;
    gz = g[2] / gm;
    d = a[0] * gx + a[1] * gy + a[2] * gz;
    lx = a[0] - d * gx;
    ly = a[1] - d * gy;
    lz = a[2] - d * gz;
    if (fabsf(gz) < 0.85f) {
        rx = 0;
        ry = 0;
        rz = 1;
    } else {
        rx = 1;
        ry = 0;
        rz = 0;
    }
    xx = gy * rz - gz * ry;
    xy = gz * rx - gx * rz;
    xz = gx * ry - gy * rx;
    xn = sqrtf(xx * xx + xy * xy + xz * xz);
    if (xn < 1e-4f)
        return 0;
    xx /= xn;
    xy /= xn;
    xz /= xn;
    yx = xy * gz - xz * gy;
    yy = xz * gx - xx * gz;
    yz = xx * gy - xy * gx;
    *lat_x = (lx * xx + ly * xy + lz * xz) / (float)GPMF_GRAVITY;
    *lat_y = (lx * yx + ly * yy + lz * yz) / (float)GPMF_GRAVITY;
    return 0;
}

static int load_mutex_init(gpmf_load *L)
{
#ifdef _WIN32
    InitializeCriticalSection(&L->mu);
    L->mu_init = 1;
    return 0;
#else
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0)
        return -1;
    if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0) {
        pthread_mutexattr_destroy(&attr);
        return -1;
    }
    if (pthread_mutex_init(&L->mu, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        return -1;
    }
    pthread_mutexattr_destroy(&attr);
    L->mu_init = 1;
    return 0;
#endif
}

static void load_mutex_destroy(gpmf_load *L)
{
    if (!L || !L->mu_init)
        return;
#ifdef _WIN32
    DeleteCriticalSection(&L->mu);
#else
    pthread_mutex_destroy(&L->mu);
#endif
    L->mu_init = 0;
}

/* 0 on a read or a skipped payload, -1 when parsing runs out of memory.
 * ready is set before the lock is released, so a sample sees the new values. */
static int read_payload(gpmf_file *file, uint32_t index)
{
    gpmf_load *L = (gpmf_load *)file->load;
    payload_t *p;
    int dvid = 0;
    char name[64];
    if (!L || index >= L->n)
        return 0;
    p = &L->pays[index];
    if (p->ready)
        return 0;
    name[0] = '\0';
    if (p->size == 0 || p->size > 1024u * 1024u) {
        warn_set(file, "a gpmd payload was skipped because of its size");
        file_lock(file);
        p->ready = 1;
        file_unlock(file);
        return 0;
    }
    if (p->size > L->bufcap) {
        uint8_t *nb = (uint8_t *)realloc(L->buf, p->size);
        if (!nb)
            return -1;
        L->buf = nb;
        L->bufcap = p->size;
    }
    if (!L->fp || seek_at(L->fp, p->off) != 0 ||
        fread(L->buf, 1, p->size, L->fp) != p->size) {
        warn_set(file, "a gpmd payload could not be read");
        file_lock(file);
        p->ready = 1;
        file_unlock(file);
        return 0;
    }
    file_lock(file);
    if (parse_sequence(file, &dvid, name, sizeof name, L->buf, p->size, 0,
                       p->t_in, p->t_out) != 0) {
        file_unlock(file);
        return -1;
    }
    if (file->clock_source != GPMF_CLOCK_GPSU &&
        file->clock_source != GPMF_CLOCK_GPS9)
        clock_from_gps9(file);
    p->ready = 1;
    file_unlock(file);
    return 0;
}

static uint32_t payload_from(const gpmf_load *L, double t0)
{
    uint32_t lo = 0;
    uint32_t hi = L->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (L->pays[mid].t_out <= t0)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= L->n)
        return L->n - 1;
    return lo;
}

static uint32_t payload_covering(const gpmf_load *L, double t)
{
    uint32_t lo = 0;
    uint32_t hi = L->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (L->pays[mid].t_in <= t)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return 0;
    return lo - 1;
}

int gpmf_ensure(gpmf_file *file, double t, double ahead)
{
    gpmf_load *L;
    double t0, t1;
    uint32_t start, i;
    int nread = 0;
    if (!file)
        return 0;
    L = (gpmf_load *)file->load;
    if (!L || !L->pays || L->n == 0)
        return 0;
    if (ahead < 0)
        ahead = 0;
    t0 = t - 1.5;
    t1 = t + ahead;
    start = payload_from(L, t0);
    for (i = start; i < L->n && nread < 8; i++) {
        if (i > start && L->pays[i].t_in >= t1)
            break;
        if (L->pays[i].ready)
            continue;
        if (read_payload(file, i) != 0)
            return -1;
        nread++;
    }
    return nread;
}

int gpmf_ready(gpmf_file *file, double t)
{
    gpmf_load *L;
    int ready = 0;
    if (!file)
        return 0;
    L = (gpmf_load *)file->load;
    if (!L || !L->pays || L->n == 0)
        return 0;
    file_lock(file);
    ready = L->pays[payload_covering(L, t)].ready;
    file_unlock(file);
    return ready;
}

void gpmf_close(gpmf_file *file)
{
    int i;
    gpmf_load *L;
    if (!file)
        return;
    L = (gpmf_load *)file->load;
    if (L) {
        if (L->fp)
            fclose(L->fp);
        free(L->pays);
        free(L->buf);
        load_mutex_destroy(L);
        free(L);
        file->load = NULL;
    }
    for (i = 0; i < file->series_count; i++) {
        free(file->series[i].values);
        free(file->series[i].runs);
    }
    free(file->series);
    free(file);
}

int gpmf_open_index(gpmf_file **out, const char *path, char *err, size_t errlen)
{
    char dummy[8];
    FILE *fp;
    int64_t fsz;
    uint64_t pos = 0;
    uint8_t *moov = NULL;
    size_t moov_len = 0;
    gpmf_file *file;
    gpmf_load *L;
    mp4_ctx ctx;

    if (!out)
        return -1;
    *out = NULL;
    if (!err || errlen == 0) {
        err = dummy;
        errlen = sizeof dummy;
    }
    err[0] = '\0';
    if (!path || !path[0]) {
        snprintf(err, errlen, "missing file path");
        return -1;
    }
    fp = open_bin(path);
    if (!fp) {
        snprintf(err, errlen, "cannot open %s", path);
        return -1;
    }
    fsz = file_end(fp);
    if (fsz < 16 || seek_at(fp, 0) != 0) {
        fclose(fp);
        snprintf(err, errlen, "cannot read %s", path);
        return -1;
    }
    while ((int64_t)pos + 8 <= fsz) {
        uint8_t hdr[16];
        uint32_t size32, typ;
        uint64_t size;
        size_t hlen = 8;
        if (seek_at(fp, pos) != 0 || fread(hdr, 1, 8, fp) != 8)
            break;
        size32 = ru32(hdr);
        typ = ru32(hdr + 4);
        size = size32;
        if (size32 == 1) {
            if (fread(hdr + 8, 1, 8, fp) != 8)
                break;
            size = ru64(hdr + 8);
            hlen = 16;
        } else if (size32 == 0) {
            size = (uint64_t)fsz - pos;
        }
        if (size < hlen || pos + size > (uint64_t)fsz)
            break;
        if (typ == ID_MOOV) {
            moov_len = (size_t)(size - hlen);
            if (moov_len > 64u * 1024u * 1024u) {
                fclose(fp);
                snprintf(err, errlen, "moov atom is too large");
                return -1;
            }
            moov = (uint8_t *)malloc(moov_len ? moov_len : 1);
            if (!moov || fread(moov, 1, moov_len, fp) != moov_len) {
                free(moov);
                fclose(fp);
                snprintf(err, errlen, "cannot read moov");
                return -1;
            }
            break;
        }
        pos += size;
    }
    if (!moov) {
        fclose(fp);
        snprintf(err, errlen, "no moov atom");
        return -1;
    }
    file = (gpmf_file *)calloc(1, sizeof *file);
    L = (gpmf_load *)calloc(1, sizeof *L);
    if (!file || !L || load_mutex_init(L) != 0) {
        if (L && L->mu_init)
            load_mutex_destroy(L);
        free(L);
        free(file);
        free(moov);
        fclose(fp);
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    file->load = L;
    L->fp = fp;
    memset(&ctx, 0, sizeof ctx);
    ctx.file = file;
    ctx.movie_ts = 0;
    ctx.err = err;
    ctx.errlen = errlen;
    if (find_mvhd(file, moov, moov_len, &ctx.movie_ts) != 0 || ctx.movie_ts <= 0) {
        ctx.movie_ts = 1000;
        warn_set(file, "movie timescale missing; edit list uses 1000");
    }
    if (walk_atoms(&ctx, moov, moov_len, NULL) != 0) {
        free(moov);
        gpmf_close(file);
        if (err[0] == '\0')
            snprintf(err, errlen, "cannot parse moov");
        return -1;
    }
    free(moov);
    if (!ctx.committed) {
        gpmf_close(file);
        snprintf(err, errlen, "no gpmd track");
        return -1;
    }
    *out = file;
    return 0;
}

int gpmf_open(gpmf_file **out, const char *path, char *err, size_t errlen)
{
    gpmf_file *file = NULL;
    gpmf_load *L;
    uint32_t i;
    char dummy[8];
    if (!out)
        return -1;
    *out = NULL;
    if (!err || errlen == 0) {
        err = dummy;
        errlen = sizeof dummy;
    }
    if (gpmf_open_index(&file, path, err, errlen) != 0)
        return -1;
    L = (gpmf_load *)file->load;
    for (i = 0; L && i < L->n; i++) {
        if (read_payload(file, i) != 0) {
            gpmf_close(file);
            snprintf(err, errlen, "out of memory reading telemetry");
            return -1;
        }
    }
    align_grav(file);
    *out = file;
    return 0;
}

static int clock_parts_unlocked(const gpmf_file *file, double t,
                     int *year, int *month, int *day,
                     int *hour, int *minute, int *second)
{
    double unix_sec;
    int64_t z, days, rem, era, doe, yoe, y, doy, mp;
    if (!file || !file->clock_valid || !year || !month || !day ||
        !hour || !minute || !second)
        return -1;
    unix_sec = file->clock_unix0 + t;
    if (!isfinite(unix_sec))
        return -1;
    z = (int64_t)floor(unix_sec);
    days = z / 86400;
    rem = z % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    *hour = (int)(rem / 3600);
    *minute = (int)((rem % 3600) / 60);
    *second = (int)(rem % 60);
    z = days + 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *day = (int)(doy - (153 * mp + 2) / 5 + 1);
    *month = (int)(mp + (mp < 10 ? 3 : -9));
    *year = (int)(y + (*month <= 2));
    return 0;
}

int gpmf_clock_parts(const gpmf_file *file, double t,
                     int *year, int *month, int *day,
                     int *hour, int *minute, int *second)
{
    int rc;
    file_lock((gpmf_file *)file);
    rc = clock_parts_unlocked(file, t, year, month, day, hour, minute, second);
    file_unlock((gpmf_file *)file);
    return rc;
}
