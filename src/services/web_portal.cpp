#include "services/web_portal.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>

#include <cstdio>
#include <cstring>

#include "config.h"
#include "services/radar_location.h"
#include "services/wifi_setup.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"

#ifndef PLANE_RADAR_VERSION
#define PLANE_RADAR_VERSION "dev"
#endif

namespace services::web {

namespace {

WebServer s_server(80);
bool s_mounted = false;
bool s_active = false;
bool s_routes_registered = false;

const char* contentType(const char* path) {
  const size_t len = strlen(path);
  auto ends = [&](const char* ext) {
    const size_t el = strlen(ext);
    return len >= el && strcmp(path + len - el, ext) == 0;
  };
  if (ends(".html")) return "text/html";
  if (ends(".css")) return "text/css";
  if (ends(".js")) return "application/javascript";
  if (ends(".json")) return "application/json";
  return "text/plain";
}

void serveFile(const char* path) {
  if (!s_mounted || !LittleFS.exists(path)) {
    s_server.send(404, "text/plain", "Not found");
    return;
  }
  File f = LittleFS.open(path, "r");
  if (!f) {
    s_server.send(500, "text/plain", "Read error");
    return;
  }
  s_server.streamFile(f, contentType(path));
  f.close();
}

const char* checkboxArg(const JsonDocument& doc, const char* key) {
  return doc[key].as<bool>() ? "on" : "";
}

void handleGetRadar() {
  JsonDocument doc;
  doc["lat"] = services::location::lat();
  doc["lon"] = services::location::lon();
  doc["elevFt"] = services::location::elevationFt();
  doc["rangeIndex"] = ui::radar::rangeIndex();
  doc["useMiles"] = ui::radar::useMiles();
  doc["showRunways"] = ui::radar::showRunways();
  doc["clockWindowSec"] = ui::radar::clockMinuteWindowSec();
  doc["clockOnly"] = ui::radar::clockOnlyModeEnabled();
  doc["radarOnly"] = ui::radar::radarOnlyModeEnabled();

  String out;
  serializeJson(doc, out);
  s_server.send(200, "application/json", out);
}

void handleGetLocations() {
  JsonDocument doc;
  doc["max"] = static_cast<int>(services::location::kMaxSavedLocations);
  const uint8_t active = services::location::activeLocationIndex();
  doc["active"] =
      active == services::location::kNoActiveSlot ? -1 : static_cast<int>(active);

  JsonArray arr = doc["locations"].to<JsonArray>();
  const services::location::SavedLocation* locs = services::location::savedLocations();
  for (uint8_t slot = 0; slot < services::location::kMaxSavedLocations; ++slot) {
    JsonObject o = arr.add<JsonObject>();
    o["slot"] = slot;
    o["valid"] = locs[slot].valid;
    o["name"] = locs[slot].name;
    o["lat"] = locs[slot].lat;
    o["lon"] = locs[slot].lon;
    o["elevFt"] = locs[slot].elev_ft;
    o["active"] = (active == slot);
  }

  String out;
  serializeJson(doc, out);
  s_server.send(200, "application/json", out);
}

void handlePostLocations() {
  JsonDocument doc;
  if (deserializeJson(doc, s_server.arg("plain"))) {
    s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
    return;
  }

  const char* action = doc["action"] | "";
  const uint8_t slot = doc["slot"].as<uint8_t>();

  if (strcmp(action, "use") == 0) {
    if (!services::location::setActiveLocation(slot)) {
      s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid slot\"}");
      return;
    }
  } else if (strcmp(action, "delete") == 0) {
    services::location::deleteLocation(slot);
  } else if (strcmp(action, "save") == 0) {
    char lat_buf[24];
    char lon_buf[24];
    char elev_buf[16];
    snprintf(lat_buf, sizeof(lat_buf), "%.6f", doc["lat"].as<double>());
    snprintf(lon_buf, sizeof(lon_buf), "%.6f", doc["lon"].as<double>());
    snprintf(elev_buf, sizeof(elev_buf), "%ld", static_cast<long>(doc["elevFt"].as<long>()));

    if (!services::location::saveLocation(slot, doc["name"] | "", lat_buf, lon_buf,
                                          elev_buf)) {
      s_server.send(400, "application/json",
                    "{\"ok\":false,\"error\":\"invalid location\"}");
      return;
    }
  } else {
    s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad action\"}");
    return;
  }

  s_server.send(200, "application/json", "{\"ok\":true}");
}

void handlePostRadar() {
  JsonDocument doc;
  if (deserializeJson(doc, s_server.arg("plain"))) {
    s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
    return;
  }

  char lat_buf[24];
  char lon_buf[24];
  char elev_buf[16];
  snprintf(lat_buf, sizeof(lat_buf), "%.6f", doc["lat"].as<double>());
  snprintf(lon_buf, sizeof(lon_buf), "%.6f", doc["lon"].as<double>());
  snprintf(elev_buf, sizeof(elev_buf), "%ld", static_cast<long>(doc["elevFt"].as<long>()));
  if (!services::location::saveFromStrings(lat_buf, lon_buf, elev_buf)) {
    Serial.println("Invalid lat/lon/elev in API - keeping previous location");
  }

  ui::radar::saveMilesFromPortal(checkboxArg(doc, "useMiles"));
  ui::radar::saveRunwaysFromPortal(checkboxArg(doc, "showRunways"));

  char range_buf[8];
  snprintf(range_buf, sizeof(range_buf), "%ld",
           static_cast<long>(doc["rangeIndex"].as<long>()));
  ui::radar::saveRangeIndexFromPortal(range_buf);

  char clock_buf[8];
  snprintf(clock_buf, sizeof(clock_buf), "%ld", static_cast<long>(doc["clockWindowSec"].as<long>()));
  ui::radar::saveClockMinuteWindowSecFromPortal(clock_buf);
  ui::radar::saveClockOnlyModeFromPortal(checkboxArg(doc, "clockOnly"));
  ui::radar::saveRadarOnlyModeFromPortal(checkboxArg(doc, "radarOnly"));

  ui::radarDisplayDraw();
  s_server.send(200, "application/json", "{\"ok\":true}");
}

void handleGetDevice() {
  JsonDocument doc;
  doc["version"] = PLANE_RADAR_VERSION;
  doc["hostname"] = config::kPortalHostname;
  doc["mdns"] = String(config::kPortalHostname) + ".local";
  doc["ip"] = WiFi.localIP().toString();
  doc["ssid"] = WiFi.SSID();
  String out;
  serializeJson(doc, out);
  s_server.send(200, "application/json", out);
}

void handleRestart() {
  s_server.send(200, "application/json", "{\"ok\":true}");
  delay(300);
  esp_restart();
}

void handleWifiReset() {
  s_server.send(200, "application/json", "{\"ok\":true}");
  delay(300);
  wifiEraseCredentialsAndReboot();
}

void handleDeviceReset() {
  s_server.send(200, "application/json", "{\"ok\":true}");
  delay(300);
  wifiResetCredentialsAndReboot();
}

void registerRoutes() {
  s_server.on("/", HTTP_GET, [] { serveFile("/index.html"); });
  s_server.on("/radar", HTTP_GET, [] { serveFile("/radar.html"); });
  s_server.on("/locs", HTTP_GET, [] { serveFile("/locs.html"); });
  s_server.on("/device", HTTP_GET, [] { serveFile("/device.html"); });
  s_server.on("/radar.css", HTTP_GET, [] { serveFile("/radar.css"); });
  s_server.on("/radar.js", HTTP_GET, [] { serveFile("/radar.js"); });
  s_server.on("/locs.js", HTTP_GET, [] { serveFile("/locs.js"); });
  s_server.on("/device.js", HTTP_GET, [] { serveFile("/device.js"); });

  s_server.on("/api/radar", HTTP_GET, handleGetRadar);
  s_server.on("/api/radar", HTTP_POST, handlePostRadar);
  s_server.on("/api/locations", HTTP_GET, handleGetLocations);
  s_server.on("/api/locations", HTTP_POST, handlePostLocations);
  s_server.on("/api/device", HTTP_GET, handleGetDevice);
  s_server.on("/api/restart", HTTP_POST, handleRestart);
  s_server.on("/api/wifi/reset", HTTP_POST, handleWifiReset);
  s_server.on("/api/device/reset", HTTP_POST, handleDeviceReset);
  s_server.onNotFound([] { s_server.send(404, "text/plain", "Not found"); });
}

}  // namespace

bool begin() {
  if (s_mounted) {
    return true;
  }
  if (!LittleFS.begin(false)) {
    Serial.println("LittleFS mount failed - web assets unavailable");
    return false;
  }
  s_mounted = true;
  Serial.println("LittleFS mounted - web assets ready");
  return true;
}

void startStaServer() {
  if (s_active) {
    return;
  }
  if (!s_routes_registered) {
    registerRoutes();
    s_routes_registered = true;
  }
  s_server.begin();
  s_active = true;
}

void stopStaServer() {
  if (!s_active) {
    return;
  }
  s_server.stop();
  s_active = false;
}

void handleClient() {
  if (s_active) {
    s_server.handleClient();
  }
}

bool serverActive() { return s_active; }

}  // namespace services::web
