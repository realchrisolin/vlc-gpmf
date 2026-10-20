/*
 * gpmf-dump — print a gpmd track and one sample time.
 * Copyright (C) 2026 Chris Olin
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "gpmf_read.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void print_vec(const char *name, const float *v, int n)
{
    int i;
    printf("  %s", name);
    for (i = 0; i < n; i++)
        printf(" %.5f", v[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    gpmf_file *file = NULL;
    char err[512];
    double when = 10.0;
    int i;
    float total = 0, lx = 0, ly = 0;
    const gpmf_series *accl;

    if (argc < 2) {
        fprintf(stderr, "usage: gpmf-dump FILE [seconds]\n");
        return 1;
    }
    if (argc >= 3)
        when = atof(argv[2]);
    if (gpmf_open(&file, argv[1], err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err[0] ? err : "open failed");
        return 1;
    }
    printf("device: %s (DVID %d)\n", file->device[0] ? file->device : "unknown", file->dvid);
    printf("payloads: %d\n", file->payloads);
    printf("presentation: first %.6f s, end %.6f s\n", file->t_first, file->duration);
    if (file->warning[0])
        printf("warning: %s\n", file->warning);
    printf("streams:\n");
    for (i = 0; i < file->seen_count; i++) {
        const gpmf_seen *s = &file->seen[i];
        printf("  %s samples %d %s\n", s->fourcc_s, s->samples, s->stored ? "stored" : "skipped");
    }
    for (i = 0; i < file->series_count; i++) {
        const gpmf_series *s = &file->series[i];
        printf("  store %s ch %d n %d runs %d scale %.6g orin %s t0 %.6f\n",
               s->fourcc_s, s->channels, s->count, s->run_count, s->scale0,
               s->orin[0] ? s->orin : "-",
               s->run_count ? s->runs[0].t0 : 0);
        if (s->stnm[0])
            printf("    name %s\n", s->stnm);
    }
    printf("at t=%.3f s\n", when);
    for (i = 0; i < file->series_count; i++) {
        float tmp[GPMF_MAX_CH];
        int n = gpmf_sample(&file->series[i], when, tmp);
        if (n < 0)
            printf("  %s no sample\n", file->series[i].fourcc_s);
        else
            print_vec(file->series[i].fourcc_s, tmp, n);
    }
    {
        int Y = 0, M = 0, D = 0, hh = 0, mm = 0, ss = 0;
        const char *src = "none";
        if (file->clock_source == GPMF_CLOCK_GPSU)
            src = "gpsu";
        else if (file->clock_source == GPMF_CLOCK_GPS9)
            src = "gps9";
        else if (file->clock_source == GPMF_CLOCK_FILE)
            src = "file";
        if (gpmf_clock_parts(file, 0, &Y, &M, &D, &hh, &mm, &ss) == 0)
            printf("clock: %04d-%02d-%02d %02d:%02d:%02d source %s\n",
                   Y, M, D, hh, mm, ss, src);
        else
            printf("clock: none\n");
    }
    printf("grav map: %s perm %d %d %d sign %d %d %d\n",
           file->grav_aligned ? "aligned" : "none",
           file->grav_perm[0], file->grav_perm[1], file->grav_perm[2],
           file->grav_sign[0], file->grav_sign[1], file->grav_sign[2]);
    if (gpmf_gforce(file, when, &total, &lx, &ly) == 0)
        printf("  g total %.3f  lateral %.3f %.3f\n", total, lx, ly);
    else
        printf("  g no accelerometer sample\n");
    accl = gpmf_series_find(file, GPMF_FOURCC('A', 'C', 'C', 'L'));
    if (accl && accl->count > 0) {
        printf("  ACCL[0]");
        for (i = 0; i < accl->channels; i++)
            printf(" %.5f", accl->values[i]);
        printf("\n");
    }
    if (!isfinite(total) || total > 80.0) {
        fprintf(stderr, "accelerometer scale looks wrong (g=%.3f)\n", total);
        gpmf_close(file);
        return 2;
    }
    gpmf_close(file);
    return 0;
}
