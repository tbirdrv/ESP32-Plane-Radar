#include "ui/radar_range.h"

#include "ui/radar_theme.h"
#include "config.h"

#include <Preferences.h>
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ui::radar {

namespace {

constexpr char kPrefsNamespace[] = "planeradar";
constexpr char kPrefsRangeKey[] = "rangeIdx";
constexpr char kPrefsMilesKey[] = "useMiles";
constexpr char kPrefsRunwaysKey[] = "showRwys";
constexpr char kPrefsGroundKey[] = "showGnd";
constexpr char kPrefsClockWindowKey[] = "clkWinSec";
constexpr char kPrefsLanPortalKey[] = "lanPortal";
constexpr char kPrefsClockOnlyKey[] = "clockOnly";
constexpr char kPrefsRadarOnlyKey[] = "radarOnly";
constexpr uint8_t kDefaultRangeIndex = 1;  // 10 km ring
constexpr uint8_t kDefaultClockMinuteWindowSec = 10;
constexpr uint8_t kMinClockMinuteWindowSec = 0;
constexpr uint8_t kMaxClockMinuteWindowSec = 59;
constexpr bool kDefaultLanPortalEnabled = true;
constexpr bool kDefaultClockOnlyEnabled = false;
constexpr bool kDefaultRadarOnlyEnabled = false;
constexpr float kKmPerMile = 1.609344f;

Preferences s_prefs;
uint8_t s_range_index = kDefaultRangeIndex;
bool s_use_miles = true;
bool s_show_runways = true;
bool s_show_ground_aircraft = config::kAdsbShowGroundAircraft;
uint8_t s_clock_minute_window_sec = kDefaultClockMinuteWindowSec;
bool s_lan_portal_enabled = kDefaultLanPortalEnabled;
bool s_clock_only_enabled = kDefaultClockOnlyEnabled;
bool s_radar_only_enabled = kDefaultRadarOnlyEnabled;

void saveRangeIndex() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putUChar(kPrefsRangeKey, s_range_index);
  s_prefs.end();
}

void saveUseMiles() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsMilesKey, s_use_miles);
  s_prefs.end();
}

void saveShowRunways() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsRunwaysKey, s_show_runways);
  s_prefs.end();
}

void saveShowGroundAircraft() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsGroundKey, s_show_ground_aircraft);
  s_prefs.end();
}

void saveClockMinuteWindowSec() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putUChar(kPrefsClockWindowKey, s_clock_minute_window_sec);
  s_prefs.end();
}

void saveLanPortalEnabled() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsLanPortalKey, s_lan_portal_enabled);
  s_prefs.end();
}

void saveClockOnlyMode() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsClockOnlyKey, s_clock_only_enabled);
  s_prefs.end();
}

void saveRadarOnlyMode() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsRadarOnlyKey, s_radar_only_enabled);
  s_prefs.end();
}

bool portalCheckboxChecked(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  // WiFiManager checkbox submits its value= attribute ("T", or "F" if we prefilled F).
  if ((value[0] == 'T' || value[0] == 't' || value[0] == 'F' || value[0] == 'f') &&
      value[1] == '\0') {
    return true;
  }
  return strcmp(value, "on") == 0;
}

}  // namespace

void rangeInit() {
  if (!s_prefs.begin(kPrefsNamespace, true)) {
    return;
  }
  const uint8_t saved = s_prefs.getUChar(kPrefsRangeKey, kDefaultRangeIndex);
  s_range_index =
      (saved < kRangePresetCount) ? saved : kDefaultRangeIndex;
  s_use_miles = s_prefs.getBool(kPrefsMilesKey, true);
  s_show_runways = s_prefs.getBool(kPrefsRunwaysKey, true);
    s_show_ground_aircraft =
      s_prefs.getBool(kPrefsGroundKey, config::kAdsbShowGroundAircraft);
    const uint8_t saved_clock_window =
      s_prefs.getUChar(kPrefsClockWindowKey, kDefaultClockMinuteWindowSec);
    s_clock_minute_window_sec =
      (saved_clock_window <= kMaxClockMinuteWindowSec)
        ? saved_clock_window
        : kDefaultClockMinuteWindowSec;
    if (s_prefs.isKey(kPrefsLanPortalKey)) {
      s_lan_portal_enabled =
          s_prefs.getBool(kPrefsLanPortalKey, kDefaultLanPortalEnabled);
    } else {
      s_lan_portal_enabled = kDefaultLanPortalEnabled;
    }
    s_clock_only_enabled = s_prefs.getBool(kPrefsClockOnlyKey, kDefaultClockOnlyEnabled);
    s_radar_only_enabled = s_prefs.getBool(kPrefsRadarOnlyKey, kDefaultRadarOnlyEnabled);
  s_prefs.end();
}

void rangeNext() {
  s_range_index = static_cast<uint8_t>((s_range_index + 1) % kRangePresetCount);
  saveRangeIndex();
}

const RangePreset& rangeCurrent() { return kRangePresets[s_range_index]; }

uint8_t rangeIndex() { return s_range_index; }

float fetchRadiusKm() {
  const float outer_km = rangeCurrent().outer_km;
  const float screen_r_px =
      static_cast<float>(kCenterX - kBeyondRingScreenMarginPx);
  return outer_km * (screen_r_px / static_cast<float>(kGridOuterRadius));
}

bool useMiles() { return s_use_miles; }

bool showRunways() { return s_show_runways; }

bool showGroundAircraft() { return s_show_ground_aircraft; }

uint8_t clockMinuteWindowSec() { return s_clock_minute_window_sec; }

bool lanPortalEnabled() { return s_lan_portal_enabled; }

bool clockOnlyModeEnabled() { return s_clock_only_enabled; }

bool radarOnlyModeEnabled() { return s_radar_only_enabled; }

void saveMilesFromPortal(const char* checkbox_value) {
  s_use_miles = portalCheckboxChecked(checkbox_value);
  saveUseMiles();
  Serial.printf("Distance units: %s\n", s_use_miles ? "miles" : "km");
}

void saveRunwaysFromPortal(const char* checkbox_value) {
  s_show_runways = portalCheckboxChecked(checkbox_value);
  saveShowRunways();
  Serial.printf("Runway overlay: %s\n", s_show_runways ? "on" : "off");
}

void saveShowGroundAircraftFromPortal(const char* checkbox_value) {
  s_show_ground_aircraft = portalCheckboxChecked(checkbox_value);
  saveShowGroundAircraft();
  Serial.printf("Show ground aircraft: %s\n",
                s_show_ground_aircraft ? "on" : "off");
}

void saveRangeIndexFromPortal(const char* index_value) {
  if (index_value == nullptr || index_value[0] == '\0') {
    return;
  }

  char* end = nullptr;
  long index = strtol(index_value, &end, 10);
  if (end == index_value || (end != nullptr && *end != '\0')) {
    Serial.printf("Range preset unchanged (invalid value: '%s')\n", index_value);
    return;
  }

  if (index < 0) {
    index = 0;
  }
  if (index >= static_cast<long>(kRangePresetCount)) {
    index = static_cast<long>(kRangePresetCount) - 1;
  }

  s_range_index = static_cast<uint8_t>(index);
  saveRangeIndex();
  Serial.printf("Range preset index: %u (ring3 %.0f km)\n",
                static_cast<unsigned>(s_range_index),
                static_cast<double>(kRangePresets[s_range_index].ring3_km));
}

void saveRangeMilesFromPortal(const char* miles_value) {
  if (miles_value == nullptr || miles_value[0] == '\0') {
    return;
  }

  char* end = nullptr;
  const float miles = strtof(miles_value, &end);
  if (end == miles_value || (end != nullptr && *end != '\0') ||
      !std::isfinite(miles)) {
    Serial.printf("Range preset unchanged (invalid miles value: '%s')\n",
                  miles_value);
    return;
  }

  const int miles_int = static_cast<int>(lroundf(miles));
  bool allowed = false;
  for (const int v : {1, 5, 10, 15, 20, 25}) {
    if (miles_int == v) {
      allowed = true;
      break;
    }
  }
  if (!allowed) {
    Serial.printf("Range preset unchanged (unsupported miles value: '%s')\n",
                  miles_value);
    return;
  }

  uint8_t best_index = 0;
  float best_error = 1e9f;
  for (uint8_t i = 0; i < kRangePresetCount; ++i) {
    const float preset_miles = kRangePresets[i].ring3_km / kKmPerMile;
    const float error = fabsf(preset_miles - miles);
    if (error < best_error) {
      best_error = error;
      best_index = i;
    }
  }

  s_range_index = best_index;
  saveRangeIndex();
  Serial.printf("Range preset: %.2f mi (index %u)\n",
                static_cast<double>(kRangePresets[s_range_index].ring3_km /
                                    kKmPerMile),
                static_cast<unsigned>(s_range_index));
}

void saveClockMinuteWindowSecFromPortal(const char* seconds_value) {
  if (seconds_value == nullptr || seconds_value[0] == '\0') {
    return;
  }

  char* end = nullptr;
  long seconds = strtol(seconds_value, &end, 10);
  if (end == seconds_value || (end != nullptr && *end != '\0')) {
    Serial.printf("Clock minute window unchanged (invalid value: '%s')\n",
                  seconds_value);
    return;
  }

  if (seconds < kMinClockMinuteWindowSec) {
    seconds = kMinClockMinuteWindowSec;
  }
  if (seconds > kMaxClockMinuteWindowSec) {
    seconds = kMaxClockMinuteWindowSec;
  }

  s_clock_minute_window_sec = static_cast<uint8_t>(seconds);
  saveClockMinuteWindowSec();
  Serial.printf("Clock minute window: %u s\n",
                static_cast<unsigned>(s_clock_minute_window_sec));
}

void saveLanPortalEnabledFromPortal(const char* checkbox_value) {
  s_lan_portal_enabled = portalCheckboxChecked(checkbox_value);
  saveLanPortalEnabled();
  Serial.printf("LAN portal: %s\n", s_lan_portal_enabled ? "always-on" : "off");
}

void saveClockOnlyModeFromPortal(const char* checkbox_value) {
  s_clock_only_enabled = portalCheckboxChecked(checkbox_value);
  saveClockOnlyMode();
  Serial.printf("Clock-only mode: %s\n", s_clock_only_enabled ? "on" : "off");
}

void saveRadarOnlyModeFromPortal(const char* checkbox_value) {
  s_radar_only_enabled = portalCheckboxChecked(checkbox_value);
  saveRadarOnlyMode();
  Serial.printf("Radar-only mode: %s\n", s_radar_only_enabled ? "on" : "off");
}

void formatRing3Label(char* buf, size_t len, float ring3_km, bool use_miles) {
  if (use_miles) {
    const int mi = static_cast<int>(lroundf(ring3_km / kKmPerMile));
    snprintf(buf, len, "%dmi", mi);
  } else {
    const int km = static_cast<int>(lroundf(ring3_km));
    snprintf(buf, len, "%dkm", km);
  }
}

void formatCurrentRing3Label(char* buf, size_t len) {
  formatRing3Label(buf, len, rangeCurrent().ring3_km, s_use_miles);
}

void unitsReset() {
  s_use_miles = true;
  s_show_runways = true;
  s_show_ground_aircraft = config::kAdsbShowGroundAircraft;
  s_clock_minute_window_sec = kDefaultClockMinuteWindowSec;
  s_lan_portal_enabled = kDefaultLanPortalEnabled;
  s_clock_only_enabled = kDefaultClockOnlyEnabled;
  s_radar_only_enabled = kDefaultRadarOnlyEnabled;
  if (s_prefs.begin(kPrefsNamespace, false)) {
    s_prefs.remove(kPrefsMilesKey);
    s_prefs.remove(kPrefsRunwaysKey);
    s_prefs.remove(kPrefsGroundKey);
    s_prefs.remove(kPrefsClockWindowKey);
    s_prefs.remove(kPrefsLanPortalKey);
    s_prefs.remove(kPrefsClockOnlyKey);
    s_prefs.remove(kPrefsRadarOnlyKey);
    s_prefs.end();
  }
}

}  // namespace ui::radar
