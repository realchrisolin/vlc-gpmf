#ifndef HUD_ZONE_H
#define HUD_ZONE_H
/*
 * Timezone for the HUD clock.
 *
 * GPS time is UTC. "local" is this PC's timezone. "relative" is the IANA
 * zone that contains the recording's GNSS fix. The label is the offset in
 * effect at that instant: UTC-5, UTC+5:30, or UTC.
 *
 * The boundary database is ZoneDetect (BSD-3-Clause) with timezone polygons
 * from timezone-boundary-builder (ODbL). Offsets come from IANA tzdata.
 *
 * Copyright (C) 2026 Chris Olin
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

int hud_zone_open_at(const char *dir);
int hud_zone_open(void);
void hud_zone_close(void);

/* 0 on success. tzid receives America/New_York and the like. */
int hud_zone_lookup(double lat, double lon, char *tzid, int tzid_len);

int hud_zone_local(int64_t unix_sec, int *year, int *month, int *day,
                   int *hour, int *minute, int *second, int *utoff);
int hud_zone_in(const char *tzid, int64_t unix_sec, int *year, int *month, int *day,
                int *hour, int *minute, int *second, int *utoff);

void hud_zone_label(char *dst, int dst_len, int utoff);
#endif
