#include "services/timezone_calc.h"
#include <math.h>
#include <Arduino.h>

// Cached location for fast access
static double cached_lat = 41.7401;   // Default: Toledo, OH
static double cached_lon = -83.6491;

int32_t tzCalc_getUTCOffset(double lat, double lon) {
  // Civil time zones do not strictly follow 15-degree longitude bands.
  // Use a simple contiguous-US heuristic first (most common deployment),
  // then fall back to longitude-based rounding for the rest of the world.
  int32_t offset_hours = 0;

  const bool in_contiguous_us =
      (lat >= 24.0 && lat <= 50.0 && lon >= -125.0 && lon <= -66.0);
  if (in_contiguous_us) {
    // Approximate U.S. political timezone boundaries (standard time).
    // This avoids common 1-hour errors around places like Northwest Ohio.
    if (lon >= -85.0) {
      offset_hours = -5;  // Eastern
    } else if (lon >= -104.0) {
      offset_hours = -6;  // Central
    } else if (lon >= -114.0) {
      offset_hours = -7;  // Mountain
    } else {
      offset_hours = -8;  // Pacific
    }
  } else {
    // Fallback: nearest 15-degree timezone hour.
    offset_hours = (int32_t)lround(lon / 15.0);
  }
  
  // Clamp to ±12 hours
  if (offset_hours > 12) offset_hours = 12;
  if (offset_hours < -12) offset_hours = -12;
  
  // Convert to seconds
  return offset_hours * 3600;
}

/**
 * Helper: Determine if a given year is a leap year
 */
static bool isLeapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

/**
 * Helper: Get day of week for a given date (0=Sunday, 6=Saturday)
 * Uses Zeller's congruence algorithm
 */
static int getDayOfWeek(int year, int month, int day) {
  // Zeller's congruence (modified for day-of-week: 0=Sunday)
  if (month < 3) {
    month += 12;
    year--;
  }
  
  int q = day;
  int m = month;
  int k = year % 100;
  int j = year / 100;
  
  int h = (q + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 - 2 * j) % 7;
  
  // Zeller returns: 0=Sat, 1=Sun, 2=Mon, etc.
  // Convert to: 0=Sun, 1=Mon, etc.
  return (h + 6) % 7;
}

/**
 * Helper: Get the date of the Nth occurrence of a weekday in a month
 * @param year Year
 * @param month Month (1-12)
 * @param weekday Weekday to find (0=Sunday, 6=Saturday)
 * @param occurrence Which occurrence (1=first, 2=second, etc.)
 * @return Day of month (1-31)
 */
static int getNthWeekdayInMonth(int year, int month, int weekday, int occurrence) {
  // Find the first day of the month that matches the weekday
  int first_match_day = 1;
  while (getDayOfWeek(year, month, first_match_day) != weekday) {
    first_match_day++;
  }
  
  // Calculate the Nth occurrence
  int target_day = first_match_day + (occurrence - 1) * 7;
  
  // Make sure we don't go past the end of the month
  int days_in_month[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (isLeapYear(year)) {
    days_in_month[2] = 29;
  }
  
  if (target_day > days_in_month[month]) {
    target_day -= 7;
  }
  
  return target_day;
}

/**
 * Helper: Convert date/time to Unix timestamp
 */
static time_t mktime_utc(int year, int month, int day, int hour, int min, int sec) {
  // Simple calculation: days since epoch + seconds in current day
  // This is approximate but works for our DST boundary checks
  
  // Count days from 1970-01-01 to target date
  int64_t total_days = 0;
  
  // Add days for complete years from 1970 to year-1
  for (int y = 1970; y < year; y++) {
    total_days += isLeapYear(y) ? 366 : 365;
  }
  
  // Add days for complete months in target year
  int days_in_month[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (isLeapYear(year)) {
    days_in_month[2] = 29;
  }
  for (int m = 1; m < month; m++) {
    total_days += days_in_month[m];
  }
  
  // Add days in current month
  total_days += day - 1;
  
  // Convert to seconds and add time of day
  time_t result = total_days * 86400LL + hour * 3600LL + min * 60LL + sec;
  
  return result;
}

bool tzCalc_isDST(time_t unixTime, double lat) {
  // Only apply DST for Northern Hemisphere (lat > 0)
  if (lat <= 0) {
    return false;
  }
  
  // US DST rule (Northern Hemisphere):
  // Start: 2nd Sunday in March at 02:00 UTC
  // End: 1st Sunday in November at 02:00 UTC
  
  // Convert Unix timestamp to year/month/day/hour
  time_t utc_time = unixTime;
  struct tm *tm_info = gmtime(&utc_time);
  
  int year = tm_info->tm_year + 1900;
  int month = tm_info->tm_mon + 1;
  int day = tm_info->tm_mday;
  int hour = tm_info->tm_hour;
  int min = tm_info->tm_min;
  int sec = tm_info->tm_sec;
  
  // Calculate DST start: 2nd Sunday in March at 02:00 UTC
  int dst_start_day = getNthWeekdayInMonth(year, 3, 0, 2);  // 0 = Sunday
  time_t dst_start = mktime_utc(year, 3, dst_start_day, 2, 0, 0);
  
  // Calculate DST end: 1st Sunday in November at 02:00 UTC
  int dst_end_day = getNthWeekdayInMonth(year, 11, 0, 1);  // 0 = Sunday
  time_t dst_end = mktime_utc(year, 11, dst_end_day, 2, 0, 0);
  
  // Check if current time is within DST range
  return (unixTime >= dst_start && unixTime < dst_end);
}

time_t tzCalc_getLocalTime(time_t unixTime, double lat, double lon) {
  // Get base UTC offset from longitude
  int32_t utc_offset_sec = tzCalc_getUTCOffset(lat, lon);
  
  // Check if DST applies and add 1 hour if so
  int32_t dst_offset_sec = 0;
  if (tzCalc_isDST(unixTime, lat)) {
    dst_offset_sec = 3600;  // +1 hour for DST
  }
  
  // Return UTC time adjusted by timezone and DST offsets
  return unixTime + utc_offset_sec + dst_offset_sec;
}

time_t tzCalc_getLocalTimeFromCache(time_t unixTime) {
  return tzCalc_getLocalTime(unixTime, cached_lat, cached_lon);
}

void tzCalc_setCachedLocation(double lat, double lon) {
  cached_lat = lat;
  cached_lon = lon;
  
  int32_t offset = tzCalc_getUTCOffset(lat, lon);
  Serial.printf("[TZCalc] Updated timezone cache: lat=%.4f lon=%.4f offset=%ld sec\n", 
                lat, lon, offset);
}
