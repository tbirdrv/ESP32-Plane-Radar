#include "services/wifi_setup.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>

#include <Preferences.h>
#include <esp_system.h>
#include <esp_wifi.h>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include <cmath>
#include <cstdio>

#include "config.h"
#include "services/radar_location.h"
#include "services/web_portal.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

portMUX_TYPE s_boot_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_boot_tap_pending = false;
volatile bool s_boot_double_click_pending = false;
volatile bool s_boot_is_down = false;
volatile unsigned long s_boot_down_ms = 0;
volatile unsigned long s_boot_last_tap_ms = 0;
volatile int s_boot_tap_count = 0;
bool s_long_press_handled = false;
bool s_boot_interrupt_attached = false;

const unsigned long kDoubleClickWindowMs = 500UL;

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
      // Short tap detected
      if (now - s_boot_last_tap_ms < kDoubleClickWindowMs) {
        // Second tap within window = double-click
        s_boot_tap_count++;
        if (s_boot_tap_count >= 2) {
          s_boot_double_click_pending = true;
          s_boot_tap_count = 0;
        }
      } else {
        // First tap or timeout since last tap
        s_boot_tap_count = 1;
      }
      s_boot_last_tap_ms = now;
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

constexpr char kWifiPrefsNamespace[] = "wifi";
constexpr char kPrefsForcePortalKey[] = "portal";

WiFiManager s_wm;
bool s_wm_configured = false;
bool s_force_config_portal = false;
bool s_wifi_save_seen = false;

static const char kPortalHeadScript[] PROGMEM =
  "<script>"
  "document.addEventListener('DOMContentLoaded',function(){"
  "var apply=function(){"
  "var c=document.getElementById('clock_only');"
  "var r=document.getElementById('radar_only');"
  "var w=document.getElementById('clock_window_sec');"
  "var m=document.getElementById('range_miles');"
  "if(!c||!r||!w){return;}"
  "var forLabel=function(id){return document.querySelector('label[for=\\\"'+id+'\\\"]');};"
  "var hideField=function(el){"
  "el.style.display='none';"
  "var l=forLabel(el.id);if(l){l.style.display='none';}"
  "var p=el.closest('label');if(p&&p!==l){p.style.display='none';}"
  "};"
  "hideField(c);hideField(r);"
  "var form=c.form||document.querySelector('form');if(!form){return;}"
  "var ensureRangePreset=function(){"
  "if(!m||document.getElementById('range_miles_select')){return;}"
  "var options=['1','5','10','15','20','25'];"
  "var current=(options.indexOf(m.value)>=0)?m.value:'5';"
  "m.value=current;"
   "var s=document.createElement('select');s.id='range_miles_select';s.style.width='100%';s.style.margin='0 0 10px 0';"
  "options.forEach(function(v){var o=document.createElement('option');o.value=v;o.textContent=v+' mi';s.appendChild(o);});"
  "s.value=current;"
  "s.addEventListener('change',function(){m.value=s.value;});"
  "form.addEventListener('submit',function(){m.value=s.value;});"
   "var p=m.parentNode;if(p){p.insertBefore(s,m.nextSibling);}"
   "m.type='hidden';m.style.display='none';"
  "};"
  "ensureRangePreset();"
  "var hideOriginal=function(el){var l=forLabel(el.id);if(l){l.style.display='none';}el.style.display='none';};"
  "var buildCheckboxRow=function(id,text){"
  "var el=document.getElementById(id);if(!el||document.getElementById(id+'_row')){return;}"
  "hideOriginal(el);"
  "var row=document.createElement('label');row.id=id+'_row';row.style.display='flex';row.style.alignItems='center';row.style.gap='8px';row.style.margin='8px 0 10px 0';"
  "var box=document.createElement('input');box.type='checkbox';box.checked=el.checked;"
  "box.addEventListener('change',function(){el.checked=box.checked;});"
  "row.appendChild(box);row.appendChild(document.createTextNode(text));"
  "if(el.parentNode){el.parentNode.insertBefore(row,el.nextSibling);}"
  "form.addEventListener('submit',function(){el.checked=box.checked;});"
  "};"
  "var buildNumberRow=function(id,text){"
  "var el=document.getElementById(id);if(!el||document.getElementById(id+'_row')){return;}"
  "hideOriginal(el);"
  "var row=document.createElement('div');row.id=id+'_row';row.style.margin='8px 0 10px 0';"
  "var lab=document.createElement('div');lab.textContent=text;lab.style.fontWeight='600';lab.style.margin='4px 0';"
  "var input=document.createElement('input');input.type='number';input.min='0';input.max='59';input.step='1';input.value=el.value;input.style.width='100%';"
  "input.addEventListener('change',function(){el.value=input.value;});"
  "row.appendChild(lab);row.appendChild(input);"
  "if(el.parentNode){el.parentNode.insertBefore(row,el.nextSibling);}"
  "form.addEventListener('submit',function(){el.value=input.value;});"
  "return input;"
  "};"
  "buildCheckboxRow('show_ground','Show aircraft on the ground');"
  "var cwInput=buildNumberRow('clock_window_sec','Clock at each minute (sec)');"
  "var group=document.getElementById('display_mode_group');"
  "if(!group){"
  "group=document.createElement('div');group.id='display_mode_group';group.style.margin='8px 0 10px 0';"
  "var title=document.createElement('div');title.textContent='Display mode';title.style.fontWeight='600';title.style.margin='4px 0';group.appendChild(title);"
  "var mk=function(v,t){var lab=document.createElement('label');lab.style.display='block';lab.style.margin='2px 0';var i=document.createElement('input');i.type='radio';i.name='display_mode';i.value=v;i.style.marginRight='6px';lab.appendChild(i);lab.appendChild(document.createTextNode(t));group.appendChild(lab);return i;};"
  "var autoOpt=mk('auto','Auto');"
  "var clockOpt=mk('clock','Clock only');"
  "var radarOpt=mk('radar','Radar only');"
  "if(c.checked){clockOpt.checked=true;}else if(r.checked){radarOpt.checked=true;}else{autoOpt.checked=true;}"
  "var wLabel=forLabel('clock_window_sec');"
  "var sync=function(){"
  "var s=form.querySelector('input[name=display_mode]:checked');"
  "var mode=s?s.value:'auto';"
  "c.checked=(mode==='clock');"
  "r.checked=(mode==='radar');"
  "var dis=(mode!=='auto');"
  "if(cwInput){cwInput.disabled=dis;var cwRow=cwInput.parentNode;if(cwRow){cwRow.style.opacity=dis?'0.45':'1';}cwInput.style.opacity=dis?'0.45':'1';}"
  "};"
  "group.addEventListener('change',sync);"
  "form.addEventListener('submit',sync);"
  "form.insertBefore(group,w);"
  "sync();"
  "}"
  "};"
  "apply();setTimeout(apply,250);"
  "});"
  "</script>";

constexpr int kCoordParamLen = 20;
constexpr int kElevParamLen = 8;
constexpr int kRangeParamLen = 4;
constexpr int kClockWindowParamLen = 4;
constexpr float kKmPerMile = 1.609344f;

constexpr char kCoordInputAttrs[] = " type=\"number\" step=\"0.000001\"";
constexpr char kElevInputAttrs[] =
  " type=\"number\" min=\"-1500\" max=\"30000\" step=\"1\"";
constexpr char kRangeInputAttrs[] =
  " type=\"number\" min=\"1\" max=\"25\" step=\"1\"";
constexpr char kClockWindowInputAttrs[] =
  " type=\"number\" min=\"0\" max=\"59\" step=\"1\"";

WiFiManagerParameter s_param_lat("radar_lat", "Latitude (deg)", "0",
                 kCoordParamLen, kCoordInputAttrs);
WiFiManagerParameter s_param_lon("radar_lon", "Longitude (deg)", "0",
                 kCoordParamLen, kCoordInputAttrs);
WiFiManagerParameter s_param_elev("field_elev_ft", "Field elevation (ft)",
                  "0", kElevParamLen, kElevInputAttrs);
WiFiManagerParameter s_param_range("range_miles", "Initial range (mi)", "5",
                   kRangeParamLen, kRangeInputAttrs);

char s_miles_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_miles("use_miles", "Display distances in miles", "T", 2,
                                   s_miles_checkbox_attrs, WFM_LABEL_AFTER);

char s_runways_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_runways("show_runways", "Show airport runways", "T", 2,
                                     s_runways_checkbox_attrs, WFM_LABEL_AFTER);

char s_ground_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_ground("show_ground", "Show aircraft on the ground", "T", 2,
                                    s_ground_checkbox_attrs, WFM_LABEL_AFTER);

WiFiManagerParameter s_param_clock_window("clock_window_sec",
                                          "Clock at each minute (sec)", "10",
                                          kClockWindowParamLen,
                                          kClockWindowInputAttrs);

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

bool wifiLinkUp() {
  return WiFi.status() == WL_CONNECTED &&
         WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void refreshPortalParamDefaults() {
  char lat_buf[kCoordParamLen + 1];
  char lon_buf[kCoordParamLen + 1];
  char elev_buf[kElevParamLen + 1];
  char range_buf[kRangeParamLen + 1];
  char clock_buf[kClockWindowParamLen + 1];

  snprintf(lat_buf, sizeof(lat_buf), "%.6f", services::location::lat());
  snprintf(lon_buf, sizeof(lon_buf), "%.6f", services::location::lon());
  snprintf(elev_buf, sizeof(elev_buf), "%ld",
           static_cast<long>(services::location::elevationFt()));
  snprintf(range_buf, sizeof(range_buf), "%u",
           static_cast<unsigned>(lroundf(ui::radar::rangeCurrent().ring3_km /
                                         kKmPerMile)));
  snprintf(clock_buf, sizeof(clock_buf), "%u",
           static_cast<unsigned>(ui::radar::clockMinuteWindowSec()));

  s_param_lat.setValue(lat_buf, kCoordParamLen);
  s_param_lon.setValue(lon_buf, kCoordParamLen);
  s_param_elev.setValue(elev_buf, kElevParamLen);
  s_param_range.setValue(range_buf, kRangeParamLen);
  s_param_clock_window.setValue(clock_buf, kClockWindowParamLen);

  snprintf(s_miles_checkbox_attrs, sizeof(s_miles_checkbox_attrs),
           "type=\"checkbox\"%s", ui::radar::useMiles() ? " checked" : "");
  s_param_miles.setValue("T", 2);

  snprintf(s_runways_checkbox_attrs, sizeof(s_runways_checkbox_attrs),
           "type=\"checkbox\"%s", ui::radar::showRunways() ? " checked" : "");
  s_param_runways.setValue("T", 2);

  snprintf(s_ground_checkbox_attrs, sizeof(s_ground_checkbox_attrs),
           "type=\"checkbox\"%s",
           ui::radar::showGroundAircraft() ? " checked" : "");
  s_param_ground.setValue("T", 2);

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
                                           s_param_lon.getValue(),
                                           s_param_elev.getValue())) {
    Serial.println("Invalid lat/lon/elevation in setup portal; keeping previous values");
  }

  ui::radar::saveRangeMilesFromPortal(s_param_range.getValue());
  ui::radar::saveMilesFromPortal(s_param_miles.getValue());
  ui::radar::saveRunwaysFromPortal(s_param_runways.getValue());
  ui::radar::saveShowGroundAircraftFromPortal(s_param_ground.getValue());
  ui::radar::saveClockMinuteWindowSecFromPortal(s_param_clock_window.getValue());

  bool clock_only = portalOptionChecked(s_param_clock_only.getValue());
  bool radar_only = portalOptionChecked(s_param_radar_only.getValue());
  if (clock_only && radar_only) {
    clock_only = false;
  }
  ui::radar::saveClockOnlyModeFromPortal(clock_only ? "T" : "");
  ui::radar::saveRadarOnlyModeFromPortal(radar_only ? "T" : "");
}

void onPortalWifiSaved() {
  s_wifi_save_seen = true;
  Serial.println("[Portal] Save WiFi callback fired");
}

void attachPortalParams(WiFiManager& wm) {
  refreshPortalParamDefaults();
  wm.addParameter(&s_param_lat);
  wm.addParameter(&s_param_lon);
  wm.addParameter(&s_param_elev);
  wm.addParameter(&s_param_range);
  wm.addParameter(&s_param_miles);
  wm.addParameter(&s_param_runways);
  wm.addParameter(&s_param_ground);
  wm.addParameter(&s_param_clock_window);
  wm.addParameter(&s_param_clock_only);
  wm.addParameter(&s_param_radar_only);
  wm.setSaveParamsCallback(onPortalParamsSaved);
  wm.setSaveConfigCallback(onPortalWifiSaved);
}

void onConfigPortalApStarted(WiFiManager*) {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
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

void ensureWifiManager() {
  if (s_wm_configured) {
    return;
  }
  s_wm.setConfigPortalTimeout(config::kWifiPortalTimeoutSec);
  s_wm.setMinimumSignalQuality(config::kWifiScanMinQualityPct);
  s_wm.setAPStaticIPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                           IPAddress(255, 255, 255, 0));
  s_wm.setHostname(config::kPortalHostname);
  s_wm.setCustomHeadElement(kPortalHeadScript);
  s_wm.setAPCallback(onConfigPortalApStarted);
  attachPortalParams(s_wm);
  s_wm_configured = true;
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

void prepareSta() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(config::kPortalHostname);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
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
    return false;
  }

  ensureWifiManager();
  const String ssid = s_wm.getWiFiSSID();
  if (ssid.length() == 0) {
    return false;
  }
  const String pass = s_wm.getWiFiPass();
  return tryConnectWithUi(ssid, pass, show_ui);
}

void startLanWebPortal() {
  if (!wifiLinkUp() || services::web::serverActive() || s_wm.getConfigPortalActive()) {
    return;
  }
  refreshPortalParamDefaults();
  WiFi.mode(WIFI_STA);
#ifdef WM_MDNS
  MDNS.end();
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
  }
#endif
  services::web::startStaServer();
  Serial.printf("LAN config: http://%s.local or http://%s\n",
                config::kPortalHostname, WiFi.localIP().toString().c_str());
}

void stopLanWebPortal() {
  if (services::web::serverActive()) {
    services::web::stopStaServer();
  }
#ifdef WM_MDNS
  MDNS.end();
#endif
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
  WiFi.mode(config::kWifiPortalApOnly ? WIFI_AP : WIFI_AP_STA);

  s_wm.setConfigPortalBlocking(false);
  if (!s_wm.startConfigPortal(config::kPortalApName)) {
    return false;
  }

  while (s_wm.getConfigPortalActive()) {
    bootButtonPollLongPress();
    if (s_wm.process() && wifiLinkUp()) {
      return true;
    }
    if (s_wifi_save_seen && wifiLinkUp()) {
      return true;
    }
    delay(10);
  }

  return wifiLinkUp();
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

bool bootButtonConsumeDoubleClick() {
  portENTER_CRITICAL(&s_boot_mux);
  const bool double_click = s_boot_double_click_pending;
  if (double_click) {
    s_boot_double_click_pending = false;
  }
  portEXIT_CRITICAL(&s_boot_mux);
  return double_click;
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
      Serial.println("BOOT held - resetting WiFi");
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

void wifiEraseCredentialsAndReboot() {
  markForceConfigPortal();
  eraseWifiCredentials();
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

bool wifiPortalActive() { return s_wm.getConfigPortalActive(); }

void wifiLoop() {
  ensureWifiManager();

  if (s_wm.getConfigPortalActive()) {
    bootButtonPollLongPress();
    s_wm.process();
    return;
  }

  if (wifiLinkUp()) {
    if (!services::web::serverActive()) {
      startLanWebPortal();
    }
    if (services::web::serverActive()) {
      services::web::handleClient();
    }
  } else {
    if (services::web::serverActive()) {
      services::web::stopStaServer();
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
    Serial.println("Saved WiFi could not connect - opening setup portal");
  } else {
    Serial.println("No saved WiFi - opening setup portal");
  }

  if (openConfigPortal() && wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  statusScreenConnectFailed();
  return false;
}
