#include "services/wifi_setup.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>

#include <cstdio>

#include <Preferences.h>
#include <esp_system.h>
#include <esp_wifi.h>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include "config.h"
#include "services/radar_location.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

portMUX_TYPE s_boot_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_boot_tap_pending = false;
volatile bool s_boot_is_down = false;
volatile unsigned long s_boot_down_ms = 0;
bool s_long_press_handled = false;
bool s_boot_interrupt_attached = false;

void IRAM_ATTR onBootButtonIsr() {
  const bool down = digitalRead(config::kBootPin) == LOW;
  const unsigned long now = millis();
  portENTER_CRITICAL_ISR(&s_boot_mux);
  if (down) {
    s_boot_is_down = true;
    s_boot_down_ms = now;
  } else if (s_boot_is_down) {
    const unsigned long held = now - s_boot_down_ms;
    if (held >= config::kBootTapMinMs && held < config::kBootResetHoldMs) {
      s_boot_tap_pending = true;
    }
    s_boot_is_down = false;
  }
  portEXIT_CRITICAL_ISR(&s_boot_mux);
}

void initBootButton() {
  pinMode(config::kBootPin, INPUT_PULLUP);
  if (s_boot_interrupt_attached) {
    return;
  }
  attachInterrupt(digitalPinToInterrupt(static_cast<uint8_t>(config::kBootPin)),
                  onBootButtonIsr, CHANGE);
  s_boot_interrupt_attached = true;
}

namespace {

/** Separate from planeradar prefs (rangeInit) to avoid NVS handle conflicts. */
constexpr char kWifiPrefsNamespace[] = "wifi";
constexpr char kPrefsForcePortalKey[] = "portal";

bool s_force_config_portal = false;
WiFiManager s_wm;
bool s_wm_configured = false;
bool s_prev_wifi_link_up = false;
bool s_prev_portal_active = false;
unsigned long s_lan_portal_grace_deadline_ms = 0;
unsigned long s_last_lan_ap_watchdog_ms = 0;
bool s_switch_to_ap_only_wifi_requested = false;
bool s_stop_portal_requested = false;
bool s_lan_portal_on_demand_requested = false;
bool s_lan_portal_on_demand_active = false;
bool s_wifi_save_seen = false;

constexpr unsigned long kLanPortalGraceMs = 600000;  // 10 minutes after connect

void ensureWifiManager();
void startLanWebPortal();
void stopLanWebPortal();
bool wifiLinkUp();

void onPortalWebServerReady() {
  if (s_wm.server == nullptr) {
    return;
  }

  Serial.printf("[Portal] WebServer callback fired. Mode=%d ConfigPortal=%d WebPortal=%d Heap=%u\n",
                (int)WiFi.getMode(), s_wm.getConfigPortalActive(), 
                s_wm.getWebPortalActive(), ESP.getFreeHeap());

  auto redirect_to_portal = []() {
    if (s_wm.server == nullptr) {
      return;
    }
    // Keep the config portal quiet: captive redirects can trigger repeated
    // browser handoffs on ESP32-C3 AP mode. The setup page is still reachable
    // directly at 192.168.4.1.
    if (s_wm.getConfigPortalActive()) {
      s_wm.server->send(204, "text/plain", "");
      return;
    }
    Serial.printf("[Portal] Captive probe -> /\n");
    s_wm.server->sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
    s_wm.server->send(302, "text/plain", "");
  };

  auto serve_root = []() {
    if (s_wm.server == nullptr) {
      return;
    }
    Serial.printf("[Portal] GET / G�� serving lightweight portal home\n");
    static const char kPortalHomeLan[] PROGMEM =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<link rel='icon' href='data:,'>"
        "<title>Plane Radar Setup</title>"
        "<style>body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;"
        "margin:0;padding:18px;background:#f3f6f8;color:#12202b}"
        "h1{font-size:1.2rem;margin:0 0 10px}p{margin:0 0 12px;color:#30495c}"
        "small{display:block;margin:-6px 0 14px;color:#567;font-size:.82rem}"
        "a{display:block;text-decoration:none;margin:10px 0;padding:12px 14px;"
        "border-radius:10px;background:#0e7a70;color:#fff;font-weight:600}"
        "a.secondary{background:#456a86}</style></head><body>"
        "<h1>Plane Radar Setup</h1>"
        "<p>Choose an action:</p>"
        "<a href='/param'>Device Settings</a>"
        "<a href='/0wifi'>WiFi Settings</a>"
        "<a class='secondary' href='/info'>Status</a>"
        "<small>To change Wi-Fi network, connect to the<br>"
        "<b>PlaneRadar-Setup</b> AP and open 192.168.4.1</small>"
        "</body></html>";
      s_wm.server->send(200, "text/html", kPortalHomeLan);
  };

  // Register root page and captive portal probe endpoints.
  s_wm.server->on("/", HTTP_GET, serve_root);

  auto switch_to_wifi_ap_only = []() {
    if (s_wm.server == nullptr) {
      return;
    }
    s_wm.server->sendHeader("Location", "/0wifi", true);
    s_wm.server->send(302, "text/plain", "");
  };
  s_wm.server->on("/wifi_ap", HTTP_GET, switch_to_wifi_ap_only);

  s_wm.server->on("/generate_204", redirect_to_portal);
  s_wm.server->on("/gen_204", redirect_to_portal);
  s_wm.server->on("/hotspot-detect.html", redirect_to_portal);
  s_wm.server->on("/connecttest.txt", redirect_to_portal);
  s_wm.server->on("/ncsi.txt", redirect_to_portal);
  s_wm.server->on("/fwlink", redirect_to_portal);
  Serial.printf("[Portal] Root page and captive portal probes registered.\n");
}

bool portalOptionChecked(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  if ((value[0] == 'T' || value[0] == 't' || value[0] == 'F' || value[0] == 'f') &&
      value[1] == '\0') {
    return true;
  }
  return strcmp(value, "on") == 0;
}

constexpr int kCoordParamLen = 20;
constexpr int kClockWindowParamLen = 4;
constexpr char kCoordInputAttrs[] =
    " type=\"number\" step=\"0.000001\"";

WiFiManagerParameter s_param_lat("radar_lat", "Latitude (deg)", "0",
                                kCoordParamLen, kCoordInputAttrs);
WiFiManagerParameter s_param_lon("radar_lon", "Longitude (deg)", "0",
                                kCoordParamLen, kCoordInputAttrs);

char s_miles_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_miles("use_miles", "Display distances in miles", "T", 2,
                                   s_miles_checkbox_attrs, WFM_LABEL_AFTER);

char s_runways_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_runways("show_runways", "Show airport runways", "T", 2,
                                     s_runways_checkbox_attrs, WFM_LABEL_AFTER);

char s_lan_portal_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_lan_portal("lan_portal",
                                        "Keep LAN portal always enabled", "T", 2,
                                        s_lan_portal_checkbox_attrs,
                                        WFM_LABEL_AFTER);

char s_clock_only_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_clock_only("clock_only",
                                        "Clock mode only (disable ADS-B radar)",
                                        "T", 2, s_clock_only_checkbox_attrs,
                                        WFM_LABEL_AFTER);

char s_radar_only_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_radar_only("radar_only",
                                        "Radar mode only (never show clock)",
                                        "T", 2, s_radar_only_checkbox_attrs,
                                        WFM_LABEL_AFTER);

constexpr char kClockWindowInputAttrs[] =
    " type=\"number\" min=\"0\" max=\"59\" step=\"1\"";
WiFiManagerParameter s_param_clock_window("clock_window_sec",
                                          "Clock at each minute (sec)", "10",
                                          kClockWindowParamLen,
                                          kClockWindowInputAttrs);

void refreshPortalParamDefaults() {
  char lat_buf[kCoordParamLen + 1];
  char lon_buf[kCoordParamLen + 1];
  char clock_window_buf[kClockWindowParamLen + 1];
  snprintf(lat_buf, sizeof(lat_buf), "%.6f", services::location::lat());
  snprintf(lon_buf, sizeof(lon_buf), "%.6f", services::location::lon());
  snprintf(clock_window_buf, sizeof(clock_window_buf), "%u",
           static_cast<unsigned>(ui::radar::clockMinuteWindowSec()));
  s_param_lat.setValue(lat_buf, kCoordParamLen);
  s_param_lon.setValue(lon_buf, kCoordParamLen);
  s_param_clock_window.setValue(clock_window_buf, kClockWindowParamLen);
  snprintf(s_miles_checkbox_attrs, sizeof(s_miles_checkbox_attrs), "type=\"checkbox\"%s",
           ui::radar::useMiles() ? " checked" : "");
  s_param_miles.setValue("T", 2);
  snprintf(s_runways_checkbox_attrs, sizeof(s_runways_checkbox_attrs),
           "type=\"checkbox\"%s", ui::radar::showRunways() ? " checked" : "");
  s_param_runways.setValue("T", 2);
  snprintf(s_lan_portal_checkbox_attrs, sizeof(s_lan_portal_checkbox_attrs),
           "type=\"checkbox\"%s",
           ui::radar::lanPortalEnabled() ? " checked" : "");
  s_param_lan_portal.setValue("T", 2);
  snprintf(s_clock_only_checkbox_attrs, sizeof(s_clock_only_checkbox_attrs),
           "type=\"checkbox\"%s",
           ui::radar::clockOnlyModeEnabled() ? " checked" : "");
  s_param_clock_only.setValue("T", 2);
  snprintf(s_radar_only_checkbox_attrs, sizeof(s_radar_only_checkbox_attrs),
           "type=\"checkbox\"%s",
           ui::radar::radarOnlyModeEnabled() ? " checked" : "");
  s_param_radar_only.setValue("T", 2);
}

void onPortalParamsSaved() {
  Serial.println("[Portal] Save params callback fired");
  if (!services::location::saveFromStrings(s_param_lat.getValue(),
                                           s_param_lon.getValue())) {
    Serial.println("Invalid lat/lon in portal G�� keeping previous location");
  }
  ui::radar::saveMilesFromPortal(s_param_miles.getValue());
  ui::radar::saveRunwaysFromPortal(s_param_runways.getValue());
  ui::radar::saveClockMinuteWindowSecFromPortal(s_param_clock_window.getValue());
  ui::radar::saveLanPortalEnabledFromPortal(s_param_lan_portal.getValue());
  bool clock_only = portalOptionChecked(s_param_clock_only.getValue());
  bool radar_only = portalOptionChecked(s_param_radar_only.getValue());

  // Keep mode selection mutually exclusive. If both appear set, prefer radar-only.
  if (clock_only && radar_only) {
    clock_only = false;
  }
  ui::radar::saveClockOnlyModeFromPortal(clock_only ? "T" : "");
  ui::radar::saveRadarOnlyModeFromPortal(radar_only ? "T" : "");

  // If LAN portal was turned off, exit portal mode right after save so
  // display/network returns to normal operation.
  if (!ui::radar::lanPortalEnabled()) {
    s_stop_portal_requested = true;
  }

  // If this was an on-demand portal session, auto-close after save so
  // ADS-B resumes and the display returns to normal operation.
  if (s_lan_portal_on_demand_active) {
    Serial.println("[Portal] On-demand portal: closing after config save, restarting ADS-B");
    s_stop_portal_requested = true;
  }
}

void onPortalWifiSaved() {
  s_wifi_save_seen = true;
  Serial.println("[Portal] Save WiFi callback fired");
}

void attachPortalParams(WiFiManager& wm) {
  refreshPortalParamDefaults();
  wm.addParameter(&s_param_lat);
  wm.addParameter(&s_param_lon);
  wm.addParameter(&s_param_clock_window);
  wm.addParameter(&s_param_miles);
  wm.addParameter(&s_param_runways);
  wm.addParameter(&s_param_lan_portal);
  wm.addParameter(&s_param_clock_only);
  wm.addParameter(&s_param_radar_only);
  wm.setSaveParamsCallback(onPortalParamsSaved);
  wm.setSaveConfigCallback(onPortalWifiSaved);
}

void markForceConfigPortal() {
  s_force_config_portal = true;
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    return;
  }
  prefs.putBool(kPrefsForcePortalKey, true);
  prefs.end();
}

bool consumeForceConfigPortal() {
  if (s_force_config_portal) {
    s_force_config_portal = false;
    Preferences prefs;
    if (prefs.begin(kWifiPrefsNamespace, false)) {
      prefs.remove(kPrefsForcePortalKey);
      prefs.end();
    }
    return true;
  }

  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  if (!pending) {
    return false;
  }

  if (prefs.begin(kWifiPrefsNamespace, false)) {
    prefs.remove(kPrefsForcePortalKey);
    prefs.end();
  }
  return true;
}

bool storedWifiCredentials() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
    return false;
  }
  return conf.sta.ssid[0] != '\0';
}

void eraseWifiCredentials() {
  stopLanWebPortal();
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_OFF);
  delay(100);

  ensureWifiManager();
  WiFi.persistent(true);
  s_wm.resetSettings();
  s_wm.erase();
  WiFi.disconnect(true, true);
  WiFi.persistent(false);

  WiFi.mode(WIFI_OFF);
  delay(100);
}

void resetWifiCredentials() {
  markForceConfigPortal();
  eraseWifiCredentials();
  services::location::clear();
  ui::radar::unitsReset();
  Serial.println("WiFi credentials, location, and units cleared");
}

void onConfigPortalApStarted(WiFiManager*) {
  // Only show the full setup status screen when STA is not connected.
  // During LAN/grace portal while connected, keep the radar display visible.
  if (!wifiLinkUp()) {
    statusScreenPortal();
  }
#ifdef WM_MDNS
  MDNS.end();
  if (MDNS.begin(config::kPortalHostname)) {
    Serial.printf("Setup portal: http://%s.local (or http://%s)\n",
                  config::kPortalHostname, config::kPortalIp);
  } else {
    Serial.printf("Setup portal: http://%s (mDNS unavailable)\n", config::kPortalIp);
  }
#else
  Serial.printf("Setup portal: http://%s\n", config::kPortalIp);
#endif
}

bool wifiLinkUp() {
  return WiFi.status() == WL_CONNECTED &&
         WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

bool lanPortalGraceActive() {
  if (s_lan_portal_grace_deadline_ms == 0) {
    return false;
  }
  return static_cast<long>(s_lan_portal_grace_deadline_ms - millis()) > 0;
}

bool ensureLanAccessPoint(bool ap_only) {
  WiFi.mode(ap_only ? WIFI_AP : WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));
  if (!WiFi.softAP(config::kPortalApName, "")) {
    Serial.println("LAN AP ensure failed: softAP() returned false");
    return false;
  }
  delay(100);
  const IPAddress ap_ip = WiFi.softAPIP();
  if (ap_ip == IPAddress(0, 0, 0, 0)) {
    Serial.println("LAN AP ensure failed: AP has no IP");
    return false;
  }
  Serial.printf("LAN AP ready: SSID '%s' IP %s\n", config::kPortalApName,
                ap_ip.toString().c_str());
  return true;
}

void ensureWifiManager() {
  if (s_wm_configured) {
    return;
  }
  static const char kPortalHeadScript[] PROGMEM =
      "<script>"
      "document.addEventListener('DOMContentLoaded',function(){"
      "var r=function(){document.querySelectorAll('a,button,input[type=submit]').forEach(function(e){"
      "var t=(e.textContent||e.value||'').trim();"
      "if(t==='Configure WiFi'){if(e.textContent)e.textContent='Configure Plane-Radar';if(e.value)e.value='Configure Plane-Radar';}"
      "});};"
      "var m=function(){"
      "var c=document.getElementById('clock_only');"
      "var d=document.getElementById('radar_only');"
      "if(!c||!d){return;}"
      "var hideField=function(el,id){"
      "el.style.display='none';"
      "var forLabel=document.querySelector('label[for='+id+']');if(forLabel){forLabel.style.display='none';}"
      "var parentLabel=el.closest('label');"
      "if(parentLabel&&parentLabel.textContent&&parentLabel.textContent.trim().length>0){parentLabel.style.display='none';}"
      "};"
      "hideField(c,'clock_only');hideField(d,'radar_only');"
      "var form=c.form||document.querySelector('form');"
      "if(!form){return;}"
      "var wrap=document.getElementById('display_mode_group');"
      "if(!wrap){"
      "wrap=document.createElement('div');wrap.id='display_mode_group';wrap.style.margin='8px 0 12px 0';"
      "var title=document.createElement('div');title.textContent='Display Mode';title.style.fontWeight='600';title.style.margin='4px 0';wrap.appendChild(title);"
      "var addOpt=function(val,text){var lab=document.createElement('label');lab.style.display='block';lab.style.margin='2px 0';var i=document.createElement('input');i.type='radio';i.name='display_mode';i.value=val;i.style.marginRight='6px';lab.appendChild(i);lab.appendChild(document.createTextNode(text));wrap.appendChild(lab);return i;};"
      "var autoOpt=addOpt('auto','Auto');"
      "var clockOpt=addOpt('clock','Clock only');"
      "var radarOpt=addOpt('radar','Radar only');"
      "if(c.checked){clockOpt.checked=true;}else if(d.checked){radarOpt.checked=true;}else{autoOpt.checked=true;}"
      "var cwEl=document.getElementById('clock_window_sec');"
      "var cwLabel=cwEl?document.querySelector('label[for=clock_window_sec]'):null;"
      "var updateCw=function(v){if(!cwEl){return;}var dis=(v!=='auto');cwEl.disabled=dis;cwEl.style.opacity=dis?'0.4':'1';if(cwLabel){cwLabel.style.opacity=dis?'0.4':'1';}};"
      "var apply=function(){var s=form.querySelector('input[name=display_mode]:checked');var v=s?s.value:'auto';c.checked=(v==='clock');d.checked=(v==='radar');updateCw(v);};"
      "wrap.addEventListener('change',apply);form.addEventListener('submit',apply);"
      "form.insertBefore(wrap,form.firstChild);"
      "apply();updateCw(autoOpt.checked?'auto':clockOpt.checked?'clock':'radar');"
      "}"
      "};"
      "r();setTimeout(r,200);"
      "m();setTimeout(m,200);"
      "});"
      "</script>";
  s_wm.setConfigPortalTimeout(config::kWifiPortalTimeoutSec);
  s_wm.setMinimumSignalQuality(config::kWifiScanMinQualityPct);
  s_wm.setWiFiAPHidden(false);
  s_wm.setWiFiAPChannel(1);
  // Captive DNS redirects cause AP instability on ESP32-C3. Keep portal
  // explicit at 192.168.4.1 — the setup screen shows the URL.
  s_wm.setCaptivePortalEnable(false);
  s_wm.setWebServerCallback(onPortalWebServerReady);
  s_wm.setAPStaticIPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                           IPAddress(255, 255, 255, 0));
  s_wm.setHostname(config::kPortalHostname);
  s_wm.setCustomHeadElement(kPortalHeadScript);
  s_wm.setAPCallback(onConfigPortalApStarted);
  attachPortalParams(s_wm);
  s_wm_configured = true;
}

void startLanWebPortal() {
  if (s_wm.getWebPortalActive() || s_wm.getConfigPortalActive()) {
    Serial.println("LAN portal not started: portal already active");
    return;
  }
  if (!wifiLinkUp()) {
    Serial.println("LAN portal not started: STA link is down");
    return;
  }
  refreshPortalParamDefaults();
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  s_wm.setConfigPortalBlocking(false);
  // Run portal on STA only; avoid AP+STA to preserve heap for ADS-B/TLS.
  s_wm.startWebPortal();
  Serial.printf("LAN portal running on STA. IP %s\n",
                WiFi.localIP().toString().c_str());
}

void stopLanWebPortal() {
  bool had_portal = false;
  if (s_wm.getWebPortalActive()) {
    s_wm.stopWebPortal();
    had_portal = true;
  }
  if (s_wm.getConfigPortalActive()) {
    s_wm.stopConfigPortal();
    had_portal = true;
  }
  if (!had_portal) {
    return;
  }
  WiFi.mode(WIFI_STA);
#ifdef WM_MDNS
  MDNS.end();
#endif
}

void prepareSta() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setAutoReconnect(true);
}

void startStaConnect(const String& ssid, const String& pass) {
  prepareSta();
  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    WiFi.begin();
  }
}

bool waitForLinkWithUi(const char* ssid_for_ui, unsigned long attempt_ms) {
  const unsigned long deadline = millis() + attempt_ms;
  while (millis() < deadline) {
    if (wifiLinkUp()) {
      return true;
    }
    bootButtonPollLongPress();
    statusScreenConnectingTick();
    delay(config::kWifiConnectingFrameMs);
  }
  return wifiLinkUp();
}

bool tryConnectWithUi(const String& ssid, const String& pass, bool show_ui) {
  if (wifiLinkUp()) {
    return true;
  }

  const char* ui_ssid = ssid.length() > 0 ? ssid.c_str() : "network";
  if (show_ui) {
    statusScreenConnectingBegin(ui_ssid);
  }

  for (uint8_t attempt = 1; attempt <= config::kWifiConnectAttempts; ++attempt) {
    // Always reset the STA state before each manual attempt. This prevents
    // begin() from failing with "sta is connecting" after a prior failed try.
    if (attempt > 1) {
      Serial.printf("WiFi connect retry %u/%u\n", attempt,
                    config::kWifiConnectAttempts);
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(400);

    startStaConnect(ssid, pass);

    if (waitForLinkWithUi(ui_ssid, config::kWifiConnectAttemptMs)) {
      return true;
    }
  }

  return false;
}

bool connectSavedNetwork(bool show_ui) {
  if (!storedWifiCredentials()) {
    Serial.println("No stored Wi-Fi credentials available");
    return false;
  }

  ensureWifiManager();
  const String ssid = s_wm.getWiFiSSID();
  if (ssid.length() == 0) {
    Serial.println("WiFiManager has no saved SSID");
    return false;
  }
  const String pass = s_wm.getWiFiPass();
  Serial.printf("Trying saved Wi-Fi network '%s'\n", ssid.c_str());
  return tryConnectWithUi(ssid, pass, show_ui);
}

bool openConfigPortal() {
  stopLanWebPortal();
  s_wifi_save_seen = false;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  statusScreenPortal();
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setAutoReconnect(false);
  // Use AP-only setup portal mode. The /wifi route is forced to /0wifi
  // (no scan), so STA is not needed here and AP-only is more stable.
  const bool use_ap_sta_for_setup = false;
  WiFi.mode(use_ap_sta_for_setup ? WIFI_AP_STA : WIFI_AP);
  Serial.printf("Starting config portal AP '%s' (AP-only=%d, runtime AP+STA=%d)\n",
                config::kPortalApName, config::kWifiPortalApOnly ? 1 : 0,
                use_ap_sta_for_setup ? 1 : 0);
  s_wm.setConfigPortalBlocking(false);
  s_wm.startConfigPortal(config::kPortalApName);

  const IPAddress ap_ip = WiFi.softAPIP();
  Serial.printf("WiFi mode after portal start: %d\n", WiFi.getMode());
  Serial.printf("SoftAP IP after portal start: %s\n", ap_ip.toString().c_str());

  if (!s_wm.getConfigPortalActive()) {
    Serial.println("Failed to start config portal");
    return false;
  }

  if (ap_ip == IPAddress(0, 0, 0, 0)) {
    Serial.println("Config portal AP has no IP address. Retrying AP startup...");
    if (!ensureLanAccessPoint(config::kWifiPortalApOnly)) {
      Serial.println("Fallback softAP() failed");
      return false;
    }
    delay(500);
    const IPAddress fallback_ip = WiFi.softAPIP();
    Serial.printf("Fallback AP IP: %s\n", fallback_ip.toString().c_str());
    if (fallback_ip == IPAddress(0, 0, 0, 0)) {
      Serial.println("Fallback softAP has no IP. Portal startup failed.");
      return false;
    }
  }

  Serial.printf("Config portal AP started: SSID '%s'\n", config::kPortalApName);
  Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
  unsigned long last_client_log_ms = 0;
  unsigned long last_ap_watchdog_ms = 0;
  const unsigned long kClientLogIntervalMs = 1000;
  const unsigned long kApWatchdogIntervalMs = 3000;
  Serial.printf("Initial AP clients: %u\n", WiFi.softAPgetStationNum());
  while (s_wm.getConfigPortalActive()) {
    bootButtonPollLongPress();
    
    // If device settings were saved with LAN portal disabled, close the portal
    if (s_stop_portal_requested) {
      Serial.println("[Portal] Device settings saved — closing portal");
      s_stop_portal_requested = false;
      s_wm.stopConfigPortal();
      WiFi.mode(WIFI_STA);
      delay(100);
      statusScreenConnectingBegin("network");
      return false;
    }
    
    // After WiFi credentials are saved, give the STA connection time to complete
    // before WiFiManager decides the portal is done.
    if (s_wifi_save_seen && !wifiLinkUp()) {
      Serial.println("[Portal] Credentials saved — waiting for STA connection...");
      for (int i = 0; i < 50; ++i) {
        if (wifiLinkUp()) {
          Serial.println("[Portal] STA connected after save!");
          break;
        }
        delay(100);
      }
      if (wifiLinkUp()) {
        Serial.println("WiFi connected after save — stopping config portal");
        s_wm.stopConfigPortal();
        WiFi.mode(WIFI_STA);
        delay(100);
        ui::radarDisplayDraw();
        return true;
      }
    }
    
    if (s_wm.process()) {
      Serial.printf("[Portal] process() returned connected (save_seen=%d)\n",
                    s_wifi_save_seen ? 1 : 0);
      Serial.println("WiFiManager reported connected");
      return wifiLinkUp();
    }
    if (wifiLinkUp()) {
      Serial.println("STA link is up; leaving setup portal loop");
      return true;
    }
    const unsigned long now = millis();
    if (now - last_ap_watchdog_ms >= kApWatchdogIntervalMs) {
      last_ap_watchdog_ms = now;
      const wifi_mode_t mode = WiFi.getMode();
      const IPAddress ap_now = WiFi.softAPIP();
      const bool ap_mode_ok = (mode == WIFI_AP || mode == WIFI_AP_STA);
      const bool ap_ip_ok = (ap_now != IPAddress(0, 0, 0, 0));
      if (!ap_mode_ok || !ap_ip_ok) {
        Serial.println("AP watchdog: setup AP appears down, restarting...");
        WiFi.mode(config::kWifiPortalApOnly ? WIFI_AP : WIFI_AP_STA);
        WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                          IPAddress(255, 255, 255, 0));
        if (!WiFi.softAP(config::kPortalApName, "")) {
          Serial.println("AP watchdog: softAP restart failed");
        } else {
          delay(100);
          Serial.printf("AP watchdog: AP restored at %s\n",
                        WiFi.softAPIP().toString().c_str());
        }
      }
    }
    if (now - last_client_log_ms >= kClientLogIntervalMs) {
      last_client_log_ms = now;
      Serial.printf("AP clients: %u\n", WiFi.softAPgetStationNum());
    }
    delay(10);
  }
  Serial.println("[Portal] Config portal loop exited");
  // Clean up portal and return to STA mode
  s_wm.stopConfigPortal();
  WiFi.mode(WIFI_STA);
  delay(100);
  
  // Force display update when portal closes
  if (wifiLinkUp()) {
    Serial.println("[Portal] Connected — showing radar");
    ui::radarDisplayDraw();
  } else {
    Serial.println("[Portal] Not connected — showing connecting screen");
    statusScreenConnectingBegin("network");
  }
  return wifiLinkUp();
}

}  // namespace

bool wifiShowsSetupScreenOnBoot() {
  if (s_force_config_portal) {
    return true;
  }
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  return pending;
}

bool wifiBootButtonPressed() {
  return digitalRead(config::kBootPin) == LOW;
}

void bootButtonInit() { initBootButton(); }

bool bootButtonConsumeTap() {
  portENTER_CRITICAL(&s_boot_mux);
  const bool tap = s_boot_tap_pending;
  if (tap) {
    s_boot_tap_pending = false;
  }
  portEXIT_CRITICAL(&s_boot_mux);
  return tap;
}

void bootButtonPollLongPress() {
  if (wifiBootButtonPressed()) {
    portENTER_CRITICAL(&s_boot_mux);
    if (!s_boot_is_down) {
      s_boot_is_down = true;
      s_boot_down_ms = millis();
    }
    const unsigned long down_ms = s_boot_down_ms;
    portEXIT_CRITICAL(&s_boot_mux);

    if (!s_long_press_handled &&
        millis() - down_ms >= config::kBootResetHoldMs) {
      s_long_press_handled = true;
      Serial.println("BOOT held G�� resetting WiFi");
      wifiResetCredentialsAndReboot();
    }
  } else {
    portENTER_CRITICAL(&s_boot_mux);
    s_boot_is_down = false;
    portEXIT_CRITICAL(&s_boot_mux);
    s_long_press_handled = false;
  }
}

void wifiResetCredentialsAndReboot() {
  resetWifiCredentials();
  statusScreenWifiReset();
  delay(800);
  esp_restart();
}

bool wifiReconnect() {
  initBootButton();
  Serial.println("WiFi reconnecting...");
  const bool ok = connectSavedNetwork(true);
  if (ok && wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("WiFi reconnected: %s  IP %s  RSSI %d\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
  } else {
    Serial.println("WiFi reconnect failed (saved credentials)");
  }
  return ok;
}

bool wifiPortalActive() {
  return s_wm.getWebPortalActive() || s_wm.getConfigPortalActive();
}

void wifiRequestLanPortalOnDemand() {
  s_lan_portal_on_demand_requested = true;
}

void wifiLoop() {
  ensureWifiManager();

  if (s_stop_portal_requested) {
    s_stop_portal_requested = false;
    Serial.println("[Portal] Stopping portal after save (LAN portal disabled)");
    s_lan_portal_grace_deadline_ms = 0;
    s_lan_portal_on_demand_active = false;
    const bool was_link_up = wifiLinkUp();
    stopLanWebPortal();
    if (was_link_up) {
      WiFi.mode(WIFI_STA);
    } else {
      prepareSta();
      WiFi.begin();
    }
    if (wifiLinkUp()) {
      ui::radarDisplayDraw();
    }
  }

  if (s_switch_to_ap_only_wifi_requested) {
    s_switch_to_ap_only_wifi_requested = false;
    Serial.println("[Portal] Switching to AP-only mode for /wifi page");
    stopLanWebPortal();
    WiFi.setSleep(WIFI_PS_NONE);
    WiFi.setAutoReconnect(false);
    // Do not erase saved credentials while transitioning portal mode.
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_AP);
    ensureLanAccessPoint(true);
    s_wm.setConfigPortalBlocking(false);
    s_wm.startConfigPortal(config::kPortalApName);
  }

  if (s_lan_portal_on_demand_requested) {
    s_lan_portal_on_demand_requested = false;
    if (!wifiLinkUp()) {
      Serial.println("[Portal] On-demand request ignored: STA link is down");
    } else {
      if (!s_wm.getWebPortalActive() && !s_wm.getConfigPortalActive()) {
        startLanWebPortal();
      }
      s_lan_portal_on_demand_active =
          s_wm.getWebPortalActive() || s_wm.getConfigPortalActive();
      if (s_lan_portal_on_demand_active) {
        Serial.printf("[Portal] On-demand LAN portal: http://%s\n",
                      WiFi.localIP().toString().c_str());
      }
    }
  }

  const bool link_up = wifiLinkUp();
  s_prev_wifi_link_up = link_up;

  if (link_up) {
    const bool allow_lan_portal =
        ui::radar::lanPortalEnabled() || s_lan_portal_on_demand_active;
    if (allow_lan_portal) {
      if (!s_wm.getWebPortalActive() && !s_wm.getConfigPortalActive()) {
        startLanWebPortal();
      }
    } else if (s_wm.getWebPortalActive() || s_wm.getConfigPortalActive()) {
      stopLanWebPortal();
    }
    if (s_wm.getWebPortalActive() || s_wm.getConfigPortalActive()) {
      bootButtonPollLongPress();
      const unsigned long now = millis();
      if (s_wm.getConfigPortalActive() &&
          now - s_last_lan_ap_watchdog_ms >= 3000) {
        s_last_lan_ap_watchdog_ms = now;
        const IPAddress ap_ip = WiFi.softAPIP();
        const bool ap_ok = (ap_ip != IPAddress(0, 0, 0, 0));
        if (!ap_ok) {
          Serial.println("LAN AP watchdog: AP appears down, restarting AP");
          ensureLanAccessPoint(false);
        }
      }
      s_wm.process();
    }
  } else {
    if (s_wm.getWebPortalActive() || s_wm.getConfigPortalActive()) {
      bootButtonPollLongPress();
      const unsigned long now = millis();
      if (now - s_last_lan_ap_watchdog_ms >= 3000) {
        s_last_lan_ap_watchdog_ms = now;
        const IPAddress ap_ip = WiFi.softAPIP();
        if (ap_ip == IPAddress(0, 0, 0, 0)) {
          Serial.println("LAN AP watchdog: AP down while STA disconnected, restarting AP");
          ensureLanAccessPoint(config::kWifiPortalApOnly);
        }
      }
      if (!s_wm.getConfigPortalActive() && s_wm.getWebPortalActive()) {
        // STA-only portal is unreachable while disconnected.
        s_wm.stopWebPortal();
      }
      s_wm.process();
    }
  }

  const bool portal_active_now = s_wm.getWebPortalActive() || s_wm.getConfigPortalActive();
  if (s_prev_portal_active && !portal_active_now) {
    Serial.printf("[Portal] Portal closed. link_up=%d\n", wifiLinkUp() ? 1 : 0);
    if (wifiLinkUp()) {
      ui::radarDisplayDraw();
    } else {
      statusScreenConnectingBegin("network");
    }
  }
  s_prev_portal_active = portal_active_now;
}

bool wifiSetupConnect() {
  initBootButton();
  ensureWifiManager();

  const bool force_portal = consumeForceConfigPortal();
  WiFi.setAutoReconnect(false);

  if (force_portal) {
    eraseWifiCredentials();
    WiFi.mode(WIFI_OFF);
    delay(100);
  }

  if (force_portal) {
    Serial.println("Opening WiFi setup portal (after reset)");
    if (openConfigPortal() && wifiLinkUp()) {
      Serial.printf("Connected: %s  IP %s\n",
                    WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
      return true;
    }
    Serial.println("WiFi connection failed");
    statusScreenConnectFailed();
    return false;
  }

  Serial.println("Connecting to WiFi (portal opens if needed)...");

  if (wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials() && connectSavedNetwork(true)) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials()) {
    Serial.println("Saved WiFi could not connect G�� opening setup portal");
  } else {
    Serial.println("No saved WiFi G�� opening setup portal");
  }

  if (openConfigPortal() && wifiLinkUp()) {
    Serial.printf("Connected: %s  IP %s\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    return true;
  }
  statusScreenConnectFailed();
  return false;
}

