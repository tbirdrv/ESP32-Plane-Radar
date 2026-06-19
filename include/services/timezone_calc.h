#ifndef TIMEZONE_CALC_H
#define TIMEZONE_CALC_H

#include <time.h>

/**
 * Calculate UTC offset (in seconds) based on latitude and longitude
 * Uses a contiguous-US heuristic first, then falls back to longitude rounding.
 * Result is clamped to ±12 hours.
 * 
 * @param lat Latitude in degrees (-90 to +90, positive = North)
 * @param lon Longitude in degrees (-180 to +180, positive = East)
 * @return UTC offset in seconds (negative = west of UTC, positive = east)
 */
int32_t tzCalc_getUTCOffset(double lat, double lon);

/**
 * Check if daylight saving time is active for Northern Hemisphere
 * DST rule: 2nd Sunday in March 02:00 UTC → 1st Sunday in November 02:00 UTC
 * 
 * @param unixTime Unix timestamp to check
 * @param lat Latitude (only applied if lat > 0 for Northern Hemisphere)
 * @return true if DST is active, false otherwise
 */
bool tzCalc_isDST(time_t unixTime, double lat);

/**
 * Get local Unix timestamp adjusted for timezone and DST
 * Returns the time as it would be displayed in the local timezone
 * 
 * @param unixTime UTC Unix timestamp
 * @param lat Latitude in degrees
 * @param lon Longitude in degrees
 * @return Unix timestamp adjusted for local timezone and DST
 */
time_t tzCalc_getLocalTime(time_t unixTime, double lat, double lon);

/**
 * Helper: Get current local Unix timestamp using stored lat/lon
 * Requires that timezone cache is initialized (see radar_location.cpp)
 * 
 * @param unixTime UTC Unix timestamp
 * @return Unix timestamp adjusted to local timezone
 */
time_t tzCalc_getLocalTimeFromCache(time_t unixTime);

/**
 * Cache the timezone offset after location change
 * Called by radar_location.cpp when location is updated
 * 
 * @param lat Latitude
 * @param lon Longitude
 */
void tzCalc_setCachedLocation(double lat, double lon);

#endif // TIMEZONE_CALC_H
