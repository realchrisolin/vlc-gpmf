# Testing

`make check` builds `build/gpmf-check` and runs it. On Windows the binary is `build/gpmf-check.exe`:

```
mingw32-make CC=gcc check
```

A passing run prints `gpmf-check ok` and exits 0. A failure prints `FAIL ...` on stderr and exits 1.

The check links `tests/check.c` with `src/gpmf_read.c`, `src/hud_map.c`, `src/hud_zone.c`, and ZoneDetect. It does not link `src/plugin.c` or libvlccore, and it does not need `pkg-config`. On Windows it also builds `build/libgpmf_plugin.dll` so the import check can read it. That DLL build needs the VLC 3.0.23 headers and the installed `libvlccore.dll`, same as `make`.

Leave the binary in `build/`. From there, zone lookup finds `../third_party/zonedetect/`, which holds `timezone21.bin` and `zoneinfo/`.

The run writes three MP4 files beside the binary and deletes them when it finishes:

- `build/check-seam.mp4`
- `build/check-gps9.mp4`
- `build/check-file.mp4`

`make clean` removes the check binary and any of those files left behind after a failed run.

## Windowed read

`check-seam.mp4` has four `gpmd` payloads. Each is one second of presentation time, holds two GPS9 fixes, and carries a GPSU stamp. The payloads sit 2 MB apart, so reading them seeks.

The check loads the payloads from the end of the track, one at a time. The four GPS9 runs stay split. `gpmf_open` reads them in order and merges them into one run. Both readers must return the same nine GPS9 channels at every quarter-second from 0.25 s through 3.75 s, including the boundary between payloads. At 1.0 s the latitude is 43.375.

After the last payload is read, and before the first payload is read, `gpmf_ready` at the start of the file is still 0, and the clock at presentation 0 is already `2026-03-08 06:59:00` from GPSU.

## Clock

GPSU wins when it is present. The GPS9 days and seconds in the seam file are zero, which would be year 2000 if they became the clock.

`check-gps9.mp4` has one GPS9 sample and no GPSU. At 0.5 s the source is GPS9 and the parts are `2026-03-08 06:59:00`.

`check-file.mp4` has a `DEVC` and no GPS. Its MP4 creation time is `2026-01-09 03:18:47`. The parts at presentation 0 are those digits. One second later they are `03:18:48`. The same instant in `America/New_York` is a different wall clock. The FILE parts stay the stored digits.

## Zone

The boundary database and the vendored tzdata supply the relative zone.

| Instant (UTC) | America/New_York |
| --- | --- |
| 2026-03-08 06:59:00 | 01:59, UTC-5 |
| 2026-03-08 07:00:00 | 03:00, UTC-4 |
| 2026-11-01 05:59:00 | 01:59, UTC-4 |
| 2026-11-01 06:00:00 | 01:00, UTC-5 |

A fix at 43.09355, -76.08021 resolves to `America/New_York`. The labels for the two offsets are `UTC-5` and `UTC-4`.

## Map math

- A smoothing time of 0 copies the raw fix.
- A heading of 350° catching up to 10° moves across 0°, and the result stays under 30°.
- Bearing due south is 180°, north 0°, east 90°, west 270°, and the result stays in `[0, 360)`.
- Arrow length clamps to 24..240 picture pixels.
- Opacity steps by 10 and clamps to 10..100.
- On a full-frame immersive map, the center is not a drag and a corner is not a grip. The zoom-in button still hits.

## Windows DLL

On Windows the check reads the import table of `build/libgpmf_plugin.dll`. The DLL must import `libvlccore`. An import of `libgcc_s` or `winpthread` fails the run. VLC rejects a plugin that needs those libraries, and a rejected plugin leaves a `plugins.dat` of a few dozen bytes.

## Playback

Drawing, the gear sheet, the mouse, tile fetch, and playback need a VLC window. Load the plugin as described in the README, with `--sub-source=gpmf`.
