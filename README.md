# vlc-gpmf

## About

While this repository was largely written with Grok 4.7, this section of the README was written by a human. In the age of "AI slop", I felt the need to type out some sort of message to anyone who comes across this git repo.

I "vibe coded" this VLC plugin to visualize the GPMD/GMPF sensor/telemetry data recorded by most GoPro cameras (I still haven't forgiven GoPro for the HERO12). I say "vibe coded" because I would have never been able to write this otherwise. I barely understand C well enough to read it (much less write it), but I digress. At best, this plugin is in a beta release status on Windows. While I've prompted support for building the plugin in Linux/Mac, that functionality is better described as being in an alpha status (and currently untested).

I say "beta" because I'm still dialing in how the data is animated/visualized and what that looks like in different "presets". Right now, I have "default" and "immersive", the latter of which effectively stretches a transparent map over the video.

I'm not going to pretend I understand any of the code in this repo. All I can say is I tested this on my own machine and really dialed in the performance before putting this on the internet.

## Start

vlc-gpmf is a VLC 3.0 plugin. During playback it reads the GoPro `gpmd` track and draws a HUD.

The HUD shows the date, then the time, then speed, altitude, bearing, and a compass point when those streams exist. Each of those four can be hidden from the settings sheet, and so can the course arrow. Hiding a reading leaves the arrow. Drag the panel to move it for the session. The corner mark hides it; the tab opens it again. The gear opens a settings sheet. A choice there changes this playback. Save as default writes the starting look for the next launch.

**Default** is a map inset with the stacked HUD. Drag inside the map to move it, a corner or edge to resize it, and the stepper to zoom. **Immersive** draws the map across the video, with a compact HUD at the top center. A dock on the right zooms, sets how strongly the tiles show, and hides the map. The course arrow, the dock, and the OpenStreetMap credit stay solid.

Letters are Segoe UI on Windows. Linux and macOS use the built-in glyphs.

GPL-2.0-or-later. The license is [`LICENSE`](LICENSE). The grant is also in [`COPYING`](COPYING). Source files carry `SPDX-License-Identifier: GPL-2.0-or-later`. `src/gpmf_read.c` is an original reader. This tree does not include GoPro's gpmf-parser.

The module score is 0, so it stays off until `--sub-source=gpmf` or the GPMF sub-source preference turns it on. A file with no `gpmd` track plays without the overlay.

## Time

The clock uses GPSU when that stream exists, then GPS9, then the MP4 creation time. GPS times are UTC. A file timestamp is shown as the digits stored in the file.

`gpmf-zone` applies to GPS time:

- `relative` (the default) uses the timezone at the GNSS position. Daylight time follows that zone's rule on the recording's date and at its clock. The label ends with `(relative)`.
- `local` uses this computer's timezone and ends with `(local)`.
- `utc` shows UTC and ends with `(utc)`.

The offset in the label is the one in effect for that instant, such as `UTC-4` or `UTC+5:30`.

## Map

OpenStreetMap is the default and needs no token. Tiles come from `tile.openstreetmap.org`. The overlay keeps the `(c) OpenStreetMap` credit at the bottom-left of the video, in a smaller face than the HUD. Mapbox Streets and Google roadmap need a token in `gpmf-map-token`.

## Preferences

Options use the `gpmf-` prefix, on the command line (`--gpmf-zone=utc`) and under the module in VLC's preferences. The page is grouped as HUD, Map, Source, and Saved layout. These values are the starting look for the next launch and for Convert/Save. The gear does not write them. Save as default does.

| Option | Default | Meaning |
| --- | --- | --- |
| `gpmf-preset` | `default` | `default` or `immersive`. |
| `gpmf-zone` | `relative` | `relative`, `local`, or `utc`. |
| `gpmf-units` | `imperial` | `imperial` (mph and feet) or `metric` (km/h and metres). |
| `gpmf-date` | `long` | `long` (`September 1, 2026`) or `iso` (`2026-09-01`). |
| `gpmf-time` | `12h` | `12h` (`12:30:00 PM`) or `24h` (`12:30:00`). |
| `gpmf-show-speed` | on | Show the speed reading. |
| `gpmf-show-alt` | on | Show the altitude reading. |
| `gpmf-show-bearing` | on | Show the bearing reading. The map arrow stays. |
| `gpmf-show-orient` | on | Show the compass point. The map arrow stays. |
| `gpmf-hud` | on | Show the date, the time, and the readings. |
| `gpmf-hud-size` | 18 | Clock and reading height in screen pixels, 12–48. Date and labels are a step smaller. Map zoom and map size do not change it. The sheet has no size row. |
| `gpmf-pos` | 5 | Starting corner for the default preset. See below. |
| `gpmf-map` | on | Show the inset and the full-frame map. |
| `gpmf-map-source` | `osm` | `osm`, `mapbox`, or `google`. |
| `gpmf-show-arrow` | on | Show the course arrow on the map. |
| `gpmf-map-arrow` | 64 | Arrow length in picture pixels, 24–240. Resizing the map does not change it. |
| `gpmf-map-smooth` | 500 | Milliseconds for the marker to catch the GNSS fix, 0–3000. `0` follows every fix. Speed and altitude stay exact. |
| `gpmf-map-zoom` | 15 | Web Mercator zoom, 2–18. |
| `gpmf-map-opacity` | 70 | Immersive tile strength, 10–100. The inset ignores this. |
| `gpmf-file` | empty | Video to read telemetry from. Empty uses the file being played. |
| `gpmf-map-token` | empty | Mapbox access token or Google Maps API key. |
| `gpmf-hud-custom-default` | off | On after Save as default when the default HUD was dragged. Off returns to the corner. |
| `gpmf-hud-custom-immersive` | off | On after Save as default when the immersive HUD was dragged. Off returns to the top center. |
| `gpmf-hud-x-default` | 0 | Default HUD horizontal place, as a fraction of the frame from 0 to 10000. |
| `gpmf-hud-y-default` | 0 | Default HUD vertical place, as a fraction of the frame from 0 to 10000. |
| `gpmf-hud-x-immersive` | 0 | Immersive HUD horizontal place, as a fraction of the frame from 0 to 10000. |
| `gpmf-hud-y-immersive` | 0 | Immersive HUD vertical place, as a fraction of the frame from 0 to 10000. |
| `gpmf-map-custom` | off | On after Save as default when the inset was moved or resized. Off returns to the built-in inset. |
| `gpmf-map-x` | 0 | Inset left edge, as a fraction of the frame from 0 to 10000. |
| `gpmf-map-y` | 0 | Inset top edge, as a fraction of the frame from 0 to 10000. |
| `gpmf-map-w` | 0 | Inset width, as a fraction of the frame from 0 to 10000. |
| `gpmf-map-h` | 0 | Inset height, as a fraction of the frame from 0 to 10000. The immersive map stays full frame. |

`gpmf-pos` uses VLC's corner numbers: 0 center, 1 left, 2 right, 4 top, 8 bottom, 5 top left, 6 top right, 9 bottom left, 10 bottom right. It is the starting corner for the default preset. Immersive starts at the top center. A drag lasts for this playback until Save as default.

The settings sheet lists preset, zone, units, date, time, then show or hide for speed, altitude, bearing, and orientation, then map source, arrow show or hide, arrow length, and smoothing. A Defaults row at the bottom saves that arrangement and does not close the sheet. Hiding a reading leaves the arrow. Hiding the arrow leaves the dock, the credit, and the map hide mark. Zoom, opacity, and hide stay on the picture. The map token and HUD size stay in Preferences. Save as default writes the live values, and writes a dragged position as a fraction of the frame. A drag that was not saved lasts for this playback.

## Build

VLC 3.0. From the repository:

```
make
```

Linux and macOS need `pkg-config` to find `vlc-plugin`. That writes `build/libgpmf_plugin.so` or `build/libgpmf_plugin.dylib`.

Windows uses the VLC 3.0.23 headers in `third_party/vlc` and links the installed `libvlccore.dll`. `mingw32-make` looks for `cc` unless `CC` is set:

```
mingw32-make CC=gcc
```

That writes `build/libgpmf_plugin.dll`. The link uses `-static-libgcc`, so the DLL does not depend on `libgcc_s` or `winpthread`. VLC rejects a plugin that needs those.

`make` builds the plugin only. `src/dump.c` prints a track. On Windows:

```
gcc -std=c11 -O2 -Wall -Wextra -o build/gpmf-dump.exe src/dump.c src/gpmf_read.c -lm
```

On Linux and macOS, add `-pthread`.

## Testing

`make check` runs the reader, clock, and map checks. On Windows use `mingw32-make CC=gcc check`. A passing run prints `gpmf-check ok`. The cases, the Windows DLL import check, and what still needs a VLC window are in [TESTING.md](TESTING.md).

## Load the plugin

Point `VLC_PLUGIN_PATH` at the directory that contains the built plugin, regenerate the cache, and start VLC from that same shell. A Start-menu launch will not see a variable set somewhere else. Run the cache generator again after each rebuild.

Windows:

```
$env:VLC_PLUGIN_PATH = "C:\path\to\vlc-gpmf\build"
& "C:\Program Files\VideoLAN\VLC\vlc-cache-gen.exe" $env:VLC_PLUGIN_PATH
& "C:\Program Files\VideoLAN\VLC\vlc.exe" --sub-source=gpmf "C:\path\to\clip.MP4"
```

Linux and macOS:

```
export VLC_PLUGIN_PATH="$PWD/build"
vlc-cache-gen "$VLC_PLUGIN_PATH"
vlc --sub-source=gpmf /path/to/clip.MP4
```

## Timezone data

Relative zone needs `timezone21.bin` and a `zoneinfo/` tree beside it. The plugin looks beside itself, then in `../third_party/zonedetect/`, then in `../share/`. A development build in `build/` finds `third_party/zonedetect/` that way. On Linux and macOS, a missing zone file can fall back to `/usr/share/zoneinfo` or `/var/db/timezone/zoneinfo`. Windows uses the copy in this tree.

## Third-party notices

- **ZoneDetect** (`third_party/zonedetect/`), Copyright (c) 2018 Bertold Van den Bergh, BSD-3-Clause. The license is at the top of `zonedetect.c` and `zonedetect.h`. See `third_party/zonedetect/NOTICE`.
- **timezone21.bin** is compiled for ZoneDetect from timezone-boundary-builder. That project is derived from OpenStreetMap and is offered under the Open Database License: <https://opendatacommons.org/licenses/odbl/>. It contains Natural Earth data, placed in the public domain.
- **zoneinfo/** is the IANA time zone database, public domain, packaged from tzdata 2026.5.
- **stb_image** v2.30 (`src/stb_image.h`) decodes map tiles. Sean Barrett released it into the public domain, with an MIT alternative in the same file.
- **VLC 3.0.23 plugin SDK** under `third_party/vlc` is VideoLAN's headers, used to build on Windows, and remains under the GNU GPL. Linux and macOS builds use the headers from the installed VLC development package.
