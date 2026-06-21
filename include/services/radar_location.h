#pragma once

#include <cstddef>
#include <cstdint>

namespace services::location {

constexpr size_t kMaxSavedLocations = 3;
constexpr uint8_t kNoActiveSlot = 0xFF;

struct SavedLocation {
	char name[13];  // max 12 chars + null terminator
	double lat;
	double lon;
	int32_t elev_ft;
	bool valid;
};

/** Load saved lat/lon from NVS, or use config defaults. Call once before WiFi setup. */
void init();

/** Factory defaults when nothing is stored (also used for portal field prefill). */
double lat();
double lon();
int32_t elevationFt();

/** Parse portal strings, validate, persist to NVS, update runtime values. */
bool saveFromStrings(const char* lat_str, const char* lon_str,
					 const char* elev_ft_str = nullptr);

/** Clear stored coordinates (e.g. with WiFi credential reset). */
void clear();

// --- Saved location slots ---

size_t savedLocationCount();
const SavedLocation* savedLocations();

/** Index of active slot, or kNoActiveSlot when using custom coords. */
uint8_t activeLocationIndex();

/** Save or overwrite a slot. Validates lat/lon; trims name to 12 chars. */
bool saveLocation(uint8_t slot, const char* name, const char* lat_str,
				  const char* lon_str, const char* elev_ft_str = nullptr);

/** Set given slot as active and move radar center to its coords. */
bool setActiveLocation(uint8_t slot);

/** Remove slot by index. */
void deleteLocation(uint8_t slot);

}  // namespace services::location
