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
unsigned long s_lan_portal_grace_deadline_ms = 0;
unsigned long s_last_lan_ap_watchdog_ms = 0;
bool s_switch_to_ap_only_wifi_requested = false;
bool s_stop_portal_requested = false;

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
    Serial.printf("[Portal] Captive probe -> /\n");
    // Redirect captive portal probes to the root so WiFiManager can handle them.
    s_wm.server->sendHeader("Location", "/", true);
    s_wm.server->send(302, "text/plain", "");
  };

  auto serve_root = []() {
    if (s_wm.server == nullptr) {
      return;
    }
    Serial.printf("[Portal] GET / — serving lightweight portal home\n");
    // Detect whether the request came from the AP subnet (192.168.4.x) or LAN.
    // WiFi scan on /wifi disrupts STA momentarily, so hide that link on LAN.
    const bool from_ap =
        s_wm.server->client().remoteIP()[2] == 4;  // 192.168.4.x
    // <link rel='icon' href='data:,'> suppresses the browser's automatic
    // favicon.ico request which would otherwise produce a 404 in the logs.
    static const char kPortalHomeAp[] PROGMEM =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<link rel='icon' href='data:,'>"
        "<title>Plane Radar Setup</title>"
        "<style>body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;"
        "margin:0;padding:18px;background:#f3f6f8;color:#12202b}"
        "h1{font-size:1.2rem;margin:0 0 10px}p{margin:0 0 12px;color:#30495c}"
        "a{display:block;text-decoration:none;margin:10px 0;padding:12px 14px;"
        "border-radius:10px;background:#0e7a70;color:#fff;font-weight:600}"
        "a.secondary{background:#456a86}</style></head><body>"
        "<h1>Plane Radar Setup</h1>"
        "<p>Choose an action:</p>"
        "<a href='/wifi_ap'>Configure Wi-Fi &amp; Settings</a>"
        "<a class='secondary' href='/info'>Status</a>"
        "</body></html>";
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
        "<a class='secondary' href='/info'>Status</a>"
        "<small>To change Wi-Fi network, connect to the<br>"
        "<b>PlaneRadar-Setup</b> AP and open 192.168.4.1</small>"
        "</body></html>";
    s_wm.server->send(200, "text/html",
                      from_ap ? kPortalHomeAp : kPortalHomeLan);
  };

  // Register a lightweight root page. This avoids costly repeated scans from
  // forcing every captive-portal probe directly onto /wifi.
  s_wm.server->on("/", HTTP_GET, serve_root);

  auto switch_to_wifi_ap_only = []() {
    if (s_wm.server == nullptr) {
      return;
    }
    const wifi_mode_t mode = WiFi.getMode();
    if (mode == WIFI_AP) {
      s_wm.server->sendHeader("Location", "/wifi", true);
      s_wm.server->send(302, "text/plain", "");
      return;
    }

    s_switch_to_ap_only_wifi_requested = true;
    static const char kSwitchingPage[] PROGMEM =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<link rel='icon' href='data:,'>"
        "<meta http-equiv='refresh' content='6;url=/wifi'>"
        "<title>Switching Setup Mode</title>"
        "<style>body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;"
        "margin:0;padding:20px;background:#f3f6f8;color:#12202b}"
        "h1{font-size:1.1rem;margin:0 0 8px}p{margin:0 0 8px;color:#30495c}</style>"
        "</head><body><h1>Preparing Wi-Fi Setup</h1>"
        "<p>Switching to AP-only mode for reliable Wi-Fi scan.</p>"
        "<p>If disconnected, reconnect to <b>PlaneRadar-Setup</b> and open "
        "<b>http://192.168.4.1/wifi</b>.</p></body></html>";
    s_wm.server->send(200, "text/html", kSwitchingPage);
  };
  s_wm.server->on("/wifi_ap", HTTP_GET, switch_to_wifi_ap_only);

  // Only register captive portal probe endpoints.
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
  if (!services::location::saveFromStrings(s_param_lat.getValue(),
                                           s_param_lon.getValue())) {
    Serial.println("Invalid lat/lon in portal — keeping previous location");
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
  // Captive DNS redirects create extra request churn on some clients and can
  // destabilize AP sessions on low-memory targets. Keep portal explicit at
  // 192.168.4.1 / plane-radar.local for reliability.
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
  refreshPortalParamDefaults();
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_AP_STA);
  ensureLanAccessPoint(false);
  s_wm.setConfigPortalBlocking(false);
  // Start the config portal in non-blocking mode so WiFiManager brings up
  // both HTTP and DNS handlers for AP/LAN clients.
  s_wm.startConfigPortal(config::kPortalApName);
  Serial.printf("LAN/AP portal running (AP-only=%d). STA IP %s  AP IP %s\n",
                0,
                WiFi.localIP().toString().c_str(),
                WiFi.softAPIP().toString().c_str());
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
    if (attempt > 1) {
      Serial.printf("WiFi connect retry %u/%u\n", attempt,
                    config::kWifiConnectAttempts);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(400);
    }

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
    Serial.println("WiFiManager has no saved SSID; trying SDK-stored STA credentials");
    return tryConnectWithUi(String(), String(), show_ui);
  }
  const String pass = s_wm.getWiFiPass();
  Serial.printf("Trying saved Wi-Fi network '%s'\n", ssid.c_str());
  return tryConnectWithUi(ssid, pass, show_ui);
}

bool openConfigPortal() {
  stopLanWebPortal();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  statusScreenPortal();
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setAutoReconnect(false);
  if (config::kWifiPortalApOnly) {
    WiFi.mode(WIFI_AP);
  } else {
    WiFi.mode(WIFI_AP_STA);
  }
  ensureLanAccessPoint(config::kWifiPortalApOnly);
  Serial.printf("Starting config portal AP '%s' (AP-only=%d)\n",
                config::kPortalApName, config::kWifiPortalApOnly ? 1 : 0);
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
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                      IPAddress(255, 255, 255, 0));
    if (!WiFi.softAP(config::kPortalApName, "")) {
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
    if (s_wm.process()) {
      // WiFiManager indicates STA connect succeeded.
      // Keep AP/portal active; wifiLoop will keep it available during grace period.
      Serial.println("WiFiManager reported connected — keeping AP portal active");
      return wifiLinkUp();
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
      Serial.println("BOOT held — resetting WiFi");
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
  return connectSavedNetwork(true);
}

bool wifiPortalActive() {
  return s_wm.getWebPortalActive() || s_wm.getConfigPortalActive();
}

void wifiLoop() {
  ensureWifiManager();

  if (s_stop_portal_requested) {
    s_stop_portal_requested = false;
    Serial.println("[Portal] Stopping portal after save (LAN portal disabled)");
    s_lan_portal_grace_deadline_ms = 0;
    stopLanWebPortal();
    prepareSta();
    WiFi.begin();
    if (WiFi.status() == WL_CONNECTED) {
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

  const bool link_up = wifiLinkUp();
  if (link_up && !s_prev_wifi_link_up) {
    s_lan_portal_grace_deadline_ms = millis() + kLanPortalGraceMs;
    if (!ui::radar::lanPortalEnabled()) {
      Serial.println("LAN portal grace window active (10 min)");
    }
  }
  s_prev_wifi_link_up = link_up;

  if (link_up) {
    const bool allow_grace_portal = lanPortalGraceActive();
    const bool allow_lan_portal =
        ui::radar::lanPortalEnabled() || allow_grace_portal;
    if (allow_lan_portal) {
      if (!s_wm.getWebPortalActive() && !s_wm.getConfigPortalActive()) {
        startLanWebPortal();
      }
    } else if (s_wm.getWebPortalActive() || s_wm.getConfigPortalActive()) {
      // Keep existing LAN portal running until disconnect/reboot to avoid
      // dropping active browser sessions with ERR_NETWORK_CHANGED.
    }
    if (s_wm.getWebPortalActive() || s_wm.getConfigPortalActive()) {
      bootButtonPollLongPress();
      const unsigned long now = millis();
      if (now - s_last_lan_ap_watchdog_ms >= 3000) {
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
    // Keep AP/web portal alive even if STA link drops to avoid client-side
    // ERR_NETWORK_CHANGED while configuring from AP.
    if (!s_wm.getWebPortalActive() && !s_wm.getConfigPortalActive()) {
      startLanWebPortal();
    }
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
      s_wm.process();
    }
  }
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
      WiFi.setAutoReconnect(true);
      Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
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
    Serial.println("Saved WiFi could not connect — opening setup portal");
  } else {
    Serial.println("No saved WiFi — opening setup portal");
  }

  if (openConfigPortal() && wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("WiFi connection failed");
  statusScreenConnectFailed();
  return false;
}
