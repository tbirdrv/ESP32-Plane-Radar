#include "services/radar_location.h"

#include <Preferences.h>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "services/timezone_calc.h"

namespace services::location {

namespace {

constexpr char kPrefsNamespace[] = "radar";
constexpr char kKeyLat[] = "lat";
constexpr char kKeyLon[] = "lon";
constexpr char kKeyElevFt[] = "elev_ft";

double s_lat = config::kDefaultRadarLat;
double s_lon = config::kDefaultRadarLon;
int32_t s_elev_ft = config::kAltitudeColorFieldElevationFt;

bool parseCoord(const char* text, double* out) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  const double v = strtod(text, &end);
  if (end == text || (end != nullptr && *end != '\0')) {
    return false;
  }
  *out = v;
  return true;
}

bool parseElevationFt(const char* text, int32_t* out) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  const long v = strtol(text, &end, 10);
  if (end == text || (end != nullptr && *end != '\0')) {
    return false;
  }
  *out = static_cast<int32_t>(v);
  return true;
}

bool validLatLon(double lat, double lon) {
  return lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

bool validElevationFt(int32_t elev_ft) {
  return elev_ft >= -1500 && elev_ft <= 30000;
}

void persist(double lat, double lon, int32_t elev_ft) {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.putDouble(kKeyLat, lat);
  prefs.putDouble(kKeyLon, lon);
  prefs.putInt(kKeyElevFt, elev_ft);
  prefs.end();
  s_lat = lat;
  s_lon = lon;
  s_elev_ft = elev_ft;
  
  // Update timezone cache when location changes
  tzCalc_setCachedLocation(lat, lon);
}

}  // namespace

void init() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, true);
  if (prefs.isKey(kKeyLat) && prefs.isKey(kKeyLon)) {
    const double lat = prefs.getDouble(kKeyLat, config::kDefaultRadarLat);
    const double lon = prefs.getDouble(kKeyLon, config::kDefaultRadarLon);
    if (validLatLon(lat, lon)) {
      s_lat = lat;
      s_lon = lon;
    }
  }
  if (prefs.isKey(kKeyElevFt)) {
    const int32_t elev_ft =
        prefs.getInt(kKeyElevFt, config::kAltitudeColorFieldElevationFt);
    if (validElevationFt(elev_ft)) {
      s_elev_ft = elev_ft;
    }
  }
  prefs.end();
  
  // Initialize timezone cache on startup
  tzCalc_setCachedLocation(s_lat, s_lon);
}

double lat() { return s_lat; }

double lon() { return s_lon; }

int32_t elevationFt() { return s_elev_ft; }

bool saveFromStrings(const char* lat_str, const char* lon_str,
                     const char* elev_ft_str) {
  double lat = 0.0;
  double lon = 0.0;
  int32_t elev_ft = s_elev_ft;
  if (!parseCoord(lat_str, &lat) || !parseCoord(lon_str, &lon)) {
    return false;
  }
  if (elev_ft_str != nullptr && elev_ft_str[0] != '\0') {
    if (!parseElevationFt(elev_ft_str, &elev_ft)) {
      return false;
    }
  }
  if (!validLatLon(lat, lon)) {
    return false;
  }
  if (!validElevationFt(elev_ft)) {
    return false;
  }
  persist(lat, lon, elev_ft);
  Serial.printf("Radar location saved: %.6f, %.6f elev=%ld ft\n", lat, lon,
                static_cast<long>(elev_ft));
  return true;
}

void clear() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.remove(kKeyLat);
  prefs.remove(kKeyLon);
  prefs.remove(kKeyElevFt);
  prefs.end();
  s_lat = config::kDefaultRadarLat;
  s_lon = config::kDefaultRadarLon;
  s_elev_ft = config::kAltitudeColorFieldElevationFt;
}

}  // namespace services::location
