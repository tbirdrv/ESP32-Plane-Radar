#include "services/time_sync.h"
#include <Arduino.h>
#include <time.h>
#include <sys/time.h>

// Fallback Unix timestamp: 2024-01-01 00:00:00 UTC
// Used if NTP fails to sync
#define FALLBACK_UNIX_TIME 1704067200

// NTP configuration
#define NTP_SERVER "pool.ntp.org"
#define TZ_INFO "UTC0"  // We'll handle timezone in timezone_calc.cpp
#define NTP_TIMEOUT_MS 10000

// Static variables to track sync state
static bool ntp_synced = false;
static uint32_t last_sync_millis = 0;
static time_t last_sync_time = 0;

void timeSync_init() {
  // Configure system time from NTP
  // The configTime function is a built-in ESP32 function that:
  // 1. Sets the timezone offset (we use UTC, then apply offset in timezone_calc)
  // 2. Starts NTP synchronization in background
  
  Serial.println("[TimeSync] Initializing NTP time sync...");
  
  // Set timezone to UTC (no DST) - we'll apply timezone offset in timezone_calc
  configTime(0, 0, NTP_SERVER);
  
  // Give NTP time to sync (up to timeout)
  time_t now = time(nullptr);
  uint32_t start_ms = millis();
  
  // Wait for time to be set (system time > FALLBACK_UNIX_TIME indicates sync)
  while (now < FALLBACK_UNIX_TIME && (millis() - start_ms) < NTP_TIMEOUT_MS) {
    delay(100);
    now = time(nullptr);
  }
  
  if (now > FALLBACK_UNIX_TIME) {
    ntp_synced = true;
    last_sync_time = now;
    last_sync_millis = millis();
    
    // Print sync info
    struct tm timeinfo = *localtime(&now);
    char buffer[64];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S UTC", &timeinfo);
    Serial.printf("[TimeSync] NTP sync successful: %s\n", buffer);
  } else {
    ntp_synced = false;
    last_sync_time = FALLBACK_UNIX_TIME;
    last_sync_millis = millis();
    Serial.printf("[TimeSync] NTP sync timeout. Using fallback time.\n");
  }
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

bool timeSync_isSynced() {
  return ntp_synced;
}

void timeSync_resync() {
  // Trigger re-synchronization
  Serial.println("[TimeSync] Re-syncing NTP...");
  
  // Re-configure NTP (restarts background sync)
  configTime(0, 0, NTP_SERVER);
  
  time_t now = time(nullptr);
  uint32_t start_ms = millis();
  
  // Wait for updated time
  while ((millis() - start_ms) < NTP_TIMEOUT_MS) {
    delay(100);
    now = time(nullptr);
    
    // Check if time has advanced significantly (indicates fresh sync)
    if (now > (last_sync_time + 300)) {  // At least 5 minutes in future
      break;
    }
  }
  
  if (now > (last_sync_time + 300)) {
    ntp_synced = true;
    last_sync_time = now;
    last_sync_millis = millis();
    
    struct tm timeinfo = *localtime(&now);
    char buffer[64];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S UTC", &timeinfo);
    Serial.printf("[TimeSync] NTP re-sync successful: %s\n", buffer);
  } else {
    Serial.printf("[TimeSync] NTP re-sync failed.\n");
  }
}
