/**
 * Plane Radar — WiFi setup, then radar UI on the round GC9A01 display.
 */

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "services/time_sync.h"
#include "services/timezone_calc.h"
#include "services/wifi_setup.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

namespace {

bool g_radar_visible = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_adsb_fetch_ms = 0;
// Track clock refresh when no aircraft (update every second)
unsigned long g_last_clock_ms = 0;
bool g_prev_wifi_connected = false;

void showRadarIfConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    return;
  }
  ui::radarDisplayDraw();
  g_radar_visible = true;
}

void onRangeTap() {
  ui::radar::rangeNext();
  char range_label[12];
  ui::radar::formatCurrentRing3Label(range_label, sizeof(range_label));
  Serial.printf("Range: %s (outer ~%.0f km)\n", range_label,
                ui::radar::rangeCurrent().outer_km);

  if (g_radar_visible && WiFi.status() == WL_CONNECTED) {
    ui::radarDisplayDraw();
  }
}

void handleBootButton() {
  bootButtonPollLongPress();
  static uint8_t tap_count = 0;
  static unsigned long first_tap_ms = 0;
  const unsigned long now = millis();
  constexpr unsigned long kTripleTapWindowMs = 1000;

  // If taps time out, treat them as normal range taps.
  if (tap_count > 0 && now - first_tap_ms > kTripleTapWindowMs) {
    while (tap_count > 0) {
      onRangeTap();
      --tap_count;
    }
    first_tap_ms = 0;
  }

  if (bootButtonConsumeTap()) {
    Serial.printf("BOOT tap received (count=%u)\n", static_cast<unsigned>(tap_count + 1));

    if (tap_count == 0) {
      tap_count = 1;
      first_tap_ms = now;
      return;
    }

    if ((now - first_tap_ms) > kTripleTapWindowMs) {
      // Prior sequence expired; start a fresh one.
      tap_count = 1;
      first_tap_ms = now;
      return;
    }

    ++tap_count;
    if (tap_count >= 3 && (now - first_tap_ms) <= kTripleTapWindowMs) {
      tap_count = 0;
      first_tap_ms = 0;
      Serial.println("BOOT triple-tap: opening LAN portal on demand");
      wifiRequestLanPortalOnDemand();
      return;
    }
  }
}

void fetchAndDrawAircraft() {
  const bool clock_only_effective =
      ui::radar::clockOnlyModeEnabled() && !ui::radar::radarOnlyModeEnabled();
  if (clock_only_effective) {
    return;
  }
  // Pause ADS-B fetches whenever a portal is active so the web server
  // can use CPU and socket resources without starvation.
  if (wifiPortalActive()) {
    return;
  }
  const float fetch_km = ui::radar::fetchRadiusKm();
  services::adsb::fetchUpdate(services::location::lat(),
                              services::location::lon(), fetch_km);
  // Refresh on both success and failure. On failure we keep cached aircraft,
  // which prevents the UI from appearing stuck in clock mode.
  ui::radarDisplayRefreshAircraft();
  handleBootButton();
}

bool minuteClockWindowActive() {
  if (ui::radar::radarOnlyModeEnabled()) {
    return false;
  }
  if (ui::radar::clockOnlyModeEnabled()) {
    return true;
  }
  const uint8_t minute_window_sec = ui::radar::clockMinuteWindowSec();
  if (!timeSync_isSynced()) {
    return false;
  }
  if (minute_window_sec == 0) {
    return false;
  }
  const time_t utc_time = timeSync_getUnixTime();
  const time_t local_time = tzCalc_getLocalTimeFromCache(utc_time);
  struct tm* tm_info = localtime(&local_time);
  return tm_info != nullptr && tm_info->tm_sec < minute_window_sec;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Plane Radar");
  Serial.printf("[SETUP] Heap: free=%u bytes, min=%u bytes\n",
                ESP.getFreeHeap(), ESP.getMinFreeHeap());

  bootButtonInit();
  displayInit();
  Serial.println("Initializing location and radar services");
  services::location::init();
  ui::radar::rangeInit();
  services::adsb::setPollFn(wifiLoop);

  const bool setup_on_boot = wifiShowsSetupScreenOnBoot();
  Serial.printf("Boot Wi-Fi setup pending: %s\n", setup_on_boot ? "yes" : "no");
  if (setup_on_boot) {
    statusScreenPortal();
  }

  if (wifiSetupConnect()) {
    Serial.println("Wi-Fi setup complete: connected");
    // Initialize NTP time sync after WiFi connects
    timeSync_init();
    showRadarIfConnected();
    // Let heap/network settle briefly after portal close before first TLS fetch.
    g_last_adsb_fetch_ms = millis();
  } else {
    Serial.println("Wi-Fi setup complete: not connected");
  }
}

void loop() {
  handleBootButton();
  timeSync_poll();
  wifiLoop();

  // While a portal is active, keep the loop lightweight so WiFiManager
  // request handling is not starved by display/radar work.
  if (wifiPortalActive()) {
    delay(10);
    return;
  }

  const bool wifi_connected_now = (WiFi.status() == WL_CONNECTED);
  if (wifi_connected_now != g_prev_wifi_connected) {
    if (wifi_connected_now) {
      Serial.printf("[WiFi] Link up: SSID '%s' IP %s RSSI %d\n",
                    WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                    WiFi.RSSI());
    } else {
      Serial.println("[WiFi] Link down");
    }
    g_prev_wifi_connected = wifi_connected_now;
  }

  // Periodically log heap to diagnose memory pressure during portal activity.
  static unsigned long last_heap_log_ms = 0;
  if (millis() - last_heap_log_ms >= 15000) {
    last_heap_log_ms = millis();
    Serial.printf("[LOOP] Portal=%d Aircraft=%u Heap: free=%u, min=%u\n",
                  (int)wifiPortalActive(),
                  static_cast<unsigned>(services::adsb::aircraftCount()),
                  ESP.getFreeHeap(), ESP.getMinFreeHeap());
  }

  if (!wifi_connected_now) {
    if (g_radar_visible) {
      if (!wifiPortalActive()) {
        Serial.println("WiFi lost — will reconnect");
      }
      g_radar_visible = false;
    }

    if (wifiPortalActive()) {
      g_wifi_down_since = 0;
      delay(10);
      return;
    }

    if (g_wifi_down_since == 0) {
      g_wifi_down_since = millis();
    }

    const unsigned long down_ms = millis() - g_wifi_down_since;
    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      g_last_reconnect_ms = millis();
      if (wifiReconnect()) {
        g_wifi_down_since = 0;
        showRadarIfConnected();
      }
    }
  } else {
    g_wifi_down_since = 0;
    if (!g_radar_visible) {
      showRadarIfConnected();
      // reset clock timer
      g_last_clock_ms = millis();
      const bool clock_only_effective =
          ui::radar::clockOnlyModeEnabled() && !ui::radar::radarOnlyModeEnabled();
      if (!clock_only_effective) {
        g_last_adsb_fetch_ms = millis() - config::kAdsbFetchIntervalMs;
        fetchAndDrawAircraft();
        g_last_adsb_fetch_ms = millis();
      }
    } else {
      if (ui::radar::radarOnlyModeEnabled()) {
        if (millis() - g_last_clock_ms >= config::kAdsbFetchIntervalMs) {
          g_last_clock_ms = millis();
          ui::radarDisplayRefreshAircraft();
        }
      }

      const bool clock_window_active = minuteClockWindowActive();
      const size_t cached_aircraft_count = services::adsb::aircraftCount();
      unsigned long fetch_interval_ms = config::kAdsbFetchIntervalMs;
      if (cached_aircraft_count == 0) {
        // In no-aircraft clock mode, poll less often to keep second-hand ticks stable.
        fetch_interval_ms = 10000;
      }

      // Poll ADS-B on interval, but avoid blocking fetches during minute clock window.
      const bool clock_only_effective =
          ui::radar::clockOnlyModeEnabled() && !ui::radar::radarOnlyModeEnabled();
      if (!clock_only_effective && !clock_window_active &&
          millis() - g_last_adsb_fetch_ms >= fetch_interval_ms) {
        g_last_adsb_fetch_ms = millis();
        fetchAndDrawAircraft();
      }

      // If there are no aircraft, still update the clock once per second so
      // the second hand advances smoothly between ADS-B polls.
      const size_t aircraft_count = services::adsb::aircraftCount();
      if (aircraft_count == 0 || clock_window_active) {
        if (millis() - g_last_clock_ms >= 1000) {
          g_last_clock_ms = millis();
          ui::radarDisplayRefreshAircraft();
        }
      }
    }
  }

  delay(10);
}
