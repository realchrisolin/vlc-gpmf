#ifndef HUD_MAP_H
#define HUD_MAP_H
/*
 * GNSS map inset. The arrow is the current fix and points along travel.
 * North is up. Course is degrees clockwise from north.
 *
 * The panel is drawn first, then map tiles, then the marker.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
enum {
    HUD_MAP_HIT_NONE = 0,
    HUD_MAP_HIT_MOVE,
    HUD_MAP_HIT_HIDE,
    HUD_MAP_HIT_ZOOM_IN,
    HUD_MAP_HIT_ZOOM_OUT,
    HUD_MAP_HIT_N,
    HUD_MAP_HIT_S,
    HUD_MAP_HIT_E,
    HUD_MAP_HIT_W,
    HUD_MAP_HIT_NE,
    HUD_MAP_HIT_NW,
    HUD_MAP_HIT_SE,
    HUD_MAP_HIT_SW,
    HUD_MAP_HIT_OPACITY_UP,
    HUD_MAP_HIT_OPACITY_DOWN
};

typedef struct {
    int w, h;
    int hide_x, hide_y, hide_w, hide_h;
    int zin_x, zin_y, zin_w, zin_h;
    int zout_x, zout_y, zout_w, zout_h;
    int oup_x, oup_y, oup_w, oup_h;
    int odn_x, odn_y, odn_w, odn_h;
    int grip;
    int full; /* immersive picture. No card, no drag, no grips. */
    int lift; /* clearance above the player controls */
} hud_map_box;

/* Tile coordinates at this zoom. One tile is 1.0 by 1.0. */
void hud_map_mercator(double lat, double lon, int zoom, double *x, double *y);

/* Initial bearing, degrees clockwise from north, in [0, 360). */
double hud_map_bearing(double lat1, double lon1, double lat2, double lon2);

double hud_map_distance_m(double lat1, double lon1, double lat2, double lon2);

/* Tip-to-tail length in picture pixels. The panel size is not an input,
 * so growing the map does not grow the arrow. Clamped to 24..240. */
int hud_map_arrow_len(int px);

/* Glide the shown fix toward the raw one. tau and dt are seconds.
 * Heading takes the short way around. A non-positive tau copies the raw fix. */
void hud_map_ease(double *lat, double *lon, double *course,
                  double raw_lat, double raw_lon, double raw_course,
                  int have_course, double dt, double tau);

/* user_w and user_h are the session size. Zero uses the default square. */
void hud_map_box_of(int video_w, int video_h, int expanded,
                    int user_w, int user_h, hud_map_box *box);

/* Full picture when expanded. Collapsed is the MAP tab size. */
void hud_map_immersive_box(int video_w, int video_h, int expanded, hud_map_box *box);

/* Local coordinates. Corners and edges beat the buttons, and the buttons beat a move.
 * A full-frame box has no grips and no move. */
int hud_map_hit(const hud_map_box *box, int expanded, int x, int y);

/* Strip first, then the map. from_strip is 1 when the pointer is on the strip.
 * Map coordinates are local to (ox, oy). A null box skips the map. */
int hud_map_aim(const hud_map_box *box, int expanded, int ox, int oy,
                int sx, int sy, int sw, int sh,
                int hx, int hy, int hw, int hh,
                int x, int y, int *from_strip);

/* Step map opacity by 10. dir is +1 or -1. Result stays in 10..100. */
int hud_map_opacity_step(int opacity, int dir);

/* l0,t0,r0,b0 are the panel edges at the press. The edge opposite the grip stays put. */
void hud_map_resize(int hit, int l0, int t0, int r0, int b0,
                    int pointer_x, int pointer_y, int video_w, int video_h,
                    int *x, int *y, int *w, int *h);

/* Rounded chrome. A collapsed box also draws the MAP tab. */
void hud_map_panel(unsigned char *pixels, int pitch, int pix,
                   const hud_map_box *box, int expanded);

/* Arrow and the hide mark. arrow_px is the tip-to-tail length in the
 * pixels of this buffer. It does not follow the panel size. show_arrow
 * 0 skips the arrow and still draws the inset hide mark. A collapsed
 * box draws nothing here. */
void hud_map_marker(unsigned char *pixels, int pitch, int pix,
                    const hud_map_box *box, int expanded, int arrow_px,
                    double course_deg, int show_arrow);

/* Corner brackets and the zoom stepper. Drawn above the tiles, under the marker.
 * zoom_down is 1 while plus is held and 2 while minus is held. */
void hud_map_controls(unsigned char *pixels, int pitch, int pix,
                      const hud_map_box *box, int expanded, int hot,
                      int zoom, int zoom_down);

/* Zoom, opacity, and hide. Drawn on a full-frame map. opacity_down is 1
 * while more-opaque is held and 2 while less-opaque is held. */
void hud_map_dock(unsigned char *pixels, int pitch, int pix,
                  const hud_map_box *box, int zoom, int zoom_down,
                  int opacity, int opacity_down);

/* One dock button, painted at the origin of a buffer the size of that button.
 * pressed brightens the disc further. The glyph stays muted at a limit. */
void hud_map_dock_face(unsigned char *pixels, int pitch, int pix,
                       int w, int h, int hit, int pressed,
                       int zoom, int opacity);

/* Solid credit behind the attribution line. */
void hud_map_pill(unsigned char *pixels, int pitch, int pix,
                  const hud_map_box *box, int x, int y, int w, int h);

/* Panel, then marker. Tiles belong between those two calls. */
void hud_map_draw(unsigned char *pixels, int pitch, int pix,
                  const hud_map_box *box, int expanded, int arrow_px,
                  double course_deg);

/* 1 when (x, y) lies on the rounded inner panel. */
int hud_map_content(const hud_map_box *box, int x, int y);

/* Darken a band along the bottom of the inner panel. */
void hud_map_shade(unsigned char *pixels, int pitch, int pix,
                   const hud_map_box *box, int band);
#endif
