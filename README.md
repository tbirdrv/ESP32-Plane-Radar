# Plane Radar

[![Build](https://github.com/tbirdrv/ESP32-Plane-Radar/actions/workflows/build.yml/badge.svg)](https://github.com/tbirdrv/ESP32-Plane-Radar/actions/workflows/build.yml)
[![Prerelease](https://img.shields.io/github/v/release/tbirdrv/ESP32-Plane-Radar?include_prereleases&label=prerelease)](https://github.com/tbirdrv/ESP32-Plane-Radar/releases/tag/nightly-main)

<img width="800" height="450" alt="plane-radar" src="https://github.com/user-attachments/assets/716d0992-dab8-47ba-8f1a-2aec7f607419" />

**3D printed case (STL + assembly):** [MakerWorld](https://makerworld.com/en/models/2872376-esp32-plane-radar-live-ads-b-on-a-round-display#profileId-3207083) · **Firmware:** [Releases](https://github.com/tbirdrv/ESP32-Plane-Radar/releases)

**Repository:** [github.com/tbirdrv/ESP32-Plane-Radar](https://github.com/tbirdrv/ESP32-Plane-Radar)

Firmware for an **ESP32-C3 Super Mini** and a **1.28″ round GC9A01** display (240×240). Shows a circular **ADS-B radar** around your configured location, with **WiFiManager** for first-time setup.

## What it does

1. **Wi‑Fi setup** (if needed) — captive portal on AP **`PlaneRadar-Setup`**
2. **Radar** — live aircraft from [adsb.fi](https://opendata.adsb.fi/) on a sonar-style grid

After Wi‑Fi is saved, the device reconnects automatically; the radar runs in the main loop with periodic ADS-B updates (~5 s).

## Controls (BOOT, GPIO 9, active LOW)

| Action | Effect |
|--------|--------|
| **Short tap** | Cycle range preset (1 → 5 → 10 → 15 → 20 → 25 mi); saved to flash |
| **Hold 3 s** | Clear Wi‑Fi, location, and units; reboot into setup portal |

During setup you can also hold BOOT at power-on to force a credential reset (same as the long press).

## Wi‑Fi setup portal

**First-time setup** (no saved Wi‑Fi):

1. Connect to **`PlaneRadar-Setup`**
2. Open **`http://plane-radar.local`** (preferred) or **`http://192.168.4.1`** — both are shown on the yellow setup screen; captive portal may open automatically
3. Set home Wi‑Fi, then save

**Reconfigure anytime** (after the device is on your network):

1. Open **`http://plane-radar.local`** or **`http://<device-ip>`** (e.g. from your router or serial log at boot)
2. Change device settings on the LAN config pages; save

The setup AP page and LAN config pages expose the same core radar settings, but they are implemented separately. mDNS hostname is `plane-radar` → **plane-radar.local** (`kPortalHostname` in `config.h`). Some clients resolve `.local` slowly; use the IP if needed.

**Custom fields** (stored in NVS):

| Field | Purpose |
|-------|---------|
| **Latitude / Longitude** | Radar center and ADS-B query position (defaults in `config.h` until set) |
| **Field elevation (ft)** | Local field elevation used as the altitude color baseline |
| **Initial range** | Select one of the shipped mile presets: 1, 5, 10, 15, 20, or 25 mi |
| **Display distances in miles** | Ring scale label in **mi** instead of **km** |
| **Show airport runways** | Major-airport runway overlay on the radar (off to hide) |
| **Show aircraft on the ground** | Include aircraft reported as `ground` in ADS-B results |

After a reset, the device reboots and shows the setup screen immediately (no “Connecting” loop on stale credentials).

## Radar display

### Grid

- Dark blue background, subdued green rings and crosshairs
- White **N / S / E / W** at the bezel; range label on the **east** spoke (ring 3 = ¾ of outer radius)
- White center dot

Layout and colors: `include/ui/radar_theme.h`.

### Range presets

| Ring 3 label | Outer radius (aircraft scale) |
|------------|-------------------------------|
| 1 mi / 1.6 km | ~2.1 km |
| 5 mi / 8.0 km | ~10.7 km |
| 10 mi / 16.1 km | ~21.5 km |
| 15 mi / 24.1 km | ~32.2 km |
| 20 mi / 32.2 km | ~42.9 km |
| 25 mi / 40.2 km | ~53.6 km |

Preset, miles/km choice, runway overlay, and ground-aircraft visibility persist across reboot (`planeradar` NVS namespace).

### Runways

- Major airports from OurAirports (`large_airport`); all open runway strips in range (helipads excluded)
- Teal runway lines with one ICAO label per airport (e.g. `KJFK`); toggle in the Wi‑Fi setup portal
- Update the embedded list: `python3 scripts/build_large_airports.py`

### Aircraft

- **Inside the outer ring** — heading triangle, speed vector, and altitude tag color are altitude-based gradients (cooler at low altitude, warmer at high altitude)
- **Outside the ring** (still within ADS-B fetch) — small **gradient-colored dot on the screen rim** at the correct bearing (direction cue; not distance-accurate past the ring)
- **Tags** — placed toward the **center**: west (left) → tag on the **right** of the symbol; east (right) → tag on the **left**

When aircraft overlap, higher-altitude aircraft are drawn above lower-altitude aircraft.

As range decreases (or aircraft approach), targets move inward; beyond-ring dots become full symbols when they cross the outer ring.

### ADS-B

- Source: `https://opendata.adsb.fi/api/v3/`
- Fetch radius: `ui::radar::fetchRadiusKm()` — scales with the active preset to roughly the screen edge (so rim dots have data)
- Poll interval: `kAdsbFetchIntervalMs` (5 s) in `config.h`
- Ground aircraft hidden by default (`kAdsbShowGroundAircraft`)

## Configuration

Edit **`include/config.h`** for hardware and behavior:

| Area | Keys / notes |
|------|----------------|
| Portal | `kPortalApName`, `kPortalIp`, `kPortalHostname` / `kPortalHostUrl` (mDNS; needs `-DWM_MDNS` in `platformio.ini`) |
| Wi‑Fi timing | connect attempts, reconnect grace, portal timeout (`0` = no timeout) |
| BOOT | `kBootPin`, `kBootResetHoldMs`, `kBootTapMinMs` |
| Display SPI | pins, `kDisplayInvert`, `kDisplayRgbOrder`, `kDisplaySpiWriteHz` |
| Default location | `kDefaultRadarLat`, `kDefaultRadarLon` (until portal overrides) |
| ADS-B | `kAdsbFetchIntervalMs`, `kAdsbShowGroundAircraft` |

Range presets: `include/ui/radar_range.h` (`kRangePresets`).

## Project layout

```
include/
  config.h
  hardware/
    lgfx_config.hpp
    display.h
    display_font.h
  data/
    large_airports.h
  ui/
    radar_theme.h
    radar_range.h
    radar_display.h
    runway_overlay.h
    status_screens.h
  services/
    wifi_setup.h
    radar_location.h
    adsb_client.h
data/
  ui_font.vlw              — embedded smooth UI font (Noto Sans Bold)
scripts/
  build_large_airports.py
src/
  main.cpp
  data/
    large_airports_data.cpp
  hardware/
  ui/
  services/
```

## Wiring (GC9A01 ↔ ESP32-C3 Super Mini)

| Display | ESP32-C3 |
|---------|----------|
| VCC | 3V3 |
| GND | GND |
| RST | GPIO **0** |
| CS | GPIO **1** |
| DC | GPIO **10** |
| SDA (MOSI) | GPIO **3** |
| SCL (SCLK) | GPIO **4** |
| BOOT (user) | GPIO **9** |

## Build

```bash
pio run -t upload
pio device monitor
```

- PlatformIO env: **`supermini`**
- Serial: **115200** baud
- USB CDC on boot enabled in `platformio.ini` for the Super Mini

### Web-flashable release image

Single `.bin` for [esptool-js](https://espressif.github.io/esptool-js/) and similar tools (ESP32-C3, 4 MB, flash at **0x0**). This merged image includes the bootloader, partitions, app firmware, and LittleFS web UI files:

Use **Chrome** or **Edge** for Web Serial flashing.

```bash
chmod +x scripts/merge-firmware.sh   # once
./scripts/merge-firmware.sh
```

Writes `release/plane-radar-merged.bin`. Skip rebuild if firmware is already built:

```bash
./scripts/merge-firmware.sh --no-build
```

Or via PlatformIO only (output: `.pio/build/supermini/firmware-merged.bin`):

```bash
pio run -e supermini
pio run -t merge -e supermini
```

Put the board in download mode (hold **BOOT**, tap **RESET**), then flash with Chrome/Edge over USB.

Flash address for merged image: **`0x0`**.

If this is a **first-time flash**, or you are upgrading from an older build with a **different partition layout**, erase flash first for a clean start. A normal reflash keeps saved Wi‑Fi and settings.

### CI and releases (GitHub Actions)

| Workflow | When | Output |
|----------|------|--------|
| [Build](https://github.com/tbirdrv/ESP32-Plane-Radar/blob/main/.github/workflows/build.yml) | Push / PR to `main` | Artifact `plane-radar-supermini` (merged + split `.bin` files, ~90 days) |
| [Prerelease](https://github.com/tbirdrv/ESP32-Plane-Radar/blob/main/.github/workflows/prerelease.yml) | Push to `main` | Rolling prerelease asset `plane-radar-main-latest.bin` on tag `nightly-main` |
| [Release](https://github.com/tbirdrv/ESP32-Plane-Radar/blob/main/.github/workflows/release.yml) | Git tag `v*` (e.g. `v1.0.0`) | GitHub Release asset `plane-radar-v1.0.0.bin` + `.sha256` |

Latest build artifact download page: https://github.com/tbirdrv/ESP32-Plane-Radar/actions/workflows/build.yml

To ship a version users can download:

```bash
git tag v1.0.0
git push origin v1.0.0
```

The release workflow builds firmware in CI and attaches the merged image to the release. Download from **Releases** on GitHub, then flash at **0x0** (ESP32-C3, 4 MB). No separate LittleFS flashing step is required for that release image.

Release policy:

- Push to `main` updates the rolling prerelease (`nightly-main`, asset: `plane-radar-main-latest.bin`).
- Push tag `v*` creates a stable GitHub release (asset: `plane-radar-vX.Y.Z.bin`).

## Troubleshooting

### TLS / `start_ssl_client` errors

- Symptom: serial shows `start_ssl_client: -1` and `adsb: HTTP -1`.
- Meaning: HTTPS/TLS handshake failed for that ADS-B poll.
- Typical causes: temporary Wi-Fi instability, low/fragmented heap, or remote TLS timeout.
- Notes: firmware uses short fail-fast TLS timeouts and backoff to reduce display stalls.

### Portal / AP access issues

- Connect to AP `PlaneRadar-Setup`, then browse directly to `http://192.168.4.1`.
- If `plane-radar.local` is slow/not found, use the IP address.
- If setup AP is unstable, reboot and retry with only one phone/laptop connected.
- BOOT hold (3 s) resets Wi-Fi/location/units and reopens setup mode.

### Flashing issues

- Use merged image (`firmware-merged.bin`) at address **`0x0`**.
- The merged release image already includes the LittleFS web files.
- Erase flash before programming if this is a fresh board or the partition layout changed.
- Ensure board is in download mode (hold BOOT, tap RESET) before flashing.

## Dependencies

- [LovyanGFX](https://github.com/lovyan03/LovyanGFX)
- [WiFiManager](https://github.com/tzapu/WiFiManager)
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson)
