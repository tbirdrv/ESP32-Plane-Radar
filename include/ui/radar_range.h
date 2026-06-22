#pragma once

#include <cstddef>
#include <cstdint>

namespace ui::radar {

/**
 * Range presets (label on ring 3 = ¾ of outer radius).
 *
 * Stored internally in km, but configured for user-facing mile presets:
 *   1, 5, 10, 15, 20, 25 miles.
 *
 * Outer radius (for aircraft math) is ring-3 distance ÷ 0.75.
 */
struct RangePreset {
  /** Distance shown on ring 3 (¾ of outer radius), always stored in km. */
  float ring3_km;
  float outer_km;
};

constexpr float kRing3ToOuterKm = 4.0f / 3.0f;

constexpr RangePreset kRangePresets[] = {
    {1.609344f, 1.609344f * kRing3ToOuterKm},
    {8.04672f, 8.04672f * kRing3ToOuterKm},
    {16.09344f, 16.09344f * kRing3ToOuterKm},
    {24.14016f, 24.14016f * kRing3ToOuterKm},
    {32.18688f, 32.18688f * kRing3ToOuterKm},
    {40.2336f, 40.2336f * kRing3ToOuterKm},
};

constexpr size_t kRangePresetCount =
    sizeof(kRangePresets) / sizeof(kRangePresets[0]);

/** Load saved range and distance units from flash. Call once after boot. */
void rangeInit();
/** Cycle preset and save to flash. */
void rangeNext();
const RangePreset& rangeCurrent();
uint8_t rangeIndex();
/** ADSB fetch radius (km): scaled to screen edge so beyond-ring dots have data. */
float fetchRadiusKm();

bool useMiles();
bool showRunways();
bool showGroundAircraft();
uint8_t clockMinuteWindowSec();
bool lanPortalEnabled();
bool clockOnlyModeEnabled();
bool radarOnlyModeEnabled();
/** WiFi portal checkbox: "T" = miles, otherwise km. */
void saveMilesFromPortal(const char* checkbox_value);
void saveRunwaysFromPortal(const char* checkbox_value);
void saveShowGroundAircraftFromPortal(const char* checkbox_value);
/** WiFi portal selection field: range preset index (0..kRangePresetCount-1). */
void saveRangeIndexFromPortal(const char* index_value);
/** WiFi portal numeric field: range in miles (maps to nearest preset). */
void saveRangeMilesFromPortal(const char* miles_value);
/** WiFi portal numeric field: seconds (0-59) for per-minute clock window. */
void saveClockMinuteWindowSecFromPortal(const char* seconds_value);
void saveLanPortalEnabledFromPortal(const char* checkbox_value);
void saveClockOnlyModeFromPortal(const char* checkbox_value);
void saveRadarOnlyModeFromPortal(const char* checkbox_value);
void formatRing3Label(char* buf, size_t len, float ring3_km, bool use_miles);
void formatCurrentRing3Label(char* buf, size_t len);
/** Reset distance units to miles (e.g. with WiFi credential wipe). */
void unitsReset();

}  // namespace ui::radar
