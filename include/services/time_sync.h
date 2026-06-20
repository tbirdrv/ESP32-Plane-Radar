#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#include <time.h>
#include <stdint.h>

/**
 * Initialize NTP time synchronization
 * Attempts to sync with NTP pool on boot
 * If NTP fails, falls back to compile-time constant
 * Should be called after WiFi is connected
 */
void timeSync_init();

/**
 * Get current time as Unix timestamp (seconds since Jan 1, 1970 UTC)
 * This uses NTP-synced system time adjusted by millis() since last sync
 * @return Unix timestamp in seconds (time_t)
 */
time_t timeSync_getUnixTime();

/**
 * Get the time value used for clock rendering.
 * When NTP is not synced yet, this returns a frozen 12:00 placeholder.
 */
time_t timeSync_getClockUnixTime();

/**
 * Check if NTP sync has completed successfully
 * @return true if time is synchronized from NTP, false if using fallback
 */
bool timeSync_isSynced();

/**
 * Manually trigger NTP re-sync (useful for periodic updates)
 * Can be called periodically (e.g., every 12 hours) to maintain accuracy
 */
void timeSync_resync();

/**
 * Poll NTP sync state and retry periodically if unsynced.
 * Call from the main loop so fallback time never gets shown as real time.
 */
void timeSync_poll();

#endif // TIME_SYNC_H
