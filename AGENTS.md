# AGENTS.md

PlatformIO (Arduino, ESP32-C3) firmware. No tests or linters — CI only builds.

## Commands

```bash
pio run -e supermini              # build (envs: supermini, xiao)
pio run -t upload -e supermini    # flash over USB
pio device monitor                # serial, 115200 baud
pio run -t merge -e supermini     # merged web-flash bin (.pio/build/supermini/firmware-merged.bin)
```

- Prefer `.venv/bin/pio` (repo-local PlatformIO venv) if `pio` is not on PATH.
- `scripts/merge-firmware.sh [--no-build] [--env NAME]` → `release/plane-radar-merged.bin`.
- Verify a change by building: `pio run -e supermini` (CI builds this env only).

## Regenerated files — do not hand-edit

`python3 scripts/build_large_airports.py` regenerates `include/data/large_airports.h` and `src/data/large_airports_data.cpp` from OurAirports CSVs (network fetch). Edit the script, not its outputs.

## Architecture

- Header-heavy layout: declarations in `include/{hardware,ui,services,data}/`, implementations in the mirrored `src/` dirs. Entry point is `src/main.cpp`.
- `include/config.h` holds nearly all tunables (portal, Wi-Fi timing, BOOT button, pins, ADS-B/weather intervals).
- Board pin variants are build-flag overrides (`-DCONFIG_DISPLAY_PIN_*`), not code changes — see the `env:xiao` section of `platformio.ini`.
- `data/ui_font.vlw` is embedded into the binary via `board_build.embed_files`.
- Custom partition table `partitions/plane_radar.csv`: two 1.75 MB OTA app slots. Consequence: the merged/full image (bootloader + partitions + app) must never be uploaded via the device's OTA page; only `firmware.bin` / `-ota.bin`.

## Release

Push a `v*` tag (e.g. `v1.0.0`); `.github/workflows/release.yml` builds `supermini` and attaches `-full.bin` (flash at 0x0) and `-ota.bin` assets. Tagging is the release process — don't commit binaries.
