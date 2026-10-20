/*
 * VLC 3.0 sub-source: GoPro gpmd information HUD during playback.
 *
 * One panel shows the date on one line, then the time, then speed,
 * altitude, bearing, and a compass orientation when those streams exist. Letters are Segoe UI
 * on Windows. Linux and macOS use the built-in bitmap glyphs.
 * Drag inside a panel to move it. On the map, the corners and edges resize
 * it, and the stepper changes the zoom. A street name is drawn a third
 * larger than the tile art. That enlarge is cached per tile, and a clock
 * tick reuses the map picture. The zoom level still picks the tile.
 * The corner mark hides a panel.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef __PLUGIN__
#error Build with -D__PLUGIN__ and -DMODULE_STRING set to the module name
#endif

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vlc_common.h>
/* vlc_plugin.h snapshots this while it is included. A later define does not export. */
#define VLC_MODULE_LICENSE \
    "\x4c\x69\x63\x65\x6e\x73\x65\x64\x20\x75\x6e\x64\x65\x72\x20\x74" \
    "\x68\x65\x20\x74\x65\x72\x6d\x73\x20\x6f\x66\x20\x74\x68\x65\x20" \
    "\x47\x4e\x55\x20\x47\x65\x6e\x65\x72\x61\x6c\x20\x50\x75\x62\x6c" \
    "\x69\x63\x20\x4c\x69\x63\x65\x6e\x73\x65\x2c\x20\x76\x65\x72\x73" \
    "\x69\x6f\x6e\x20\x32\x20\x6f\x72\x20\x6c\x61\x74\x65\x72\x2e"
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_input.h>
#include <vlc_input_item.h>
#include <vlc_playlist.h>
#include <vlc_url.h>
#include <vlc_subpicture.h>
#include <vlc_picture_pool.h>
#include <vlc_vout_window.h>

#include "gpmf_read.h"
#include "hud_font.h"
#include "hud_map.h"
#include "hud_tiles.h"
#include "hud_zone.h"

#define CFG_PREFIX "gpmf-"

static const int pos_values[] = { 0, 1, 2, 4, 8, 5, 6, 9, 10 };
static const char *const pos_text[] = {
    "Center", "Left", "Right", "Top", "Bottom",
    "Top left", "Top right", "Bottom left", "Bottom right"
};
static const char *const unit_values[] = { "imperial", "metric" };
static const char *const unit_text[] = { "Miles and feet", "Kilometres and metres" };
static const char *const date_values[] = { "long", "iso" };
static const char *const date_text[] = { "September 1, 2026", "2026-09-01" };
static const char *const time_values[] = { "12h", "24h" };
static const char *const time_text[] = { "12:30:00 PM", "12:30:00" };
static const char *const zone_values[] = { "local", "relative", "utc" };
static const char *const zone_text[] = {
    "This PC's timezone", "Timezone at the GNSS position", "UTC"
};
static const char *const map_values[] = { "osm", "mapbox", "google" };
static const char *const map_text[] = {
    "OpenStreetMap", "Mapbox Streets", "Google roadmap"
};

enum { ZONE_LOCAL = 0, ZONE_UTC = 1, ZONE_RELATIVE = 2 };

static const char *const preset_values[] = { "default", "immersive" };
static const char *const preset_text[] = { "Default", "Immersive" };

enum { PRESET_DEFAULT = 0, PRESET_IMMERSIVE = 1 };

/* Hits above the map's own range. The gear toggles the sheet. A press
 * outside the sheet and the HUD closes it. Padding inside the sheet does not. */
enum {
    HIT_GEAR = 1000,
    HIT_DISMISS = 1001,
    HIT_SHEET = 1002,
    HIT_SAVE = 1003,
    HIT_OPT = 1100
};

/* Reading toggles sit between the time chips and the map divider.
 * Arrow show/hide sits under the map source. Length and smooth stay
 * steppers. The index alone does not say which. */
enum {
    OPT_PRESET = 0,
    OPT_ZONE,
    OPT_UNITS,
    OPT_DATE,
    OPT_TIME,
    OPT_SPEED,
    OPT_ALT,
    OPT_BEARING,
    OPT_ORIENT,
    OPT_SOURCE,
    OPT_SHOW_ARROW,
    OPT_ARROW,
    OPT_SMOOTH,
    OPT_COUNT
};

enum { SET_HIT_CAP = 32 };

static const char *const options[] = {
    "file", "units", "date", "time", "zone", "hud", "pos", "preset", "map",
    "map-source", "map-token", "map-zoom", "map-arrow", "map-smooth",
    "map-opacity", "hud-size",
    "show-speed", "show-alt", "show-bearing", "show-orient", "show-arrow",
    "hud-custom-default", "hud-custom-immersive",
    "hud-x-default", "hud-y-default", "hud-x-immersive", "hud-y-immersive",
    "map-custom", "map-x", "map-y", "map-w", "map-h", NULL
};

struct filter_sys_t {
    gpmf_file *gpmf;
    input_thread_t *input;
    int imperial;
    int date_long;
    int time_12;
    int zone_mode;
    int show_speed;
    int show_alt;
    int show_bearing;
    int show_orient;
    int show_arrow;
    char rel_tz[80];
    int rel_state;
    int rel_logged;
    vlc_tick_t refresh;
    vlc_tick_t last;
    vlc_tick_t anchor_date;
    vlc_tick_t anchor_time;
    int anchor_valid;
    int logged_sec;
    int failed;

    vlc_object_t *mouse;
    int left_down;
    int dragging;
    int drag_x, drag_y;
    int press_x, press_y;
    int press_hit;
    int user_moved;
    int visible;
    int hud_x, hud_y;
    int panel_x, panel_y, panel_w, panel_h;
    int hide_x, hide_y, hide_w, hide_h;
    int gear_x, gear_y, gear_w, gear_h;
    int settings_open;
    int map_source;
    int set_x, set_y, set_w, set_h;
    int set_hit_n;
    int set_id[SET_HIT_CAP];
    int set_hx[SET_HIT_CAP], set_hy[SET_HIT_CAP];
    int set_hw[SET_HIT_CAP], set_hh[SET_HIT_CAP];
    int video_w, video_h;
    int click_slop;
    int pos;
    int pos_seen;

    int map_visible;
    int map_user_moved;
    int map_user_sized;
    int map_w, map_h;
    int map_hot;
    int dock_hot;
    int dock_log_x, dock_log_y, dock_log_w;
    int map_zoom;
    int map_span;
    int map_logged_span;
    vlc_tick_t map_span_until;
    picture_t *map_pic;
    int map_pic_ok;
    int map_pic_dw, map_pic_dh;
    int map_pic_qx, map_pic_qy, map_pic_course;
    int map_pic_zoom, map_pic_opacity, map_pic_arrow, map_pic_show_arrow, map_pic_chrome;
    unsigned long long map_pic_tiles;
    int map_reuse_logged;
    int resize_l, resize_t, resize_r, resize_b;
    hud_map_box map_geom;
    int map_has;
    int map_course_set;
    int map_logged;
    int preset;
    int map_opacity;
    int hud_slot_x[2];
    int hud_slot_y[2];
    int hud_slot_moved[2];
    int map_arrow;
    int map_smooth_ms;
    int hud_size;
    /* Dragged this session. A saved fraction sets user_moved and does not. */
    int hud_dragged[2];
    int hud_fx[2];
    int hud_fy[2];
    int hud_frac_ok[2];
    int map_dragged;
    int map_fx, map_fy, map_fw, map_fh;
    int map_frac_ok;
    int saved_hud_custom[2];
    int saved_hud_x[2];
    int saved_hud_y[2];
    int saved_hud_applied[2];
    int saved_map_custom;
    int saved_map_x, saved_map_y, saved_map_w, saved_map_h;
    int saved_map_applied;
    int hud_logged_size;
    int hud_logged_cap;
    int map_fix_valid;
    double map_fix_t;
    double map_course;
    double bearing_deg;
    int bearing_set;
    int bearing_logged;
    double map_lat, map_lon;
    hud_tiles *tiles;
    int map_x, map_y;
    int map_px, map_py, map_pw, map_ph;
    int map_hx, map_hy, map_hw, map_hh;

    /* Immersive overlay is authored in display pixels. 0 means the last
     * frame was the video-pixel inset. */
    int overlay_display;
    int space_w, space_h;
    int imm_w, imm_h;
    int imm_measured;
    int view_valid;
    int view_dw, view_dh;
    int view_zoom, view_opacity, view_course;
    int view_qx, view_qy;
    int view_hud_vis, view_map_vis;
    int view_hud_x, view_hud_y;
    int view_arrow, view_preset;
    int view_settings;
    int view_imperial, view_date_long, view_time_12, view_zone;
    int view_show_speed, view_show_alt, view_show_bearing, view_show_orient;
    int view_show_arrow;
    int view_source, view_smooth, view_hud_size;
    int view_dragging, view_press, view_dock;
    unsigned long long view_tiles;
    char view_date[40];
    char view_clock[64];
    char view_speed[12];
    char view_alt[12];
    char view_bearing[16];
    char view_orient[20];

    /* Telemetry payloads are read on this thread, around the playhead.
     * The video thread does not walk the whole gpmd track. */
    vlc_thread_t loader;
    vlc_mutex_t load_mu;
    vlc_cond_t load_cond;
    double load_hint;
    int load_mu_init;
    int loader_started;
    int loader_stop;
    int loader_failed;
};

static int Open(vlc_object_t *);
static void Close(vlc_object_t *);
static void expand_cache_free(void);
static subpicture_t *Filter(filter_t *, vlc_tick_t);
static int map_source_of(const char *);

vlc_module_begin()
    set_shortname("GPMF")
    set_description("GoPro GPMF HUD")
    set_help("Read the gpmd track and draw an information HUD during playback. These values are the starting look for the next launch and for Convert/Save. The gear changes this playback and does not write them. Save as default does.")
    set_capability("sub source", 0)
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_SUBPIC)
    set_callbacks(Open, Close)
    add_shortcut("gpmf")

    set_section("HUD", "Date, time, readings, and where the panel starts")
    add_string(CFG_PREFIX "preset", "default", "Preset",
               "default: the inset map and the stacked HUD. immersive: a full-frame map over the video, and a compact HUD at the top center.", false)
        change_string_list(preset_values, preset_text)
    add_string(CFG_PREFIX "zone", "relative", "Zone",
               "relative: GPS time is shown in the timezone at the GNSS position and marked (relative). Daylight time follows that zone's rule on the recording's date and at its clock. local: GPS time is shown in this PC's timezone and marked (local). utc: GPS time is shown as UTC and marked (utc). The label is the offset in effect then, such as UTC-5 or UTC+5:30. A file timestamp is shown as stored.", false)
        change_string_list(zone_values, zone_text)
    add_string(CFG_PREFIX "units", "imperial", "Units",
               "imperial: mph and feet. metric: km/h and metres.", false)
        change_string_list(unit_values, unit_text)
    add_string(CFG_PREFIX "date", "long", "Date",
               "long: September 1, 2026. iso: 2026-09-01.", false)
        change_string_list(date_values, date_text)
    add_string(CFG_PREFIX "time", "12h", "Time",
               "12h: 12:30:00 PM. 24h: 12:30:00.", false)
        change_string_list(time_values, time_text)
    add_bool(CFG_PREFIX "show-speed", true, "Show speed",
             "Show the speed reading on the HUD.", false)
    add_bool(CFG_PREFIX "show-alt", true, "Show altitude",
             "Show the altitude reading on the HUD.", false)
    add_bool(CFG_PREFIX "show-bearing", true, "Show bearing",
             "Show the bearing reading on the HUD. The map arrow stays.", false)
    add_bool(CFG_PREFIX "show-orient", true, "Show orientation",
             "Show the compass point on the HUD. The map arrow stays.", false)
    add_bool(CFG_PREFIX "hud", true, "Show HUD",
             "Show the date, the time, and the readings. The corner mark hides the panel, and the tab opens it again.", false)
    add_integer(CFG_PREFIX "hud-size", 18, "HUD size",
                "Height of the clock and the readings, in screen pixels. The date and the labels are a step smaller. Map zoom and the size of the map do not change it.", false)
        change_integer_range(12, 48)
    add_integer(CFG_PREFIX "pos", 5, "HUD corner",
                "Starting corner for the default preset. Immersive starts at the top center. A drag lasts for this playback.", false)
        change_integer_list(pos_values, pos_text)

    set_section("Map", "Inset, full-frame map, and the course arrow")
    add_bool(CFG_PREFIX "map", true, "Show map",
             "Show the inset and the full-frame map. The corner mark and the dock hide it, and the tab opens it again.", false)
    add_string(CFG_PREFIX "map-source", "osm", "Map source",
               "osm: OpenStreetMap. mapbox: Mapbox Streets, which needs a token. google: Google roadmap, which needs an API key.", false)
        change_string_list(map_values, map_text)
    add_bool(CFG_PREFIX "show-arrow", true, "Show arrow",
             "Show the course arrow on the map. The dock, the credit, and the map hide mark stay.", false)
    add_integer(CFG_PREFIX "map-arrow", 64, "Arrow length",
                "Course arrow length in picture pixels. Resizing the map does not change it. 64 is a small marker.", false)
        change_integer_range(24, 240)
    add_integer(CFG_PREFIX "map-smooth", 500, "Map smoothing",
                "How long the map takes to catch the GNSS fix, in milliseconds. 0 follows every fix. 500 glides over small jumps. Speed and altitude stay exact.", false)
        change_integer_range(0, 3000)
    add_integer(CFG_PREFIX "map-zoom", 15, "Map zoom",
                "Web Mercator zoom from 2 to 18. 15 is about a street.", false)
        change_integer_range(2, 18)
    add_integer(CFG_PREFIX "map-opacity", 70, "Map opacity",
                "Immersive map strength, from 10 to 100. The arrow, the controls, and the credit stay solid. The default inset ignores this.", false)
        change_integer_range(10, 100)

    set_section("Source", "Telemetry file and map token")
    add_loadfile(CFG_PREFIX "file", NULL, "GPMF file",
                 "Video file to read telemetry from. Empty uses the file being played.", false)
    add_password(CFG_PREFIX "map-token", NULL, "Map token",
                 "Mapbox access token or Google Maps API key. OpenStreetMap leaves this empty.", false)

    set_section("Saved layout", "Fractions of the frame written by Save as default. Turning a custom flag off returns to the corner or the built-in inset.")
    add_bool(CFG_PREFIX "hud-custom-default", false, "Custom default HUD",
             "On after Save as default when the default HUD was dragged. Off returns to the HUD corner.", true)
    add_bool(CFG_PREFIX "hud-custom-immersive", false, "Custom immersive HUD",
             "On after Save as default when the immersive HUD was dragged. Off returns to the top center.", true)
    add_integer(CFG_PREFIX "hud-x-default", 0, "Default HUD X",
                "Horizontal place of the default HUD, as a fraction of the frame from 0 to 10000. Save as default writes this after a drag.", true)
        change_integer_range(0, 10000)
    add_integer(CFG_PREFIX "hud-y-default", 0, "Default HUD Y",
                "Vertical place of the default HUD, as a fraction of the frame from 0 to 10000. Save as default writes this after a drag.", true)
        change_integer_range(0, 10000)
    add_integer(CFG_PREFIX "hud-x-immersive", 0, "Immersive HUD X",
                "Horizontal place of the immersive HUD, as a fraction of the frame from 0 to 10000. Save as default writes this after a drag.", true)
        change_integer_range(0, 10000)
    add_integer(CFG_PREFIX "hud-y-immersive", 0, "Immersive HUD Y",
                "Vertical place of the immersive HUD, as a fraction of the frame from 0 to 10000. Save as default writes this after a drag.", true)
        change_integer_range(0, 10000)
    add_bool(CFG_PREFIX "map-custom", false, "Custom map inset",
             "On after Save as default when the inset was moved or resized. Off returns to the built-in inset. The immersive map stays full frame.", true)
    add_integer(CFG_PREFIX "map-x", 0, "Map inset X",
                "Inset left edge, as a fraction of the frame from 0 to 10000. Save as default writes this when the inset was moved or resized.", true)
        change_integer_range(0, 10000)
    add_integer(CFG_PREFIX "map-y", 0, "Map inset Y",
                "Inset top edge, as a fraction of the frame from 0 to 10000. Save as default writes this when the inset was moved or resized.", true)
        change_integer_range(0, 10000)
    add_integer(CFG_PREFIX "map-w", 0, "Map inset width",
                "Inset width, as a fraction of the frame from 0 to 10000. Save as default writes this when the inset was moved or resized.", true)
        change_integer_range(0, 10000)
    add_integer(CFG_PREFIX "map-h", 0, "Map inset height",
                "Inset height, as a fraction of the frame from 0 to 10000. Save as default writes this when the inset was moved or resized.", true)
        change_integer_range(0, 10000)

    add_obsolete_integer(CFG_PREFIX "g-pos")
    add_obsolete_integer(CFG_PREFIX "speed-pos")
    add_obsolete_integer(CFG_PREFIX "alt-pos")
    add_obsolete_integer(CFG_PREFIX "path-pos")
vlc_module_end()

typedef struct {
    uint8_t *p;
    int w, h, pitch, pix;
} canvas_t;

typedef struct {
    char date[40];
    char clock[64];
    char speed[12];
    char speed_unit[8];
    char alt[12];
    char alt_unit[8];
    char bearing[16];
    char orient[20];
    int has_speed;
    int has_alt;
    int has_bearing;
    int has_orient;
} hud_info_t;

typedef struct {
    const char *label;
    const char *value;
    const char *unit;
    const char *slot;
} hud_met_t;

typedef struct {
    int w, h;
    int scale, ps, ls;
    int pad;
    int y_time, y_date, y_rule, y_label, y_value;
    int content_x;
    int col_x[4];
    int col_w[4];
    int num_w[4];
    int nmet;
    int show_date;
    int hide_x, hide_y, hide_w, hide_h;
    int gear_x, gear_y, gear_w, gear_h;
    int compact;
    int x_date, x_time;
    int rad;
    int bar_x, bar_w;
    int gap;
    int token;
    int text_cap;
    int label_cap;
} hud_layout_t;

static const uint8_t *glyph(char c)
{
    static const uint8_t g0[] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E};
    static const uint8_t g1[] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E};
    static const uint8_t g2[] = {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F};
    static const uint8_t g3[] = {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E};
    static const uint8_t g4[] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02};
    static const uint8_t g5[] = {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E};
    static const uint8_t g6[] = {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E};
    static const uint8_t g7[] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08};
    static const uint8_t g8[] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E};
    static const uint8_t g9[] = {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E};
    static const uint8_t gdot[] = {0x00,0x00,0x00,0x00,0x00,0x04,0x04};
    static const uint8_t gmin[] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00};
    static const uint8_t gsp[] = {0,0,0,0,0,0,0};
    static const uint8_t gsl[] = {0x01,0x02,0x04,0x08,0x10,0x00,0x00};
    static const uint8_t gcol[] = {0x00,0x04,0x04,0x00,0x04,0x04,0x00};
    static const uint8_t gcom[] = {0x00,0x00,0x00,0x00,0x00,0x04,0x08};
    static const uint8_t gplus[] = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00};
    static const uint8_t glp[] = {0x04,0x08,0x10,0x10,0x10,0x08,0x04};
    static const uint8_t grp[] = {0x04,0x02,0x01,0x01,0x01,0x02,0x04};
    static const uint8_t font_az[26][7] = {
        {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* A */
        {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* B */
        {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C */
        {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, /* D */
        {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
        {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* F */
        {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}, /* G */
        {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* H */
        {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* I */
        {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, /* J */
        {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
        {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* L */
        {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* M */
        {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, /* N */
        {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
        {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* P */
        {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /* Q */
        {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* R */
        {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E}, /* S */
        {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
        {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* U */
        {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* V */
        {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, /* W */
        {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* X */
        {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, /* Y */
        {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, /* Z */
    };
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z')
        return font_az[c - 'A'];
    switch (c) {
    case '0': return g0; case '1': return g1; case '2': return g2;
    case '3': return g3; case '4': return g4; case '5': return g5;
    case '6': return g6; case '7': return g7; case '8': return g8;
    case '9': return g9; case '.': return gdot; case '-': return gmin;
    case '/': return gsl; case ':': return gcol;
    case ',': return gcom; case '+': return gplus;
    case '(': return glp; case ')': return grp;
    default: return gsp;
    }
}

static void put_px(canvas_t *c, int x, int y, int r, int g, int b, int a)
{
    uint8_t *px;
    if (x < 0 || y < 0 || x >= c->w || y >= c->h)
        return;
    px = c->p + (size_t)y * c->pitch + (size_t)x * c->pix;
    px[0] = (uint8_t)r;
    px[1] = (uint8_t)g;
    px[2] = (uint8_t)b;
    px[3] = (uint8_t)a;
}

static void fill_rect(canvas_t *c, int x, int y, int w, int h, int r, int g, int b, int a)
{
    int iy, ix;
    for (iy = y; iy < y + h; iy++)
        for (ix = x; ix < x + w; ix++)
            put_px(c, ix, iy, r, g, b, a);
}

static void fill_round_rect(canvas_t *c, int x, int y, int w, int h, int rad,
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
                put_px(c, x + ix, y + iy, r, g, b, a);
        }
    }
}

static void fill_disc(canvas_t *c, int cx, int cy, int rad, int r, int g, int b, int a)
{
    int y, x;
    for (y = -rad; y <= rad; y++)
        for (x = -rad; x <= rad; x++)
            if (x * x + y * y <= rad * rad)
                put_px(c, cx + x, cy + y, r, g, b, a);
}

static void line(canvas_t *c, int x0, int y0, int x1, int y1, int r, int g, int b, int a)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put_px(c, x0, y0, r, g, b, a);
        if (x0 == x1 && y0 == y1)
            break;
        if (2 * err >= dy) { err += dy; x0 += sx; }
        if (2 * err <= dx) { err += dx; y0 += sy; }
    }
}

static void line_thick(canvas_t *c, int x0, int y0, int x1, int y1, int thick,
                       int r, int g, int b, int a)
{
    int o;
    if (thick < 1)
        thick = 1;
    for (o = -thick / 2; o <= thick / 2; o++)
        line(c, x0 + o, y0, x1 + o, y1, r, g, b, a);
}

static int text_px_track(const char *s, int scale, int track)
{
    int n = (int)strlen(s);
    if (n <= 0)
        return 0;
    if (track < 1)
        track = 1;
    return n * (5 * scale + track) - track;
}

static int text_px(const char *s, int scale)
{
    return text_px_track(s, scale, scale) + (s[0] ? scale : 0);
}

static void draw_text_track(canvas_t *c, int x, int y, int scale, int track,
                            const char *s, int r, int g, int b, int a)
{
    int i;
    int adv;
    if (track < 1)
        track = 1;
    adv = 5 * scale + track;
    for (i = 0; s[i]; i++) {
        const uint8_t *gl = glyph(s[i]);
        int row, col;
        for (row = 0; row < 7; row++) {
            for (col = 0; col < 5; col++) {
                if (gl[row] & (0x10 >> col)) {
                    int py, px;
                    for (py = 0; py < scale; py++)
                        for (px = 0; px < scale; px++)
                            put_px(c, x + i * adv + col * scale + px,
                                   y + row * scale + py, r, g, b, a);
                }
            }
        }
    }
}

static void draw_text(canvas_t *c, int x, int y, int scale, const char *s,
                      int r, int g, int b, int a)
{
    draw_text_track(c, x, y, scale, scale, s, r, g, b, a);
}

static void draw_text_c(canvas_t *c, int x, int y, int scale, const char *s,
                        int r, int g, int b)
{
    int sh = scale > 2 ? 2 : 1;
    draw_text(c, x + sh, y + sh, scale, s, 0, 0, 0, 170);
    draw_text(c, x, y, scale, s, r, g, b, 255);
}

static subpicture_region_t *new_rgba(int w, int h, int align, int x, int y)
{
    video_format_t fmt;
    subpicture_region_t *region;
    picture_t *pic;
    canvas_t c;
    int row;
    video_format_Init(&fmt, VLC_CODEC_RGBA);
    fmt.i_width = fmt.i_visible_width = w;
    fmt.i_height = fmt.i_visible_height = h;
    fmt.i_sar_num = fmt.i_sar_den = 1;
    region = subpicture_region_New(&fmt);
    video_format_Clean(&fmt);
    if (!region)
        return NULL;
    if (!region->p_picture) {
        region->p_picture = picture_NewFromFormat(&region->fmt);
        if (!region->p_picture) {
            subpicture_region_Delete(region);
            return NULL;
        }
    }
    pic = region->p_picture;
    c.p = pic->p[0].p_pixels;
    c.w = w;
    c.h = h;
    c.pitch = pic->p[0].i_pitch;
    c.pix = pic->p[0].i_pixel_pitch > 0 ? pic->p[0].i_pixel_pitch : 4;
    if (!c.p) {
        subpicture_region_Delete(region);
        return NULL;
    }
    for (row = 0; row < h && row < pic->p[0].i_lines; row++)
        memset(c.p + (size_t)row * c.pitch, 0, (size_t)w * c.pix);
    region->i_align = align;
    region->i_x = x;
    region->i_y = y;
    region->i_alpha = 255;
    return region;
}

static canvas_t canvas_of(subpicture_region_t *region)
{
    canvas_t c;
    picture_t *pic = region->p_picture;
    c.p = pic->p[0].p_pixels;
    c.w = (int)region->fmt.i_visible_width;
    c.h = (int)region->fmt.i_visible_height;
    c.pitch = pic->p[0].i_pitch;
    c.pix = pic->p[0].i_pixel_pitch > 0 ? pic->p[0].i_pixel_pitch : 4;
    return c;
}

static int clamp_hud_size(int v)
{
    if (v < 12)
        return 12;
    if (v > 48)
        return 48;
    return v;
}

/* Chrome tracks the text. The font itself uses the pixel cap, not this step. */
static int chrome_scale(int text_cap)
{
    int s = (text_cap + 3) / 7;
    if (s < 2)
        s = 2;
    if (s > 10)
        s = 10;
    return s;
}

static int smaller_cap(int text_cap)
{
    int step = text_cap / 5;
    int label;
    if (step < 3)
        step = 3;
    label = text_cap - step;
    if (label < 11)
        label = 11;
    if (label > text_cap)
        label = text_cap;
    return label;
}

static int sample_speed(const gpmf_file *file, double t, float *mps, float *alt_m)
{
    const gpmf_series *gps;
    const gpmf_series *fix;
    float v[GPMF_MAX_CH];
    int n;
    gps = gpmf_series_find(file, GPMF_FOURCC('G', 'P', 'S', '9'));
    if (!gps)
        gps = gpmf_series_find(file, GPMF_FOURCC('G', 'P', 'S', '5'));
    if (!gps)
        return -1;
    n = gpmf_sample(gps, t, v);
    if (n < 4)
        return -1;
    fix = gpmf_series_find(file, GPMF_FOURCC('G', 'P', 'S', 'F'));
    if (fix) {
        float fv;
        if (gpmf_sample(fix, t, &fv) < 1)
            return -1;
    }
    if (mps)
        *mps = v[3];
    if (alt_m && n >= 3)
        *alt_m = v[2];
    return 0;
}

static int imax(int a, int b)
{
    return a > b ? a : b;
}

/* Nearest multiple of 4, and at least 4. Every HUD gap uses this step. */
static int snap4(int v)
{
    if (v < 4)
        return 4;
    return ((v + 2) / 4) * 4;
}

static const char *month_name(int month)
{
    static const char *const names[] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };
    if (month < 1 || month > 12)
        return NULL;
    return names[month - 1];
}

static int zone_mode_of(const char *zoneopt)
{
    if (zoneopt && strcmp(zoneopt, "utc") == 0)
        return ZONE_UTC;
    if (zoneopt && strcmp(zoneopt, "relative") == 0)
        return ZONE_RELATIVE;
    return ZONE_LOCAL;
}

static const char *zone_mode_name(int mode)
{
    if (mode == ZONE_UTC)
        return "utc";
    if (mode == ZONE_RELATIVE)
        return "relative";
    return "local";
}

/* GPSU and GPS9 are UTC. local uses the PC zone. relative uses the IANA zone
 * cached from the GNSS fix, and the offset in effect at this instant. The
 * zone's daylight rule sees the recording's date and its clock, and both the
 * date and the time shift by that offset. A file timestamp is left as stored. */
static void apply_zone(const gpmf_file *file, double t, int zone_mode, const char *rel_tz,
                      int *Y, int *M, int *D, int *h, int *mi, int *s,
                      char *zlabel, int zlen)
{
    double unix_f;
    int64_t sec;
    int utoff = 0;
    int gps;
    zlabel[0] = 0;
    if (!file || !file->clock_valid)
        return;
    gps = file->clock_source == GPMF_CLOCK_GPSU || file->clock_source == GPMF_CLOCK_GPS9;
    if (!gps)
        return;
    unix_f = file->clock_unix0 + t;
    if (!isfinite(unix_f))
        return;
    sec = (int64_t)floor(unix_f);
    if (zone_mode == ZONE_LOCAL) {
        if (hud_zone_local(sec, Y, M, D, h, mi, s, &utoff) == 0)
            hud_zone_label(zlabel, zlen, utoff);
        return;
    }
    if (zone_mode == ZONE_UTC) {
        hud_zone_label(zlabel, zlen, 0);
        return;
    }
    if (rel_tz && rel_tz[0] &&
        hud_zone_in(rel_tz, sec, Y, M, D, h, mi, s, &utoff) == 0)
        hud_zone_label(zlabel, zlen, utoff);
    else
        hud_zone_label(zlabel, zlen, 0);
}

static void format_clock(char *dst, size_t n, int h, int mi, int s, int time_12,
                         const char *zlabel, const char *mode)
{
    char body[32];
    int hour12;
    if (h < 0 || mi < 0 || s < 0) {
        snprintf(dst, n, "--:--");
        return;
    }
    if (!time_12) {
        snprintf(body, sizeof body, "%02d:%02d:%02d", h, mi, s);
    } else {
        hour12 = h % 12;
        if (hour12 == 0)
            hour12 = 12;
        snprintf(body, sizeof body, "%d:%02d:%02d %s", hour12, mi, s, h >= 12 ? "PM" : "AM");
    }
    if (zlabel && zlabel[0])
        snprintf(dst, n, "%s %s (%s)", body, zlabel, mode);
    else
        snprintf(dst, n, "%s (%s)", body, mode);
}

static const char *bearing_cardinal(double deg);

static void build_hud(const gpmf_file *file, double t, int imperial,
                      int date_long, int time_12, int zone_mode, const char *rel_tz,
                      hud_info_t *info, int have_bearing, double bearing_deg)
{
    int Y, M, D, h, mi, s;
    float mps, alt;
    const char *month;
    memset(info, 0, sizeof *info);
    if (gpmf_clock_parts(file, t, &Y, &M, &D, &h, &mi, &s) == 0) {
        char zlabel[24];
        apply_zone(file, t, zone_mode, rel_tz, &Y, &M, &D, &h, &mi, &s,
                   zlabel, (int)sizeof zlabel);
        month = month_name(M);
        if (date_long && month)
            snprintf(info->date, sizeof info->date, "%s %d, %d", month, D, Y);
        else
            snprintf(info->date, sizeof info->date, "%04d-%02d-%02d", Y, M, D);
        format_clock(info->clock, sizeof info->clock, h, mi, s, time_12,
                     zlabel, zone_mode_name(zone_mode));
    } else {
        snprintf(info->clock, sizeof info->clock, "--:--");
    }
    if (sample_speed(file, t, &mps, &alt) == 0 && mps >= 0) {
        double shown = imperial ? (double)mps * 2.2369362920544 : (double)mps * 3.6;
        double height = imperial ? (double)alt * 3.2808398950131 : (double)alt;
        info->has_speed = 1;
        info->has_alt = 1;
        snprintf(info->speed, sizeof info->speed, shown >= 100.0 ? "%.0f" : "%.1f", shown);
        snprintf(info->speed_unit, sizeof info->speed_unit, "%s", imperial ? "MPH" : "KM/H");
        snprintf(info->alt, sizeof info->alt, "%.0f", height);
        snprintf(info->alt_unit, sizeof info->alt_unit, "%s", imperial ? "FT" : "M");
    }
    if (have_bearing) {
        int shown;
        double deg = bearing_deg;
        while (deg < 0)
            deg += 360.0;
        while (deg >= 360.0)
            deg -= 360.0;
        shown = (int)(deg + 0.5);
        if (shown >= 360)
            shown = 0;
        info->has_bearing = 1;
        info->has_orient = 1;
        snprintf(info->bearing, sizeof info->bearing, "%03d\xB0", shown);
        snprintf(info->orient, sizeof info->orient, "%s",
                 bearing_cardinal((double)shown));
    }
}

/* Eight compass names. 0 is north, and each name covers 45 degrees. */
static const char *bearing_cardinal(double deg)
{
    static const char *name[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int i;
    while (deg < 0)
        deg += 360.0;
    while (deg >= 360.0)
        deg -= 360.0;
    i = (int)((deg + 22.5) / 45.0) % 8;
    if (i < 0)
        i = 0;
    return name[i];
}

/* Course over ground from the fix one second back. Nearly stopped, or a
 * step under 0.8 m, keeps the last course. The map arrow eases on its own. */
static int sample_gnss(const gpmf_file *file, double t, double *lat, double *lon, double *mps);

static void update_bearing(const gpmf_file *file, double t, double *held, int *set)
{
    double lat, lon, mps, lat0, lon0;
    if (!held || !set || sample_gnss(file, t, &lat, &lon, &mps) != 0)
        return;
    if (!(mps >= 0 && mps < 0.5) &&
        sample_gnss(file, t - 1.0, &lat0, &lon0, NULL) == 0 &&
        hud_map_distance_m(lat0, lon0, lat, lon) >= 0.8) {
        *held = hud_map_bearing(lat0, lon0, lat, lon);
        *set = 1;
    }
}

static int hud_metrics(const hud_info_t *info, hud_met_t *m)
{
    int n = 0;
    if (info->has_speed) {
        m[n].label = "SPEED";
        m[n].value = info->speed;
        m[n].unit = info->speed_unit;
        m[n].slot = "00.0";
        n++;
    }
    if (info->has_alt) {
        m[n].label = "ALT";
        m[n].value = info->alt;
        m[n].unit = info->alt_unit;
        m[n].slot = "0000";
        n++;
    }
    if (info->has_bearing) {
        m[n].label = "BEARING";
        m[n].value = info->bearing;
        m[n].unit = "";
        m[n].slot = "000\xB0";
        n++;
    }
    if (info->has_orient) {
        m[n].label = "ORIENT";
        m[n].value = info->orient;
        m[n].unit = "";
        m[n].slot = "NW";
        n++;
    }
    return n;
}

static int font_on(void)
{
    return hud_font_ok();
}

static int measure_label(const char *s, int scale, int track)
{
    if (font_on())
        return hud_font_width(HUD_FACE_LABEL, s);
    return text_px_track(s, scale, track);
}

static int measure_face(int face, const char *s, int scale)
{
    if (font_on())
        return hud_font_width(face, s);
    return text_px(s, scale);
}

static void paint_label(canvas_t *c, int x, int y, int scale, int track,
                        const char *s, int r, int g, int b, int a)
{
    if (font_on()) {
        hud_font_draw(c->p, c->pitch, c->pix, c->w, c->h,
                      x, y, HUD_FACE_LABEL, s, r, g, b, a);
        return;
    }
    draw_text_track(c, x, y, scale, track, s, r, g, b, a);
}

static void paint_face(canvas_t *c, int x, int y, int face, int scale,
                       const char *s, int r, int g, int b, int a)
{
    if (font_on()) {
        hud_font_draw(c->p, c->pitch, c->pix, c->w, c->h,
                      x, y, face, s, r, g, b, a);
        return;
    }
    draw_text(c, x, y, scale, s, r, g, b, a);
}

static void paint_clock(canvas_t *c, int x, int y, int scale, const char *s,
                        int r, int g, int b)
{
    int sh = scale > 2 ? 2 : 1;
    if (font_on()) {
        hud_font_draw(c->p, c->pitch, c->pix, c->w, c->h,
                      x + sh, y + sh, HUD_FACE_TIME, s, 0, 0, 0, 170);
        hud_font_draw(c->p, c->pitch, c->pix, c->w, c->h,
                      x, y, HUD_FACE_TIME, s, r, g, b, 255);
        return;
    }
    draw_text_c(c, x, y, scale, s, r, g, b);
}

/* Clock and readings use text_cap. The date and the labels use label_cap. */
static void prepare_strip_font(int text_cap, int label_cap)
{
    if (text_cap < 8)
        text_cap = 8;
    if (label_cap < 8)
        label_cap = 8;
    if (label_cap > text_cap)
        label_cap = text_cap;
    hud_font_prepare(label_cap, text_cap, text_cap, label_cap, label_cap);
}

static void restore_hud_font(const hud_layout_t *L)
{
    if (!L)
        return;
    if (L->compact)
        prepare_strip_font(L->text_cap, L->label_cap);
    else
        hud_font_prepare(L->label_cap, L->text_cap, L->text_cap, L->text_cap, L->text_cap);
}

static void layout_strip(int text_cap, int expanded, const hud_info_t *info,
                         hud_layout_t *L, int hit_floor, int pad_floor)
{
    hud_met_t mets[4];
    int i, use_font, nmet;
    int date_h, time_h, label_h, value_h, digit;
    int gap, token, section, hit, pad, btn_gap;
    int date_w = 0, time_w = 0, met_w = 0;
    int x, line_h;
    int scale;
    if (text_cap < 8)
        text_cap = 8;
    scale = chrome_scale(text_cap);
    memset(L, 0, sizeof *L);
    L->compact = 1;
    L->scale = scale;
    L->text_cap = text_cap;
    L->label_cap = smaller_cap(text_cap);
    L->ps = scale;
    L->ls = scale - 2;
    if (L->ls < 2)
        L->ls = 2;
    if (L->ls > 4)
        L->ls = 4;
    L->rad = snap4(scale * 3);
    if (!expanded) {
        int tab = text_cap;
        int tw, line;
        L->label_cap = tab;
        hud_font_prepare(tab, tab, tab, tab, tab);
        use_font = font_on();
        tw = use_font ? hud_font_width(HUD_FACE_TAB, "HUD") : text_px("HUD", scale);
        line = use_font ? hud_font_cap(HUD_FACE_TAB) : tab;
        L->ps = scale;
        L->pad = snap4(line / 2);
        L->w = L->pad + tw + L->pad;
        L->h = L->pad + line + L->pad;
        L->hide_w = L->w;
        L->hide_h = L->h;
        return;
    }
    /* Date and labels sit a step under the clock and the readings. On a
     * short window the old clamp made every face the same size. */
    prepare_strip_font(L->text_cap, L->label_cap);
    use_font = font_on();
    L->show_date = info->date[0] != 0;
    nmet = hud_metrics(info, mets);
    L->nmet = nmet;
    date_h = use_font ? hud_font_cap(HUD_FACE_DATE) : 7 * L->ps;
    time_h = use_font ? hud_font_cap(HUD_FACE_TIME) : 7 * L->ps;
    label_h = use_font ? hud_font_cap(HUD_FACE_LABEL) : 7 * L->ls;
    value_h = use_font ? hud_font_cap(HUD_FACE_VALUE) : 7 * L->ps;
    digit = use_font ? hud_font_digit(HUD_FACE_VALUE) : 6 * L->ps;
    token = snap4(digit / 2);
    if (token < pad_floor / 3)
        token = pad_floor / 3;
    if (token < 2)
        token = 2;
    gap = snap4(digit);
    if (gap < pad_floor * 2 / 3)
        gap = pad_floor * 2 / 3;
    if (gap < 4)
        gap = 4;
    section = snap4(imax(pad_floor * 2 / 3, scale * 2));
    hit = (scale + 3) * 2 + scale * 2;
    if (hit < hit_floor)
        hit = hit_floor;
    btn_gap = snap4(hit / 2);
    if (btn_gap < 4)
        btn_gap = 4;
    pad = snap4(imax(pad_floor, scale * 2));
    L->pad = pad;
    L->token = token;
    L->gap = gap;
    L->content_x = pad;
    L->bar_w = 0;
    if (L->show_date)
        date_w = measure_face(HUD_FACE_DATE, info->date, L->ps);
    time_w = use_font ? hud_font_width(HUD_FACE_TIME, info->clock)
                      : text_px(info->clock, L->ps);
    for (i = 0; i < nmet; i++) {
        int track = L->ls * 2;
        int label_w = measure_label(mets[i].label, L->ls, track);
        int val_w = measure_face(HUD_FACE_VALUE, mets[i].value, L->ps);
        int unit_w = mets[i].unit[0] ? measure_label(mets[i].unit, L->ls, track) : 0;
        int block = label_w + token + val_w + (unit_w ? token + unit_w : 0);
        L->col_w[i] = block;
        L->num_w[i] = val_w;
        met_w += block;
        if (i + 1 < nmet)
            met_w += gap;
    }
    {
        int date_span = use_font ? hud_font_span(HUD_FACE_DATE) : date_h;
        int time_span = use_font ? hud_font_span(HUD_FACE_TIME) : time_h;
        int label_span = use_font ? hud_font_span(HUD_FACE_LABEL) : label_h;
        int value_span = use_font ? hud_font_span(HUD_FACE_VALUE) : value_h;
        int line1 = (L->show_date ? date_w + gap : 0) + time_w;
        int inner = line1 > met_w ? line1 : met_w;
        /* Date and time stay on the first row. Speed, altitude, bearing,
         * and orientation share the row below, with at least half a line between them. */
        line_h = imax(imax(date_span, time_span), imax(label_span, value_span));
        if (section < line_h / 2)
            section = snap4(line_h / 2);
        x = pad;
        if (L->show_date) {
            L->x_date = x;
            x += date_w + gap;
        }
        L->x_time = x;
        x = pad;
        for (i = 0; i < nmet; i++) {
            L->col_x[i] = x - pad;
            x += L->col_w[i] + (i + 1 < nmet ? gap : 0);
        }
        L->y_time = pad + line_h - time_span;
        L->y_date = pad + line_h - date_span;
        L->y_value = pad + line_h + section + line_h - value_span;
        L->y_label = L->y_value + value_h - label_h;
        L->w = pad + inner + gap + hit + btn_gap + hit + pad;
        L->h = pad + line_h + (nmet ? section + line_h : 0) + pad;
    }
    if (L->h < pad + line_h + pad)
        L->h = pad + line_h + pad;
    L->hide_w = hit;
    L->hide_h = hit;
    L->hide_x = L->w - pad - hit;
    if (L->hide_x < 0)
        L->hide_x = 0;
    /* Gear and hide sit on the clock row, clear of the readings. */
    L->hide_y = pad + (line_h - hit) / 2;
    if (L->hide_y < 0)
        L->hide_y = 0;
    L->gear_w = hit;
    L->gear_h = hit;
    L->gear_x = L->hide_x - btn_gap - hit;
    if (L->gear_x < 0)
        L->gear_x = 0;
    L->gear_y = L->hide_y;
    if (L->w < 8)
        L->w = 8;
    if (L->h < 8)
        L->h = 8;
    (void)expanded;
}

static void layout_hud(int text_cap, int expanded, const hud_info_t *info, hud_layout_t *L,
                       int strip, int hit_floor, int pad_floor)
{
    hud_met_t mets[4];
    int i;
    int use_font;
    int scale;
    if (text_cap < 8)
        text_cap = 8;
    scale = chrome_scale(text_cap);
    memset(L, 0, sizeof *L);
    L->scale = scale;
    L->ps = scale;
    L->text_cap = text_cap;
    L->label_cap = smaller_cap(text_cap);
    L->ls = scale - 2;
    if (L->ls < 2)
        L->ls = 2;
    if (L->ls > 4)
        L->ls = 4;
    L->rad = snap4(scale * 3);
    if (!expanded) {
        int tab = text_cap;
        int tw, line;
        L->label_cap = tab;
        hud_font_prepare(tab, tab, tab, tab, tab);
        use_font = font_on();
        tw = use_font ? hud_font_width(HUD_FACE_TAB, "HUD") : text_px("HUD", scale);
        line = use_font ? hud_font_cap(HUD_FACE_TAB) : tab;
        L->ps = scale;
        L->pad = snap4(line / 2);
        L->w = L->pad + tw + L->pad;
        L->h = L->pad + line + L->pad;
        L->hide_w = L->w;
        L->hide_h = L->h;
        return;
    }
    if (strip) {
        layout_strip(text_cap, expanded, info, L, hit_floor, pad_floor);
        return;
    }
    (void)hit_floor;
    (void)pad_floor;
    /* Date and time share the reading size. Labels sit a step under it.
     * The cap is the preference, not the map. */
    L->label_cap = text_cap * 3 / 4;
    if (L->label_cap < 11)
        L->label_cap = 11;
    if (L->label_cap > text_cap)
        L->label_cap = text_cap;
    hud_font_prepare(L->label_cap, text_cap, text_cap, text_cap, text_cap);
    use_font = font_on();
    L->show_date = info->date[0] != 0;
    L->nmet = hud_metrics(info, mets);
    {
        int date_h = use_font ? hud_font_cap(HUD_FACE_DATE) : 7 * L->ps;
        int time_h = use_font ? hud_font_cap(HUD_FACE_TIME) : 7 * L->ps;
        int label_h = use_font ? hud_font_cap(HUD_FACE_LABEL) : 7 * L->ls;
        int value_h = use_font ? hud_font_cap(HUD_FACE_VALUE) : 7 * L->ps;
        int digit = use_font ? hud_font_digit(HUD_FACE_VALUE) : 6 * L->ps;
        int in_clock = snap4(imax(date_h, time_h) / 2);
        int in_label = snap4(label_h / 2);
        int token = snap4(digit / 2);
        int gutter = snap4(digit + digit / 2);
        int section = snap4(imax(in_clock, in_label) * 2);
        int hit = imax(36, (scale + 3) * 2 + scale * 2);
        int btn_gap = snap4(hit / 2);
        int pad = snap4(imax(section, imax(gutter, hit + snap4(8))));
        int time_w = use_font ? hud_font_width(HUD_FACE_TIME, info->clock)
                              : text_px(info->clock, L->ps);
        int date_w = 0;
        int clock_w;
        int clock_h;
        int x = 0;
        int after;
        int top_y, top_h;
        /* Half a line inside a group. Twice that between the clock and the
         * metrics. Frame padding matches that section gap, and is at least
         * a gutter of one and a half value digits. */
        L->pad = pad;
        L->token = token;
        L->gap = gutter;
        L->bar_w = scale > 4 ? 4 : 3;
        L->bar_x = snap4(pad / 4);
        L->content_x = pad;
        L->hide_w = hit;
        L->hide_h = hit;
        if (L->show_date)
            date_w = measure_face(HUD_FACE_DATE, info->date, L->ps);
        /* Date and the clock share the top row. Readings sit under the rule. */
        clock_w = time_w;
        clock_h = time_h;
        L->x_time = pad;
        L->y_time = pad;
        if (L->show_date) {
            if (date_h > clock_h)
                clock_h = date_h;
            clock_w = date_w + in_clock + time_w;
            L->x_date = pad;
            L->x_time = pad + date_w + in_clock;
        }
        L->y_date = pad + (clock_h - date_h) / 2;
        L->y_time = pad + (clock_h - time_h) / 2;
        for (i = 0; i < L->nmet; i++) {
            int track = L->ls * 2;
            int slot_w = measure_face(HUD_FACE_VALUE, mets[i].slot, L->ps);
            int val_w = measure_face(HUD_FACE_VALUE, mets[i].value, L->ps);
            int num_w = imax(slot_w, val_w);
            int unit_w = mets[i].unit[0] ? measure_label(mets[i].unit, L->ls, track) : 0;
            int label_w = measure_label(mets[i].label, L->ls, track);
            int block = num_w + (unit_w ? token + unit_w : 0);
            int col = imax(label_w, block);
            L->col_x[i] = x;
            L->col_w[i] = col;
            L->num_w[i] = num_w;
            x += col + (i + 1 < L->nmet ? gutter : 0);
        }
        if (x > clock_w)
            clock_w = x;
        if (btn_gap < 4)
            btn_gap = 4;
        /* The gear sits just inside the hide mark, clear of the text. */
        L->w = pad + clock_w + btn_gap + hit + btn_gap + hit + pad;
        after = pad + clock_h;
        top_y = pad;
        top_h = clock_h;
        L->hide_x = L->w - pad - hit;
        if (L->hide_x < 0)
            L->hide_x = 0;
        L->hide_y = top_y + (top_h - hit) / 2;
        if (L->hide_y < snap4(8))
            L->hide_y = snap4(8);
        L->gear_w = hit;
        L->gear_h = hit;
        L->gear_x = L->hide_x - btn_gap - hit;
        if (L->gear_x < 0)
            L->gear_x = 0;
        L->gear_y = L->hide_y;
        if (L->nmet > 0) {
            int ink = use_font ? hud_font_span(HUD_FACE_VALUE) : value_h;
            int need;
            L->y_rule = after + section;
            L->y_label = L->y_rule + 2 + section;
            L->y_value = L->y_label + label_h + in_label;
            L->h = L->y_value + value_h + pad;
            need = L->y_value + ink + snap4(8);
            if (L->h < need)
                L->h = need;
        } else {
            L->h = after + pad;
        }
    }
    if (L->w < 8)
        L->w = 8;
    if (L->h < 8)
        L->h = 8;
}

static void paint_chrome(canvas_t *c, const hud_layout_t *L)
{
    int edge = 2;
    int inner_rad = L->rad - edge;
    if (inner_rad < 1)
        inner_rad = 1;
    fill_round_rect(c, 0, 0, L->w, L->h, L->rad, 255, 255, 255, 48);
    fill_round_rect(c, edge, edge, L->w - edge * 2, L->h - edge * 2,
                    inner_rad, 9, 11, 15, 198);
}

static void paint_hide(canvas_t *c, const hud_layout_t *L)
{
    int cx = L->hide_x + L->hide_w / 2;
    int cy = L->hide_y + L->hide_h / 2;
    int rad = L->hide_w / 2 - 4;
    int thick = L->scale / 2;
    int arm;
    if (rad < 10)
        rad = 10;
    if (rad > L->hide_w / 2 - 1)
        rad = L->hide_w / 2 - 1;
    if (thick < 2)
        thick = 2;
    arm = rad * 2 / 3;
    if (arm < 5)
        arm = 5;
    fill_disc(c, cx, cy, rad, 255, 255, 255, 36);
    line_thick(c, cx - arm, cy - arm, cx + arm, cy + arm, thick, 236, 238, 242, 255);
    line_thick(c, cx + arm, cy - arm, cx - arm, cy + arm, thick, 236, 238, 242, 255);
}

/* Eight teeth around a ring. Open uses the amber already used for the clock. */
static void paint_gear(canvas_t *c, const hud_layout_t *L, int open)
{
    int x = L->gear_x;
    int y = L->gear_y;
    int w = L->gear_w;
    int h = L->gear_h;
    int cx, cy, rad, body, hole, tooth, i;
    int br, bg, bb, ba;
    int mr, mg, mb;
    static const int cs[8] = { 1024, 724, 0, -724, -1024, -724, 0, 724 };
    static const int sn[8] = { 0, 724, 1024, 724, 0, -724, -1024, -724 };
    if (w < 4 || h < 4)
        return;
    cx = x + w / 2;
    cy = y + h / 2;
    rad = w < h ? w : h;
    rad = rad / 2 - 1;
    if (rad < 4)
        rad = 4;
    if (rad > w / 2)
        rad = w / 2;
    if (rad > h / 2)
        rad = h / 2;
    if (open) {
        br = 255; bg = 176; bb = 40; ba = 230;
        mr = 28; mg = 20; mb = 8;
    } else {
        br = 255; bg = 255; bb = 255; ba = 36;
        mr = 236; mg = 238; mb = 242;
    }
    fill_disc(c, cx, cy, rad, br, bg, bb, ba);
    tooth = rad / 4;
    if (tooth < 2)
        tooth = 2;
    body = rad * 2 / 3;
    if (body < 3)
        body = 3;
    hole = body / 2;
    if (hole < 2)
        hole = 2;
    for (i = 0; i < 8; i++) {
        int tx = cx + cs[i] * (rad - tooth / 2) / 1024;
        int ty = cy + sn[i] * (rad - tooth / 2) / 1024;
        fill_disc(c, tx, ty, tooth, mr, mg, mb, 255);
    }
    fill_disc(c, cx, cy, body, mr, mg, mb, 255);
    if (open)
        fill_disc(c, cx, cy, hole, br, bg, bb, ba);
    else
        fill_disc(c, cx, cy, hole, 9, 11, 15, 230);
}

static subpicture_region_t *draw_panel(int x, int y, int expanded,
                                       const hud_info_t *info, const hud_layout_t *L,
                                       int gear_open)
{
    subpicture_region_t *region;
    canvas_t c;
    hud_met_t mets[4];
    int i;
    region = new_rgba(L->w, L->h, 0, x, y);
    if (!region)
        return NULL;
    c = canvas_of(region);
    paint_chrome(&c, L);
    if (!expanded) {
        paint_face(&c, L->pad, L->pad, HUD_FACE_TAB, L->ps, "HUD", 255, 196, 64, 255);
        return region;
    }
    if (L->compact) {
        hud_met_t mets[4];
        if (L->show_date)
            paint_face(&c, L->x_date, L->y_date, HUD_FACE_DATE, L->ps, info->date,
                       176, 184, 196, 255);
        paint_clock(&c, L->x_time, L->y_time, L->ps, info->clock, 255, 196, 64);
        if (L->nmet > 0) {
            int mi;
            hud_metrics(info, mets);
            for (mi = 0; mi < L->nmet; mi++) {
                int track = L->ls * 2;
                int cx = L->content_x + L->col_x[mi];
                int label_w = measure_label(mets[mi].label, L->ls, track);
                int value_cap = font_on() ? hud_font_cap(HUD_FACE_VALUE) : 7 * L->ps;
                int label_cap = font_on() ? hud_font_cap(HUD_FACE_LABEL) : 7 * L->ls;
                int vx = cx + label_w + L->token;
                paint_label(&c, cx, L->y_label, L->ls, track, mets[mi].label,
                            148, 160, 176, 255);
                paint_face(&c, vx, L->y_value, HUD_FACE_VALUE, L->ps, mets[mi].value,
                           244, 246, 248, 255);
                if (mets[mi].unit[0]) {
                    int uw = measure_face(HUD_FACE_VALUE, mets[mi].value, L->ps);
                    int uy = L->y_value + value_cap - label_cap;
                    paint_label(&c, vx + uw + L->token, uy, L->ls, track, mets[mi].unit,
                                255, 186, 48, 255);
                }
            }
        }
        if (L->bar_w > 0) {
            int rh = L->h - L->pad * 2;
            int ry = L->pad + rh / 6;
            rh = rh - rh / 3;
            if (rh < 4)
                rh = 4;
            fill_rect(&c, L->bar_x, ry, L->bar_w, rh, 255, 255, 255, 64);
        }
        paint_hide(&c, L);
        paint_gear(&c, L, gear_open);
        return region;
    }
    if (L->bar_w > 0) {
        int bar_h = L->h - L->pad * 2;
        if (bar_h < 4)
            bar_h = 4;
        fill_rect(&c, L->bar_x, L->pad, L->bar_w, bar_h, 255, 176, 40, 255);
    }
    if (L->show_date)
        paint_face(&c, L->x_date, L->y_date, HUD_FACE_DATE, L->ps, info->date,
                   176, 184, 196, 255);
    paint_clock(&c, L->x_time, L->y_time, L->ps, info->clock, 255, 196, 64);
    if (L->nmet > 0) {
        int rule_h = imax(2, L->scale / 3);
        int rule_w = L->w - L->content_x - L->pad;
        if (rule_w < 1)
            rule_w = 1;
        fill_rect(&c, L->content_x, L->y_rule, rule_w, rule_h, 255, 255, 255, 40);
        hud_metrics(info, mets);
        for (i = 0; i < L->nmet; i++) {
            int cx = L->content_x + L->col_x[i];
            int track = L->ls * 2;
            int value_cap = font_on() ? hud_font_cap(HUD_FACE_VALUE) : 7 * L->ps;
            int label_cap = font_on() ? hud_font_cap(HUD_FACE_LABEL) : 7 * L->ls;
            int vw = measure_face(HUD_FACE_VALUE, mets[i].value, L->ps);
            int num_w = L->num_w[i];
            int vx = cx + num_w - vw;
            if (vx < cx)
                vx = cx;
            paint_label(&c, cx, L->y_label, L->ls, track, mets[i].label,
                        148, 160, 176, 255);
            paint_face(&c, vx, L->y_value, HUD_FACE_VALUE, L->ps, mets[i].value,
                       244, 246, 248, 255);
            if (mets[i].unit[0]) {
                int uy = L->y_value + value_cap - label_cap;
                paint_label(&c, cx + num_w + L->token, uy, L->ls, track, mets[i].unit,
                            255, 186, 48, 255);
            }
            if (i + 1 < L->nmet) {
                int sx = cx + L->col_w[i] + L->gap / 2;
                int sy = L->y_label;
                int span = font_on() ? hud_font_span(HUD_FACE_VALUE) : value_cap;
                int sh = L->y_value + span - sy;
                fill_rect(&c, sx, sy, imax(1, L->scale / 4), sh, 255, 255, 255, 36);
            }
        }
    }
    paint_hide(&c, L);
    paint_gear(&c, L, gear_open);
    return region;
}

static void corner_xy(int pos, int vw, int vh, int pw, int ph, int *x, int *y)
{
    int margin = 36;
    int left = margin;
    int top = margin;
    int right, bottom;
    if (vw <= 0)
        vw = 1920;
    if (vh <= 0)
        vh = 1080;
    right = vw - pw - margin;
    bottom = vh - ph - margin;
    if (right < left)
        right = left;
    if (bottom < top)
        bottom = top;
    switch (pos) {
    case 6: *x = right; *y = top; break;
    case 9: *x = left; *y = bottom; break;
    case 10: *x = right; *y = bottom; break;
    case 1: *x = left; *y = (vh - ph) / 2; break;
    case 2: *x = right; *y = (vh - ph) / 2; break;
    case 4: *x = (vw - pw) / 2; *y = top; break;
    case 8: *x = (vw - pw) / 2; *y = bottom; break;
    case 0: *x = (vw - pw) / 2; *y = (vh - ph) / 2; break;
    case 5:
    default: *x = left; *y = top; break;
    }
    if (*x < 0)
        *x = 0;
    if (*y < 0)
        *y = 0;
}

static int inside(int x, int y, int rx, int ry, int rw, int rh)
{
    return rw > 0 && rh > 0 && x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

static void clamp_hud(struct filter_sys_t *sys)
{
    int keep = 64;
    int w = sys->panel_w > 0 ? sys->panel_w : keep;
    int vw = sys->video_w;
    int vh = sys->video_h;
    if (sys->overlay_display && sys->space_w > 0 && sys->space_h > 0) {
        vw = sys->space_w;
        vh = sys->space_h;
        keep = 64 * sys->space_h / (sys->video_h > 0 ? sys->video_h : sys->space_h);
        if (keep < 16)
            keep = 16;
    }
    if (vw <= 0 || vh <= 0)
        return;
    if (sys->hud_x > vw - keep)
        sys->hud_x = vw - keep;
    if (sys->hud_y > vh - keep)
        sys->hud_y = vh - keep;
    if (sys->hud_x < -(w - keep))
        sys->hud_x = -(w - keep);
    if (sys->hud_y < 0)
        sys->hud_y = 0;
}

static void clamp_map(struct filter_sys_t *sys)
{
    int keep = 64;
    int w = sys->map_pw > 0 ? sys->map_pw : keep;
    if (sys->video_w <= 0 || sys->video_h <= 0)
        return;
    if (sys->map_x > sys->video_w - keep)
        sys->map_x = sys->video_w - keep;
    if (sys->map_y > sys->video_h - keep)
        sys->map_y = sys->video_h - keep;
    if (sys->map_x < -(w - keep))
        sys->map_x = -(w - keep);
    if (sys->map_y < 0)
        sys->map_y = 0;
}

static vlc_object_t *find_mouse(filter_t *filter)
{
    vlc_object_t *obj = VLC_OBJECT(filter);
    int guard = 0;
    while (obj && guard++ < 16) {
        if (var_Type(obj, "mouse-moved") & VLC_VAR_COORDS)
            return obj;
        obj = obj->obj.parent;
    }
    return NULL;
}

static int resize_hit(int hit)
{
    return hit >= HUD_MAP_HIT_N && hit <= HUD_MAP_HIT_SW;
}

static int settings_at(const struct filter_sys_t *sys, int x, int y)
{
    int i;
    for (i = 0; i < sys->set_hit_n; i++) {
        if (inside(x, y, sys->set_hx[i], sys->set_hy[i], sys->set_hw[i], sys->set_hh[i]))
            return sys->set_id[i];
    }
    return HIT_SHEET;
}

static void settings_close(struct filter_sys_t *sys)
{
    sys->settings_open = 0;
    sys->set_w = 0;
    sys->set_hit_n = 0;
}

/* Writes the live variable only. Preferences on disk stay as they were. */
static int apply_setting(filter_t *filter, struct filter_sys_t *sys, int hit)
{
    int code, opt, seg;
    static const char *const preset_v[] = { "default", "immersive" };
    static const char *const zone_v[] = { "local", "relative", "utc" };
    static const char *const units_v[] = { "imperial", "metric" };
    static const char *const date_v[] = { "long", "iso" };
    static const char *const time_v[] = { "12h", "24h" };
    static const char *const source_v[] = { "osm", "mapbox", "google" };
    const char *value = NULL;
    const char *key = NULL;
    if (hit < HIT_OPT)
        return 0;
    code = hit - HIT_OPT;
    opt = code / 8;
    seg = code % 8;
    switch (opt) {
    case OPT_PRESET:
        if (seg < 0 || seg > 1)
            return 0;
        if (seg == (sys->preset == PRESET_IMMERSIVE))
            return 0;
        key = "preset";
        value = preset_v[seg];
        break;
    case OPT_ZONE:
        if (seg < 0 || seg > 2)
            return 0;
        if (seg == (sys->zone_mode == ZONE_RELATIVE ? 1 : sys->zone_mode == ZONE_UTC ? 2 : 0))
            return 0;
        key = "zone";
        value = zone_v[seg];
        break;
    case OPT_UNITS:
        if (seg < 0 || seg > 1)
            return 0;
        if (seg == (sys->imperial ? 0 : 1))
            return 0;
        key = "units";
        value = units_v[seg];
        break;
    case OPT_DATE:
        if (seg < 0 || seg > 1)
            return 0;
        if (seg == (sys->date_long ? 0 : 1))
            return 0;
        key = "date";
        value = date_v[seg];
        break;
    case OPT_TIME:
        if (seg < 0 || seg > 1)
            return 0;
        if (seg == (sys->time_12 ? 0 : 1))
            return 0;
        key = "time";
        value = time_v[seg];
        break;
    case OPT_SPEED:
    case OPT_ALT:
    case OPT_BEARING:
    case OPT_ORIENT:
    case OPT_SHOW_ARROW: {
        int on, cur;
        char name[32];
        if (seg > 1)
            return 0;
        on = seg == 0 ? 1 : 0;
        if (opt == OPT_SPEED) {
            key = "show-speed";
            cur = sys->show_speed;
        } else if (opt == OPT_ALT) {
            key = "show-alt";
            cur = sys->show_alt;
        } else if (opt == OPT_BEARING) {
            key = "show-bearing";
            cur = sys->show_bearing;
        } else if (opt == OPT_ORIENT) {
            key = "show-orient";
            cur = sys->show_orient;
        } else {
            key = "show-arrow";
            cur = sys->show_arrow;
        }
        if ((cur ? 1 : 0) == on)
            return 0;
        snprintf(name, sizeof name, "%s%s", CFG_PREFIX, key);
        var_SetBool(filter, name, on);
        msg_Info(filter, "gpmf settings %s %s", key, on ? "on" : "off");
        return 1;
    }
    case OPT_SOURCE:
        if (seg < 0 || seg > 2)
            return 0;
        if (seg == sys->map_source)
            return 0;
        key = "map-source";
        value = source_v[seg];
        break;
    case OPT_ARROW: {
        int v = sys->map_arrow + (seg == 0 ? -8 : 8);
        if (seg > 1)
            return 0;
        if (v < 24)
            v = 24;
        if (v > 240)
            v = 240;
        if (v == sys->map_arrow)
            return 0;
        var_SetInteger(filter, CFG_PREFIX "map-arrow", v);
        msg_Info(filter, "gpmf settings arrow %d", v);
        return 1;
    }
    case OPT_SMOOTH: {
        int v = sys->map_smooth_ms + (seg == 0 ? -100 : 100);
        if (seg > 1)
            return 0;
        if (v < 0)
            v = 0;
        if (v > 3000)
            v = 3000;
        if (v == sys->map_smooth_ms)
            return 0;
        var_SetInteger(filter, CFG_PREFIX "map-smooth", v);
        msg_Info(filter, "gpmf settings smooth %d", v);
        return 1;
    }
    default:
        return 0;
    }
    {
        char name[32];
        snprintf(name, sizeof name, "%s%s", CFG_PREFIX, key);
        var_SetString(filter, name, value);
    }
    msg_Info(filter, "gpmf settings %s %s", key, value);
    return 1;
}

/* 0..10000 of the frame. A window pixel is the wrong unit for a 4K encode. */
static int frac_clamp(int v)
{
    if (v < 0)
        return 0;
    if (v > 10000)
        return 10000;
    return v;
}

static int frac_of(int pos, int span)
{
    int v;
    if (span <= 0)
        return 0;
    v = (int)(((int64_t)pos * 10000 + span / 2) / span);
    return frac_clamp(v);
}

static int frac_px(int frac, int span)
{
    if (span <= 0)
        return 0;
    frac = frac_clamp(frac);
    return (int)(((int64_t)frac * span + 5000) / 10000);
}

/* Span that hud_x and hud_y are currently in. Immersive uses the last
 * display size. Default uses the picture. */
static void hud_span_now(const struct filter_sys_t *sys, int preset, int *bw, int *bh)
{
    if (preset == PRESET_IMMERSIVE && sys->space_w >= 32 && sys->space_h >= 32) {
        *bw = sys->space_w;
        *bh = sys->space_h;
    } else if (preset == PRESET_IMMERSIVE && sys->imm_w >= 32 && sys->imm_h >= 32) {
        *bw = sys->imm_w;
        *bh = sys->imm_h;
    } else if (preset != PRESET_IMMERSIVE) {
        *bw = sys->video_w;
        *bh = sys->video_h;
    } else {
        *bw = 0;
        *bh = 0;
    }
}

static void note_hud_frac(struct filter_sys_t *sys, int preset, int bw, int bh)
{
    if (preset < 0 || preset > 1 || !sys->hud_dragged[preset])
        return;
    if (bw < 32 || bh < 32)
        return;
    sys->hud_fx[preset] = frac_of(sys->hud_x, bw);
    sys->hud_fy[preset] = frac_of(sys->hud_y, bh);
    sys->hud_frac_ok[preset] = 1;
}

/* Move-only leaves map_w at 0 until a later frame copies the drawn size.
 * Callers that already know the drawn box pass it through note in Filter. */
static void note_map_frac(struct filter_sys_t *sys)
{
    if (!sys->map_dragged || sys->preset != PRESET_DEFAULT)
        return;
    if (sys->video_w < 32 || sys->video_h < 32)
        return;
    if (sys->map_w < 1 || sys->map_h < 1)
        return;
    sys->map_fx = frac_of(sys->map_x, sys->video_w);
    sys->map_fy = frac_of(sys->map_y, sys->video_h);
    sys->map_fw = frac_of(sys->map_w, sys->video_w);
    sys->map_fh = frac_of(sys->map_h, sys->video_h);
    sys->map_frac_ok = 1;
}

static void put_int(filter_t *filter, const char *key, int value)
{
    char name[64];
    snprintf(name, sizeof name, "%s%s", CFG_PREFIX, key);
    config_PutInt(filter, name, value);
}

static void put_str(filter_t *filter, const char *key, const char *value)
{
    char name[64];
    snprintf(name, sizeof name, "%s%s", CFG_PREFIX, key);
    config_PutPsz(filter, name, value ? value : "");
}

/* The only call to config_SaveConfigFile. Gear, dock, and drag do not. */
static void save_defaults(filter_t *filter, struct filter_sys_t *sys)
{
    char *preset = var_GetString(filter, CFG_PREFIX "preset");
    char *zone = var_GetString(filter, CFG_PREFIX "zone");
    char *units = var_GetString(filter, CFG_PREFIX "units");
    char *dateopt = var_GetString(filter, CFG_PREFIX "date");
    char *timeopt = var_GetString(filter, CFG_PREFIX "time");
    char *source = var_GetString(filter, CFG_PREFIX "map-source");
    char *token = var_GetString(filter, CFG_PREFIX "map-token");
    int bw, bh;
    int rc;
    const char *srcname;
    hud_span_now(sys, sys->preset, &bw, &bh);
    note_hud_frac(sys, sys->preset, bw, bh);
    note_map_frac(sys);
    if (sys->map_source == HUD_TILE_MAPBOX)
        srcname = "mapbox";
    else if (sys->map_source == HUD_TILE_GOOGLE)
        srcname = "google";
    else
        srcname = "osm";
    put_str(filter, "preset", preset && preset[0] ? preset :
            (sys->preset == PRESET_IMMERSIVE ? "immersive" : "default"));
    put_str(filter, "zone", zone && zone[0] ? zone : zone_mode_name(sys->zone_mode));
    put_str(filter, "units", units && units[0] ? units :
            (sys->imperial ? "imperial" : "metric"));
    put_str(filter, "date", dateopt && dateopt[0] ? dateopt :
            (sys->date_long ? "long" : "iso"));
    put_str(filter, "time", timeopt && timeopt[0] ? timeopt :
            (sys->time_12 ? "12h" : "24h"));
    put_int(filter, "show-speed", var_GetBool(filter, CFG_PREFIX "show-speed") ? 1 : 0);
    put_int(filter, "show-alt", var_GetBool(filter, CFG_PREFIX "show-alt") ? 1 : 0);
    put_int(filter, "show-bearing", var_GetBool(filter, CFG_PREFIX "show-bearing") ? 1 : 0);
    put_int(filter, "show-orient", var_GetBool(filter, CFG_PREFIX "show-orient") ? 1 : 0);
    put_int(filter, "show-arrow", var_GetBool(filter, CFG_PREFIX "show-arrow") ? 1 : 0);
    put_int(filter, "hud", var_GetBool(filter, CFG_PREFIX "hud") ? 1 : 0);
    put_int(filter, "hud-size", (int)var_GetInteger(filter, CFG_PREFIX "hud-size"));
    put_int(filter, "map", var_GetBool(filter, CFG_PREFIX "map") ? 1 : 0);
    put_str(filter, "map-source", source && source[0] ? source : srcname);
    put_str(filter, "map-token", token);
    put_int(filter, "map-zoom", (int)var_GetInteger(filter, CFG_PREFIX "map-zoom"));
    put_int(filter, "map-arrow", (int)var_GetInteger(filter, CFG_PREFIX "map-arrow"));
    put_int(filter, "map-smooth", (int)var_GetInteger(filter, CFG_PREFIX "map-smooth"));
    put_int(filter, "map-opacity", (int)var_GetInteger(filter, CFG_PREFIX "map-opacity"));
    put_int(filter, "pos", (int)var_GetInteger(filter, CFG_PREFIX "pos"));
    if (sys->hud_dragged[PRESET_DEFAULT] && sys->hud_frac_ok[PRESET_DEFAULT]) {
        put_int(filter, "hud-custom-default", 1);
        put_int(filter, "hud-x-default", sys->hud_fx[PRESET_DEFAULT]);
        put_int(filter, "hud-y-default", sys->hud_fy[PRESET_DEFAULT]);
    }
    if (sys->hud_dragged[PRESET_IMMERSIVE] && sys->hud_frac_ok[PRESET_IMMERSIVE]) {
        put_int(filter, "hud-custom-immersive", 1);
        put_int(filter, "hud-x-immersive", sys->hud_fx[PRESET_IMMERSIVE]);
        put_int(filter, "hud-y-immersive", sys->hud_fy[PRESET_IMMERSIVE]);
    }
    if (sys->map_dragged && sys->map_frac_ok) {
        put_int(filter, "map-custom", 1);
        put_int(filter, "map-x", sys->map_fx);
        put_int(filter, "map-y", sys->map_fy);
        put_int(filter, "map-w", sys->map_fw);
        put_int(filter, "map-h", sys->map_fh);
    }
    free(preset);
    free(zone);
    free(units);
    free(dateopt);
    free(timeopt);
    free(source);
    free(token);
    rc = config_SaveConfigFile(filter);
    if (rc == 0)
        msg_Info(filter, "gpmf settings saved");
    else
        msg_Warn(filter, "gpmf settings save failed (%d)", rc);
}

/* Dock button under the pointer, or none. Immersive coordinates are display
 * pixels, and only after a frame has published that space. */
static int dock_at(const struct filter_sys_t *sys, int x, int y)
{
    int hit;
    if (sys->preset != PRESET_IMMERSIVE || !sys->overlay_display ||
        !sys->map_visible || !sys->map_geom.full || sys->map_pw <= 0)
        return HUD_MAP_HIT_NONE;
    /* The strip and the sheet sit above the dock and take the pointer. */
    if (inside(x, y, sys->panel_x, sys->panel_y, sys->panel_w, sys->panel_h))
        return HUD_MAP_HIT_NONE;
    if (sys->settings_open &&
        inside(x, y, sys->set_x, sys->set_y, sys->set_w, sys->set_h))
        return HUD_MAP_HIT_NONE;
    hit = hud_map_hit(&sys->map_geom, 1, x - sys->map_px, y - sys->map_py);
    switch (hit) {
    case HUD_MAP_HIT_ZOOM_IN:
    case HUD_MAP_HIT_ZOOM_OUT:
    case HUD_MAP_HIT_OPACITY_UP:
    case HUD_MAP_HIT_OPACITY_DOWN:
    case HUD_MAP_HIT_HIDE:
        return hit;
    default:
        return HUD_MAP_HIT_NONE;
    }
}

/* Read the video-output mouse from the overlay thread. Callbacks on those
 * variables deadlocked VLC 3.0.23 during startup, so this polls instead.
 * Returns 1 when the panel moved or its visibility changed. */
static int poll_mouse(filter_t *filter, struct filter_sys_t *sys)
{
    int x = 0, y = 0, buttons, left, changed = 0;
    int over;
    if (!sys->mouse)
        sys->mouse = find_mouse(filter);
    if (!sys->mouse)
        return 0;
    var_GetCoords(sys->mouse, "mouse-moved", &x, &y);
    buttons = (int)var_GetInteger(sys->mouse, "mouse-button-down");
    left = (buttons & 1) != 0;
    /* mouse-moved stays in picture pixels. The immersive frame is display pixels.
     * Round the same way the dock was scaled, or the edge of a button misses. */
    if (sys->overlay_display && sys->video_w > 0 && sys->space_w > 0 &&
        sys->video_h > 0 && sys->space_h > 0) {
        x = (int)(((int64_t)x * sys->space_w + sys->video_w / 2) / sys->video_w);
        y = (int)(((int64_t)y * sys->space_h + sys->video_h / 2) / sys->video_h);
    }
    {
        int hot = dock_at(sys, x, y);
        if (hot != sys->dock_hot) {
            sys->dock_hot = hot;
            changed = 1;
        }
    }
    over = sys->map_pw > 0 && inside(x, y, sys->map_px, sys->map_py, sys->map_pw, sys->map_ph);
    if (sys->dragging == 3)
        over = 1;
    if (over != sys->map_hot) {
        sys->map_hot = over;
        changed = 1;
    }
    if (left && !sys->left_down) {
        int immersive = sys->preset == PRESET_IMMERSIVE;
        int from_strip = 0;
        int map_hit = over && sys->dragging != 3;
        int hud_hit = inside(x, y, sys->panel_x, sys->panel_y, sys->panel_w, sys->panel_h);
        int on_gear = sys->gear_w > 0 &&
            inside(x, y, sys->gear_x, sys->gear_y, sys->gear_w, sys->gear_h);
        int on_sheet = sys->settings_open && sys->set_w > 0 &&
            inside(x, y, sys->set_x, sys->set_y, sys->set_w, sys->set_h);
        int hit = HUD_MAP_HIT_NONE;
        int claimed = 0;
        /* The gear sits inside the HUD. Claim it before the panel drag,
         * or that drag replaces the settings click. */
        if (on_sheet || on_gear || (sys->settings_open && !hud_hit)) {
            sys->drag_x = x;
            sys->drag_y = y;
            sys->press_x = x;
            sys->press_y = y;
            sys->dragging = 4;
            if (on_sheet)
                sys->press_hit = settings_at(sys, x, y);
            else if (on_gear)
                sys->press_hit = HIT_GEAR;
            else
                sys->press_hit = HIT_DISMISS;
            claimed = 1;
        } else if (immersive) {
            /* A full-frame dock is still a target when the pointer is on its
             * padding, past the exact picture rect the inset uses. */
            const hud_map_box *aim = NULL;
            if (sys->map_pw > 0 && (over || sys->map_geom.full))
                aim = &sys->map_geom;
            hit = hud_map_aim(aim, sys->map_visible,
                              sys->map_px, sys->map_py,
                              sys->panel_x, sys->panel_y, sys->panel_w, sys->panel_h,
                              sys->hide_x, sys->hide_y, sys->hide_w, sys->hide_h,
                              x, y, &from_strip);
            if (!from_strip && (hit == HUD_MAP_HIT_MOVE || resize_hit(hit)))
                hit = HUD_MAP_HIT_NONE;
            hud_hit = from_strip;
        } else if (map_hit) {
            hit = hud_map_hit(&sys->map_geom, sys->map_visible, x - sys->map_px, y - sys->map_py);
        }
        if (!claimed && (hit != HUD_MAP_HIT_NONE || hud_hit)) {
            sys->drag_x = x;
            sys->drag_y = y;
            sys->press_x = x;
            sys->press_y = y;
            sys->press_hit = hit;
            if (!from_strip && resize_hit(hit)) {
                sys->dragging = 3;
                sys->resize_l = sys->map_px;
                sys->resize_t = sys->map_py;
                sys->resize_r = sys->map_px + sys->map_pw;
                sys->resize_b = sys->map_py + sys->map_ph;
            } else if (!from_strip && hit == HUD_MAP_HIT_HIDE &&
                       (sys->map_visible || sys->preset == PRESET_IMMERSIVE))
                sys->dragging = 4;
            else if (!from_strip && (hit == HUD_MAP_HIT_ZOOM_IN || hit == HUD_MAP_HIT_ZOOM_OUT ||
                                     hit == HUD_MAP_HIT_OPACITY_UP || hit == HUD_MAP_HIT_OPACITY_DOWN))
                sys->dragging = 4;
            else if (!from_strip && hit != HUD_MAP_HIT_NONE)
                sys->dragging = 2;
            else {
                sys->dragging = 1;
                sys->press_hit = inside(x, y, sys->hide_x, sys->hide_y, sys->hide_w, sys->hide_h)
                    ? HUD_MAP_HIT_HIDE : HUD_MAP_HIT_MOVE;
            }
        }
    } else if (left && sys->dragging == 3) {
        int nx, ny, nw, nh;
        hud_map_resize(sys->press_hit, sys->resize_l, sys->resize_t,
                       sys->resize_r, sys->resize_b, x, y,
                       sys->video_w, sys->video_h, &nx, &ny, &nw, &nh);
        if (nx != sys->map_x || ny != sys->map_y || nw != sys->map_w || nh != sys->map_h) {
            sys->map_x = nx;
            sys->map_y = ny;
            sys->map_w = nw;
            sys->map_h = nh;
            sys->map_px = nx;
            sys->map_py = ny;
            sys->map_pw = nw;
            sys->map_ph = nh;
            sys->map_user_sized = 1;
            sys->map_user_moved = 1;
            sys->map_dragged = 1;
            clamp_map(sys);
            changed = 1;
        }
    } else if (left && sys->dragging && sys->dragging != 4) {
        int dx = x - sys->drag_x;
        int dy = y - sys->drag_y;
        if (dx || dy) {
            if (sys->dragging == 2) {
                sys->map_x += dx;
                sys->map_y += dy;
                sys->map_user_moved = 1;
                sys->map_dragged = 1;
                clamp_map(sys);
            } else {
                sys->hud_x += dx;
                sys->hud_y += dy;
                sys->user_moved = 1;
                sys->hud_dragged[sys->preset] = 1;
                clamp_hud(sys);
            }
            sys->drag_x = x;
            sys->drag_y = y;
            changed = 1;
        }
    } else if (!left && sys->left_down && sys->dragging) {
        int dx = x - sys->press_x;
        int dy = y - sys->press_y;
        int slop = sys->click_slop > 0 ? sys->click_slop : 32;
        int click;
        if (sys->overlay_display && sys->space_w > 0) {
            slop = sys->space_w / 120;
            if (slop < 6)
                slop = 6;
        }
        click = dx * dx + dy * dy <= slop * slop;
        /* A dock press is not a drag. Releasing on the same button counts
         * even when the pointer traveled farther than the slop. */
        if (sys->dragging == 4 && sys->preset == PRESET_IMMERSIVE) {
            int now = dock_at(sys, x, y);
            if (now != HUD_MAP_HIT_NONE && now == sys->press_hit)
                click = 1;
        }
        if (click && sys->press_hit == HIT_GEAR) {
            if (sys->visible && !sys->settings_open)
                sys->settings_open = 1;
            else
                settings_close(sys);
            changed = 1;
        } else if (click && sys->press_hit == HIT_DISMISS) {
            settings_close(sys);
            changed = 1;
        } else if (click && sys->press_hit == HIT_SAVE) {
            save_defaults(filter, sys);
            changed = 1;
        } else if (click && (sys->press_hit == HIT_SHEET || sys->press_hit >= HIT_OPT)) {
            if (sys->press_hit >= HIT_OPT)
                apply_setting(filter, sys, sys->press_hit);
            changed = 1;
        } else if (click && sys->press_hit == HUD_MAP_HIT_HIDE) {
            if (sys->dragging == 1) {
                sys->visible = !sys->visible;
                var_SetBool(filter, CFG_PREFIX "hud", sys->visible);
                if (!sys->visible)
                    settings_close(sys);
            } else {
                sys->map_visible = !sys->map_visible;
                var_SetBool(filter, CFG_PREFIX "map", sys->map_visible);
            }
            changed = 1;
        } else if (click && sys->dragging == 4 &&
                   (sys->press_hit == HUD_MAP_HIT_ZOOM_IN ||
                    sys->press_hit == HUD_MAP_HIT_ZOOM_OUT)) {
            int z = sys->map_zoom;
            if (sys->press_hit == HUD_MAP_HIT_ZOOM_IN && z < 18)
                z++;
            if (sys->press_hit == HUD_MAP_HIT_ZOOM_OUT && z > 2)
                z--;
            if (z != sys->map_zoom) {
                sys->map_zoom = z;
                var_SetInteger(filter, CFG_PREFIX "map-zoom", z);
                changed = 1;
            }
        } else if (click && sys->dragging == 4 &&
                   (sys->press_hit == HUD_MAP_HIT_OPACITY_UP ||
                    sys->press_hit == HUD_MAP_HIT_OPACITY_DOWN)) {
            int dir = sys->press_hit == HUD_MAP_HIT_OPACITY_UP ? 1 : -1;
            int o = hud_map_opacity_step(sys->map_opacity, dir);
            if (o != sys->map_opacity) {
                sys->map_opacity = o;
                var_SetInteger(filter, CFG_PREFIX "map-opacity", o);
                changed = 1;
            }
        }
        sys->dragging = 0;
    }
    sys->left_down = left;
    return changed;
}

static playlist_t *find_playlist(filter_t *filter)
{
    vlc_object_t *obj = VLC_OBJECT(filter);
    int guard = 0;
    while (obj && guard++ < 16) {
        const char *kind = obj->obj.object_type;
        if (kind && strcmp(kind, "playlist") == 0)
            return (playlist_t *)obj;
        obj = obj->obj.parent;
    }
    return NULL;
}

/* The input is a sibling of the video output, under the playlist.
 * playlist_CurrentInput only takes the playlist lock for the lookup.
 * Call it from Filter, after the input has finished creating the vout. */
static void ensure_input(filter_t *filter, struct filter_sys_t *sys)
{
    playlist_t *playlist;
    if (sys->input)
        return;
    playlist = find_playlist(filter);
    if (!playlist)
        return;
    sys->input = playlist_CurrentInput(playlist);
}

static char *path_of_input(input_thread_t *input)
{
    input_item_t *item;
    char *uri, *path;
    if (!input)
        return NULL;
    item = input_GetItem(input);
    if (!item)
        return NULL;
    uri = input_item_GetURI(item);
    if (!uri)
        return NULL;
    path = vlc_uri2path(uri);
    free(uri);
    return path;
}

static void remember_video(struct filter_sys_t *sys)
{
    int w, h, slop;
    if (!sys->gpmf)
        return;
    w = sys->gpmf->video_w;
    h = sys->gpmf->video_h;
    slop = w > 0 ? w / 120 : 32;
    if (slop < 24)
        slop = 24;
    sys->video_w = w;
    sys->video_h = h;
    sys->click_slop = slop;
}

static void *loader_thread(void *data)
{
    struct filter_sys_t *sys = data;
    for (;;) {
        double hint;
        int stop;
        int n;
        vlc_mutex_lock(&sys->load_mu);
        stop = sys->loader_stop;
        hint = sys->load_hint;
        vlc_mutex_unlock(&sys->load_mu);
        if (stop || !sys->gpmf)
            break;
        n = gpmf_ensure(sys->gpmf, hint, 3.0);
        if (n < 0) {
            sys->failed = 1;
            break;
        }
        if (n == 0) {
            vlc_mutex_lock(&sys->load_mu);
            if (!sys->loader_stop)
                vlc_cond_timedwait(&sys->load_cond, &sys->load_mu,
                                   mdate() + VLC_TICK_FROM_MS(50));
            vlc_mutex_unlock(&sys->load_mu);
        }
    }
    return NULL;
}

static void set_load_hint(struct filter_sys_t *sys, double t)
{
    if (!sys->load_mu_init)
        return;
    vlc_mutex_lock(&sys->load_mu);
    sys->load_hint = t;
    vlc_cond_signal(&sys->load_cond);
    vlc_mutex_unlock(&sys->load_mu);
}

static void start_loader(filter_t *filter, struct filter_sys_t *sys)
{
    if (sys->loader_started || sys->loader_failed || !sys->load_mu_init)
        return;
    if (vlc_clone(&sys->loader, loader_thread, sys, VLC_THREAD_PRIORITY_LOW) == 0)
        sys->loader_started = 1;
    else {
        sys->loader_failed = 1;
        msg_Warn(filter, "gpmf telemetry will load on the video thread");
    }
}

static int load_file(filter_t *filter, struct filter_sys_t *sys, const char *path)
{
    char err[512];
    if (!path || !path[0])
        return -1;
    /* The index is one read of moov. Payloads stay on disk until the
     * playhead needs them, so opening the movie does not seek once per
     * second of telemetry. */
    if (gpmf_open_index(&sys->gpmf, path, err, sizeof err) != 0) {
        msg_Err(filter, "gpmf: %s", err[0] ? err : "open failed");
        return -1;
    }
    remember_video(sys);
    vlc_mutex_init(&sys->load_mu);
    vlc_cond_init(&sys->load_cond);
    sys->load_mu_init = 1;
    sys->load_hint = 0;
    return 0;
}

static int Open(vlc_object_t *obj)
{
    filter_t *filter = (filter_t *)obj;
    struct filter_sys_t *sys;
    char *fileopt = NULL;
    char *units = NULL;
    char *dateopt = NULL;
    char *timeopt = NULL;
    char *zoneopt = NULL;

    sys = calloc(1, sizeof *sys);
    if (!sys)
        return VLC_ENOMEM;
    filter->p_sys = sys;
    sys->logged_sec = -1;
    sys->click_slop = 32;
    sys->visible = 1;
    config_ChainParse(filter, CFG_PREFIX, options, filter->p_cfg);
    fileopt = var_CreateGetString(filter, CFG_PREFIX "file");
    units = var_CreateGetString(filter, CFG_PREFIX "units");
    dateopt = var_CreateGetString(filter, CFG_PREFIX "date");
    timeopt = var_CreateGetString(filter, CFG_PREFIX "time");
    zoneopt = var_CreateGetString(filter, CFG_PREFIX "zone");
    sys->visible = var_CreateGetBool(filter, CFG_PREFIX "hud");
    sys->show_speed = var_CreateGetBool(filter, CFG_PREFIX "show-speed");
    sys->show_alt = var_CreateGetBool(filter, CFG_PREFIX "show-alt");
    sys->show_bearing = var_CreateGetBool(filter, CFG_PREFIX "show-bearing");
    sys->show_orient = var_CreateGetBool(filter, CFG_PREFIX "show-orient");
    sys->show_arrow = var_CreateGetBool(filter, CFG_PREFIX "show-arrow");
    sys->map_visible = var_CreateGetBool(filter, CFG_PREFIX "map");
    sys->pos = (int)var_CreateGetInteger(filter, CFG_PREFIX "pos");
    sys->pos_seen = sys->pos;
    sys->imperial = !units || strcmp(units, "metric") != 0;
    sys->date_long = !dateopt || strcmp(dateopt, "iso") != 0;
    sys->time_12 = !timeopt || strcmp(timeopt, "24h") != 0;
    sys->zone_mode = zone_mode_of(zoneopt);
    sys->refresh = VLC_TICK_FROM_MS(50);
    if (fileopt && fileopt[0] && load_file(filter, sys, fileopt) != 0) {
        free(fileopt);
        free(units);
        free(dateopt);
        free(timeopt);
        free(zoneopt);
        Close(obj);
        return VLC_EGENERIC;
    }
    free(fileopt);
    free(units);
    free(dateopt);
    free(timeopt);
    free(zoneopt);
    {
        char *srcopt = var_CreateGetString(filter, CFG_PREFIX "map-source");
        char *token = var_CreateGetString(filter, CFG_PREFIX "map-token");
        int zoom = (int)var_CreateGetInteger(filter, CFG_PREFIX "map-zoom");
        char *preset = var_CreateGetString(filter, CFG_PREFIX "preset");
        sys->map_arrow = (int)var_CreateGetInteger(filter, CFG_PREFIX "map-arrow");
        sys->map_smooth_ms = (int)var_CreateGetInteger(filter, CFG_PREFIX "map-smooth");
        sys->map_opacity = (int)var_CreateGetInteger(filter, CFG_PREFIX "map-opacity");
        sys->hud_size = clamp_hud_size((int)var_CreateGetInteger(filter, CFG_PREFIX "hud-size"));
        sys->preset = preset && strcmp(preset, "immersive") == 0
            ? PRESET_IMMERSIVE : PRESET_DEFAULT;
        free(preset);
        if (sys->map_arrow < 24)
            sys->map_arrow = 24;
        if (sys->map_arrow > 240)
            sys->map_arrow = 240;
        if (sys->map_opacity < 10)
            sys->map_opacity = 10;
        if (sys->map_opacity > 100)
            sys->map_opacity = 100;
        sys->saved_hud_custom[PRESET_DEFAULT] =
            var_CreateGetBool(filter, CFG_PREFIX "hud-custom-default") ? 1 : 0;
        sys->saved_hud_custom[PRESET_IMMERSIVE] =
            var_CreateGetBool(filter, CFG_PREFIX "hud-custom-immersive") ? 1 : 0;
        sys->saved_hud_x[PRESET_DEFAULT] =
            frac_clamp((int)var_CreateGetInteger(filter, CFG_PREFIX "hud-x-default"));
        sys->saved_hud_y[PRESET_DEFAULT] =
            frac_clamp((int)var_CreateGetInteger(filter, CFG_PREFIX "hud-y-default"));
        sys->saved_hud_x[PRESET_IMMERSIVE] =
            frac_clamp((int)var_CreateGetInteger(filter, CFG_PREFIX "hud-x-immersive"));
        sys->saved_hud_y[PRESET_IMMERSIVE] =
            frac_clamp((int)var_CreateGetInteger(filter, CFG_PREFIX "hud-y-immersive"));
        sys->saved_map_custom =
            var_CreateGetBool(filter, CFG_PREFIX "map-custom") ? 1 : 0;
        sys->saved_map_x = frac_clamp((int)var_CreateGetInteger(filter, CFG_PREFIX "map-x"));
        sys->saved_map_y = frac_clamp((int)var_CreateGetInteger(filter, CFG_PREFIX "map-y"));
        sys->saved_map_w = frac_clamp((int)var_CreateGetInteger(filter, CFG_PREFIX "map-w"));
        sys->saved_map_h = frac_clamp((int)var_CreateGetInteger(filter, CFG_PREFIX "map-h"));
        if (sys->map_smooth_ms < 0)
            sys->map_smooth_ms = 0;
        if (sys->map_smooth_ms > 3000)
            sys->map_smooth_ms = 3000;
        sys->tiles = hud_tiles_open(obj);
        sys->map_source = map_source_of(srcopt);
        hud_tiles_configure(sys->tiles, sys->map_source, token, zoom);
        free(srcopt);
        free(token);
    }
    filter->pf_sub_source = Filter;
    return VLC_SUCCESS;
}

static void Close(vlc_object_t *obj)
{
    filter_t *filter = (filter_t *)obj;
    struct filter_sys_t *sys = filter->p_sys;
    if (!sys)
        return;
    if (sys->load_mu_init) {
        vlc_mutex_lock(&sys->load_mu);
        sys->loader_stop = 1;
        vlc_cond_signal(&sys->load_cond);
        vlc_mutex_unlock(&sys->load_mu);
    }
    if (sys->loader_started)
        vlc_join(sys->loader, NULL);
    hud_tiles_close(sys->tiles);
    sys->tiles = NULL;
    if (sys->map_pic)
        picture_Release(sys->map_pic);
    sys->map_pic = NULL;
    expand_cache_free();
    hud_zone_close();
    gpmf_close(sys->gpmf);
    if (sys->load_mu_init) {
        vlc_cond_destroy(&sys->load_cond);
        vlc_mutex_destroy(&sys->load_mu);
    }
    if (sys->input)
        vlc_object_release(sys->input);
    free(sys);
    filter->p_sys = NULL;
}

/* date is the OSD clock (mdate), not the movie time. The input "time"
 * variable is the movie clock in vlc ticks and moves about four times
 * a second. Between those updates, step it with the OSD clock. */
static double media_seconds(filter_t *filter, struct filter_sys_t *sys, vlc_tick_t date)
{
    vlc_tick_t ticks;
    int state;
    float rate = 1.f;
    double speed = 0;
    double extra = 0;
    ensure_input(filter, sys);
    if (!sys->input || !(var_Type(sys->input, "time") & VLC_VAR_INTEGER))
        return 0;
    ticks = var_GetInteger(sys->input, "time");
    state = (int)var_GetInteger(sys->input, "state");
    if (var_Type(sys->input, "rate") & VLC_VAR_FLOAT)
        rate = var_GetFloat(sys->input, "rate");
    if (state == PLAYING_S && rate > 0.01f)
        speed = (double)rate;
    if (!sys->anchor_valid || ticks != sys->anchor_time || date < sys->anchor_date) {
        sys->anchor_time = ticks;
        sys->anchor_date = date;
        sys->anchor_valid = 1;
    }
    if (speed != 0 && date >= sys->anchor_date)
        extra = (double)(date - sys->anchor_date) / (double)CLOCK_FREQ * speed;
    if (extra > 1.0)
        extra = 1.0;
    return (double)ticks / (double)CLOCK_FREQ + extra;
}

static const char *clock_name(int source)
{
    if (source == GPMF_CLOCK_GPSU)
        return "gpsu";
    if (source == GPMF_CLOCK_GPS9)
        return "gps9";
    if (source == GPMF_CLOCK_FILE)
        return "file";
    return "none";
}

/* mps receives 2D speed in m/s, or -1 when the sample has no speed column. */
static int sample_gnss(const gpmf_file *file, double t, double *lat, double *lon, double *mps)
{
    const gpmf_series *gps;
    const gpmf_series *fix;
    float v[GPMF_MAX_CH];
    int n;
    if (mps)
        *mps = -1;
    if (!file)
        return -1;
    gps = gpmf_series_find(file, GPMF_FOURCC('G', 'P', 'S', '9'));
    if (!gps)
        gps = gpmf_series_find(file, GPMF_FOURCC('G', 'P', 'S', '5'));
    if (!gps)
        return -1;
    n = gpmf_sample(gps, t, v);
    if (n < 2)
        return -1;
    fix = gpmf_series_find(file, GPMF_FOURCC('G', 'P', 'S', 'F'));
    if (fix) {
        float fv;
        if (gpmf_sample(fix, t, &fv) < 1 || fv < 0.5f)
            return -1;
    }
    if (v[0] < -90.f || v[0] > 90.f || v[1] < -180.f || v[1] > 180.f)
        return -1;
    if (v[0] > -0.01f && v[0] < 0.01f && v[1] > -0.01f && v[1] < 0.01f)
        return -1;
    *lat = (double)v[0];
    *lon = (double)v[1];
    if (mps && n >= 4)
        *mps = (double)v[3];
    return 0;
}

static int gnss_latlon(const gpmf_file *file, double t, double *lat, double *lon)
{
    return sample_gnss(file, t, lat, lon, NULL);
}

/* Hold the last course while nearly stopped. The shown fix glides toward
 * the raw one unless the playback jumps. */
static void update_map_course(struct filter_sys_t *sys, double t)
{
    double lat, lon, mps, lat0, lon0, dt;
    double course;
    int have = 0;
    if (sample_gnss(sys->gpmf, t, &lat, &lon, &mps) != 0)
        return;
    sys->map_has = 1;
    course = sys->map_course;
    if (!(mps >= 0 && mps < 0.5) &&
        sample_gnss(sys->gpmf, t - 1.0, &lat0, &lon0, NULL) == 0 &&
        hud_map_distance_m(lat0, lon0, lat, lon) >= 0.8) {
        course = hud_map_bearing(lat0, lon0, lat, lon);
        have = 1;
    } else if (sys->map_course_set) {
        have = 1;
    }
    if (!sys->map_fix_valid) {
        sys->map_lat = lat;
        sys->map_lon = lon;
        if (have) {
            sys->map_course = course;
            sys->map_course_set = 1;
        }
        sys->map_fix_valid = 1;
        sys->map_fix_t = t;
        return;
    }
    dt = t - sys->map_fix_t;
    sys->map_fix_t = t;
    if (sys->map_smooth_ms <= 0 || dt < 0.0 || dt > 2.0 ||
        hud_map_distance_m(sys->map_lat, sys->map_lon, lat, lon) > 250.0) {
        sys->map_lat = lat;
        sys->map_lon = lon;
        if (have) {
            sys->map_course = course;
            sys->map_course_set = 1;
        }
        return;
    }
    hud_map_ease(&sys->map_lat, &sys->map_lon, &sys->map_course,
                 lat, lon, course, have, dt, sys->map_smooth_ms / 1000.0);
    if (have)
        sys->map_course_set = 1;
}

static int map_source_of(const char *s)
{
    if (s && strcmp(s, "mapbox") == 0)
        return HUD_TILE_MAPBOX;
    if (s && strcmp(s, "google") == 0)
        return HUD_TILE_GOOGLE;
    return HUD_TILE_OSM;
}

#if defined(_WIN32)
struct video_hwnd {
    HWND best;
    int w, h, area;
};

static BOOL CALLBACK pick_video_hwnd(HWND hwnd, LPARAM lp)
{
    struct video_hwnd *found = (struct video_hwnd *)lp;
    char name[160];
    RECT rect;
    int w, h, area;
    if (!GetClassNameA(hwnd, name, (int)sizeof name))
        return TRUE;
    if (strncmp(name, "VLC video output", 16) != 0)
        return TRUE;
    if (!GetClientRect(hwnd, &rect))
        return TRUE;
    w = rect.right - rect.left;
    h = rect.bottom - rect.top;
    if (w < 32 || h < 32)
        return TRUE;
    area = w * h;
    if (area > found->area) {
        found->best = hwnd;
        found->w = w;
        found->h = h;
        found->area = area;
    }
    return TRUE;
}

static int client_size(HWND hwnd, int *w, int *h)
{
    RECT rect;
    struct video_hwnd found;
    if (!hwnd || !IsWindow(hwnd))
        return 0;
    memset(&found, 0, sizeof found);
    EnumChildWindows(hwnd, pick_video_hwnd, (LPARAM)&found);
    if (found.best) {
        *w = found.w;
        *h = found.h;
        return 1;
    }
    if (!GetClientRect(hwnd, &rect))
        return 0;
    *w = rect.right - rect.left;
    *h = rect.bottom - rect.top;
    return *w >= 32 && *h >= 32;
}
#endif

/* Picture size on screen, in the same pixels VLC uses to place the video.
 * The window object is a child of the vout. Its video child matches the
 * picture; otherwise the window client area is the fit rectangle. */
static int display_pixels(vlc_object_t *obj, int *w, int *h, int depth)
{
    vlc_list_t *list;
    int i, ok = 0;
    if (!obj || depth > 4)
        return 0;
    list = vlc_list_children(obj);
    if (!list)
        return 0;
    for (i = 0; i < list->i_count && !ok; i++) {
        vlc_object_t *child = list->p_values[i].p_address;
        vout_window_t *window;
        if (!child || !child->obj.object_type)
            continue;
        if (strcmp(child->obj.object_type, "window") != 0)
            continue;
        window = (vout_window_t *)child;
#if defined(_WIN32)
        if (window->type == VOUT_WINDOW_TYPE_HWND)
            ok = client_size((HWND)window->handle.hwnd, w, h);
#else
        (void)window;
#endif
    }
    for (i = 0; !ok && i < list->i_count; i++) {
        vlc_object_t *child = list->p_values[i].p_address;
        if (child)
            ok = display_pixels(child, w, h, depth + 1);
    }
    vlc_list_release(list);
    return ok;
}

/* Tile art is authored at 256px. Draw it a third larger so street names
 * read on the picture. Zoom still chooses which tile is fetched. */
#define MAP_TILE_SCREEN (256 * 4 / 3)

/* One tile should land near MAP_TILE_SCREEN screen pixels. The default
 * overlay is stored in video pixels, then VLC scales it with the picture. */
static int map_tile_span(filter_t *filter, struct filter_sys_t *sys, int video_w, int video_h)
{
    vlc_tick_t now = mdate();
    int dw = 0, dh = 0;
    int span;
    if (sys->map_span > 0 && now < sys->map_span_until)
        return sys->map_span;
    if (!sys->mouse)
        sys->mouse = find_mouse(filter);
    if (video_w > 0 && video_h > 0 && display_pixels(sys->mouse, &dw, &dh, 0) && dw > 0 && dh > 0) {
        int64_t sx = (int64_t)MAP_TILE_SCREEN * video_w / dw;
        int64_t sy = (int64_t)MAP_TILE_SCREEN * video_h / dh;
        span = (int)(sx > sy ? sx : sy);
    } else if (video_h > 1080) {
        span = MAP_TILE_SCREEN * video_h / 1080;
    } else {
        span = MAP_TILE_SCREEN;
    }
    if (span < 64)
        span = 64;
    if (span > 1536)
        span = 1536;
    if (span != sys->map_span)
        msg_Info(filter, "gpmf map draws one tile across %d video px (picture %dx%d)",
                 span, dw, dh);
    sys->map_span = span;
    sys->map_span_until = now + 300000;
    return span;
}

/* Video-pixel chrome, scaled into a display-sized immersive buffer. */
static int mul_div(int v, int num, int den)
{
    if (v == 0 || den <= 0 || num <= 0)
        return 0;
    return (int)(((int64_t)v * num + den / 2) / den);
}

/* Clock height in the pixels this preset draws. Map zoom and the map panel
 * are not inputs. Immersive is already in screen pixels. The default inset
 * is stored in video pixels, so a screen pixel grows with the picture. */
static int hud_draw_cap(struct filter_sys_t *sys, int immersive, int video_h)
{
    int pref = clamp_hud_size(sys->hud_size);
    int dw = 0, dh = 0;
    if (immersive || video_h <= 0)
        return pref;
    if (sys->mouse && display_pixels(sys->mouse, &dw, &dh, 0) && dh >= 32)
        return mul_div(pref, video_h, dh);
    return mul_div(pref, video_h, 1080);
}

static void scale_box(hud_map_box *box, int dw, int dh, int vw, int vh)
{
    if (!box || vw <= 0 || vh <= 0 || dw <= 0 || dh <= 0)
        return;
    box->hide_x = mul_div(box->hide_x, dw, vw);
    box->hide_y = mul_div(box->hide_y, dh, vh);
    box->hide_w = mul_div(box->hide_w, dw, vw);
    box->hide_h = mul_div(box->hide_h, dh, vh);
    box->zin_x = mul_div(box->zin_x, dw, vw);
    box->zin_y = mul_div(box->zin_y, dh, vh);
    box->zin_w = mul_div(box->zin_w, dw, vw);
    box->zin_h = mul_div(box->zin_h, dh, vh);
    box->zout_x = mul_div(box->zout_x, dw, vw);
    box->zout_y = mul_div(box->zout_y, dh, vh);
    box->zout_w = mul_div(box->zout_w, dw, vw);
    box->zout_h = mul_div(box->zout_h, dh, vh);
    box->oup_x = mul_div(box->oup_x, dw, vw);
    box->oup_y = mul_div(box->oup_y, dh, vh);
    box->oup_w = mul_div(box->oup_w, dw, vw);
    box->oup_h = mul_div(box->oup_h, dh, vh);
    box->odn_x = mul_div(box->odn_x, dw, vw);
    box->odn_y = mul_div(box->odn_y, dh, vh);
    box->odn_w = mul_div(box->odn_w, dw, vw);
    box->odn_h = mul_div(box->odn_h, dh, vh);
    box->lift = mul_div(box->lift, dh, vh);
    box->grip = mul_div(box->grip, dh, vh);
    if (box->full) {
        box->w = dw;
        box->h = dh;
    } else {
        box->w = mul_div(box->w, dw, vw);
        box->h = mul_div(box->h, dh, vh);
        if (box->w < 8)
            box->w = 8;
        if (box->h < 8)
            box->h = 8;
    }
}

/* On-screen video size. The immersive map is drawn here, one tile per
 * MAP_TILE_SCREEN px.
 * A missing window falls back near 1080p, never the raw 4K frame.
 * GetClientRect is already in the pixels VLC uses for this window. */
static void immersive_buffer(filter_t *filter, struct filter_sys_t *sys,
                             int vw, int vh, int *dw, int *dh)
{
    int mw = 0, mh = 0;
    int have = 0;
    if (!sys->mouse)
        sys->mouse = find_mouse(filter);
    if (vw > 0 && vh > 0 && display_pixels(sys->mouse, &mw, &mh, 0) && mw >= 32 && mh >= 32)
        have = 1;
    if (!have && sys->imm_measured && sys->imm_w >= 32 && sys->imm_h >= 32) {
        *dw = sys->imm_w;
        *dh = sys->imm_h;
        return;
    }
    if (!have) {
        if (vh > 1080) {
            *dh = 1080;
            *dw = vw > 0 ? (int)((int64_t)vw * 1080 / vh) : 1920;
        } else {
            *dw = vw > 0 ? vw : 1920;
            *dh = vh > 0 ? vh : 1080;
        }
        if (*dw < 32)
            *dw = 32;
        if (*dh < 32)
            *dh = 32;
    } else {
        *dw = mw;
        *dh = mh;
        sys->imm_measured = 1;
    }
    if (*dw != sys->imm_w || *dh != sys->imm_h) {
        if (sys->user_moved && sys->imm_w > 0 && sys->imm_h > 0) {
            sys->hud_x = (int)((int64_t)sys->hud_x * *dw / sys->imm_w);
            sys->hud_y = (int)((int64_t)sys->hud_y * *dh / sys->imm_h);
        }
        msg_Info(filter, "gpmf immersive buffer %dx%d", *dw, *dh);
        sys->imm_w = *dw;
        sys->imm_h = *dh;
    }
}

static subpicture_region_t *draw_map(struct filter_sys_t *sys, int x, int y,
                                    const hud_map_box *box, int expanded,
                                    double course_deg)
{
    subpicture_region_t *region = new_rgba(box->w, box->h, 0, x, y);
    canvas_t c;
    int zoom_down = 0;
    if (!region)
        return NULL;
    c = canvas_of(region);
    if (!box->full)
        hud_map_panel(c.p, c.pitch, c.pix, box, expanded);
    /* The credit is its own region at the video corner, not part of this picture. */
    if (expanded && sys->tiles)
        hud_tiles_paint(sys->tiles, c.p, c.pitch, c.pix, box, NULL, 0,
                        box->full ? sys->map_opacity : 100);
    if (!box->full && sys->left_down && sys->dragging == 4) {
        if (sys->press_hit == HUD_MAP_HIT_ZOOM_IN)
            zoom_down = 1;
        else if (sys->press_hit == HUD_MAP_HIT_ZOOM_OUT)
            zoom_down = 2;
    }
    if (box->full && expanded) {
        /* Pressed and hovered buttons are a separate region. Baking them
         * into this picture would resample the whole map on every hover. */
        hud_map_dock(c.p, c.pitch, c.pix, box, sys->map_zoom, 0,
                     sys->map_opacity, 0);
    } else {
        hud_map_controls(c.p, c.pitch, c.pix, box, expanded, sys->map_hot || sys->dragging == 3,
                         sys->map_zoom, zoom_down);
    }
    {
        int arrow = sys->map_arrow;
        if (box->full && sys->video_h > 0 && box->h > 0 && box->h != sys->video_h)
            arrow = mul_div(sys->map_arrow, box->h, sys->video_h);
        if (arrow < 8)
            arrow = 8;
        hud_map_marker(c.p, c.pitch, c.pix, box, expanded, arrow, course_deg,
                       sys->show_arrow);
    }
    return region;
}

static int dock_rect(const hud_map_box *box, int hit, int *x, int *y, int *w, int *h)
{
    switch (hit) {
    case HUD_MAP_HIT_ZOOM_IN:
        *x = box->zin_x; *y = box->zin_y; *w = box->zin_w; *h = box->zin_h;
        break;
    case HUD_MAP_HIT_ZOOM_OUT:
        *x = box->zout_x; *y = box->zout_y; *w = box->zout_w; *h = box->zout_h;
        break;
    case HUD_MAP_HIT_OPACITY_UP:
        *x = box->oup_x; *y = box->oup_y; *w = box->oup_w; *h = box->oup_h;
        break;
    case HUD_MAP_HIT_OPACITY_DOWN:
        *x = box->odn_x; *y = box->odn_y; *w = box->odn_w; *h = box->odn_h;
        break;
    case HUD_MAP_HIT_HIDE:
        *x = box->hide_x; *y = box->hide_y; *w = box->hide_w; *h = box->hide_h;
        break;
    default:
        return 0;
    }
    return *w > 0 && *h > 0;
}

/* Bright disc for the dock button under the pointer. Small on purpose:
 * the full-frame map stays cached. */
static subpicture_region_t *draw_dock_face(struct filter_sys_t *sys,
                                          const hud_map_box *box, int ox, int oy)
{
    int x, y, w, h, pressed;
    subpicture_region_t *region;
    canvas_t c;
    if (!sys->dock_hot || !dock_rect(box, sys->dock_hot, &x, &y, &w, &h))
        return NULL;
    region = new_rgba(w, h, 0, ox + x, oy + y);
    if (!region)
        return NULL;
    c = canvas_of(region);
    pressed = sys->left_down && sys->dragging == 4 && sys->press_hit == sys->dock_hot;
    hud_map_dock_face(c.p, c.pitch, c.pix, w, h, sys->dock_hot, pressed,
                      sys->map_zoom, sys->map_opacity);
    return region;
}

/* Screen pixels, in the same space the credit region is drawn. */
static int credit_px(const struct filter_sys_t *sys, int immersive, int video_h,
                     int screen_px)
{
    int dw = 0, dh = 0;
    if (screen_px <= 0)
        return 0;
    if (immersive || video_h <= 0)
        return screen_px;
    if (sys->mouse && display_pixels(sys->mouse, &dw, &dh, 0) && dh >= 32)
        return mul_div(screen_px, video_h, dh);
    return mul_div(screen_px, video_h, 1080);
}

/* Attribution at the bottom-left of the video. Nine screen pixels, so it
 * stays smaller than the HUD labels and the settings sheet. The pill is
 * its own region and stays solid when the tiles are faint. */
static subpicture_region_t *draw_credit(struct filter_sys_t *sys,
                                       int space_w, int space_h,
                                       int immersive, int video_h)
{
    char credit[160];
    canvas_t c;
    hud_map_box chip;
    subpicture_region_t *region;
    int cap, margin, pad_x, pad_y, top, th, tw, rw, rh, rx, ry;
    int use, scale;
    if (!sys || !sys->tiles || space_w < 16 || space_h < 16)
        return NULL;
    credit[0] = 0;
    hud_tiles_credit(sys->tiles, credit, sizeof credit);
    if (!credit[0])
        return NULL;
    cap = credit_px(sys, immersive, video_h, 9);
    margin = credit_px(sys, immersive, video_h, 6);
    pad_x = credit_px(sys, immersive, video_h, 6);
    pad_y = credit_px(sys, immersive, video_h, 3);
    top = credit_px(sys, immersive, video_h, 3);
    if (cap < 6)
        cap = 6;
    if (margin < 2)
        margin = 2;
    if (pad_x < 3)
        pad_x = 3;
    if (pad_y < 2)
        pad_y = 2;
    if (top < 2)
        top = 2;
    use = hud_font_set_credit(cap) == 0 && font_on();
    scale = cap / 7;
    if (scale < 1)
        scale = 1;
    if (scale > 6)
        scale = 6;
    th = use ? hud_font_span(HUD_FACE_CREDIT) : 7 * scale;
    tw = use ? hud_font_width(HUD_FACE_CREDIT, credit) : text_px(credit, scale);
    if (th < 1 || tw < 1)
        return NULL;
    rw = tw + pad_x * 2;
    rh = top + th + pad_y;
    if (rw > space_w - margin)
        rw = space_w - margin;
    if (rh > space_h - margin)
        rh = space_h - margin;
    if (rw < 8 || rh < 8)
        return NULL;
    rx = margin;
    ry = space_h - margin - rh;
    if (ry < 0)
        ry = 0;
    region = new_rgba(rw, rh, 0, rx, ry);
    if (!region)
        return NULL;
    c = canvas_of(region);
    memset(&chip, 0, sizeof chip);
    chip.w = rw;
    chip.h = rh;
    chip.full = 1;
    hud_map_pill(c.p, c.pitch, c.pix, &chip, 0, 0, rw, rh);
    if (use)
        paint_face(&c, pad_x, top, HUD_FACE_CREDIT, scale, credit, 244, 246, 248, 255);
    else
        draw_text(&c, pad_x, top, scale, credit, 244, 246, 248, 255);
    return region;
}

static int take_zone(filter_t *filter, struct filter_sys_t *sys, double t)
{
    double lat, lon;
    char tz[80];
    if (gnss_latlon(sys->gpmf, t, &lat, &lon) != 0)
        return 0;
    if (hud_zone_lookup(lat, lon, tz, (int)sizeof tz) != 0)
        return 0;
    snprintf(sys->rel_tz, sizeof sys->rel_tz, "%s", tz);
    sys->rel_state = 1;
    if (!sys->rel_logged) {
        msg_Info(filter, "gpmf zone %s at %.5f %.5f", tz, lat, lon);
        sys->rel_logged = 1;
    }
    return 1;
}

/* One zone for the file, from the first fix. The offset is applied later, per frame. */
static void resolve_relative(filter_t *filter, struct filter_sys_t *sys, double t)
{
    static const double probes[] = { 0, 1, 5, 30, 120, 600 };
    int i;
    if (sys->rel_state == 1)
        return;
    if (hud_zone_open() != 0) {
        if (!sys->rel_logged) {
            msg_Warn(filter, "gpmf: timezone database was not found");
            sys->rel_logged = 1;
        }
        return;
    }
    if (sys->rel_state == 0) {
        for (i = 0; i < (int)(sizeof probes / sizeof probes[0]); i++) {
            if (take_zone(filter, sys, probes[i]))
                return;
        }
        sys->rel_state = -1;
    }
    take_zone(filter, sys, t);
}

static void apply_preset(filter_t *filter, struct filter_sys_t *sys)
{
    char *preset = var_GetString(filter, CFG_PREFIX "preset");
    int next = preset && strcmp(preset, "immersive") == 0
        ? PRESET_IMMERSIVE : PRESET_DEFAULT;
    int opacity = (int)var_GetInteger(filter, CFG_PREFIX "map-opacity");
    free(preset);
    if (next != sys->preset) {
        int bw, bh;
        hud_span_now(sys, sys->preset, &bw, &bh);
        note_hud_frac(sys, sys->preset, bw, bh);
        sys->hud_slot_x[sys->preset] = sys->hud_x;
        sys->hud_slot_y[sys->preset] = sys->hud_y;
        sys->hud_slot_moved[sys->preset] = sys->user_moved;
        sys->preset = next;
        sys->hud_x = sys->hud_slot_x[next];
        sys->hud_y = sys->hud_slot_y[next];
        sys->user_moved = sys->hud_slot_moved[next];
    }
    if (opacity < 10)
        opacity = 10;
    if (opacity > 100)
        opacity = 100;
    sys->map_opacity = opacity;
}

static void copy_txt(char *dst, size_t n, const char *src)
{
    if (!src)
        src = "";
    snprintf(dst, n, "%s", src);
}

static picture_pool_t *nn_pool;
static int nn_pool_w, nn_pool_h;

struct aa_keep {
    int sw, sh, dw, dh, spitch;
    uint8_t *src;
    size_t src_n;
    picture_t *pic;
};
static struct aa_keep aa_keep[4];

static void expand_cache_free(void)
{
    int i;
    for (i = 0; i < 4; i++) {
        free(aa_keep[i].src);
        if (aa_keep[i].pic)
            picture_Release(aa_keep[i].pic);
        memset(&aa_keep[i], 0, sizeof aa_keep[i]);
    }
    if (nn_pool) {
        picture_pool_Release(nn_pool);
        nn_pool = NULL;
        nn_pool_w = 0;
        nn_pool_h = 0;
    }
}

/* A picture from the pool, or a fresh one when every pooled picture is in use.
 * The pool is allocated once per video size. Later frames skip that zero-fill. */
static picture_t *nn_acquire(int dw, int dh)
{
    video_format_t fmt;
    picture_t *pic;
    if (nn_pool && (nn_pool_w != dw || nn_pool_h != dh)) {
        picture_pool_Release(nn_pool);
        nn_pool = NULL;
    }
    if (!nn_pool) {
        video_format_Init(&fmt, VLC_CODEC_RGBA);
        fmt.i_width = fmt.i_visible_width = dw;
        fmt.i_height = fmt.i_visible_height = dh;
        fmt.i_sar_num = fmt.i_sar_den = 1;
        nn_pool = picture_pool_NewFromFormat(&fmt, 3);
        video_format_Clean(&fmt);
        if (nn_pool) {
            nn_pool_w = dw;
            nn_pool_h = dh;
        }
    }
    if (nn_pool) {
        pic = picture_pool_Get(nn_pool);
        if (pic)
            return pic;
    }
    video_format_Init(&fmt, VLC_CODEC_RGBA);
    fmt.i_width = fmt.i_visible_width = dw;
    fmt.i_height = fmt.i_visible_height = dh;
    fmt.i_sar_num = fmt.i_sar_den = 1;
    pic = picture_NewFromFormat(&fmt);
    video_format_Clean(&fmt);
    return pic;
}

/* Point-sample src into a new RGBA picture. Every destination pixel is
 * written. Rows that repeat a source line are copied. */
static picture_t *nn_picture(const uint8_t *src, int spitch, int spix,
                             int sw, int sh, int dw, int dh)
{
    picture_t *pic;
    uint8_t *dst;
    uint8_t *prev;
    int *col;
    int dpitch;
    int lines;
    int y, x;
    int prev_sy;
    if (!src || spix != 4 || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return NULL;
    if (spitch < sw * 4)
        return NULL;
    pic = nn_acquire(dw, dh);
    if (!pic || !pic->p[0].p_pixels) {
        if (pic)
            picture_Release(pic);
        return NULL;
    }
    if (pic->p[0].i_pixel_pitch != 0 && pic->p[0].i_pixel_pitch != 4) {
        picture_Release(pic);
        return NULL;
    }
    dpitch = pic->p[0].i_pitch;
    lines = pic->p[0].i_visible_lines > 0 ? pic->p[0].i_visible_lines : pic->p[0].i_lines;
    if (dpitch < dw * 4 || lines < dh) {
        picture_Release(pic);
        return NULL;
    }
    col = malloc(sizeof(int) * (size_t)dw);
    if (!col) {
        picture_Release(pic);
        return NULL;
    }
    for (x = 0; x < dw; x++) {
        int sx = (int)((int64_t)x * sw / dw);
        if (sx >= sw)
            sx = sw - 1;
        if (sx < 0)
            sx = 0;
        col[x] = sx;
    }
    dst = pic->p[0].p_pixels;
    prev = NULL;
    prev_sy = -1;
    for (y = 0; y < dh; y++) {
        int sy = (int)((int64_t)y * sh / dh);
        uint8_t *drow = dst + (size_t)y * (size_t)dpitch;
        const uint8_t *srow;
        if (sy >= sh)
            sy = sh - 1;
        if (sy < 0)
            sy = 0;
        if (sy == prev_sy && prev) {
            memcpy(drow, prev, (size_t)dw * 4);
            continue;
        }
        srow = src + (size_t)sy * (size_t)spitch;
        {
            uint32_t *dp = (uint32_t *)drow;
            x = 0;
            while (x < dw) {
                int sx = col[x];
                int x2 = x + 1;
                uint32_t pixv;
                memcpy(&pixv, srow + (size_t)sx * 4, 4);
                while (x2 < dw && col[x2] == sx)
                    x2++;
                while (x < x2)
                    dp[x++] = pixv;
            }
        }
        prev_sy = sy;
        prev = drow;
    }
    free(col);
    return pic;
}

/* Bilinear, in premultiplied alpha, for the HUD and the sheet. Nearest
 * enlargement turns antialiased strokes into steps. The full-frame map
 * stays on the point sampler so tiles stay sharp. */
static picture_t *aa_picture(const uint8_t *src, int spitch, int spix,
                             int sw, int sh, int dw, int dh)
{
    video_format_t fmt;
    picture_t *pic;
    uint8_t *dst;
    int dpitch;
    int lines;
    int y, x;
    if (!src || spix != 4 || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return NULL;
    if (spitch < sw * 4)
        return NULL;
    video_format_Init(&fmt, VLC_CODEC_RGBA);
    fmt.i_width = fmt.i_visible_width = dw;
    fmt.i_height = fmt.i_visible_height = dh;
    fmt.i_sar_num = fmt.i_sar_den = 1;
    pic = picture_NewFromFormat(&fmt);
    video_format_Clean(&fmt);
    if (!pic || !pic->p[0].p_pixels) {
        if (pic)
            picture_Release(pic);
        return NULL;
    }
    if (pic->p[0].i_pixel_pitch != 0 && pic->p[0].i_pixel_pitch != 4) {
        picture_Release(pic);
        return NULL;
    }
    dpitch = pic->p[0].i_pitch;
    lines = pic->p[0].i_visible_lines > 0 ? pic->p[0].i_visible_lines : pic->p[0].i_lines;
    if (dpitch < dw * 4 || lines < dh) {
        picture_Release(pic);
        return NULL;
    }
    dst = pic->p[0].p_pixels;
    /* Weights depend only on the axis. A flat 2x2 neighborhood is already the
     * bilinear result, so the panel interior is a copy and the glyph edge
     * keeps the same blend. */
    {
        static struct aa_tap { int i0, i1; double f, f0; } *xs, *ys;
        static int xn, yn;
        if (dw > xn) {
            struct aa_tap *grown = (struct aa_tap *)realloc(xs, (size_t)dw * sizeof *xs);
            if (!grown) {
                picture_Release(pic);
                return NULL;
            }
            xs = grown;
            xn = dw;
        }
        if (dh > yn) {
            struct aa_tap *grown = (struct aa_tap *)realloc(ys, (size_t)dh * sizeof *ys);
            if (!grown) {
                picture_Release(pic);
                return NULL;
            }
            ys = grown;
            yn = dh;
        }
        for (x = 0; x < dw; x++) {
            double sx = ((x + 0.5) * (double)sw) / (double)dw - 0.5;
            int x0 = (int)floor(sx);
            int x1 = x0 + 1;
            xs[x].f = sx - x0;
            xs[x].f0 = 1.0 - xs[x].f;
            xs[x].i0 = x0 < 0 ? 0 : (x0 >= sw ? sw - 1 : x0);
            if (x1 < 0)
                x1 = 0;
            else if (x1 >= sw)
                x1 = sw - 1;
            xs[x].i1 = x1;
        }
        for (y = 0; y < dh; y++) {
            double sy = ((y + 0.5) * (double)sh) / (double)dh - 0.5;
            int y0 = (int)floor(sy);
            int y1 = y0 + 1;
            ys[y].f = sy - y0;
            ys[y].f0 = 1.0 - ys[y].f;
            ys[y].i0 = y0 < 0 ? 0 : (y0 >= sh ? sh - 1 : y0);
            if (y1 < 0)
                y1 = 0;
            else if (y1 >= sh)
                y1 = sh - 1;
            ys[y].i1 = y1;
        }
        for (y = 0; y < dh; y++) {
            const uint8_t *row0 = src + (size_t)ys[y].i0 * (size_t)spitch;
            const uint8_t *row1 = src + (size_t)ys[y].i1 * (size_t)spitch;
            double fy = ys[y].f;
            double fy0 = ys[y].f0;
            uint8_t *drow = dst + (size_t)y * (size_t)dpitch;
            for (x = 0; x < dw; x++) {
                const uint8_t *p00 = row0 + (size_t)xs[x].i0 * 4;
                const uint8_t *p10 = row0 + (size_t)xs[x].i1 * 4;
                const uint8_t *p01 = row1 + (size_t)xs[x].i0 * 4;
                const uint8_t *p11 = row1 + (size_t)xs[x].i1 * 4;
                int r00, g00, b00, a00, r10, g10, b10, a10;
                int r01, g01, b01, a01, r11, g11, b11, a11;
                double fx, fx0, pr, pg, pb, pa;
                int ia, ir, ig, ib;
                if (p00[0] == p10[0] && p00[1] == p10[1] && p00[2] == p10[2] && p00[3] == p10[3] &&
                    p00[0] == p01[0] && p00[1] == p01[1] && p00[2] == p01[2] && p00[3] == p01[3] &&
                    p00[0] == p11[0] && p00[1] == p11[1] && p00[2] == p11[2] && p00[3] == p11[3]) {
                    drow[x * 4] = p00[0];
                    drow[x * 4 + 1] = p00[1];
                    drow[x * 4 + 2] = p00[2];
                    drow[x * 4 + 3] = p00[3];
                    continue;
                }
                fx = xs[x].f;
                fx0 = xs[x].f0;
                a00 = p00[3];
                r00 = p00[0] * a00;
                g00 = p00[1] * a00;
                b00 = p00[2] * a00;
                a10 = p10[3];
                r10 = p10[0] * a10;
                g10 = p10[1] * a10;
                b10 = p10[2] * a10;
                a01 = p01[3];
                r01 = p01[0] * a01;
                g01 = p01[1] * a01;
                b01 = p01[2] * a01;
                a11 = p11[3];
                r11 = p11[0] * a11;
                g11 = p11[1] * a11;
                b11 = p11[2] * a11;
                pr = r00 * fx0 * fy0 + r10 * fx * fy0 + r01 * fx0 * fy + r11 * fx * fy;
                pg = g00 * fx0 * fy0 + g10 * fx * fy0 + g01 * fx0 * fy + g11 * fx * fy;
                pb = b00 * fx0 * fy0 + b10 * fx * fy0 + b01 * fx0 * fy + b11 * fx * fy;
                pa = a00 * fx0 * fy0 + a10 * fx * fy0 + a01 * fx0 * fy + a11 * fx * fy;
                ia = (int)(pa + 0.5);
                if (ia <= 0) {
                    drow[x * 4] = 0;
                    drow[x * 4 + 1] = 0;
                    drow[x * 4 + 2] = 0;
                    drow[x * 4 + 3] = 0;
                    continue;
                }
                if (ia > 255)
                    ia = 255;
                ir = (int)(pr / pa + 0.5);
                ig = (int)(pg / pa + 0.5);
                ib = (int)(pb / pa + 0.5);
                if (ir < 0)
                    ir = 0;
                if (ig < 0)
                    ig = 0;
                if (ib < 0)
                    ib = 0;
                if (ir > 255)
                    ir = 255;
                if (ig > 255)
                    ig = 255;
                if (ib > 255)
                    ib = 255;
                drow[x * 4] = (uint8_t)ir;
                drow[x * 4 + 1] = (uint8_t)ig;
                drow[x * 4 + 2] = (uint8_t)ib;
                drow[x * 4 + 3] = (uint8_t)ia;
            }
        }
    }
    return pic;
}

/* The HUD and the sheet are bilinear. The same glyphs come back for most
 * map steps, so the enlarged picture is kept and compared by its source. */
static picture_t *aa_cached(const uint8_t *src, int spitch, int spix,
                            int sw, int sh, int dw, int dh)
{
    size_t bytes;
    int i, slot;
    picture_t *pic;
    uint8_t *copy;
    if (!src || spitch < sw * 4 || sh < 1)
        return NULL;
    bytes = (size_t)spitch * (size_t)sh;
    slot = -1;
    for (i = 0; i < 4; i++) {
        if (!aa_keep[i].pic) {
            if (slot < 0)
                slot = i;
            continue;
        }
        if (aa_keep[i].sw == sw && aa_keep[i].sh == sh &&
            aa_keep[i].dw == dw && aa_keep[i].dh == dh &&
            aa_keep[i].spitch == spitch && aa_keep[i].src_n == bytes &&
            aa_keep[i].src && memcmp(aa_keep[i].src, src, bytes) == 0)
            return picture_Hold(aa_keep[i].pic);
    }
    pic = aa_picture(src, spitch, spix, sw, sh, dw, dh);
    if (!pic)
        return NULL;
    if (slot < 0)
        slot = 0;
    copy = (uint8_t *)malloc(bytes);
    if (!copy)
        return pic;
    memcpy(copy, src, bytes);
    free(aa_keep[slot].src);
    if (aa_keep[slot].pic)
        picture_Release(aa_keep[slot].pic);
    memset(&aa_keep[slot], 0, sizeof aa_keep[slot]);
    aa_keep[slot].src = copy;
    aa_keep[slot].src_n = bytes;
    aa_keep[slot].sw = sw;
    aa_keep[slot].sh = sh;
    aa_keep[slot].dw = dw;
    aa_keep[slot].dh = dh;
    aa_keep[slot].spitch = spitch;
    aa_keep[slot].pic = picture_Hold(pic);
    return pic;
}

/* Replace each region's picture with video pixels. A region that already
 * fills the display is pinned to the video size so rounding leaves no gap.
 * The chain is unchanged if any picture cannot be built. */
static int expand_immersive(subpicture_region_t *head,
                            int space_w, int space_h, int vw, int vh)
{
    enum { NN_JOBS = 8 };
    struct nn_job {
        subpicture_region_t *region;
        picture_t *pic;
        int x, y, w, h;
    } job[NN_JOBS];
    int n = 0;
    int i;
    subpicture_region_t *r;
    if (!head || space_w <= 0 || space_h <= 0 || vw <= 0 || vh <= 0)
        return 0;
    memset(job, 0, sizeof job);
    for (r = head; r; r = r->p_next) {
        picture_t *old;
        picture_t *pic;
        int sw, sh, dw, dh, dx, dy;
        int spix;
        int full;
        if (n >= NN_JOBS)
            goto fail;
        old = r->p_picture;
        if (!old || !old->p[0].p_pixels)
            goto fail;
        sw = (int)r->fmt.i_visible_width;
        sh = (int)r->fmt.i_visible_height;
        spix = old->p[0].i_pixel_pitch > 0 ? old->p[0].i_pixel_pitch : 4;
        full = (r->i_x == 0 && r->i_y == 0 && sw == space_w && sh == space_h);
        if (full) {
            dw = vw;
            dh = vh;
            dx = 0;
            dy = 0;
        } else {
            dw = mul_div(sw, vw, space_w);
            dh = mul_div(sh, vh, space_h);
            dx = mul_div(r->i_x, vw, space_w);
            dy = mul_div(r->i_y, vh, space_h);
            if (dw < 1)
                dw = 1;
            if (dh < 1)
                dh = 1;
            if (dx < 0)
                dx = 0;
            if (dy < 0)
                dy = 0;
            if (dx >= vw)
                dx = vw - 1;
            if (dy >= vh)
                dy = vh - 1;
            if (dx + dw > vw)
                dw = vw - dx;
            if (dy + dh > vh)
                dh = vh - dy;
            if (dw < 1 || dh < 1)
                goto fail;
        }
        if (full)
            pic = nn_picture(old->p[0].p_pixels, old->p[0].i_pitch, spix, sw, sh, dw, dh);
        else
            pic = aa_cached(old->p[0].p_pixels, old->p[0].i_pitch, spix, sw, sh, dw, dh);
        if (!pic)
            goto fail;
        job[n].region = r;
        job[n].pic = pic;
        job[n].x = dx;
        job[n].y = dy;
        job[n].w = dw;
        job[n].h = dh;
        n++;
    }
    for (i = 0; i < n; i++) {
        picture_Release(job[i].region->p_picture);
        job[i].region->p_picture = job[i].pic;
        job[i].region->fmt.i_width = job[i].w;
        job[i].region->fmt.i_visible_width = job[i].w;
        job[i].region->fmt.i_height = job[i].h;
        job[i].region->fmt.i_visible_height = job[i].h;
        job[i].region->fmt.i_x_offset = 0;
        job[i].region->fmt.i_y_offset = 0;
        job[i].region->fmt.i_sar_num = 1;
        job[i].region->fmt.i_sar_den = 1;
        job[i].region->i_x = job[i].x;
        job[i].region->i_y = job[i].y;
    }
    return 1;
fail:
    for (i = 0; i < n; i++) {
        if (job[i].pic)
            picture_Release(job[i].pic);
    }
    return 0;
}

static void set_push(struct filter_sys_t *sys, int id, int x, int y, int w, int h)
{
    int n;
    if (!sys || id == 0 || w <= 0 || h <= 0)
        return;
    n = sys->set_hit_n;
    if (n >= SET_HIT_CAP)
        return;
    sys->set_id[n] = id;
    sys->set_hx[n] = x;
    sys->set_hy[n] = y;
    sys->set_hw[n] = w;
    sys->set_hh[n] = h;
    sys->set_hit_n = n + 1;
}

/* Screen pixels, turned into the pixels this sheet is drawn in. */
static int sheet_px(int screen_px, int space, int screen_span)
{
    int v;
    if (screen_px <= 0)
        return 0;
    if (screen_span < 1)
        screen_span = 1;
    if (space < 1)
        space = screen_span;
    v = (int)(((int64_t)screen_px * space + screen_span / 2) / screen_span);
    return v < 1 ? 1 : v;
}

static void place_sheet(int hud_x, int hud_y, int hud_w, int hud_h,
                        int sw, int sh, int space_w, int space_h, int gap,
                        int *ox, int *oy)
{
    int px = hud_x + (hud_w - sw) / 2;
    int py = hud_y + hud_h + gap;
    if (space_w < 1)
        space_w = sw;
    if (space_h < 1)
        space_h = sh;
    if (py + sh > space_h && hud_y - gap - sh >= 0)
        py = hud_y - gap - sh;
    if (px + sw > space_w)
        px = space_w - sw;
    if (py + sh > space_h)
        py = space_h - sh;
    if (px < 0)
        px = 0;
    if (py < 0)
        py = 0;
    *ox = px;
    *oy = py;
}

/* Choice-table row for a segmented option. Toggles and steppers return -1. */
static int sheet_choice_row(int opt)
{
    switch (opt) {
    case OPT_PRESET: return 0;
    case OPT_ZONE: return 1;
    case OPT_UNITS: return 2;
    case OPT_DATE: return 3;
    case OPT_TIME: return 4;
    case OPT_SOURCE: return 5;
    default: return -1;
    }
}

/* The sheet lists the options that have no control on the picture.
 * Zoom, opacity, and the hide marks stay where they are. The map token
 * stays in Preferences. A choice writes the live variable only.
 * Save as default is the one control that writes the config file. */
static subpicture_region_t *draw_settings(struct filter_sys_t *sys, const hud_layout_t *L,
                                         int hud_x, int hud_y, int hud_w, int hud_h,
                                         int space_w, int space_h)
{
    static const char *const labels[OPT_COUNT] = {
        "Preset", "Zone", "Units", "Date", "Time",
        "Speed", "Altitude", "Bearing", "Orientation",
        "Map", "Arrow", "Length", "Smooth"
    };
    /* Date, time, and units show the readout, not a code name. */
    static const char *const choice[6][3] = {
        { "Default", "Immersive", "" },
        { "Local", "Relative", "UTC" },
        { "MPH, FT", "KM/H, M", "" },
        { "September 1, 2026", "2026-09-01", "" },
        { "8:41 PM", "20:41", "" },
        { "OSM", "Mapbox", "Google" }
    };
    static const int nchoice[6] = { 2, 3, 2, 2, 2, 3 };
    const char *footer = "Map token stays in Preferences.";
    int sel[OPT_COUNT];
    int lab_w = 0;
    int panel_w, panel_h, content, text_h, gap, pad, row_h, row_gap, seg_gap;
    int hpad, split, inset, row_rad;
    int ls, track, i, y, ox, oy, pressed, nseg;
    int rad, edge, inner_rad;
    char arrow_txt[16], smooth_txt[16];
    struct {
        int id, x, y, w, h, selected, muted, kind;
        char text[32];
    } segs[SET_HIT_CAP];
    subpicture_region_t *region;
    canvas_t c;

    memset(sel, 0, sizeof sel);
    sel[OPT_PRESET] = sys->preset == PRESET_IMMERSIVE ? 1 : 0;
    sel[OPT_ZONE] = sys->zone_mode == ZONE_RELATIVE ? 1 : sys->zone_mode == ZONE_UTC ? 2 : 0;
    sel[OPT_UNITS] = sys->imperial ? 0 : 1;
    sel[OPT_DATE] = sys->date_long ? 0 : 1;
    sel[OPT_TIME] = sys->time_12 ? 0 : 1;
    sel[OPT_SPEED] = sys->show_speed ? 0 : 1;
    sel[OPT_ALT] = sys->show_alt ? 0 : 1;
    sel[OPT_BEARING] = sys->show_bearing ? 0 : 1;
    sel[OPT_ORIENT] = sys->show_orient ? 0 : 1;
    sel[OPT_SHOW_ARROW] = sys->show_arrow ? 0 : 1;
    sel[OPT_SOURCE] = sys->map_source;
    if (sel[OPT_SOURCE] < 0 || sel[OPT_SOURCE] > 2)
        sel[OPT_SOURCE] = 0;
    snprintf(arrow_txt, sizeof arrow_txt, "%d", sys->map_arrow);
    snprintf(smooth_txt, sizeof smooth_txt, "%d ms", sys->map_smooth_ms);
    /* Body type stays near 13 screen pixels. Default is drawn in video
     * pixels and scaled by VLC, so those pixels are larger than the window. */
    {
        int screen_h = space_h;
        int cap;
        if (sys->preset != PRESET_IMMERSIVE && sys->mouse) {
            int dw = 0, dh = 0;
            if (display_pixels(sys->mouse, &dw, &dh, 0) && dh >= 32)
                screen_h = dh;
        }
        cap = sheet_px(13, space_h, screen_h);
        if (cap > 40)
            cap = 40;
        hud_font_prepare(cap, cap, cap, cap, cap);
        text_h = font_on() ? hud_font_cap(HUD_FACE_LABEL) : cap;
        row_h = sheet_px(22, space_h, screen_h);
        if (row_h < text_h + sheet_px(8, space_h, screen_h))
            row_h = text_h + sheet_px(8, space_h, screen_h);
        pad = sheet_px(10, space_h, screen_h);
        gap = sheet_px(10, space_h, screen_h);
        row_gap = sheet_px(4, space_h, screen_h);
        seg_gap = sheet_px(4, space_h, screen_h);
        hpad = sheet_px(8, space_h, screen_h);
        split = sheet_px(8, space_h, screen_h);
        inset = sheet_px(2, space_h, screen_h);
        row_rad = sheet_px(6, space_h, screen_h);
    }
    ls = L->ls > 0 ? L->ls : 2;
    track = ls * 2;
    for (i = 0; i < OPT_COUNT; i++) {
        int w = measure_label(labels[i], ls, track);
        if (w > lab_w)
            lab_w = w;
    }
    {
        int dw = measure_label("Defaults", ls, track);
        if (dw > lab_w)
            lab_w = dw;
    }
    content = 0;
    for (i = 0; i < 6; i++) {
        int run = 0;
        int k;
        int widest = 0;
        for (k = 0; k < nchoice[i]; k++) {
            int tw = measure_label(choice[i][k], ls, track);
            if (tw > widest)
                widest = tw;
        }
        run = nchoice[i] * (widest + hpad * 2);
        if (run > content)
            content = run;
    }
    {
        int show_w = measure_label("Show", ls, track);
        int hide_w = measure_label("Hide", ls, track);
        int widest = show_w > hide_w ? show_w : hide_w;
        int run = 2 * (widest + hpad * 2);
        if (run > content)
            content = run;
    }
    {
        int tw = measure_label("0000 ms", ls, track);
        int val_w = tw + hpad * 2;
        int run = row_h + seg_gap + val_w + seg_gap + row_h;
        int foot = measure_label(footer, ls, track);
        int title = measure_face(HUD_FACE_VALUE, "Settings", ls);
        if (val_w < row_h)
            val_w = row_h;
        run = row_h + seg_gap + val_w + seg_gap + row_h;
        if (run > content)
            content = run;
        if (foot > lab_w + gap + content)
            content = foot - lab_w - gap;
        if (title > lab_w + gap + content)
            content = title - lab_w - gap;
        if (content < row_h * 2)
            content = row_h * 2;
        {
            int save_w = measure_label("Save as default", ls, track) + hpad * 2;
            if (save_w > content)
                content = save_w;
        }
    }
    panel_w = pad + lab_w + gap + content + pad;
    panel_h = pad + text_h + gap + OPT_COUNT * row_h + (OPT_COUNT - 1) * row_gap
        + split + row_h + row_gap + gap + text_h + pad;
    sys->set_hit_n = 0;
    sys->set_w = 0;
    place_sheet(hud_x, hud_y, hud_w, hud_h, panel_w, panel_h, space_w, space_h,
                gap, &ox, &oy);
    region = new_rgba(panel_w, panel_h, 0, ox, oy);
    if (!region) {
        restore_hud_font(L);
        return NULL;
    }
    c = canvas_of(region);
    rad = row_rad + inset;
    if (rad < 4)
        rad = 4;
    edge = inset > 0 ? inset : 1;
    inner_rad = rad - edge;
    if (inner_rad < 1)
        inner_rad = 1;
    fill_round_rect(&c, 0, 0, panel_w, panel_h, rad, 255, 255, 255, 48);
    fill_round_rect(&c, edge, edge, panel_w - edge * 2, panel_h - edge * 2,
                    inner_rad, 9, 11, 15, 250);
    paint_face(&c, pad, pad, HUD_FACE_VALUE, ls, "Settings", 244, 246, 248, 255);
    y = pad + text_h + gap;
    pressed = sys->left_down && sys->dragging == 4 ? sys->press_hit : 0;
    nseg = 0;
    for (i = 0; i < OPT_COUNT; i++) {
        int sx = pad + lab_w + gap;
        int ly = y + (row_h - text_h) / 2;
        if (i == OPT_SOURCE) {
            fill_rect(&c, pad, y + split / 2, panel_w - pad * 2, imax(1, inset),
                      255, 255, 255, 40);
            y += split;
            ly = y + (row_h - text_h) / 2;
        }
        paint_label(&c, pad, ly, ls, track, labels[i], 186, 194, 204, 255);
        {
            int row = sheet_choice_row(i);
            int toggle = i == OPT_SPEED || i == OPT_ALT ||
                         i == OPT_BEARING || i == OPT_ORIENT ||
                         i == OPT_SHOW_ARROW;
            if (row >= 0 || toggle) {
                static const char *const tog[2] = { "Show", "Hide" };
                int n = toggle ? 2 : nchoice[row];
                int seg_w = n > 0 ? content / n : content;
                int used = 0;
                int k;
                fill_round_rect(&c, sx, y, content, row_h, row_rad, 36, 40, 48, 255);
                for (k = 0; k < n && nseg < SET_HIT_CAP; k++) {
                    int sw = (k + 1 == n) ? content - used : seg_w;
                    const char *text = toggle ? tog[k] : choice[row][k];
                    segs[nseg].id = HIT_OPT + i * 8 + k;
                    segs[nseg].x = sx;
                    segs[nseg].y = y;
                    segs[nseg].w = sw;
                    segs[nseg].h = row_h;
                    segs[nseg].selected = k == sel[i];
                    segs[nseg].muted = 0;
                    segs[nseg].kind = 0;
                    snprintf(segs[nseg].text, sizeof segs[nseg].text, "%s", text);
                    nseg++;
                    sx += sw;
                    used += sw;
                }
            } else if (i == OPT_ARROW || i == OPT_SMOOTH) {
                const char *num = i == OPT_ARROW ? arrow_txt : smooth_txt;
                int lo = i == OPT_ARROW ? 24 : 0;
                int hi = i == OPT_ARROW ? 240 : 3000;
                int val = i == OPT_ARROW ? sys->map_arrow : sys->map_smooth_ms;
                int val_w = content - row_h - seg_gap - row_h - seg_gap;
                int k;
                if (val_w < row_h)
                    val_w = row_h;
                for (k = 0; k < 3 && nseg < SET_HIT_CAP; k++) {
                    int sw = k == 1 ? val_w : row_h;
                    const char *mark = k == 0 ? "-" : k == 2 ? "+" : num;
                    segs[nseg].id = k == 1 ? 0 : HIT_OPT + i * 8 + (k == 0 ? 0 : 1);
                    segs[nseg].x = sx;
                    segs[nseg].y = y;
                    segs[nseg].w = sw;
                    segs[nseg].h = row_h;
                    segs[nseg].selected = 0;
                    segs[nseg].muted = (k == 0 && val <= lo) || (k == 2 && val >= hi);
                    segs[nseg].kind = k == 1 ? 2 : 1;
                    snprintf(segs[nseg].text, sizeof segs[nseg].text, "%s", mark);
                    nseg++;
                    sx += sw + seg_gap;
                }
            }
        }
        y += row_h + row_gap;
    }
    for (i = 0; i < nseg; i++) {
        int tw, tx, ty;
        int fr, fg, fb, fa;
        int down = pressed && segs[i].id && pressed == segs[i].id;
        int span = font_on() ? hud_font_span(HUD_FACE_LABEL) : text_h;
        if (segs[i].kind == 0 && (segs[i].selected || down)) {
            int ix = segs[i].x + inset;
            int iy = segs[i].y + inset;
            int iw = segs[i].w - inset * 2;
            int ih = segs[i].h - inset * 2;
            if (iw < 1)
                iw = 1;
            if (ih < 1)
                ih = 1;
            if (segs[i].selected)
                fill_round_rect(&c, ix, iy, iw, ih, row_rad, 255, 176, 40, 255);
            else
                fill_round_rect(&c, ix, iy, iw, ih, row_rad, 58, 64, 74, 255);
        } else if (segs[i].kind == 1) {
            int br = segs[i].muted ? 36 : 48;
            int bg = segs[i].muted ? 40 : 54;
            int bb = segs[i].muted ? 48 : 64;
            fill_round_rect(&c, segs[i].x, segs[i].y, segs[i].w, segs[i].h, row_rad,
                            br, bg, bb, 255);
        } else if (segs[i].kind == 2) {
            fill_round_rect(&c, segs[i].x, segs[i].y, segs[i].w, segs[i].h, row_rad,
                            22, 26, 32, 255);
        }
        if (segs[i].selected) {
            fr = 28; fg = 22; fb = 8; fa = 255;
        } else if (segs[i].muted) {
            fr = 226; fg = 230; fb = 236; fa = 120;
        } else {
            fr = 244; fg = 246; fb = 248; fa = 255;
        }
        tw = measure_label(segs[i].text, ls, track);
        tx = segs[i].x + (segs[i].w - tw) / 2;
        ty = segs[i].y + (segs[i].h - span) / 2;
        if (tx < segs[i].x + 2)
            tx = segs[i].x + 2;
        if (ty < segs[i].y)
            ty = segs[i].y;
        paint_label(&c, tx, ty, ls, track, segs[i].text, fr, fg, fb, fa);
        if (segs[i].id)
            set_push(sys, segs[i].id, ox + segs[i].x, oy + segs[i].y, segs[i].w, segs[i].h);
    }
    {
        int sx = pad + lab_w + gap;
        int ly = y + (row_h - text_h) / 2;
        int tw, tx, ty;
        int span = font_on() ? hud_font_span(HUD_FACE_LABEL) : text_h;
        int down = pressed == HIT_SAVE;
        paint_label(&c, pad, ly, ls, track, "Defaults", 186, 194, 204, 255);
        fill_round_rect(&c, sx, y, content, row_h, row_rad,
                        down ? 58 : 48, down ? 64 : 54, down ? 74 : 64, 255);
        tw = measure_label("Save as default", ls, track);
        tx = sx + (content - tw) / 2;
        ty = y + (row_h - span) / 2;
        if (tx < sx + 2)
            tx = sx + 2;
        paint_label(&c, tx, ty, ls, track, "Save as default", 244, 246, 248, 255);
        set_push(sys, HIT_SAVE, ox + sx, oy + y, content, row_h);
    }
    paint_label(&c, pad, panel_h - pad - text_h, ls, track, footer, 168, 176, 188, 255);
    sys->set_x = ox;
    sys->set_y = oy;
    sys->set_w = panel_w;
    sys->set_h = panel_h;
    restore_hud_font(L);
    return region;
}

static void chain_append(subpicture_region_t *head, subpicture_region_t *extra)
{
    if (!head || !extra)
        return;
    while (head->p_next)
        head = head->p_next;
    head->p_next = extra;
}

static void discard_regions(subpicture_region_t *region)
{
    while (region) {
        subpicture_region_t *next = region->p_next;
        region->p_next = NULL;
        subpicture_region_Delete(region);
        region = next;
    }
}

/* The previous immersive subpicture is still on screen when every one of
 * these matches. The bucket is one mercator pixel of the fetched tile.
 * A clock change misses this and rebuilds the HUD. The map picture is
 * kept separately and reused when the view has not moved. */
static int view_same(const struct filter_sys_t *sys, int dw, int dh,
                     int qx, int qy, int course_bucket, unsigned long long tiles,
                     const hud_info_t *info, int hud_x, int hud_y)
{
    if (!sys->view_valid || sys->view_preset != PRESET_IMMERSIVE)
        return 0;
    if (sys->preset != PRESET_IMMERSIVE)
        return 0;
    if (sys->view_dw != dw || sys->view_dh != dh)
        return 0;
    if (sys->view_zoom != sys->map_zoom || sys->view_opacity != sys->map_opacity)
        return 0;
    if (sys->view_arrow != sys->map_arrow || sys->view_course != course_bucket)
        return 0;
    if (sys->view_qx != qx || sys->view_qy != qy)
        return 0;
    if (sys->view_hud_vis != (sys->visible ? 1 : 0))
        return 0;
    if (sys->view_map_vis != (sys->map_visible ? 1 : 0))
        return 0;
    if (sys->view_hud_x != hud_x || sys->view_hud_y != hud_y)
        return 0;
    if (sys->view_dragging != sys->dragging || sys->view_press != sys->press_hit)
        return 0;
    if (sys->view_dock != sys->dock_hot)
        return 0;
    if (sys->view_tiles != tiles)
        return 0;
    if (strcmp(sys->view_date, info->date) != 0)
        return 0;
    if (strcmp(sys->view_clock, info->clock) != 0)
        return 0;
    if (sys->view_settings != (sys->settings_open ? 1 : 0))
        return 0;
    if (sys->view_imperial != (sys->imperial ? 1 : 0))
        return 0;
    if (sys->view_date_long != (sys->date_long ? 1 : 0))
        return 0;
    if (sys->view_time_12 != (sys->time_12 ? 1 : 0))
        return 0;
    if (sys->view_zone != sys->zone_mode)
        return 0;
    if (sys->view_show_speed != (sys->show_speed ? 1 : 0))
        return 0;
    if (sys->view_show_alt != (sys->show_alt ? 1 : 0))
        return 0;
    if (sys->view_show_bearing != (sys->show_bearing ? 1 : 0))
        return 0;
    if (sys->view_show_orient != (sys->show_orient ? 1 : 0))
        return 0;
    if (sys->view_show_arrow != (sys->show_arrow ? 1 : 0))
        return 0;
    if (sys->view_source != sys->map_source)
        return 0;
    if (sys->view_smooth != sys->map_smooth_ms)
        return 0;
    if (sys->view_hud_size != sys->hud_size)
        return 0;
    /* The reading digits are sampled on the redraws the map and the clock
     * already require. Comparing them here rebuilt the whole frame several
     * times a second and stalled the video output. Whether each reading is
     * shown is compared above, so a sheet click updates the row. */
    return 1;
}

static int map_chrome(const struct filter_sys_t *sys)
{
    if (!sys->left_down || sys->dragging != 4)
        return 0;
    if (sys->press_hit == HUD_MAP_HIT_ZOOM_IN)
        return 1;
    if (sys->press_hit == HUD_MAP_HIT_ZOOM_OUT)
        return 2;
    if (sys->press_hit == HUD_MAP_HIT_OPACITY_UP)
        return 3;
    if (sys->press_hit == HUD_MAP_HIT_OPACITY_DOWN)
        return 4;
    return 0;
}

static void map_pic_drop(struct filter_sys_t *sys)
{
    if (sys->map_pic)
        picture_Release(sys->map_pic);
    sys->map_pic = NULL;
    sys->map_pic_ok = 0;
}

static int map_pic_matches(const struct filter_sys_t *sys, int dw, int dh,
                           int qx, int qy, int course, unsigned long long tiles)
{
    if (!sys->map_pic_ok || !sys->map_pic)
        return 0;
    if (sys->preset != PRESET_IMMERSIVE || !sys->map_visible)
        return 0;
    if (sys->map_pic_dw != dw || sys->map_pic_dh != dh)
        return 0;
    if (sys->map_pic_qx != qx || sys->map_pic_qy != qy)
        return 0;
    if (sys->map_pic_course != course)
        return 0;
    if (sys->map_pic_zoom != sys->map_zoom || sys->map_pic_opacity != sys->map_opacity)
        return 0;
    if (sys->map_pic_arrow != sys->map_arrow)
        return 0;
    /* Hover and press are draw_dock_face, not pixels of this picture. */
    if (sys->map_pic_show_arrow != (sys->show_arrow ? 1 : 0))
        return 0;
    if (sys->map_pic_tiles != tiles)
        return 0;
    return 1;
}

static void map_pic_store(struct filter_sys_t *sys, picture_t *pic, int dw, int dh,
                          int qx, int qy, int course, unsigned long long tiles)
{
    picture_t *held;
    if (!pic)
        return;
    held = picture_Hold(pic);
    if (sys->map_pic)
        picture_Release(sys->map_pic);
    sys->map_pic = held;
    sys->map_pic_ok = 1;
    sys->map_pic_dw = dw;
    sys->map_pic_dh = dh;
    sys->map_pic_qx = qx;
    sys->map_pic_qy = qy;
    sys->map_pic_course = course;
    sys->map_pic_zoom = sys->map_zoom;
    sys->map_pic_opacity = sys->map_opacity;
    sys->map_pic_arrow = sys->map_arrow;
    sys->map_pic_show_arrow = sys->show_arrow ? 1 : 0;
    sys->map_pic_chrome = map_chrome(sys);
    sys->map_pic_tiles = tiles;
}

static subpicture_region_t *region_from_picture(picture_t *pic, int x, int y)
{
    video_format_t fmt;
    subpicture_region_t *region;
    int w, h;
    if (!pic)
        return NULL;
    w = (int)pic->format.i_visible_width;
    h = (int)pic->format.i_visible_height;
    if (w < 1)
        w = (int)pic->format.i_width;
    if (h < 1)
        h = (int)pic->format.i_height;
    if (w < 1 || h < 1)
        return NULL;
    video_format_Init(&fmt, VLC_CODEC_RGBA);
    fmt.i_width = fmt.i_visible_width = w;
    fmt.i_height = fmt.i_visible_height = h;
    fmt.i_sar_num = fmt.i_sar_den = 1;
    region = subpicture_region_New(&fmt);
    video_format_Clean(&fmt);
    if (!region)
        return NULL;
    if (region->p_picture)
        picture_Release(region->p_picture);
    region->p_picture = picture_Hold(pic);
    region->i_x = x;
    region->i_y = y;
    region->i_alpha = 255;
    return region;
}

static void remember_chrome(filter_t *filter, struct filter_sys_t *sys,
                            int x, int y, const hud_layout_t *lay)
{
    sys->panel_x = x;
    sys->panel_y = y;
    sys->panel_w = lay->w;
    sys->panel_h = lay->h;
    sys->hide_x = x + lay->hide_x;
    sys->hide_y = y + lay->hide_y;
    sys->hide_w = lay->hide_w;
    sys->hide_h = lay->hide_h;
    if (sys->visible && lay->gear_w > 0) {
        int gx = x + lay->gear_x;
        int gy = y + lay->gear_y;
        if (gx != sys->gear_x || gy != sys->gear_y ||
            lay->gear_w != sys->gear_w || lay->gear_h != sys->gear_h) {
            msg_Info(filter, "gpmf settings gear %d,%d %dx%d",
                     gx, gy, lay->gear_w, lay->gear_h);
        }
        sys->gear_x = gx;
        sys->gear_y = gy;
        sys->gear_w = lay->gear_w;
        sys->gear_h = lay->gear_h;
    } else {
        sys->gear_w = 0;
        sys->gear_h = 0;
    }
}

static void view_save(struct filter_sys_t *sys, int dw, int dh,
                      int qx, int qy, int course_bucket, unsigned long long tiles,
                      const hud_info_t *info, int hud_x, int hud_y)
{
    sys->view_valid = 1;
    sys->view_preset = PRESET_IMMERSIVE;
    sys->view_dw = dw;
    sys->view_dh = dh;
    sys->view_zoom = sys->map_zoom;
    sys->view_opacity = sys->map_opacity;
    sys->view_arrow = sys->map_arrow;
    sys->view_course = course_bucket;
    sys->view_qx = qx;
    sys->view_qy = qy;
    sys->view_hud_vis = sys->visible ? 1 : 0;
    sys->view_map_vis = sys->map_visible ? 1 : 0;
    sys->view_hud_x = hud_x;
    sys->view_hud_y = hud_y;
    sys->view_dragging = sys->dragging;
    sys->view_press = sys->press_hit;
    sys->view_dock = sys->dock_hot;
    sys->view_settings = sys->settings_open ? 1 : 0;
    sys->view_imperial = sys->imperial ? 1 : 0;
    sys->view_date_long = sys->date_long ? 1 : 0;
    sys->view_time_12 = sys->time_12 ? 1 : 0;
    sys->view_zone = sys->zone_mode;
    sys->view_show_speed = sys->show_speed ? 1 : 0;
    sys->view_show_alt = sys->show_alt ? 1 : 0;
    sys->view_show_bearing = sys->show_bearing ? 1 : 0;
    sys->view_show_orient = sys->show_orient ? 1 : 0;
    sys->view_show_arrow = sys->show_arrow ? 1 : 0;
    sys->view_source = sys->map_source;
    sys->view_smooth = sys->map_smooth_ms;
    sys->view_hud_size = sys->hud_size;
    sys->view_tiles = tiles;
    copy_txt(sys->view_date, sizeof sys->view_date, info->date);
    copy_txt(sys->view_clock, sizeof sys->view_clock, info->clock);
    copy_txt(sys->view_speed, sizeof sys->view_speed, info->speed);
    copy_txt(sys->view_alt, sizeof sys->view_alt, info->alt);
    copy_txt(sys->view_bearing, sizeof sys->view_bearing, info->bearing);
    copy_txt(sys->view_orient, sizeof sys->view_orient, info->orient);
}

static subpicture_t *Filter(filter_t *filter, vlc_tick_t date)
{
    struct filter_sys_t *sys = filter->p_sys;
    subpicture_t *spu;
    subpicture_region_t *region;
    subpicture_region_t *head;
    hud_info_t info;
    hud_layout_t lay;
    double t;
    int moved;
    int x, y;
    int opt_show, opt_pos;
    if (sys->failed)
        return NULL;
    moved = poll_mouse(filter, sys);
    apply_preset(filter, sys);
    if (!moved && !sys->dragging && sys->last != 0 && date >= sys->last && date < sys->last + sys->refresh)
        return NULL;
    if (sys->last != 0 && date + CLOCK_FREQ < sys->last)
        sys->last = 0;
    if (!sys->gpmf) {
        char *path;
        ensure_input(filter, sys);
        path = path_of_input(sys->input);
        if (!path) {
            sys->last = date;
            return NULL;
        }
        if (load_file(filter, sys, path) != 0)
            sys->failed = 1;
        free(path);
        if (!sys->gpmf)
            return NULL;
    }
    t = media_seconds(filter, sys, date);
    set_load_hint(sys, t);
    start_loader(filter, sys);
    if (sys->loader_failed)
        gpmf_ensure(sys->gpmf, t, 3.0);
    if (!gpmf_ready(sys->gpmf, t))
        return NULL;
    if (sys->logged_sec < 0) {
        int Y = 0, M = 0, D = 0, hh = 0, mm = 0, ss = 0;
        float total = 0, lx = 0, ly = 0;
        int sample = gpmf_gforce(sys->gpmf, t, &total, &lx, &ly);
        int clock = gpmf_clock_parts(sys->gpmf, t, &Y, &M, &D, &hh, &mm, &ss);
        msg_Info(filter, "gpmf: %s, %d payloads, %.1f s, video %dx%d",
                 sys->gpmf->device[0] ? sys->gpmf->device : "unknown",
                 sys->gpmf->payloads, sys->gpmf->duration,
                 sys->gpmf->video_w, sys->gpmf->video_h);
        msg_Info(filter,
                 "gpmf hud %s%04d-%02d-%02d %02d:%02d:%02d source %s t=%.3f g=%s%.2f mouse %s",
                 clock == 0 ? "" : "none ",
                 clock == 0 ? Y : 0, clock == 0 ? M : 0, clock == 0 ? D : 0,
                 clock == 0 ? hh : 0, clock == 0 ? mm : 0, clock == 0 ? ss : 0,
                 clock_name(sys->gpmf->clock_source), t,
                 sample == 0 ? "" : "none ", sample == 0 ? total : 0.f,
                 sys->mouse ? "yes" : "no");
        sys->logged_sec = (int)t;
    }
    opt_show = var_GetBool(filter, CFG_PREFIX "hud");
    opt_pos = (int)var_GetInteger(filter, CFG_PREFIX "pos");
    if (opt_pos != sys->pos_seen) {
        sys->pos_seen = opt_pos;
        sys->pos = opt_pos;
        if (sys->preset == PRESET_DEFAULT)
            sys->user_moved = 0;
    }
    if ((opt_show ? 1 : 0) != (sys->visible ? 1 : 0))
        sys->visible = opt_show ? 1 : 0;
    if (!sys->visible)
        settings_close(sys);
    {
        int opt_map = var_GetBool(filter, CFG_PREFIX "map");
        if ((opt_map ? 1 : 0) != (sys->map_visible ? 1 : 0))
            sys->map_visible = opt_map ? 1 : 0;
    }
    {
        char *units = var_GetString(filter, CFG_PREFIX "units");
        char *dateopt = var_GetString(filter, CFG_PREFIX "date");
        char *timeopt = var_GetString(filter, CFG_PREFIX "time");
        char *zoneopt = var_GetString(filter, CFG_PREFIX "zone");
        sys->imperial = !units || strcmp(units, "metric") != 0;
        sys->date_long = !dateopt || strcmp(dateopt, "iso") != 0;
        sys->time_12 = !timeopt || strcmp(timeopt, "24h") != 0;
        sys->zone_mode = zone_mode_of(zoneopt);
        free(units);
        free(dateopt);
        free(timeopt);
        free(zoneopt);
        sys->show_speed = var_GetBool(filter, CFG_PREFIX "show-speed") ? 1 : 0;
        sys->show_alt = var_GetBool(filter, CFG_PREFIX "show-alt") ? 1 : 0;
        sys->show_bearing = var_GetBool(filter, CFG_PREFIX "show-bearing") ? 1 : 0;
        sys->show_orient = var_GetBool(filter, CFG_PREFIX "show-orient") ? 1 : 0;
        sys->show_arrow = var_GetBool(filter, CFG_PREFIX "show-arrow") ? 1 : 0;
    }
    {
        char *srcopt = var_GetString(filter, CFG_PREFIX "map-source");
        sys->map_source = map_source_of(srcopt);
        free(srcopt);
    }
    if (sys->tiles) {
        char *token = var_GetString(filter, CFG_PREFIX "map-token");
        int zoom = (int)var_GetInteger(filter, CFG_PREFIX "map-zoom");
        if (zoom < 2)
            zoom = 2;
        if (zoom > 18)
            zoom = 18;
        sys->map_zoom = zoom;
        hud_tiles_configure(sys->tiles, sys->map_source, token, zoom);
        {
            int arrow = (int)var_GetInteger(filter, CFG_PREFIX "map-arrow");
            int smooth = (int)var_GetInteger(filter, CFG_PREFIX "map-smooth");
            if (arrow < 24)
                arrow = 24;
            if (arrow > 240)
                arrow = 240;
            if (smooth < 0)
                smooth = 0;
            if (smooth > 3000)
                smooth = 3000;
            sys->map_arrow = arrow;
            sys->map_smooth_ms = smooth;
        }
        free(token);
    }
    if (sys->zone_mode == ZONE_RELATIVE)
        resolve_relative(filter, sys, t);
    update_bearing(sys->gpmf, t, &sys->bearing_deg, &sys->bearing_set);
    build_hud(sys->gpmf, t, sys->imperial, sys->date_long, sys->time_12,
              sys->zone_mode, sys->rel_tz, &info, sys->bearing_set, sys->bearing_deg);
    if (!sys->bearing_logged && (info.has_bearing || info.has_orient)) {
        msg_Info(filter, "gpmf bearing %s orient %s",
                 info.has_bearing ? info.bearing : "none",
                 info.has_orient ? info.orient : "none");
        sys->bearing_logged = 1;
    }
    if (!sys->show_speed)
        info.has_speed = 0;
    if (!sys->show_alt)
        info.has_alt = 0;
    if (!sys->show_bearing)
        info.has_bearing = 0;
    if (!sys->show_orient)
        info.has_orient = 0;
    {
        int immersive = sys->preset == PRESET_IMMERSIVE;
        int vw = sys->video_w > 0 ? sys->video_w : (sys->gpmf ? sys->gpmf->video_w : 0);
        int vh = sys->video_h > 0 ? sys->video_h : (sys->gpmf ? sys->gpmf->video_h : 0);
        int space_w = vw > 0 ? vw : 1920;
        int space_h = vh > 0 ? vh : 1080;
        int hit_floor = 36;
        int pad_floor = 12;
        int qx = 0, qy = 0, course_bucket = 0;
        unsigned long long tiles_gen = 0;
        hud_map_box box;
        int have_box = 0;
        int reuse_map = 0;
        int mx = 0, my = 0;
        memset(&box, 0, sizeof box);
        if (immersive)
            immersive_buffer(filter, sys, vw, vh, &space_w, &space_h);
        if (immersive) {
            int den_h = vh > 0 ? vh : space_h;
            hit_floor = mul_div(36, space_h, den_h);
            pad_floor = mul_div(12, space_h, den_h);
            if (hit_floor < 12)
                hit_floor = 12;
            if (pad_floor < 4)
                pad_floor = 4;
        }
        sys->hud_size = clamp_hud_size((int)var_GetInteger(filter, CFG_PREFIX "hud-size"));
        {
            int text_cap = hud_draw_cap(sys, immersive, vh);
            if (sys->hud_logged_size != sys->hud_size || sys->hud_logged_cap != text_cap) {
                msg_Info(filter, "gpmf hud size %d screen px, draw cap %d",
                         sys->hud_size, text_cap);
                sys->hud_logged_size = sys->hud_size;
                sys->hud_logged_cap = text_cap;
            }
            layout_hud(text_cap, sys->visible, &info, &lay, immersive,
                       hit_floor, pad_floor);
        }
        x = sys->hud_x;
        y = sys->hud_y;
        {
            int slot = immersive ? PRESET_IMMERSIVE : PRESET_DEFAULT;
            int bw = immersive ? space_w : (vw > 0 ? vw : 0);
            int bh = immersive ? space_h : (vh > 0 ? vh : 0);
            /* Once the frame is known. A drag this session wins over the save. */
            if (!sys->saved_hud_applied[slot] && bw >= 32 && bh >= 32) {
                if (!sys->user_moved && !sys->hud_dragged[slot] &&
                    sys->saved_hud_custom[slot]) {
                    x = frac_px(sys->saved_hud_x[slot], bw);
                    y = frac_px(sys->saved_hud_y[slot], bh);
                    sys->hud_x = x;
                    sys->hud_y = y;
                    sys->user_moved = 1;
                    sys->hud_slot_x[slot] = x;
                    sys->hud_slot_y[slot] = y;
                    sys->hud_slot_moved[slot] = 1;
                }
                sys->saved_hud_applied[slot] = 1;
            }
        }
        if (!sys->user_moved) {
            if (immersive) {
                int margin = mul_div(36, space_h, vh > 0 ? vh : space_h);
                if (margin < 2)
                    margin = 2;
                x = (space_w - lay.w) / 2;
                if (x < 0)
                    x = 0;
                y = margin;
            } else {
                corner_xy(sys->pos, vw > 0 ? vw : 1920, vh > 0 ? vh : 1080,
                          lay.w, lay.h, &x, &y);
            }
            sys->hud_x = x;
            sys->hud_y = y;
        }
        note_hud_frac(sys, immersive ? PRESET_IMMERSIVE : PRESET_DEFAULT,
                      immersive ? space_w : (vw > 0 ? vw : 0),
                      immersive ? space_h : (vh > 0 ? vh : 0));
        update_map_course(sys, t);
        if (sys->map_has) {
            if (!immersive && !sys->saved_map_applied && vw >= 32 && vh >= 32) {
                sys->saved_map_applied = 1;
                if (!sys->map_dragged && sys->saved_map_custom) {
                    int side_w = frac_px(sys->saved_map_w, vw);
                    int side_h = frac_px(sys->saved_map_h, vh);
                    if (side_w < 240)
                        side_w = 240;
                    if (side_h < 240)
                        side_h = 240;
                    sys->map_w = side_w;
                    sys->map_h = side_h;
                    sys->map_x = frac_px(sys->saved_map_x, vw);
                    sys->map_y = frac_px(sys->saved_map_y, vh);
                    sys->map_user_moved = 1;
                    sys->map_user_sized = 1;
                }
            }
            if (immersive)
                hud_map_immersive_box(vw, vh, sys->map_visible, &box);
            else
                hud_map_box_of(vw, vh, sys->map_visible,
                               sys->map_user_sized ? sys->map_w : 0,
                               sys->map_user_sized ? sys->map_h : 0, &box);
            mx = sys->map_x;
            my = sys->map_y;
            if (immersive) {
                if (sys->map_visible) {
                    mx = 0;
                    my = 0;
                } else {
                    int lift = box.lift > 0 ? box.lift : 140;
                    mx = vw - box.w - 36;
                    my = vh - box.h - lift;
                    if (mx < 36)
                        mx = 36;
                    if (my < 36)
                        my = 36;
                }
                scale_box(&box, space_w, space_h,
                          vw > 0 ? vw : space_w, vh > 0 ? vh : space_h);
                mx = mul_div(mx, space_w, vw > 0 ? vw : space_w);
                my = mul_div(my, space_h, vh > 0 ? vh : space_h);
            } else if (!sys->map_user_moved) {
                int lift = vh / 8;
                if (lift < 140)
                    lift = 140;
                corner_xy(10, vw, vh, box.w, box.h, &mx, &my);
                /* The player controls cover the bottom edge of the picture. */
                my -= lift;
                if (my < 36)
                    my = 36;
            }
            if (!immersive) {
                sys->map_x = mx;
                sys->map_y = my;
            }
            if (!immersive && sys->map_user_sized) {
                sys->map_w = box.w;
                sys->map_h = box.h;
            }
            if (!immersive && sys->map_dragged && vw >= 32 && vh >= 32) {
                sys->map_fx = frac_of(sys->map_x, vw);
                sys->map_fy = frac_of(sys->map_y, vh);
                sys->map_fw = frac_of(box.w, vw);
                sys->map_fh = frac_of(box.h, vh);
                sys->map_frac_ok = 1;
            }
            if (sys->tiles && sys->map_visible) {
                int span = immersive ? MAP_TILE_SCREEN
                                     : map_tile_span(filter, sys, vw, vh);
                if (immersive && span != sys->map_logged_span) {
                    msg_Info(filter, "gpmf map draws one tile across %d screen px", span);
                    sys->map_logged_span = span;
                }
                hud_tiles_want(sys->tiles, sys->map_lat, sys->map_lon, box.w, box.h, span);
            }
            have_box = 1;
        }
        if (immersive && sys->map_visible && sys->map_fix_valid) {
            double tx, ty;
            hud_map_mercator(sys->map_lat, sys->map_lon, sys->map_zoom, &tx, &ty);
            qx = (int)(tx * 256.0);
            qy = (int)(ty * 256.0);
            course_bucket = (int)(sys->map_course / 3.0);
        }
        if (immersive && sys->map_visible && sys->tiles)
            tiles_gen = hud_tiles_stored(sys->tiles);
        if (immersive && !sys->dragging &&
            view_same(sys, space_w, space_h, qx, qy, course_bucket, tiles_gen,
                      &info, x, y)) {
            sys->last = date;
            return NULL;
        }
        reuse_map = map_pic_matches(sys, space_w, space_h, qx, qy,
                                    course_bucket, tiles_gen);

        region = draw_panel(x, y, sys->visible, &info, &lay,
                            sys->settings_open && sys->visible);
        if (!region)
            return NULL;
        remember_chrome(filter, sys, x, y, &lay);
        {
            int map_ok = reuse_map || !have_box;
            int map_is_head = 0;
            /* A reused map picture used to clear this rect. The dock then
             * ignored the pointer until the car moved to the next map pixel. */
            if (!have_box) {
                sys->map_pw = 0;
            } else {
                sys->map_px = mx;
                sys->map_py = my;
                sys->map_pw = box.w;
                sys->map_ph = box.h;
                sys->map_hx = mx + box.hide_x;
                sys->map_hy = my + box.hide_y;
                sys->map_hw = box.hide_w;
                sys->map_hh = box.hide_h;
                sys->map_geom = box;
                if (immersive && box.full && sys->map_visible &&
                    (box.zin_x != sys->dock_log_x || box.zin_y != sys->dock_log_y ||
                     box.zin_w != sys->dock_log_w)) {
                    msg_Info(filter, "gpmf dock %d,%d %d",
                             box.zin_x, box.zin_y, box.zin_w);
                    sys->dock_log_x = box.zin_x;
                    sys->dock_log_y = box.zin_y;
                    sys->dock_log_w = box.zin_w;
                }
            }
            if (have_box && !reuse_map) {
                subpicture_region_t *map = draw_map(sys, mx, my, &box, sys->map_visible,
                                                   sys->map_course_set ? sys->map_course : 0);
                if (map) {
                    map_ok = 1;
                    if (immersive) {
                        map->p_next = region;
                        region = map;
                        map_is_head = 1;
                    } else {
                        region->p_next = map;
                    }
                    if (!sys->map_logged && sys->map_course_set) {
                        msg_Info(filter, "gpmf map course %.0f deg", sys->map_course);
                        sys->map_logged = 1;
                    }
                } else {
                    sys->map_pw = 0;
                }
            }
            if (immersive && map_ok && sys->map_visible && box.full && sys->dock_hot) {
                subpicture_region_t *face = draw_dock_face(sys, &box, mx, my);
                if (face) {
                    if (map_is_head) {
                        face->p_next = region->p_next;
                        region->p_next = face;
                    } else {
                        face->p_next = region;
                        region = face;
                    }
                }
            }

            if (have_box && sys->map_visible && sys->tiles) {
                int place_w = immersive ? space_w : (vw > 0 ? vw : 1920);
                int place_h = immersive ? space_h : (vh > 0 ? vh : 1080);
                subpicture_region_t *tag = draw_credit(sys, place_w, place_h, immersive,
                                                       vh > 0 ? vh : place_h);
                if (tag)
                    chain_append(region, tag);
            }
            if (sys->settings_open && sys->visible && sys->gear_w > 0) {
                int place_w = immersive ? space_w : (vw > 0 ? vw : 1920);
                int place_h = immersive ? space_h : (vh > 0 ? vh : 1080);
                subpicture_region_t *sheet = draw_settings(sys, &lay, x, y, lay.w, lay.h,
                                                          place_w, place_h);
                if (sheet) {
                    static int lx = -1, ly = -1, lw = -1, lh = -1;
                    chain_append(region, sheet);
                    if (sys->set_x != lx || sys->set_y != ly ||
                        sys->set_w != lw || sys->set_h != lh) {
                        int si;
                        msg_Info(filter, "gpmf settings sheet %d,%d %dx%d",
                                 sys->set_x, sys->set_y, sys->set_w, sys->set_h);
                        for (si = 0; si < sys->set_hit_n; si++) {
                            if (sys->set_id[si] == HIT_OPT + OPT_UNITS * 8 + 1)
                                msg_Info(filter, "gpmf settings metric %d,%d %dx%d",
                                         sys->set_hx[si], sys->set_hy[si],
                                         sys->set_hw[si], sys->set_hh[si]);
                            if (sys->set_id[si] == HIT_OPT + OPT_SPEED * 8 + 1)
                                msg_Info(filter, "gpmf settings speed-hide %d,%d %dx%d",
                                         sys->set_hx[si], sys->set_hy[si],
                                         sys->set_hw[si], sys->set_hh[si]);
                            if (sys->set_id[si] == HIT_OPT + OPT_SHOW_ARROW * 8 + 1)
                                msg_Info(filter, "gpmf settings arrow-hide %d,%d %dx%d",
                                         sys->set_hx[si], sys->set_hy[si],
                                         sys->set_hw[si], sys->set_hh[si]);
                            if (sys->set_id[si] == HIT_SAVE)
                                msg_Info(filter, "gpmf settings save %d,%d %dx%d",
                                         sys->set_hx[si], sys->set_hy[si],
                                         sys->set_hw[si], sys->set_hh[si]);
                        }
                        lx = sys->set_x;
                        ly = sys->set_y;
                        lw = sys->set_w;
                        lh = sys->set_h;
                    }
                }
            } else if (!sys->settings_open) {
                sys->set_w = 0;
                sys->set_hit_n = 0;
            }
            head = region;
            spu = filter_NewSubpicture(filter);
            if (!spu) {
                discard_regions(head);
                return NULL;
            }
            spu->i_start = date;
            spu->i_stop = 0;
            spu->b_ephemer = true;
            spu->b_absolute = true;
            if (immersive && space_w > 0 && space_h > 0) {
                int src_w = vw > 0 ? vw : 0;
                int src_h = vh > 0 ? vh : 0;
                int fitted = 0;
                int sized = 0;
                /* The display-sized paint is what the window shows. VLC still
                 * software-scales each new subpicture up to the source when
                 * the window is smaller, and that scale stalls playback.
                 * Point-sample into video pixels and report the video size so
                 * the scale factor is 1. Hits stay in display pixels.
                 * A clock tick expands only the HUD and reuses the map. */
                if (src_w > 0 && src_h > 0 &&
                    (src_w != space_w || src_h != space_h))
                    fitted = expand_immersive(head, space_w, space_h, src_w, src_h);
                sized = fitted || (src_w == space_w && src_h == space_h);
                if (reuse_map && sized) {
                    subpicture_region_t *kept = region_from_picture(sys->map_pic, 0, 0);
                    if (kept) {
                        kept->p_next = head;
                        head = kept;
                        if (!sys->map_reuse_logged) {
                            msg_Info(filter, "gpmf map picture reused");
                            sys->map_reuse_logged = 1;
                        }
                    }
                } else if (sized && head && head->p_picture &&
                           head->i_x == 0 && head->i_y == 0 &&
                           (int)head->fmt.i_visible_width == src_w &&
                           (int)head->fmt.i_visible_height == src_h &&
                           sys->map_visible) {
                    map_pic_store(sys, head->p_picture, space_w, space_h,
                                  qx, qy, course_bucket, tiles_gen);
                } else if (!reuse_map) {
                    map_pic_drop(sys);
                }
                if (sized) {
                    spu->i_original_picture_width = src_w;
                    spu->i_original_picture_height = src_h;
                } else {
                    static int warned;
                    if (!warned) {
                        msg_Warn(filter, "gpmf immersive expand failed");
                        warned = 1;
                    }
                    spu->i_original_picture_width = space_w;
                    spu->i_original_picture_height = space_h;
                }
                sys->overlay_display = 1;
                sys->space_w = space_w;
                sys->space_h = space_h;
                if (map_ok)
                    view_save(sys, space_w, space_h, qx, qy, course_bucket, tiles_gen,
                              &info, x, y);
            } else if (sys->gpmf->video_w > 0 && sys->gpmf->video_h > 0) {
                spu->i_original_picture_width = sys->gpmf->video_w;
                spu->i_original_picture_height = sys->gpmf->video_h;
                sys->overlay_display = 0;
                sys->view_valid = 0;
                map_pic_drop(sys);
            }
            spu->p_region = head;
            sys->last = date;
            return spu;
        }
    }
}
