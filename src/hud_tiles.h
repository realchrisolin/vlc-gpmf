#ifndef HUD_TILES_H
#define HUD_TILES_H
/*
 * Raster tiles for the GNSS inset. OpenStreetMap, Mapbox Streets, and
 * Google roadmap. The filter thread only paints. A worker fetches.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stddef.h>
#include <stdint.h>

#include "hud_map.h"

struct vlc_object_t;

enum {
    HUD_TILE_OSM = 0,
    HUD_TILE_MAPBOX = 1,
    HUD_TILE_GOOGLE = 2
};

typedef struct hud_tiles hud_tiles;

hud_tiles *hud_tiles_open(struct vlc_object_t *obj);
void hud_tiles_close(hud_tiles *tiles);

/* token is the Mapbox access token or the Google API key. OSM ignores it.
 * An empty token leaves Mapbox and Google on the dark panel. */
void hud_tiles_configure(hud_tiles *tiles, int source, const char *token, int zoom);

/* Ask for the tiles under a panel of view_w by view_h overlay pixels.
 * span is how many of those pixels one 256px tile covers. The art is
 * authored at 256 pixels. A larger span draws the same tile, and its
 * street names, bigger. Zoom still selects which tile is fetched. */
void hud_tiles_want(hud_tiles *tiles, double lat, double lon, int view_w, int view_h, int span);

/* Bumps when a tile is stored or the cache is cleared. Painting does not. */
uint64_t hud_tiles_stored(hud_tiles *tiles);

/* Copy the attribution line for the current source. Empty when that source
 * has no tiles to credit. */
void hud_tiles_credit(hud_tiles *tiles, char *credit, size_t credit_n);

/* Paint cached tiles into the panel. credit receives the attribution line. */
/* opacity is 10..100. 100 paints solid tiles. Below that, tile RGB stays
 * straight and the pixel alpha is the opacity, so the video shows through. */
int hud_tiles_paint(hud_tiles *tiles, unsigned char *pixels, int pitch, int pix,
                    const hud_map_box *box, char *credit, size_t credit_n,
                    int opacity);
#endif
