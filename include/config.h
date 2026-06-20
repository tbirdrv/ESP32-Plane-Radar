#pragma once

#include <cstdint>

#include <driver/gpio.h>

namespace config {

// --- Wi-Fi portal ---
constexpr char kPortalApName[] = "PlaneRadar-Setup";
constexpr char kPortalIp[] = "192.168.4.1";
/** mDNS host (no ".local" suffix); browser: http://plane-radar.local */
constexpr char kPortalHostname[] = "plane-radar";
constexpr char kPortalHostUrl[] = "plane-radar.local";

/** Per-attempt STA connect wait (ms); retried kWifiConnectAttempts times. */
constexpr unsigned long kWifiConnectAttemptMs = 15000;
constexpr uint8_t kWifiConnectAttempts = 3;
constexpr unsigned long kWifiPortalTimeoutSec = 300;  // 5 min timeout; portal restarts on next boot if AP stays dead
constexpr unsigned long kWifiConnectingFrameMs = 50;
/** Filter APs shown on /wifi by quality percent (0-100). ~60 ~= 3-4 bars. */
constexpr int kWifiScanMinQualityPct = 60;
/** Run portal as AP-only (no STA) for maximum AP stability on low-memory C3. */
constexpr bool kWifiPortalApOnly = true;
/** Wait after disconnect before reconnecting (avoids portal on brief drops). */
constexpr unsigned long kWifiDownGraceMs = 4000;
/** Minimum interval between background reconnect tries. */
constexpr unsigned long kWifiReconnectIntervalMs = 7000;

// --- BOOT button (ESP32-C3 Super Mini, active LOW) ---
constexpr gpio_num_t kBootPin = GPIO_NUM_9;
/** Ignore BOOT taps shorter than this (debounce). */
constexpr unsigned long kBootTapMinMs = 40UL;
constexpr unsigned long kBootResetHoldMs = 3000UL;

// --- Display: GC9A01 1.28" round 240×240 (SPI) ---
constexpr gpio_num_t kDisplayPinRst = GPIO_NUM_0;
constexpr gpio_num_t kDisplayPinCs = GPIO_NUM_1;
constexpr gpio_num_t kDisplayPinDc = GPIO_NUM_10;
constexpr gpio_num_t kDisplayPinMosi = GPIO_NUM_3;  // display SDA
constexpr gpio_num_t kDisplayPinSclk = GPIO_NUM_4;  // display SCL

constexpr int kDisplayWidth = 240;
constexpr int kDisplayHeight = 240;

constexpr uint32_t kDisplaySpiWriteHz = 40000000;
// GC9A01 modules often need invert + BGR for correct black/green output
constexpr bool kDisplayInvert = true;
constexpr bool kDisplayRgbOrder = true;

// --- Radar center defaults (overridden via WiFi setup portal) ---
constexpr double kDefaultRadarLat = 41.7401;
constexpr double kDefaultRadarLon = -83.6491;

/** Poll adsb.fi (API public limit: 1 req/s). */
constexpr unsigned long kAdsbFetchIntervalMs = 3000;

// --- Aircraft altitude color gradient ---
// All values in feet MSL / AGL as noted.
/** Local field elevation (ft MSL). Subtracted before gradient is applied so
 *  low-flying nearby traffic stays in cool colors. Tune to your site. */
constexpr int32_t kAltGradFieldElevFt = 600;
/** AGL threshold (ft) below which aircraft appear blue→cyan ("cool"). */
constexpr int32_t kAltGradWarmStartAglFt = 8000;
/** AGL threshold (ft) where cyan transitions to green. */
constexpr int32_t kAltGradGreenTopAglFt = 18000;
/** AGL threshold (ft) where green transitions to yellow. */
constexpr int32_t kAltGradYellowTopAglFt = 30000;
/** AGL ceiling (ft) — aircraft above this saturate at orange/red. */
constexpr int32_t kAltGradMaxAglFt = 40000;
/** Legacy scale unused — fetch uses radar::fetchRadiusKm() to screen edge. */
constexpr float kAdsbFetchRadiusScale = 1.0f;
/** false = hide aircraft with alt_baro "ground"; true = show them too. */
constexpr bool kAdsbShowGroundAircraft = true;

// --- Altitude color gradient (aircraft markers/tags) ---
// Values are in feet. AGL-like thresholds are derived from:
// reported altitude - kAltitudeColorFieldElevationFt.
// Tune these for local deployment/elevation.
constexpr int32_t kAltitudeColorFieldElevationFt = 600;
constexpr int32_t kAltitudeColorWarmStartAglFt = 8000;
constexpr int32_t kAltitudeColorGreenTopAglFt = 18000;
constexpr int32_t kAltitudeColorYellowTopAglFt = 30000;
constexpr int32_t kAltitudeColorMaxAglFt = 40000;

// --- UI colors (RGB565) — status screens ---
constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorYellow = 0xFFE0;
constexpr uint16_t kTextOnYellow = kColorBlack;
constexpr uint16_t kTextOnBlack = 0xFFFF;

}  // namespace config
