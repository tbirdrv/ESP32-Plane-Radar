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
constexpr char kKeyActiveSlot[] = "slotAct";

// Per-slot NVS keys (indexed by slot 0-2)
const char* kSlotValidKeys[kMaxSavedLocations] = {"s0v", "s1v", "s2v"};
const char* kSlotNameKeys[kMaxSavedLocations] = {"s0name", "s1name", "s2name"};
const char* kSlotLatKeys[kMaxSavedLocations] = {"s0lat", "s1lat", "s2lat"};
const char* kSlotLonKeys[kMaxSavedLocations] = {"s0lon", "s1lon", "s2lon"};
const char* kSlotElevKeys[kMaxSavedLocations] = {"s0elev", "s1elev", "s2elev"};

double s_lat = config::kDefaultRadarLat;
double s_lon = config::kDefaultRadarLon;
int32_t s_elev_ft = config::kAltitudeColorFieldElevationFt;
uint8_t s_active_slot = kNoActiveSlot;
SavedLocation s_locations[kMaxSavedLocations] = {};

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

void persistActiveSlot() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.putUChar(kKeyActiveSlot, s_active_slot);
  prefs.end();
}

void persistSlot(uint8_t slot) {
  if (slot >= kMaxSavedLocations) {
    return;
  }
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.putBool(kSlotValidKeys[slot], s_locations[slot].valid);
  if (s_locations[slot].valid) {
    prefs.putString(kSlotNameKeys[slot], s_locations[slot].name);
    prefs.putDouble(kSlotLatKeys[slot], s_locations[slot].lat);
    prefs.putDouble(kSlotLonKeys[slot], s_locations[slot].lon);
    prefs.putInt(kSlotElevKeys[slot], s_locations[slot].elev_ft);
  } else {
    prefs.remove(kSlotNameKeys[slot]);
    prefs.remove(kSlotLatKeys[slot]);
    prefs.remove(kSlotLonKeys[slot]);
    prefs.remove(kSlotElevKeys[slot]);
  }
  prefs.end();
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

  s_active_slot = prefs.getUChar(kKeyActiveSlot, kNoActiveSlot);
  if (s_active_slot >= kMaxSavedLocations) {
    s_active_slot = kNoActiveSlot;
  }

  for (uint8_t i = 0; i < kMaxSavedLocations; ++i) {
    s_locations[i].valid = prefs.getBool(kSlotValidKeys[i], false);
    s_locations[i].name[0] = '\0';
    if (s_locations[i].valid) {
      prefs.getString(kSlotNameKeys[i], s_locations[i].name,
                      sizeof(s_locations[i].name));
      s_locations[i].lat = prefs.getDouble(kSlotLatKeys[i], 0.0);
      s_locations[i].lon = prefs.getDouble(kSlotLonKeys[i], 0.0);
      s_locations[i].elev_ft =
          prefs.getInt(kSlotElevKeys[i], config::kAltitudeColorFieldElevationFt);
      if (!validLatLon(s_locations[i].lat, s_locations[i].lon)) {
        s_locations[i].valid = false;
      }
      if (!validElevationFt(s_locations[i].elev_ft)) {
        s_locations[i].elev_ft = config::kAltitudeColorFieldElevationFt;
      }
    } else {
      s_locations[i].elev_ft = config::kAltitudeColorFieldElevationFt;
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
  s_active_slot = kNoActiveSlot;
  persistActiveSlot();
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
  prefs.remove(kKeyActiveSlot);
  for (uint8_t i = 0; i < kMaxSavedLocations; ++i) {
    prefs.remove(kSlotValidKeys[i]);
    prefs.remove(kSlotNameKeys[i]);
    prefs.remove(kSlotLatKeys[i]);
    prefs.remove(kSlotLonKeys[i]);
    prefs.remove(kSlotElevKeys[i]);
  }
  prefs.end();
  s_lat = config::kDefaultRadarLat;
  s_lon = config::kDefaultRadarLon;
  s_elev_ft = config::kAltitudeColorFieldElevationFt;
  s_active_slot = kNoActiveSlot;
  for (uint8_t i = 0; i < kMaxSavedLocations; ++i) {
    s_locations[i].valid = false;
    s_locations[i].name[0] = '\0';
    s_locations[i].lat = 0.0;
    s_locations[i].lon = 0.0;
    s_locations[i].elev_ft = config::kAltitudeColorFieldElevationFt;
  }
}

size_t savedLocationCount() {
  size_t n = 0;
  for (size_t i = 0; i < kMaxSavedLocations; ++i) {
    if (s_locations[i].valid) {
      ++n;
    }
  }
  return n;
}

const SavedLocation* savedLocations() { return s_locations; }

uint8_t activeLocationIndex() { return s_active_slot; }

bool saveLocation(uint8_t slot, const char* name, const char* lat_str,
                  const char* lon_str, const char* elev_ft_str) {
  if (slot >= kMaxSavedLocations) {
    return false;
  }

  double lat = 0.0;
  double lon = 0.0;
  if (!parseCoord(lat_str, &lat) || !parseCoord(lon_str, &lon)) {
    return false;
  }
  if (!validLatLon(lat, lon)) {
    return false;
  }

  int32_t elev_ft = s_elev_ft;
  if (elev_ft_str != nullptr && elev_ft_str[0] != '\0') {
    if (!parseElevationFt(elev_ft_str, &elev_ft)) {
      return false;
    }
    if (!validElevationFt(elev_ft)) {
      return false;
    }
  }

  s_locations[slot].valid = true;
  s_locations[slot].lat = lat;
  s_locations[slot].lon = lon;
  s_locations[slot].elev_ft = elev_ft;

  const char* src_name = (name != nullptr) ? name : "";
  const size_t name_len = strnlen(src_name, sizeof(s_locations[slot].name) - 1);
  memcpy(s_locations[slot].name, src_name, name_len);
  s_locations[slot].name[name_len] = '\0';

  persistSlot(slot);
  Serial.printf("Location slot %u saved: '%s' %.6f, %.6f\n", slot,
                s_locations[slot].name, lat, lon);
  return true;
}

bool setActiveLocation(uint8_t slot) {
  if (slot >= kMaxSavedLocations || !s_locations[slot].valid) {
    return false;
  }
  persist(s_locations[slot].lat, s_locations[slot].lon, s_locations[slot].elev_ft);
  s_active_slot = slot;
  persistActiveSlot();
  Serial.printf("Active location: slot %u '%s'\n", slot, s_locations[slot].name);
  return true;
}

void deleteLocation(uint8_t slot) {
  if (slot >= kMaxSavedLocations) {
    return;
  }
  s_locations[slot].valid = false;
  s_locations[slot].name[0] = '\0';
  s_locations[slot].lat = 0.0;
  s_locations[slot].lon = 0.0;
  s_locations[slot].elev_ft = config::kAltitudeColorFieldElevationFt;
  if (s_active_slot == slot) {
    s_active_slot = kNoActiveSlot;
    persistActiveSlot();
  }
  persistSlot(slot);
  Serial.printf("Location slot %u deleted\n", slot);
}

}  // namespace services::location
