/*
 * GNSS timezone and civil-time conversion for the HUD.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Lat/lon goes through ZoneDetect. The offset comes from an IANA TZif file
 * so daylight-time rules do not depend on a Windows API.
 */
#if !defined(_WIN32)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "hud_zone.h"
#include "../third_party/zonedetect/zonedetect.h"

static char data_dir[1024];
static unsigned char *db_bytes;
static ZoneDetect *db;
static int open_state;

static int64_t *tz_at;
static unsigned char *tz_type_at;
static int32_t *tz_utoff;
static int tz_ntrans;
static int tz_ntype;
static char tz_cached[96];

enum { TZ_KIND_M = 1, TZ_KIND_JN = 2, TZ_KIND_N = 3 };

typedef struct {
    int kind;
    int a, b, c;
    int time_sec;
} tz_spec;

typedef struct {
    int valid;
    int has_dst;
    int std_off;
    int dst_off;
    tz_spec start;
    tz_spec end;
} tz_posix_rule;

static tz_posix_rule tz_rule;

static int parse_posix(const char *s);
static int posix_utoff(int64_t unix_sec);

static uint32_t ru32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int32_t rs32(const unsigned char *p)
{
    return (int32_t)ru32(p);
}

static int64_t rs64(const unsigned char *p)
{
    uint64_t u = 0;
    int i;
    for (i = 0; i < 8; i++)
        u = (u << 8) | p[i];
    return (int64_t)u;
}

static void tz_clear(void)
{
    free(tz_at);
    free(tz_type_at);
    free(tz_utoff);
    tz_at = NULL;
    tz_type_at = NULL;
    tz_utoff = NULL;
    tz_ntrans = 0;
    tz_ntype = 0;
    tz_cached[0] = 0;
    memset(&tz_rule, 0, sizeof tz_rule);
}

static int tzid_ok(const char *s)
{
    int prev_dot = 0;
    if (!s || !s[0] || *s == '/')
        return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '.') {
            if (prev_dot)
                return 0;
            prev_dot = 1;
            continue;
        }
        prev_dot = 0;
        if (c == '/')
            continue;
        if (!(isalnum(c) || c == '_' || c == '-' || c == '+'))
            return 0;
    }
    return 1;
}

static int read_all(const char *path, unsigned char **out, size_t *out_len)
{
    FILE *fp;
    long n;
    unsigned char *buf;
#if defined(_WIN32)
    wchar_t wpath[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) <= 0)
        return -1;
    fp = _wfopen(wpath, L"rb");
#else
    fp = fopen(path, "rb");
#endif
    if (!fp)
        return -1;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    n = ftell(fp);
    if (n <= 0 || n > 48 * 1024 * 1024) {
        fclose(fp);
        return -1;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    buf = (unsigned char *)malloc((size_t)n);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    *out = buf;
    *out_len = (size_t)n;
    return 0;
}

static int skip_tz_block(const unsigned char **pp, const unsigned char *end, int sixtyfour,
                         uint32_t timecnt, uint32_t typecnt, uint32_t charcnt,
                         uint32_t leapcnt, uint32_t stdcnt, uint32_t utcnt)
{
    size_t tsize = sixtyfour ? 8u : 4u;
    size_t need;
    if (timecnt > 200000 || typecnt > 256 || charcnt > 100000 || leapcnt > 100000)
        return -1;
    need = (size_t)timecnt * tsize + (size_t)timecnt + (size_t)typecnt * 6u +
           (size_t)charcnt + (size_t)leapcnt * (tsize + 4u) + stdcnt + utcnt;
    if (*pp > end || (size_t)(end - *pp) < need)
        return -1;
    *pp += need;
    return 0;
}

static int read_tz_header(const unsigned char *p, const unsigned char *end,
                          uint32_t *timecnt, uint32_t *typecnt, uint32_t *charcnt,
                          uint32_t *leapcnt, uint32_t *stdcnt, uint32_t *utcnt, int *ver2)
{
    if ((size_t)(end - p) < 44 || memcmp(p, "TZif", 4) != 0)
        return -1;
    *ver2 = p[4] >= '2';
    *utcnt = ru32(p + 20);
    *stdcnt = ru32(p + 24);
    *leapcnt = ru32(p + 28);
    *timecnt = ru32(p + 32);
    *typecnt = ru32(p + 36);
    *charcnt = ru32(p + 40);
    return 0;
}

static int tz_load_bytes(const unsigned char *b, size_t n)
{
    const unsigned char *p = b;
    const unsigned char *end = b + n;
    uint32_t timecnt, typecnt, charcnt, leapcnt, stdcnt, utcnt;
    int ver2 = 0;
    int sixtyfour = 0;
    int i;
    if (read_tz_header(p, end, &timecnt, &typecnt, &charcnt, &leapcnt, &stdcnt, &utcnt, &ver2) != 0)
        return -1;
    p += 44;
    if (ver2) {
        if (skip_tz_block(&p, end, 0, timecnt, typecnt, charcnt, leapcnt, stdcnt, utcnt) != 0)
            return -1;
        if (read_tz_header(p, end, &timecnt, &typecnt, &charcnt, &leapcnt, &stdcnt, &utcnt, &ver2) != 0)
            return -1;
        p += 44;
        sixtyfour = 1;
    }
    if (typecnt < 1 || typecnt > 256 || timecnt > 200000)
        return -1;
    if ((size_t)(end - p) < (size_t)timecnt * (sixtyfour ? 8u : 4u) + (size_t)timecnt + (size_t)typecnt * 6u)
        return -1;
    tz_clear();
    tz_ntrans = (int)timecnt;
    tz_ntype = (int)typecnt;
    tz_utoff = (int32_t *)malloc((size_t)typecnt * sizeof(int32_t));
    if (timecnt) {
        tz_at = (int64_t *)malloc((size_t)timecnt * sizeof(int64_t));
        tz_type_at = (unsigned char *)malloc(timecnt);
    }
    if (!tz_utoff || (timecnt && (!tz_at || !tz_type_at))) {
        tz_clear();
        return -1;
    }
    for (i = 0; i < (int)timecnt; i++) {
        if (sixtyfour)
            tz_at[i] = rs64(p + (size_t)i * 8u);
        else
            tz_at[i] = rs32(p + (size_t)i * 4u);
    }
    p += (size_t)timecnt * (sixtyfour ? 8u : 4u);
    if (timecnt)
        memcpy(tz_type_at, p, timecnt);
    p += timecnt;
    for (i = 0; i < (int)typecnt; i++) {
        tz_utoff[i] = rs32(p);
        p += 6;
    }
    /* Version 2/3 repeats the table, then a POSIX TZ string for instants
     * after the last stored transition. Slim tzdata ends that table early. */
    if (sixtyfour) {
        size_t rest;
        char rule[160];
        size_t len = 0;
        if (charcnt > 100000 || leapcnt > 100000 || stdcnt > 256 || utcnt > 256) {
            tz_clear();
            return -1;
        }
        rest = (size_t)charcnt + (size_t)leapcnt * (8u + 4u) + (size_t)stdcnt + (size_t)utcnt;
        if ((size_t)(end - p) < rest) {
            tz_clear();
            return -1;
        }
        p += rest;
        if (p < end && *p == '\n')
            p++;
        while (p + len < end && p[len] != '\n' && len + 1 < sizeof rule)
            len++;
        if (len > 0 && (p + len >= end || p[len] == '\n')) {
            memcpy(rule, p, len);
            rule[len] = 0;
            parse_posix(rule);
        }
    }
    return 0;
}

static int tz_load_path(const char *path)
{
    unsigned char *buf = NULL;
    size_t n = 0;
    int rc;
    if (read_all(path, &buf, &n) != 0)
        return -1;
    rc = tz_load_bytes(buf, n);
    free(buf);
    return rc;
}

static int tz_load_id(const char *tzid)
{
    char path[1200];
    if (!tzid_ok(tzid))
        return -1;
    if (strcmp(tz_cached, tzid) == 0 && tz_utoff)
        return 0;
    if (data_dir[0]) {
        snprintf(path, sizeof path, "%s/zoneinfo/%s", data_dir, tzid);
        if (tz_load_path(path) == 0)
            goto done;
    }
#if !defined(_WIN32)
    snprintf(path, sizeof path, "/usr/share/zoneinfo/%s", tzid);
    if (tz_load_path(path) == 0)
        goto done;
#if defined(__APPLE__)
    snprintf(path, sizeof path, "/var/db/timezone/zoneinfo/%s", tzid);
    if (tz_load_path(path) == 0)
        goto done;
#endif
#endif
    return -1;
done:
    snprintf(tz_cached, sizeof tz_cached, "%s", tzid);
    return 0;
}

static int tz_offset(int64_t unix_sec, int *utoff)
{
    int lo, hi, hit;
    unsigned type;
    if (!tz_utoff || tz_ntype < 1)
        return -1;
    if (tz_ntrans == 0) {
        *utoff = tz_rule.valid ? posix_utoff(unix_sec) : (int)tz_utoff[0];
        return 0;
    }
    if (unix_sec < tz_at[0]) {
        *utoff = (int)tz_utoff[0];
        return 0;
    }
    /* After the stored transitions, the POSIX rule decides. It is applied to
     * this unix instant, so the calendar date picks the transition day and
     * the clock picks which side of that day's change is in effect. */
    if (tz_rule.valid && unix_sec > tz_at[tz_ntrans - 1]) {
        *utoff = posix_utoff(unix_sec);
        return 0;
    }
    lo = 0;
    hi = tz_ntrans - 1;
    hit = 0;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (tz_at[mid] <= unix_sec) {
            hit = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    type = tz_type_at[hit];
    if (type >= (unsigned)tz_ntype)
        return -1;
    *utoff = (int)tz_utoff[type];
    return 0;
}

static int split_unix(int64_t sec, int *year, int *month, int *day,
                      int *hour, int *minute, int *second)
{
    int64_t days, rem, z, era, doe, yoe, y, doy, mp;
    if (sec >= 0) {
        days = sec / 86400;
        rem = sec % 86400;
    } else {
        days = (sec - 86399) / 86400;
        rem = sec - days * 86400;
    }
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
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

static int parts_from_unix(int64_t unix_sec, int utoff, int *year, int *month, int *day,
                           int *hour, int *minute, int *second)
{
    return split_unix(unix_sec + (int64_t)utoff, year, month, day, hour, minute, second);
}

#if defined(_WIN32)
static int tm_from_win(int local, int64_t unix_sec, struct tm *out)
{
    __time64_t sec = (__time64_t)unix_sec;
    if (local)
        return _localtime64_s(out, &sec) == 0 ? 0 : -1;
    return _gmtime64_s(out, &sec) == 0 ? 0 : -1;
}
#else
static int tm_from_posix(int local, int64_t unix_sec, struct tm *out)
{
    time_t sec = (time_t)unix_sec;
    if (local)
        return localtime_r(&sec, out) ? 0 : -1;
    return gmtime_r(&sec, out) ? 0 : -1;
}
#endif

static void parent_dir(char *path)
{
    char *slash = strrchr(path, '/');
    char *back = strrchr(path, '\\');
    char *cut = slash > back ? slash : back;
    if (cut && cut != path)
        *cut = 0;
    else if (cut)
        cut[1] = 0;
}

static int try_dir(const char *dir)
{
    char binpath[1200];
    unsigned char *buf = NULL;
    size_t n = 0;
    ZoneDetect *lib;
    if (!dir || !dir[0])
        return -1;
    snprintf(binpath, sizeof binpath, "%s/timezone21.bin", dir);
    if (read_all(binpath, &buf, &n) != 0)
        return -1;
    lib = ZDOpenDatabaseFromMemory(buf, n);
    if (!lib) {
        free(buf);
        return -1;
    }
    db_bytes = buf;
    db = lib;
    snprintf(data_dir, sizeof data_dir, "%s", dir);
    return 0;
}

int hud_zone_open_at(const char *dir)
{
    if (open_state == 1)
        return 0;
    if (try_dir(dir) != 0) {
        open_state = -1;
        return -1;
    }
    open_state = 1;
    return 0;
}

int hud_zone_open(void)
{
    char base[1024];
    char parent[1024];
    char candidate[1200];
    if (open_state == 1)
        return 0;
    if (open_state == -1)
        return -1;
    base[0] = 0;
#if defined(_WIN32)
    {
        wchar_t wmod[1024];
        HMODULE mod = NULL;
        DWORD n;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)&hud_zone_open, &mod)) {
            n = GetModuleFileNameW(mod, wmod, 1024);
            if (n > 0 && n < 1024)
                WideCharToMultiByte(CP_UTF8, 0, wmod, -1, base, (int)sizeof base, NULL, NULL);
        }
    }
#else
    {
        Dl_info info;
        if (dladdr((void *)&hud_zone_open, &info) && info.dli_fname)
            snprintf(base, sizeof base, "%s", info.dli_fname);
    }
#endif
    parent_dir(base);
    snprintf(parent, sizeof parent, "%s", base);
    snprintf(candidate, sizeof candidate, "%s", base);
    if (try_dir(candidate) == 0) {
        open_state = 1;
        return 0;
    }
    parent_dir(parent);
    snprintf(candidate, sizeof candidate, "%s/third_party/zonedetect", parent);
    if (try_dir(candidate) == 0) {
        open_state = 1;
        return 0;
    }
    snprintf(candidate, sizeof candidate, "%s/share", parent);
    if (try_dir(candidate) == 0) {
        open_state = 1;
        return 0;
    }
    open_state = -1;
    return -1;
}

void hud_zone_close(void)
{
    if (db)
        ZDCloseDatabase(db);
    db = NULL;
    free(db_bytes);
    db_bytes = NULL;
    data_dir[0] = 0;
    open_state = 0;
    tz_clear();
}

int hud_zone_lookup(double lat, double lon, char *tzid, int tzid_len)
{
    char *name;
    if (!db || !tzid || tzid_len < 8)
        return -1;
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0)
        return -1;
    name = ZDHelperSimpleLookupString(db, (float)lat, (float)lon);
    if (!name || !name[0] || !tzid_ok(name)) {
        ZDHelperSimpleLookupStringFree(name);
        return -1;
    }
    if ((int)strlen(name) >= tzid_len) {
        ZDHelperSimpleLookupStringFree(name);
        return -1;
    }
    memcpy(tzid, name, strlen(name) + 1);
    ZDHelperSimpleLookupStringFree(name);
    return 0;
}

static const char *skip_name(const char *p)
{
    if (!p || !*p)
        return NULL;
    if (*p == '<') {
        p++;
        while (*p && *p != '>')
            p++;
        if (*p != '>')
            return NULL;
        return p + 1;
    }
    if (*p == '+' || *p == '-' || *p == ',' || isdigit((unsigned char)*p))
        return NULL;
    while (*p && *p != '+' && *p != '-' && *p != ',' && !isdigit((unsigned char)*p))
        p++;
    return p;
}

/* Hours may run past 24. allow_sign is for a transition clock, which can be negative. */
static const char *parse_hms(const char *p, int *sec, int allow_sign)
{
    int neg = 0;
    int hh = 0, mm = 0, ss = 0;
    int nd;
    if (!p || !*p)
        return NULL;
    if (allow_sign && (*p == '+' || *p == '-')) {
        neg = *p == '-';
        p++;
    }
    if (!isdigit((unsigned char)*p))
        return NULL;
    while (isdigit((unsigned char)*p)) {
        hh = hh * 10 + (*p - '0');
        if (hh > 167)
            return NULL;
        p++;
    }
    if (*p == ':') {
        p++;
        nd = 0;
        if (!isdigit((unsigned char)*p))
            return NULL;
        while (isdigit((unsigned char)*p) && nd < 2) {
            mm = mm * 10 + (*p - '0');
            p++;
            nd++;
        }
        if (mm > 59)
            return NULL;
        if (*p == ':') {
            p++;
            nd = 0;
            if (!isdigit((unsigned char)*p))
                return NULL;
            while (isdigit((unsigned char)*p) && nd < 2) {
                ss = ss * 10 + (*p - '0');
                p++;
                nd++;
            }
            if (ss > 59)
                return NULL;
        }
    }
    *sec = (neg ? -1 : 1) * (hh * 3600 + mm * 60 + ss);
    return p;
}

/* POSIX offsets are hours west of UTC. TZif utoff is seconds east. */
static const char *parse_offset(const char *p, int *utoff)
{
    int west = 1;
    int sec = 0;
    if (!p || !*p)
        return NULL;
    if (*p == '+' || *p == '-') {
        west = *p != '-';
        p++;
    }
    p = parse_hms(p, &sec, 0);
    if (!p || sec < 0)
        return NULL;
    *utoff = west ? -sec : sec;
    return p;
}

static const char *parse_spec(const char *p, tz_spec *spec)
{
    int time_sec = 2 * 3600;
    if (!p || !*p)
        return NULL;
    memset(spec, 0, sizeof *spec);
    if (*p == 'M') {
        int m = 0, n = 0, d = 0;
        p++;
        if (!isdigit((unsigned char)*p))
            return NULL;
        while (isdigit((unsigned char)*p)) {
            m = m * 10 + (*p - '0');
            p++;
        }
        if (*p != '.')
            return NULL;
        p++;
        if (!isdigit((unsigned char)*p))
            return NULL;
        while (isdigit((unsigned char)*p)) {
            n = n * 10 + (*p - '0');
            p++;
        }
        if (*p != '.')
            return NULL;
        p++;
        if (!isdigit((unsigned char)*p))
            return NULL;
        while (isdigit((unsigned char)*p)) {
            d = d * 10 + (*p - '0');
            p++;
        }
        if (m < 1 || m > 12 || n < 1 || n > 5 || d > 6)
            return NULL;
        spec->kind = TZ_KIND_M;
        spec->a = m;
        spec->b = n;
        spec->c = d;
    } else if (*p == 'J') {
        int day = 0;
        p++;
        if (!isdigit((unsigned char)*p))
            return NULL;
        while (isdigit((unsigned char)*p)) {
            day = day * 10 + (*p - '0');
            p++;
        }
        if (day < 1 || day > 365)
            return NULL;
        spec->kind = TZ_KIND_JN;
        spec->a = day;
    } else if (isdigit((unsigned char)*p)) {
        int day = 0;
        while (isdigit((unsigned char)*p)) {
            day = day * 10 + (*p - '0');
            p++;
        }
        if (day > 365)
            return NULL;
        spec->kind = TZ_KIND_N;
        spec->a = day;
    } else {
        return NULL;
    }
    if (*p == '/') {
        p++;
        p = parse_hms(p, &time_sec, 1);
        if (!p)
            return NULL;
    }
    spec->time_sec = time_sec;
    return p;
}

static int parse_posix(const char *s)
{
    const char *p;
    int std_off = 0;
    int dst_off = 0;
    tz_spec start, end;
    tz_posix_rule rule;
    memset(&rule, 0, sizeof rule);
    p = skip_name(s);
    if (!p)
        return -1;
    p = parse_offset(p, &std_off);
    if (!p)
        return -1;
    if (*p == 0) {
        rule.valid = 1;
        rule.std_off = std_off;
        rule.dst_off = std_off;
        tz_rule = rule;
        return 0;
    }
    p = skip_name(p);
    if (!p)
        return -1;
    if (*p == ',' || *p == 0)
        dst_off = std_off + 3600;
    else {
        p = parse_offset(p, &dst_off);
        if (!p)
            return -1;
    }
    if (*p == 0) {
        rule.valid = 1;
        rule.std_off = std_off;
        rule.dst_off = std_off;
        tz_rule = rule;
        return 0;
    }
    if (*p != ',')
        return -1;
    p++;
    p = parse_spec(p, &start);
    if (!p || *p != ',')
        return -1;
    p++;
    p = parse_spec(p, &end);
    if (!p || *p != 0)
        return -1;
    rule.valid = 1;
    rule.has_dst = 1;
    rule.std_off = std_off;
    rule.dst_off = dst_off;
    rule.start = start;
    rule.end = end;
    tz_rule = rule;
    return 0;
}

static int is_leap(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int days_in_month(int year, int month)
{
    static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2 && is_leap(year))
        return 29;
    if (month < 1 || month > 12)
        return 30;
    return d[month - 1];
}

static int wday_sun0(int year, int month, int day)
{
    int64_t days = civil_unix(year, month, day, 0, 0, 0) / 86400;
    int w = (int)((days + 4) % 7);
    if (w < 0)
        w += 7;
    return w;
}

static int spec_ymd(int year, const tz_spec *spec, int *month, int *day)
{
    if (spec->kind == TZ_KIND_M) {
        int first = wday_sun0(year, spec->a, 1);
        int dom = 1 + (spec->c - first + 7) % 7;
        dom += 7 * (spec->b - 1);
        if (dom > days_in_month(year, spec->a))
            dom -= 7;
        *month = spec->a;
        *day = dom;
        return 0;
    }
    if (spec->kind == TZ_KIND_JN) {
        static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        int m, left = spec->a;
        for (m = 0; m < 12; m++) {
            if (left <= mdays[m]) {
                *month = m + 1;
                *day = left;
                return 0;
            }
            left -= mdays[m];
        }
        return -1;
    }
    if (spec->kind == TZ_KIND_N) {
        int m, left = spec->a;
        for (m = 1; m <= 12; m++) {
            int md = days_in_month(year, m);
            if (left < md) {
                *month = m;
                *day = left + 1;
                return 0;
            }
            left -= md;
        }
        return -1;
    }
    return -1;
}

static int64_t spec_at(int year, const tz_spec *spec, int utoff)
{
    int month = 1, day = 1;
    if (spec_ymd(year, spec, &month, &day) != 0)
        return 0;
    return civil_unix(year, month, day, 0, 0, 0) + (int64_t)spec->time_sec - (int64_t)utoff;
}

static int posix_utoff(int64_t unix_sec)
{
    int year, month, day, hour, minute, second;
    int64_t best_at = 0;
    int best_off;
    int have = 0;
    int yy;
    if (!tz_rule.valid)
        return 0;
    if (!tz_rule.has_dst)
        return tz_rule.std_off;
    split_unix(unix_sec, &year, &month, &day, &hour, &minute, &second);
    best_off = tz_rule.std_off;
    for (yy = year - 1; yy <= year + 1; yy++) {
        int64_t to_dst = spec_at(yy, &tz_rule.start, tz_rule.std_off);
        int64_t to_std = spec_at(yy, &tz_rule.end, tz_rule.dst_off);
        if (to_dst <= unix_sec && (!have || to_dst >= best_at)) {
            best_at = to_dst;
            best_off = tz_rule.dst_off;
            have = 1;
        }
        if (to_std <= unix_sec && (!have || to_std >= best_at)) {
            best_at = to_std;
            best_off = tz_rule.std_off;
            have = 1;
        }
    }
    return best_off;
}

int hud_zone_local(int64_t unix_sec, int *year, int *month, int *day,
                   int *hour, int *minute, int *second, int *utoff)
{
    struct tm tm;
    int64_t wall;
#if defined(_WIN32)
    if (tm_from_win(1, unix_sec, &tm) != 0)
        return -1;
#else
    if (tm_from_posix(1, unix_sec, &tm) != 0)
        return -1;
#endif
    *year = tm.tm_year + 1900;
    *month = tm.tm_mon + 1;
    *day = tm.tm_mday;
    *hour = tm.tm_hour;
    *minute = tm.tm_min;
    *second = tm.tm_sec;
    wall = civil_unix(*year, *month, *day, *hour, *minute, *second);
    if (utoff)
        *utoff = (int)(wall - unix_sec);
    return 0;
}

int hud_zone_in(const char *tzid, int64_t unix_sec, int *year, int *month, int *day,
                int *hour, int *minute, int *second, int *utoff)
{
    int off = 0;
    if (tz_load_id(tzid) != 0)
        return -1;
    if (tz_offset(unix_sec, &off) != 0)
        return -1;
    if (utoff)
        *utoff = off;
    return parts_from_unix(unix_sec, off, year, month, day, hour, minute, second);
}

void hud_zone_label(char *dst, int dst_len, int utoff)
{
    int neg, a, h, m;
    if (!dst || dst_len < 4)
        return;
    if (utoff == 0) {
        snprintf(dst, (size_t)dst_len, "UTC");
        return;
    }
    neg = utoff < 0;
    a = neg ? -utoff : utoff;
    h = a / 3600;
    m = (a % 3600) / 60;
    if (m == 0)
        snprintf(dst, (size_t)dst_len, "UTC%c%d", neg ? '-' : '+', h);
    else
        snprintf(dst, (size_t)dst_len, "UTC%c%d:%02d", neg ? '-' : '+', h, m);
}
