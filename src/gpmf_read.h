/*
 * gpmf_read.h — read a GoPro gpmd track for playback.
 *
 * Original reader. It does not include GoPro's gpmf-parser.
 * Copyright (C) 2026 Chris Olin
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GPMF_READ_H
#define GPMF_READ_H

#include <stdint.h>

#define GPMF_FOURCC(a, b, c, d) \
    (((uint32_t)(unsigned char)(a) << 24) | ((uint32_t)(unsigned char)(b) << 16) | \
     ((uint32_t)(unsigned char)(c) << 8) | (uint32_t)(unsigned char)(d))

#define GPMF_MAX_CH 16
#define GPMF_GRAVITY 9.80665

typedef struct gpmf_run {
    double t0;   /* presentation time of sample 0 in this run, seconds */
    double dt;   /* seconds per sample */
    int begin;   /* index into values[] */
    int count;
} gpmf_run;

typedef struct gpmf_series {
    uint32_t fourcc;
    char fourcc_s[5];
    int dvid;
    char stnm[64];
    int channels;
    int fix_col;     /* column that is a GPS fix, or -1 */
    int count;
    int capacity;
    float *values;   /* count * channels, engineering units */
    int run_count;
    int run_capacity;
    gpmf_run *runs;
    double scale0;
    char orin[4];    /* stored axis order; signs already applied */
    void *owner;     /* private; the series does not move once published */
} gpmf_series;

typedef struct gpmf_seen {
    uint32_t fourcc;
    char fourcc_s[5];
    int samples;
    int stored;
} gpmf_seen;

typedef struct gpmf_file {
    char device[64];
    int dvid;
    int payloads;
    double t_first;
    double duration; /* presentation time of the last payload end */
    int series_count;
    gpmf_series *series;
    int seen_count;
    gpmf_seen seen[32];
    char warning[256];
    /* GRAV components reordered into the accelerometer's stored axes. */
    int grav_perm[3];
    int grav_sign[3];
    int grav_aligned;
    /* Display size of the largest picture track, from tkhd. 0 if unknown. */
    int video_w;
    int video_h;
    /* Wall clock at presentation time 0, seconds since 1970-01-01 00:00:00.
     * GPSU and GPS9 are UTC. GPMF_CLOCK_FILE is the MP4 creation_time field
     * printed as stored: the digits are that integer, not shifted again. */
    double clock_unix0;
    int clock_valid;
    int clock_source;
    void *load; /* private payload reader; not for callers */
} gpmf_file;

#define GPMF_CLOCK_NONE 0
#define GPMF_CLOCK_GPSU 1
#define GPMF_CLOCK_GPS9 2
#define GPMF_CLOCK_FILE 3

/* 0 on success. On failure *out is NULL and err receives a short reason.
 * Every gpmd payload is read before this returns. */
int gpmf_open(gpmf_file **out, const char *path, char *err, size_t errlen);

/* Index the track and leave the payloads unread. Samples appear as
 * gpmf_ensure reads the window around a presentation time. One thread
 * calls gpmf_ensure. Other threads may sample. */
int gpmf_open_index(gpmf_file **out, const char *path, char *err, size_t errlen);

/* Read payloads covering [t - 1.5s, t + ahead] that are not loaded yet.
 * Returns how many were read, 0 when that window is loaded, -1 on
 * out of memory. */
int gpmf_ensure(gpmf_file *file, double t, double ahead);

/* 1 when the payload that covers presentation time t has been read. */
int gpmf_ready(gpmf_file *file, double t);

void gpmf_close(gpmf_file *file);

const gpmf_series *gpmf_series_find(const gpmf_file *file, uint32_t fourcc);

/* Linear sample. Returns the channel count, or -1 when t has no sample.
 * GPS series with a fix column return -1 across a lost fix. */
int gpmf_sample(const gpmf_series *series, double t, float *out);

/* Total specific force in g, and the gravity-removed lateral components
 * in g (screen +x right, +y up) when GRAV is present. lat is 0 without GRAV.
 * Returns 0 when ACCL exists at t. */
int gpmf_gforce(const gpmf_file *file, double t,
                float *total_g, float *lat_x, float *lat_y);

/* Calendar fields for the wall clock at presentation time t.
 * Year, month, day, and 24-hour time. Returns 0 when a clock exists. */
int gpmf_clock_parts(const gpmf_file *file, double t,
                     int *year, int *month, int *day,
                     int *hour, int *minute, int *second);

#endif
