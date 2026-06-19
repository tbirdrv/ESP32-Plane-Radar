#include "ui/radar_range.h"

#include "ui/radar_theme.h"

#include <Preferences.h>
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
constexpr char kPrefsClockWindowKey[] = "clkWinSec";
constexpr char kPrefsLanPortalKey[] = "lanPortal";
constexpr char kPrefsClockOnlyKey[] = "clockOnly";
constexpr char kPrefsRadarOnlyKey[] = "radarOnly";
constexpr uint8_t kDefaultRangeIndex = 1;  // 10 km ring
constexpr uint8_t kDefaultClockMinuteWindowSec = 10;
constexpr uint8_t kMinClockMinuteWindowSec = 0;
constexpr uint8_t kMaxClockMinuteWindowSec = 59;
constexpr bool kDefaultLanPortalEnabled = false;
constexpr bool kDefaultClockOnlyEnabled = false;
constexpr bool kDefaultRadarOnlyEnabled = false;
constexpr float kKmPerMile = 1.609344f;

Preferences s_prefs;
uint8_t s_range_index = kDefaultRangeIndex;
bool s_use_miles = false;
bool s_show_runways = true;
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
  s_use_miles = s_prefs.getBool(kPrefsMilesKey, false);
  s_show_runways = s_prefs.getBool(kPrefsRunwaysKey, true);
    const uint8_t saved_clock_window =
      s_prefs.getUChar(kPrefsClockWindowKey, kDefaultClockMinuteWindowSec);
    s_clock_minute_window_sec =
      (saved_clock_window <= kMaxClockMinuteWindowSec)
        ? saved_clock_window
        : kDefaultClockMinuteWindowSec;
    s_lan_portal_enabled = s_prefs.getBool(kPrefsLanPortalKey, kDefaultLanPortalEnabled);
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
  s_use_miles = false;
  s_show_runways = true;
  s_clock_minute_window_sec = kDefaultClockMinuteWindowSec;
  s_lan_portal_enabled = kDefaultLanPortalEnabled;
  s_clock_only_enabled = kDefaultClockOnlyEnabled;
  s_radar_only_enabled = kDefaultRadarOnlyEnabled;
  if (s_prefs.begin(kPrefsNamespace, false)) {
    s_prefs.remove(kPrefsMilesKey);
    s_prefs.remove(kPrefsRunwaysKey);
    s_prefs.remove(kPrefsClockWindowKey);
    s_prefs.remove(kPrefsLanPortalKey);
    s_prefs.remove(kPrefsClockOnlyKey);
    s_prefs.remove(kPrefsRadarOnlyKey);
    s_prefs.end();
  }
}

}  // namespace ui::radar
