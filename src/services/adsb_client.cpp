#include "services/adsb_client.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#if defined(ESP32)
#include <esp_heap_caps.h>
#endif

#include <ArduinoJson.h>

#include <cstring>

#include "config.h"

namespace services::adsb {

namespace {

constexpr char kApiBase[] = "https://opendata.adsb.fi/api/v3/lat/";
constexpr float kKmPerNm = 1.852f;
// Keep failed HTTPS attempts short so the render loop stays responsive.
constexpr int kConnectAttemptMs = 1200;
constexpr unsigned long kRequestTimeoutMs = 2500;
constexpr uint8_t kRequestRetryCount = 1;
constexpr unsigned long kRetryDelayMs = 250;
constexpr int kTlsHandshakeTimeoutSec = 2;
// Skip TLS entirely if free heap is below this threshold to avoid alloc failures.
constexpr uint32_t kMinHeapForTlsBytes = 55000;
// TLS on ESP32 also needs a sufficiently large contiguous block.
constexpr uint32_t kMinLargestBlockForTlsBytes = 32000;

Aircraft s_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
PollFn s_poll_fn = nullptr;
unsigned long s_next_fetch_allowed_ms = 0;
uint8_t s_consecutive_fetch_failures = 0;

void pollNetwork() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

void registerFetchFailure() {
  if (s_consecutive_fetch_failures < 6) {
    ++s_consecutive_fetch_failures;
  }
  unsigned long backoff_ms = 1000UL << s_consecutive_fetch_failures;
  if (backoff_ms > 30000UL) {
    backoff_ms = 30000UL;
  }
  s_next_fetch_allowed_ms = millis() + backoff_ms;
}

void registerFetchSuccess() {
  s_consecutive_fetch_failures = 0;
  s_next_fetch_allowed_ms = 0;
}

void printPayloadPreview(const String& payload) {
  const size_t max_preview = 160;
  const size_t n = payload.length() < max_preview ? payload.length() : max_preview;
  String preview = payload.substring(0, n);
  preview.replace('\n', ' ');
  preview.replace('\r', ' ');
  Serial.printf("adsb: payload preview (%u bytes): %s\n",
                static_cast<unsigned>(n), preview.c_str());
}

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickTrackHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "tas", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "ias", &v)) {
    return v;
  }
  return 0.0f;
}

bool isOnGround(const JsonObject& plane) {
  if (!plane["alt_baro"].is<const char*>()) {
    return false;
  }
  return strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

int32_t pickAltitudeFeet(const JsonObject& plane) {
  if (isOnGround(plane)) {
    return -1;
  }

  float alt = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &alt) ||
      readJsonFloat(plane, "alt_geom", &alt)) {
    return static_cast<int32_t>(lroundf(alt));
  }
  return -1;
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  out[0] = '\0';
  if (out_len == 0 || !obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

void formatAltitudeTag(const JsonObject& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }

  if (plane["alt_baro"].is<const char*>()) {
    const char* s = plane["alt_baro"].as<const char*>();
    if (strcmp(s, "ground") == 0) {
      strncpy(out, "GND", out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
  }

  float alt = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &alt) ||
      readJsonFloat(plane, "alt_geom", &alt)) {
    snprintf(out, out_len, "%d ft", static_cast<int>(lroundf(alt)));
  }
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  if (ac->callsign[0] == '\0') {
    copyJsonStringTrimmed(plane, "hex", ac->callsign, sizeof(ac->callsign));
  }

  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  formatAltitudeTag(plane, ac->alt, sizeof(ac->alt));
  ac->alt_ft = pickAltitudeFeet(plane);
}

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList() { return s_aircraft; }

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  const unsigned long now = millis();
  if (s_next_fetch_allowed_ms != 0 &&
      static_cast<long>(now - s_next_fetch_allowed_ms) < 0) {
    return false;
  }

  const float dist_nm = kmToNauticalMiles(fetch_radius_km);

  String url = kApiBase;
  url += String(center_lat, 6);
  url += "/lon/";
  url += String(center_lon, 6);
  url += "/dist/";
  url += String(dist_nm, 1);
  JsonDocument doc;
  bool parsed_ok = false;
  int last_code = HTTPC_ERROR_CONNECTION_REFUSED;
  for (uint8_t attempt = 1; attempt <= kRequestRetryCount; ++attempt) {
    pollNetwork();

    if (WiFi.status() != WL_CONNECTED) {
      last_code = HTTPC_ERROR_CONNECTION_LOST;
      if (attempt < kRequestRetryCount) {
        delay(kRetryDelayMs);
      }
      continue;
    }
    if (ESP.getFreeHeap() < kMinHeapForTlsBytes) {
      Serial.printf("adsb: skipping TLS — low heap (%u bytes)\n", ESP.getFreeHeap());
      last_code = HTTPC_ERROR_CONNECTION_REFUSED;
      break;  // no point retrying, heap won't recover mid-loop
    }

#if defined(ESP32)
    const uint32_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largest_block < kMinLargestBlockForTlsBytes) {
      Serial.printf("adsb: skipping TLS — fragmented heap (largest=%u free=%u)\n",
                    largest_block, ESP.getFreeHeap());
      last_code = HTTPC_ERROR_CONNECTION_REFUSED;
      break;
    }
#endif

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(kRequestTimeoutMs);
    client.setHandshakeTimeout(kTlsHandshakeTimeoutSec);

    HTTPClient http;
    if (!http.begin(client, url)) {
      last_code = HTTPC_ERROR_CONNECTION_REFUSED;
      if (attempt < kRequestRetryCount) {
        delay(kRetryDelayMs);
      }
      continue;
    }

    http.addHeader("Accept", "application/json");
    http.addHeader("Accept-Encoding", "identity");
    http.setUserAgent("ESP32-Plane-Radar/1.0");
    http.setReuse(false);
    http.setConnectTimeout(kConnectAttemptMs);
    http.setTimeout(kRequestTimeoutMs);
    const int code = http.GET();
    last_code = code;
    if (code == HTTP_CODE_OK) {
      JsonDocument filter;
      filter["ac"][0]["lat"] = true;
      filter["ac"][0]["lon"] = true;
      filter["ac"][0]["true_heading"] = true;
      filter["ac"][0]["mag_heading"] = true;
      filter["ac"][0]["track"] = true;
      filter["ac"][0]["dir"] = true;
      filter["ac"][0]["gs"] = true;
      filter["ac"][0]["tas"] = true;
      filter["ac"][0]["ias"] = true;
      filter["ac"][0]["alt_baro"] = true;
      filter["ac"][0]["alt_geom"] = true;
      filter["ac"][0]["flight"] = true;
      filter["ac"][0]["hex"] = true;
      filter["ac"][0]["t"] = true;
      doc.clear();
      const DeserializationError err =
          deserializeJson(doc, *http.getStreamPtr(),
                          DeserializationOption::Filter(filter));
      http.end();
      if (!err) {
        parsed_ok = true;
        break;
      }
      Serial.printf("adsb: JSON parse error: %s\n", err.c_str());
      last_code = HTTPC_ERROR_READ_TIMEOUT;
    } else {
      http.end();
    }

    if (attempt < kRequestRetryCount) {
      delay(kRetryDelayMs);
    }
  }

  if (!parsed_ok) {
    Serial.printf("adsb: HTTP %d\n", last_code);
    registerFetchFailure();
    return false;
  }

  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull()) {
    s_aircraft_count = 0;
    return true;
  }

  size_t n = 0;
  for (JsonObject plane : ac) {
    if (n >= kMaxAircraft) {
      break;
    }
    if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) {
      continue;
    }
    if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) {
      continue;
    }

    s_aircraft[n].lat = plane["lat"].as<float>();
    s_aircraft[n].lon = plane["lon"].as<float>();
    s_aircraft[n].nose_deg = pickNoseHeading(plane);
    s_aircraft[n].track_deg = pickTrackHeading(plane);
    s_aircraft[n].gs_knots = pickGroundSpeed(plane);
    s_aircraft[n].alt_ft = -1;
    fillTagFields(&s_aircraft[n], plane);
    ++n;
  }

  s_aircraft_count = n;
  registerFetchSuccess();
  Serial.printf("adsb: %u aircraft\n", static_cast<unsigned>(n));
  return true;
}

}  // namespace services::adsb
