#include "services/time_sync.h"
#include <Arduino.h>
#include <time.h>
#include <sys/time.h>

// Fallback Unix timestamp: 2024-01-01 00:00:00 UTC
// Used if NTP fails to sync
#define FALLBACK_UNIX_TIME 1704067200

// Clock placeholder used while unsynced: frozen at 12:00.
#define FALLBACK_CLOCK_UNIX_TIME 1704110400

// NTP configuration
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.google.com"
#define NTP_SERVER_3 "time.cloudflare.com"
#define TZ_INFO "UTC0"  // We'll handle timezone in timezone_calc.cpp
#define NTP_RETRY_INTERVAL_MS 5000
#define NTP_RESYNC_INTERVAL_MS (12UL * 60UL * 60UL * 1000UL)

// Static variables to track sync state
static bool ntp_synced = false;
static uint32_t last_sync_millis = 0;
static time_t last_sync_time = 0;
static uint32_t last_ntp_attempt_millis = 0;
static uint32_t last_ntp_resync_millis = 0;

static void tryMarkSyncIfReady() {
  if (ntp_synced) {
    return;
  }
  time_t now = time(nullptr);
  if (now > FALLBACK_UNIX_TIME) {
    ntp_synced = true;
    last_sync_time = now;
    last_sync_millis = millis();
    last_ntp_resync_millis = last_sync_millis;

    struct tm timeinfo = *localtime(&now);
    char buffer[64];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S UTC", &timeinfo);
    Serial.printf("[TimeSync] NTP sync successful: %s\n", buffer);
  }
}

void timeSync_init() {
  // Configure system time from NTP
  // The configTime function is a built-in ESP32 function that:
  // 1. Sets the timezone offset (we use UTC, then apply offset in timezone_calc)
  // 2. Starts NTP synchronization in background
  
  Serial.println("[TimeSync] Initializing NTP time sync...");
  
  // Set timezone to UTC (no DST) - we'll apply timezone offset in timezone_calc
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
  last_ntp_attempt_millis = millis();
  ntp_synced = false;
  last_sync_time = FALLBACK_UNIX_TIME;
  last_sync_millis = millis();
  last_ntp_resync_millis = 0;

  tryMarkSyncIfReady();
  if (ntp_synced) {
    return;
  }

  Serial.printf("[TimeSync] NTP not ready yet. Clock will stay at 12:00 and retry every %u ms.\n",
                static_cast<unsigned>(NTP_RETRY_INTERVAL_MS));
}

time_t timeSync_getUnixTime() {
  // Calculate current time based on:
  // 1. Last known NTP sync time
  // 2. System uptime (millis()) since sync
  
  uint32_t current_millis = millis();
  uint32_t elapsed_ms = current_millis - last_sync_millis;
  uint32_t elapsed_sec = elapsed_ms / 1000;
  
  time_t current_time = last_sync_time + elapsed_sec;
  return current_time;
}

time_t timeSync_getClockUnixTime() {
  if (!ntp_synced) {
    return FALLBACK_CLOCK_UNIX_TIME;
  }
  return timeSync_getUnixTime();
}

bool timeSync_isSynced() {
  return ntp_synced;
}

void timeSync_poll() {
  if (ntp_synced) {
    const uint32_t now_ms = millis();
    if (last_ntp_resync_millis != 0 &&
        static_cast<uint32_t>(now_ms - last_ntp_resync_millis) < NTP_RESYNC_INTERVAL_MS) {
      return;
    }

    last_ntp_resync_millis = now_ms;
    Serial.println("[TimeSync] 12-hour NTP resync...");
    configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
    tryMarkSyncIfReady();
    return;
  }

  const uint32_t now_ms = millis();
  if (last_ntp_attempt_millis != 0 &&
      static_cast<uint32_t>(now_ms - last_ntp_attempt_millis) < NTP_RETRY_INTERVAL_MS) {
    tryMarkSyncIfReady();
    return;
  }

  last_ntp_attempt_millis = now_ms;
  Serial.println("[TimeSync] Retrying NTP sync...");
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
  tryMarkSyncIfReady();
}

void timeSync_resync() {
  Serial.println("[TimeSync] Re-syncing NTP...");
  ntp_synced = false;
  last_ntp_attempt_millis = millis();
  last_ntp_resync_millis = 0;
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
  tryMarkSyncIfReady();
}
