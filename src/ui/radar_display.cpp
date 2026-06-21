#include "ui/radar_display.h"

#include <lgfx/v1/lgfx_fonts.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <time.h>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "services/time_sync.h"
#include "services/timezone_calc.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"
#include "ui/runway_overlay.h"

namespace fonts = lgfx::v1::fonts;

namespace ui {

// Clock display mode: true when showing clock, false when showing radar
static bool s_show_clock_mode = false;

namespace radar {

uint16_t kColorBackground = 0x0000;
uint16_t kColorGrid = 0x0320;
uint16_t kColorLabel = 0xFFFF;
uint16_t kColorCenter = 0xFFFF;
uint16_t kColorAircraft = 0x001F;
uint16_t kColorTrackVector = 0xFFFF;
uint16_t kColorTagType = 0x5DFF;
uint16_t kColorTagAltitude = 0xFFE0;
uint16_t kColorRunway = 0x4D5F;
uint16_t kColorRunwayLabel = 0x7DFF;

}  // namespace radar

namespace {

uint8_t lerpByte(uint8_t a, uint8_t b, float t) {
  return static_cast<uint8_t>(lroundf(a + (b - a) * t));
}

uint16_t rgb565Lerp(uint8_t r1, uint8_t g1, uint8_t b1,
                    uint8_t r2, uint8_t g2, uint8_t b2, float t) {
  const uint8_t r = lerpByte(r1, r2, t);
  const uint8_t g = lerpByte(g1, g2, t);
  const uint8_t b = lerpByte(b1, b2, t);
  // GC9A01 on this board has BGR panel order; swap R/B to match.
  return config::kDisplayRgbOrder ? tft.color565(b, g, r) : tft.color565(r, g, b);
}

uint16_t altitudeColor(int32_t alt_ft) {
  if (alt_ft < 0) {
    return tft.color565(160, 160, 160);
  }

  int32_t agl_ft = alt_ft - services::location::elevationFt();
  if (agl_ft < 0) {
    agl_ft = 0;
  }
  if (agl_ft > config::kAltGradMaxAglFt) {
    agl_ft = config::kAltGradMaxAglFt;
  }

  if (agl_ft <= config::kAltGradWarmStartAglFt) {
    const float t = static_cast<float>(agl_ft) /
                    static_cast<float>(config::kAltGradWarmStartAglFt);
    return rgb565Lerp(0, 64, 255, 0, 255, 255, t);
  }
  if (agl_ft <= config::kAltGradGreenTopAglFt) {
    const float t = static_cast<float>(agl_ft - config::kAltGradWarmStartAglFt) /
                    static_cast<float>(config::kAltGradGreenTopAglFt - config::kAltGradWarmStartAglFt);
    return rgb565Lerp(0, 255, 255, 0, 255, 0, t);
  }
  if (agl_ft <= config::kAltGradYellowTopAglFt) {
    const float t = static_cast<float>(agl_ft - config::kAltGradGreenTopAglFt) /
                    static_cast<float>(config::kAltGradYellowTopAglFt - config::kAltGradGreenTopAglFt);
    return rgb565Lerp(0, 255, 0, 255, 255, 0, t);
  }

  const float t = static_cast<float>(agl_ft - config::kAltGradYellowTopAglFt) /
                  static_cast<float>(config::kAltGradMaxAglFt - config::kAltGradYellowTopAglFt);
  return rgb565Lerp(255, 255, 0, 255, 64, 0, t);
}

bool s_label_metrics_ready = false;
bool s_cardinal_use_vlw = false;
bool s_scale_use_vlw = false;
bool s_clock_use_vlw = false;
float s_cardinal_vlw_size = 0.56f;
float s_scale_vlw_size = 0.50f;
float s_clock_vlw_size = 1.12f;
float s_tag_vlw_size = 0.56f;
const lgfx::GFXfont* s_cardinal_gfx = &fonts::FreeSansBold12pt7b;
const lgfx::GFXfont* s_scale_gfx = &fonts::FreeSansBold9pt7b;
const lgfx::GFXfont* s_clock_gfx = &fonts::FreeSansBold18pt7b;
const lgfx::GFXfont* s_tag_gfx = &fonts::FreeSansBold12pt7b;

bool s_tag_label_metrics_ready = false;
bool s_tag_use_vlw = false;

int s_clock_ac_last_x = radar::kCenterX;
int s_clock_ac_last_y = radar::kCenterY;
bool s_clock_ac_has_last_pos = false;

int s_scale_label_max_w = 0;
int s_scale_label_h = 0;

lgfx::LovyanGFX* s_draw = &tft;
LGFX_Sprite s_frame(&tft);
bool s_frame_ready = false;

class DrawScope {
 public:
  explicit DrawScope(lgfx::LovyanGFX& gfx) : prev_(s_draw) { s_draw = &gfx; }
  ~DrawScope() { s_draw = prev_; }

 private:
  lgfx::LovyanGFX* prev_;
};

int absDiff(int a, int b) { return std::abs(a - b); }

int measureGfxHeight(const lgfx::GFXfont& font) {
  tft.setFont(&font);
  tft.setTextSize(1);
  return tft.fontHeight();
}

int measureVlwHeight(float size) {
  tft.setTextSize(size);
  return tft.fontHeight();
}

float findVlwSizeForHeight(int target_px) {
  float lo = 0.25f;
  float hi = 1.2f;
  for (int i = 0; i < 16; ++i) {
    const float mid = (lo + hi) * 0.5f;
    if (measureVlwHeight(mid) < target_px) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return hi;
}

void applyScaleStyle();

const lgfx::GFXfont* pickGfxFontClosest(
    int target_px, const lgfx::GFXfont* const* candidates, size_t count) {
  const lgfx::GFXfont* best = candidates[0];
  int best_diff = absDiff(measureGfxHeight(*best), target_px);

  for (size_t i = 1; i < count; ++i) {
    const int diff = absDiff(measureGfxHeight(*candidates[i]), target_px);
    if (diff < best_diff) {
      best_diff = diff;
      best = candidates[i];
    }
  }
  return best;
}

void initLabelMetrics() {
  if (s_label_metrics_ready) {
    return;
  }

  const int cardinal_target = radar::kCardinalLabelHeightPx;

  if (displayFontIsSmooth()) {
    s_cardinal_use_vlw = true;
    s_cardinal_vlw_size = findVlwSizeForHeight(cardinal_target);
    const int cardinal_h = measureVlwHeight(s_cardinal_vlw_size);
    const int scale_target = cardinal_h - radar::kScaleBelowCardinalPx;
    s_scale_use_vlw = true;
    s_scale_vlw_size = findVlwSizeForHeight(scale_target);

    const int clock_target = cardinal_target * 3;
    s_clock_use_vlw = true;
    s_clock_vlw_size =
      std::max(findVlwSizeForHeight(clock_target), 1.35f);
  } else {
    const lgfx::GFXfont* cardinal_candidates[] = {&fonts::FreeSansBold12pt7b,
                                                  &fonts::FreeSansBold9pt7b};
    s_cardinal_gfx =
        pickGfxFontClosest(cardinal_target, cardinal_candidates, 2);
    s_cardinal_use_vlw = false;

    const int cardinal_h = measureGfxHeight(*s_cardinal_gfx);
    const int scale_target = cardinal_h - radar::kScaleBelowCardinalPx;
    const lgfx::GFXfont* scale_candidates[] = {&fonts::FreeSansBold9pt7b,
                                               &fonts::FreeSansBold12pt7b};
    s_scale_gfx = pickGfxFontClosest(scale_target, scale_candidates, 2);
    s_scale_use_vlw = false;

    const int clock_target = cardinal_target * 3;
    const lgfx::GFXfont* clock_candidates[] = {&fonts::FreeSansBold24pt7b,
                                               &fonts::FreeSansBold18pt7b,
                                               &fonts::FreeSansBold12pt7b};
    s_clock_gfx = pickGfxFontClosest(clock_target, clock_candidates, 3);
    s_clock_use_vlw = false;
  }

  applyScaleStyle();
  s_scale_label_h = tft.fontHeight();
  s_scale_label_max_w = 0;
  char label[12];
  for (size_t i = 0; i < radar::kRangePresetCount; ++i) {
    for (bool miles : {false, true}) {
      radar::formatRing3Label(label, sizeof(label), radar::kRangePresets[i].ring3_km,
                              miles);
      const int w = tft.textWidth(label);
      if (w > s_scale_label_max_w) {
        s_scale_label_max_w = w;
      }
    }
  }

  s_label_metrics_ready = true;
}

void initTagLabelMetrics() {
  if (s_tag_label_metrics_ready) {
    return;
  }

  const int target = radar::kAircraftTagLabelHeightPx;
  if (displayFontIsSmooth()) {
    s_tag_use_vlw = true;
    s_tag_vlw_size = findVlwSizeForHeight(target);
  } else {
    const lgfx::GFXfont* tag_candidates[] = {&fonts::FreeSansBold12pt7b,
                                               &fonts::FreeSansBold9pt7b};
    s_tag_gfx = pickGfxFontClosest(target, tag_candidates, 2);
    s_tag_use_vlw = false;
  }

  s_tag_label_metrics_ready = true;
}

void initPalette() {
  radar::kColorBackground = tft.color565(radar::kBgR, radar::kBgG, radar::kBgB);
  radar::kColorGrid = tft.color565(radar::kGridR, radar::kGridG, radar::kGridB);
  radar::kColorLabel = tft.color565(255, 255, 255);
  radar::kColorCenter = tft.color565(255, 255, 255);
  // GC9A01 BGR panel: swap R/B in color565 so logical red renders red on screen.
  if (config::kDisplayRgbOrder) {
    radar::kColorAircraft =
        tft.color565(radar::kAircraftB, radar::kAircraftG, radar::kAircraftR);
  } else {
    radar::kColorAircraft =
        tft.color565(radar::kAircraftR, radar::kAircraftG, radar::kAircraftB);
  }
  radar::kColorTrackVector =
      tft.color565(radar::kTrackR, radar::kTrackG, radar::kTrackB);
  radar::kColorTagType =
      tft.color565(radar::kTagTypeR, radar::kTagTypeG, radar::kTagTypeB);
  radar::kColorTagAltitude =
      tft.color565(radar::kTagAltR, radar::kTagAltG, radar::kTagAltB);
  radar::kColorRunway =
      tft.color565(radar::kRunwayR, radar::kRunwayG, radar::kRunwayB);
  radar::kColorRunwayLabel = tft.color565(radar::kRunwayLabelR, radar::kRunwayLabelG,
                                          radar::kRunwayLabelB);
}

constexpr float kKmPerDeg = 111.0f;

void offsetKmFromCenter(float lat, float lon, float* dx_km, float* dy_km,
                        float* dist_km) {
  *dx_km =
      static_cast<float>(lon - services::location::lon()) * kKmPerDeg;
  *dy_km =
      static_cast<float>(lat - services::location::lat()) * kKmPerDeg;
  *dist_km = sqrtf((*dx_km) * (*dx_km) + (*dy_km) * (*dy_km));
}

float innerRingMaxKm() {
  const float outer_km = radar::rangeCurrent().outer_km;
  return outer_km * (static_cast<float>(radar::kGridOuterRadius -
                                       radar::kAircraftInsideRingInsetPx) /
                     static_cast<float>(radar::kGridOuterRadius));
}

/** Flat lat/lon as x/y: 1° ≈ 111 km, north = screen up. */
void latLonToScreen(float lat, float lon, int* out_x, int* out_y) {
  const float outer_km = radar::rangeCurrent().outer_km;
  const float px_per_km = static_cast<float>(radar::kGridOuterRadius) / outer_km;

  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);

  *out_x = radar::kCenterX + static_cast<int>(lroundf(dx_km * px_per_km));
  *out_y = radar::kCenterY - static_cast<int>(lroundf(dy_km * px_per_km));
}

bool isInsideOuterRingKm(float dist_km) { return dist_km <= innerRingMaxKm(); }

int distSqFromCenter(int x, int y) {
  const int dx = x - radar::kCenterX;
  const int dy = y - radar::kCenterY;
  return dx * dx + dy * dy;
}

bool isInsideOuterRing(int x, int y) {
  const int max_r = radar::kGridOuterRadius - radar::kAircraftInsideRingInsetPx;
  return distSqFromCenter(x, y) <= max_r * max_r;
}

/** Rim dot from true bearing; always on screen edge (even if target is 50+ km away). */
bool beyondRingEdgeDotFromLatLon(float lat, float lon, int* out_x, int* out_y) {
  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);
  if (dist_km < 0.01f) {
    return false;
  }
  if (isInsideOuterRingKm(dist_km)) {
    return false;
  }

  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int rim_r = radar::kCenterX - radar::kBeyondRingScreenMarginPx;
  const float angle_rad = atan2f(dx_km, dy_km);

  *out_x = cx + static_cast<int>(lroundf(sinf(angle_rad) * rim_r));
  *out_y = cy - static_cast<int>(lroundf(cosf(angle_rad) * rim_r));
  return true;
}

void drawBeyondRingDot(int x, int y, uint16_t color) {
  s_draw->fillCircle(x, y, radar::kBeyondRingDotRadiusPx, color);
}

void clipPointToOuterRing(int x0, int y0, int* x1, int* y1) {
  const int max_r = radar::kGridOuterRadius;
  const int max_r_sq = max_r * max_r;
  if (distSqFromCenter(*x1, *y1) <= max_r_sq) {
    return;
  }

  const int dx = *x1 - x0;
  const int dy = *y1 - y0;
  float t = 1.0f;
  for (int step = 0; step < 20; ++step) {
    const int px = x0 + static_cast<int>(lroundf(dx * t));
    const int py = y0 + static_cast<int>(lroundf(dy * t));
    if (distSqFromCenter(px, py) <= max_r_sq) {
      *x1 = px;
      *y1 = py;
      return;
    }
    t -= 0.05f;
    if (t <= 0.0f) {
      *x1 = x0;
      *y1 = y0;
      return;
    }
  }
}

int speedLineLengthPx(float gs_knots) {
  if (gs_knots <= 0.0f) {
    return 0;
  }

  constexpr float kNmToKm = 1.852f;
  const float km_per_hour = gs_knots * kNmToKm;
  const float horizon_km =
      km_per_hour * (radar::kAircraftTrackHorizonSec / 3600.0f);
  const float px =
      horizon_km * radar::kGridOuterRadius / radar::kAircraftTrackRefOuterKm *
      radar::kAircraftTrackLengthScale;
  const int len = static_cast<int>(lroundf(px));
  if (len < radar::kAircraftSpeedLineMinPx) {
    return radar::kAircraftSpeedLineMinPx;
  }
  return len;
}

void noseTip(int cx, int cy, float heading_deg, int* tip_x, int* tip_y) {
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = heading_deg * kDegToRad;
  *tip_x = cx + static_cast<int>(lroundf(sinf(rad) * radar::kAircraftNoseLenPx));
  *tip_y = cy - static_cast<int>(lroundf(cosf(rad) * radar::kAircraftNoseLenPx));
}

void drawHeadingTriangle(int cx, int cy, float heading_deg, uint16_t color) {
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = heading_deg * kDegToRad;
  const float sin_h = sinf(rad);
  const float cos_h = cosf(rad);

  int tip_x = 0;
  int tip_y = 0;
  noseTip(cx, cy, heading_deg, &tip_x, &tip_y);

  const int base_x =
      cx - static_cast<int>(lroundf(sin_h * static_cast<float>(radar::kAircraftTailLenPx)));
  const int base_y =
      cy + static_cast<int>(lroundf(cos_h * static_cast<float>(radar::kAircraftTailLenPx)));

  const int wing_x = static_cast<int>(lroundf(cos_h * radar::kAircraftTailHalfPx));
  const int wing_y = static_cast<int>(lroundf(sin_h * radar::kAircraftTailHalfPx));

  s_draw->fillTriangle(tip_x, tip_y, base_x + wing_x, base_y + wing_y,
                       base_x - wing_x, base_y - wing_y, color);
}

void drawSpeedVector(int cx, int cy, float heading_deg, float track_deg,
                     float gs_knots, uint16_t color) {
  const int len = speedLineLengthPx(gs_knots);
  if (len <= 0) {
    return;
  }

  int tip_x = 0;
  int tip_y = 0;
  noseTip(cx, cy, heading_deg, &tip_x, &tip_y);

  constexpr float kDegToRad = 0.01745329252f;
  const float rad = track_deg * kDegToRad;
  int ex = tip_x + static_cast<int>(lroundf(sinf(rad) * len));
  int ey = tip_y - static_cast<int>(lroundf(cosf(rad) * len));
  clipPointToOuterRing(tip_x, tip_y, &ex, &ey);
  if (ex == tip_x && ey == tip_y) {
    return;
  }
  s_draw->drawWideLine(tip_x, tip_y, ex, ey, radar::kAircraftTrackLineHalfWidth,
                       color);
}

void applyTagStyle() {
  if (s_tag_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_tag_vlw_size);
  } else {
    displayFontSetBitmap(*s_draw, s_tag_gfx);
  }
}

int measureTagBlockWidth(const services::adsb::Aircraft& plane) {
  applyTagStyle();
  int max_w = 0;
  if (plane.callsign[0] != '\0') {
    const int w = s_draw->textWidth(plane.callsign);
    if (w > max_w) {
      max_w = w;
    }
  }
  if (plane.type[0] != '\0') {
    const int w = s_draw->textWidth(plane.type);
    if (w > max_w) {
      max_w = w;
    }
  }
  if (plane.alt[0] != '\0') {
    const int w = s_draw->textWidth(plane.alt);
    if (w > max_w) {
      max_w = w;
    }
  }
  return max_w;
}

void drawAircraftTag(int x, int y, const services::adsb::Aircraft& plane) {
  initTagLabelMetrics();
  applyTagStyle();

  const int line_h = s_draw->fontHeight();
  const int block_w = measureTagBlockWidth(plane);
  const int block_h = line_h * 3;
  int ly = y - block_h / 2;

  const int symbol_half =
      radar::kAircraftNoseLenPx + radar::kAircraftTailHalfPx;
  // West (left): tag toward center on the right; east (right): tag on the left.
  const bool tag_on_right = x < radar::kCenterX;
  int anchor_x = 0;
  if (tag_on_right) {
    anchor_x = x + symbol_half + radar::kAircraftLabelGapPx;
    anchor_x = std::min(anchor_x, radar::kSize - block_w - 1);
    s_draw->setTextDatum(textdatum_t::top_left);
  } else {
    anchor_x = x - symbol_half - radar::kAircraftLabelGapPx;
    anchor_x = std::max(anchor_x, block_w + 1);
    s_draw->setTextDatum(textdatum_t::top_right);
  }
  ly = std::max(1, std::min(ly, radar::kSize - block_h - 1));

  if (plane.callsign[0] != '\0') {
    s_draw->setTextColor(radar::kColorLabel, radar::kColorBackground);
    s_draw->drawString(plane.callsign, anchor_x, ly);
  }
  ly += line_h;

  if (plane.type[0] != '\0') {
    s_draw->setTextColor(radar::kColorTagType, radar::kColorBackground);
    s_draw->drawString(plane.type, anchor_x, ly);
  }
  ly += line_h;

  if (plane.alt[0] != '\0') {
    s_draw->setTextColor(altitudeColor(plane.alt_ft), radar::kColorBackground);
    s_draw->drawString(plane.alt, anchor_x, ly);
  }
}

struct AircraftDrawItem {
  size_t index = 0;
  int x = 0;
  int y = 0;
  int dist_sq = 0;
};

struct BeyondDotDrawItem {
  int x = 0;
  int y = 0;
  int dist_sq = 0;
  int32_t alt_ft = -1;
};

// Sort low-altitude first so higher aircraft are drawn on top.
void sortDrawItemsLowAltFirst(AircraftDrawItem* items, size_t count,
                               const services::adsb::Aircraft* planes) {
  for (size_t i = 1; i < count; ++i) {
    const AircraftDrawItem key = items[i];
    const int32_t key_alt =
        (planes[key.index].alt_ft < 0) ? INT32_MIN : planes[key.index].alt_ft;
    size_t j = i;
    while (j > 0) {
      const int32_t prev_alt_raw = planes[items[j - 1].index].alt_ft;
      const int32_t prev_alt = (prev_alt_raw < 0) ? INT32_MIN : prev_alt_raw;
      if (prev_alt <= key_alt) {
        break;
      }
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

void sortBeyondDotsFarFirst(BeyondDotDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const BeyondDotDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

void drawAircraft() {
  initLabelMetrics();

  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();
  const float draw_max_km = radar::fetchRadiusKm();

  AircraftDrawItem items[services::adsb::kMaxAircraft];
  BeyondDotDrawItem dots[services::adsb::kMaxAircraft];
  size_t draw_count = 0;
  size_t dot_count = 0;

  for (size_t i = 0; i < n; ++i) {
    float dx_km = 0.0f;
    float dy_km = 0.0f;
    float dist_km = 0.0f;
    offsetKmFromCenter(planes[i].lat, planes[i].lon, &dx_km, &dy_km, &dist_km);

    // If cached data came from an older, wider fetch range, do not keep
    // rendering those distant targets after the user tightens the range.
    if (dist_km > draw_max_km) {
      continue;
    }

    if (isInsideOuterRingKm(dist_km)) {
      int x = 0;
      int y = 0;
      latLonToScreen(planes[i].lat, planes[i].lon, &x, &y);
      items[draw_count].index = i;
      items[draw_count].x = x;
      items[draw_count].y = y;
      items[draw_count].dist_sq = distSqFromCenter(x, y);
      ++draw_count;
      continue;
    }

    int dot_x = 0;
    int dot_y = 0;
    if (!beyondRingEdgeDotFromLatLon(planes[i].lat, planes[i].lon, &dot_x,
                                     &dot_y)) {
      continue;
    }
    dots[dot_count].x = dot_x;
    dots[dot_count].y = dot_y;
    dots[dot_count].dist_sq = distSqFromCenter(dot_x, dot_y);
    dots[dot_count].alt_ft = planes[i].alt_ft;
    ++dot_count;
  }

  sortBeyondDotsFarFirst(dots, dot_count);
  for (size_t d = 0; d < dot_count; ++d) {
    drawBeyondRingDot(dots[d].x, dots[d].y, altitudeColor(dots[d].alt_ft));
  }

  sortDrawItemsLowAltFirst(items, draw_count, planes);
  for (size_t d = 0; d < draw_count; ++d) {
    const size_t i = items[d].index;
    const int x = items[d].x;
    const int y = items[d].y;
    const uint16_t aircraft_color = altitudeColor(planes[i].alt_ft);
    drawSpeedVector(x, y, planes[i].nose_deg, planes[i].track_deg,
                    planes[i].gs_knots, aircraft_color);
    drawHeadingTriangle(x, y, planes[i].nose_deg, aircraft_color);
  }
  for (size_t d = 0; d < draw_count; ++d) {
    const size_t i = items[d].index;
    drawAircraftTag(items[d].x, items[d].y, planes[i]);
  }
}

void applyCardinalStyle() {
  if (s_cardinal_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_cardinal_vlw_size);
  } else {
    displayFontSetBitmap(*s_draw, s_cardinal_gfx);
  }
}

void applyScaleStyle() {
  if (s_scale_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_scale_vlw_size);
  } else {
    displayFontSetBitmap(*s_draw, s_scale_gfx);
  }
}

void applyClockNumberStyle() {
  if (s_clock_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_clock_vlw_size);
  } else {
    displayFontSetBitmap(*s_draw, s_clock_gfx);
  }
}

void drawCardinalLabel(const char* text, int x, int y, textdatum_t datum) {
  applyCardinalStyle();
  s_draw->setTextDatum(datum);
  s_draw->setTextColor(radar::kColorLabel, radar::kColorBackground);
  s_draw->drawString(text, x, y);
}

void drawScaleLabelWithBackground(const char* text, int x, int y) {
  applyScaleStyle();
  s_draw->setTextDatum(textdatum_t::middle_right);

  const int tw = s_draw->textWidth(text);
  const int th = s_draw->fontHeight();
  constexpr int kPadX = 3;
  constexpr int kPadY = 2;

  const int left = x - tw - kPadX;
  const int top = y - th / 2 - kPadY;

  s_draw->fillRect(left, top, tw + kPadX * 2, th + kPadY * 2,
                   radar::kColorBackground);
  s_draw->setTextColor(radar::kColorGrid, radar::kColorBackground);
  s_draw->drawString(text, x, y);
}

void drawGridRing(int cx, int cy, int r, uint16_t color) {
  if (r <= 0) {
    return;
  }
  const int thickness =
      std::max(1, static_cast<int>(radar::kGridStrokeHalfWidth * 2.0f));
  for (int i = 0; i < thickness && r - i > 0; ++i) {
    s_draw->drawCircle(cx, cy, r - i, color);
  }
}

void drawRings(int cx, int cy, int outer_radius) {
  for (int i = 1; i <= radar::kRingCount; ++i) {
    const int r = (outer_radius * i) / radar::kRingCount;
    drawGridRing(cx, cy, r, radar::kColorGrid);
  }
}

void drawCrosshairs(int cx, int cy, int radius, uint16_t color) {
  s_draw->drawWideLine(cx, cy - radius, cx, cy + radius,
                       radar::kGridStrokeHalfWidth, color);
  s_draw->drawWideLine(cx - radius, cy, cx + radius, cy,
                       radar::kGridStrokeHalfWidth, color);
}

void drawCenterDot(int cx, int cy) {
  s_draw->fillCircle(cx, cy, radar::kCenterDotRadius, radar::kColorCenter);
}

void drawCardinalLabels() {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int edge = radar::kSize - 1;

  drawCardinalLabel("N", cx, radar::kCardinalNorthOffsetY, textdatum_t::top_center);
  drawCardinalLabel("S", cx, edge + radar::kCardinalSouthOffsetY,
                    textdatum_t::bottom_center);
  drawCardinalLabel("W", 0, cy, textdatum_t::middle_left);
  drawCardinalLabel("E", edge, cy, textdatum_t::middle_right);
}

void drawTimeEdgeMarkers(int cx, int cy, int outer_radius) {
  if (radar::clockOnlyModeEnabled()) {
    return;
  }

  int hours = 12;
  int minutes = 0;
  int seconds = 0;
  if (timeSync_isSynced()) {
    const time_t utc_time = timeSync_getUnixTime();
    const time_t local_time = tzCalc_getLocalTimeFromCache(utc_time);
    struct tm* tm_info = localtime(&local_time);
    if (tm_info == nullptr) {
      return;
    }
    hours = tm_info->tm_hour;
    minutes = tm_info->tm_min;
    seconds = tm_info->tm_sec;
  }

  // Clock angles with 12 at top.
  const float hour_deg = (hours % 12 + minutes / 60.0f) * 30.0f;
  const float minute_deg = minutes * 6.0f;
  const float second_deg = seconds * 6.0f;

  const float kPi = 3.14159265359f;
  const auto markerPos = [&](float deg, int* out_x, int* out_y) {
    const float a = (deg - 90.0f) * kPi / 180.0f;
    const int marker_r = outer_radius - 4;
    int x = cx + static_cast<int>(cosf(a) * marker_r);
    int y = cy + static_cast<int>(sinf(a) * marker_r);

    // Keep text safely visible at the panel edge.
    if (x < 8) x = 8;
    if (x > radar::kSize - 9) x = radar::kSize - 9;
    if (y < 8) y = 8;
    if (y > radar::kSize - 9) y = radar::kSize - 9;

    *out_x = x;
    *out_y = y;
  };

  int hx = 0;
  int hy = 0;
  int mx = 0;
  int my = 0;
  markerPos(hour_deg, &hx, &hy);
  markerPos(minute_deg, &mx, &my);

  if (s_cardinal_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_cardinal_vlw_size * 1.5f);
  } else {
    displayFontSetBitmap(*s_draw, &fonts::FreeSansBold18pt7b);
  }
  s_draw->setTextDatum(textdatum_t::middle_center);
  const uint16_t hm_color = config::kDisplayRgbOrder
                                ? tft.color565(255, 160, 80)
                                : tft.color565(80, 160, 255);
  s_draw->setTextColor(hm_color, radar::kColorBackground);
  s_draw->drawString("H", hx, hy);
  s_draw->drawString("M", mx, my);

  // Radar-only mode: show seconds as a single moving dot on the outer dial.
  int sx = 0;
  int sy = 0;
  markerPos(second_deg, &sx, &sy);
    const uint16_t sec_color = tft.color565(255, 255, 255);
  s_draw->fillCircle(sx, sy, radar::kCenterDotRadius, sec_color);
}

int scaleLabelAnchorX(int cx, int outer_radius) {
  return cx + outer_radius - radar::kScaleGapFromOuterRing;
}

void drawScaleLabel(int cx, int cy, int outer_radius) {
  char scale_label[12];
  radar::formatCurrentRing3Label(scale_label, sizeof(scale_label));
  drawScaleLabelWithBackground(scale_label,
                               scaleLabelAnchorX(cx, outer_radius), cy);
}

// Draw a 12-digit clock face with hour/minute/second hands
void drawClockFace(int cx, int cy, int radius, time_t localTime, bool tickSecond,
                   size_t aircraft_count) {
  const DrawScope scope(*s_draw);
  (void)tickSecond;

  auto pointToSegmentDistance = [](float px, float py, float x1, float y1,
                                   float x2, float y2) -> float {
    const float vx = x2 - x1;
    const float vy = y2 - y1;
    const float wx = px - x1;
    const float wy = py - y1;
    const float c1 = wx * vx + wy * vy;
    if (c1 <= 0.0f) {
      const float dx = px - x1;
      const float dy = py - y1;
      return sqrtf(dx * dx + dy * dy);
    }
    const float c2 = vx * vx + vy * vy;
    if (c2 <= c1) {
      const float dx = px - x2;
      const float dy = py - y2;
      return sqrtf(dx * dx + dy * dy);
    }
    const float t = c1 / c2;
    const float proj_x = x1 + t * vx;
    const float proj_y = y1 + t * vy;
    const float dx = px - proj_x;
    const float dy = py - proj_y;
    return sqrtf(dx * dx + dy * dy);
  };
  
  // Extract hours, minutes, seconds from local time
  struct tm *tm_info = localtime(&localTime);
  int hours = tm_info->tm_hour;
  int minutes = tm_info->tm_min;
  int seconds = tm_info->tm_sec;
  
  // Draw clock background circle
  s_draw->fillCircle(cx, cy, radius, radar::kColorBackground);
  s_draw->drawCircle(cx, cy, radius, radar::kColorGrid);
  
  // Draw 12 hour markers and digits
  applyClockNumberStyle();
  s_draw->setTextColor(radar::kColorLabel, radar::kColorBackground);
  s_draw->setTextDatum(textdatum_t::middle_center);
  
  const float kPi = 3.14159265359f;
  for (int i = 1; i <= 12; ++i) {
    const float angle_rad = (i - 3) * kPi / 6.0f;  // -3 to rotate 12 to top
    const int marker_radius = radius - 24;
    const int x = cx + static_cast<int>(cosf(angle_rad) * marker_radius);
    const int y = cy + static_cast<int>(sinf(angle_rad) * marker_radius);
    
    char digit_str[3];
    snprintf(digit_str, sizeof(digit_str), "%d", i);
    s_draw->drawString(digit_str, x, y);
  }
  
  // Draw center dot
  drawCenterDot(cx, cy);
  
  // Calculate hand angles (in radians, with 12 o'clock = -π/2)
  // Hour hand: 360° in 12 hours = 30° per hour, plus minute offset
  float hour_angle = (hours % 12 + minutes / 60.0f) * 30.0f * kPi / 180.0f;
  hour_angle -= kPi / 2.0f;  // Rotate to 12 o'clock position
  
  // Minute hand: 360° in 60 minutes = 6° per minute, plus second offset
  float minute_angle = (minutes + seconds / 60.0f) * 6.0f * kPi / 180.0f;
  minute_angle -= kPi / 2.0f;
  
  // Second hand: 360° in 60 seconds = 6° per second
  float second_angle = seconds * 6.0f * kPi / 180.0f;
  second_angle -= kPi / 2.0f;
  
  // Draw hour hand (short, thick)
  int hour_len = (radius * 4) / 10;
  int hour_x = cx + static_cast<int>(cosf(hour_angle) * hour_len);
  int hour_y = cy + static_cast<int>(sinf(hour_angle) * hour_len);
  s_draw->drawWideLine(cx, cy, hour_x, hour_y, 3.0f, radar::kColorLabel);
  
  // Draw minute hand (medium, thinner)
  int minute_len = (radius * 6) / 10;
  int minute_x = cx + static_cast<int>(cosf(minute_angle) * minute_len);
  int minute_y = cy + static_cast<int>(sinf(minute_angle) * minute_len);
  s_draw->drawWideLine(cx, cy, minute_x, minute_y, 2.0f, radar::kColorLabel);
  
  // Draw second hand (long, thin, red)
  uint16_t second_color = tft.color565(255, 0, 0);  // Red for second hand
  int second_len = (radius * 8) / 10;
  int second_x = cx + static_cast<int>(cosf(second_angle) * second_len);
  int second_y = cy + static_cast<int>(sinf(second_angle) * second_len);
  s_draw->drawWideLine(cx, cy, second_x, second_y, 1.0f, second_color);

  // Show current aircraft count while clock is visible (only when non-zero).
  if (aircraft_count > 0) {
    applyClockNumberStyle();
    s_draw->setTextColor(radar::kColorLabel, radar::kColorBackground);
    s_draw->setTextDatum(textdatum_t::middle_center);
    char count_label[16];
    snprintf(count_label, sizeof(count_label), "AC: %u",
             static_cast<unsigned>(aircraft_count));

    const int label_w = s_draw->textWidth(count_label);
    const int label_h = s_draw->fontHeight();
    const float label_r = 0.5f * sqrtf(static_cast<float>(label_w * label_w +
                                                           label_h * label_h));
    const int marker_radius = radius - 24;
    const float digit_clearance = label_r + (label_h * 0.9f);
    const float hand_clearance = label_r + std::max(6.0f, label_h * 0.55f);

    auto minDistanceToClockDigits = [&](int lx, int ly) -> float {
      float best = 1e9f;
      for (int i = 1; i <= 12; ++i) {
        const float da = (i - 3) * kPi / 6.0f;
        const int dx = cx + static_cast<int>(cosf(da) * marker_radius);
        const int dy = cy + static_cast<int>(sinf(da) * marker_radius);
        const float ddx = static_cast<float>(lx - dx);
        const float ddy = static_cast<float>(ly - dy);
        const float d = sqrtf(ddx * ddx + ddy * ddy);
        if (d < best) {
          best = d;
        }
      }
      return best;
    };

    // Search a dense set of angular positions/radii so AC label stays clear
    // of hands and digits instead of falling back near the bottom center.
    constexpr float kOrbitScales[] = {0.50f, 0.58f, 0.66f};
    int best_x = cx;
    int best_y = cy;
    float best_score = -1.0f;
    bool found_strict = false;

    int relaxed_x = cx;
    int relaxed_y = cy;
    float relaxed_score = -1.0f;

    for (float orbit_scale : kOrbitScales) {
      const float orbit_r = radius * orbit_scale;
      for (int deg = 0; deg < 360; deg += 8) {
        const float a = static_cast<float>(deg) * kPi / 180.0f;
        const int lx = cx + static_cast<int>(cosf(a) * orbit_r);
        const int ly = cy + static_cast<int>(sinf(a) * orbit_r);

        // Ensure label box stays inside the dial area.
        const float edge_dist = sqrtf(static_cast<float>((lx - cx) * (lx - cx) +
                                                          (ly - cy) * (ly - cy)));
        if (edge_dist + label_r > static_cast<float>(radius - 4)) {
          continue;
        }

        const float d_digits = minDistanceToClockDigits(lx, ly);
        const float d_hour = pointToSegmentDistance(static_cast<float>(lx),
                                                    static_cast<float>(ly),
                                                    static_cast<float>(cx),
                                                    static_cast<float>(cy),
                                                    static_cast<float>(hour_x),
                                                    static_cast<float>(hour_y));
        const float d_min = pointToSegmentDistance(static_cast<float>(lx),
                                                   static_cast<float>(ly),
                                                   static_cast<float>(cx),
                                                   static_cast<float>(cy),
                                                   static_cast<float>(minute_x),
                                                   static_cast<float>(minute_y));
        const float d_hands = d_hour < d_min ? d_hour : d_min;
        const float score = d_hands < d_digits ? d_hands : d_digits;

        // Keep a relaxed best candidate in case strict thresholds cannot be met.
        if (score > relaxed_score) {
          relaxed_score = score;
          relaxed_x = lx;
          relaxed_y = ly;
        }

        if (d_digits < digit_clearance || d_hands < hand_clearance) {
          continue;
        }

        if (score > best_score) {
          best_score = score;
          best_x = lx;
          best_y = ly;
          found_strict = true;
        }
      }
    }

    if (!found_strict && relaxed_score >= 0.0f) {
      best_x = relaxed_x;
      best_y = relaxed_y;
      best_score = relaxed_score;
    }

    // Hysteresis: keep previous position unless a new one is significantly better.
    if (s_clock_ac_has_last_pos) {
      const float last_d_hour = pointToSegmentDistance(
          static_cast<float>(s_clock_ac_last_x), static_cast<float>(s_clock_ac_last_y),
          static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(hour_x),
          static_cast<float>(hour_y));
      const float last_d_min = pointToSegmentDistance(
          static_cast<float>(s_clock_ac_last_x), static_cast<float>(s_clock_ac_last_y),
          static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(minute_x),
          static_cast<float>(minute_y));
      const float last_d_hands = last_d_hour < last_d_min ? last_d_hour : last_d_min;
      const float last_d_digits =
          minDistanceToClockDigits(s_clock_ac_last_x, s_clock_ac_last_y);
      const float last_score =
          last_d_hands < last_d_digits ? last_d_hands : last_d_digits;

      constexpr float kSwitchImprovePx = 10.0f;
      if (best_score < (last_score + kSwitchImprovePx)) {
        best_x = s_clock_ac_last_x;
        best_y = s_clock_ac_last_y;
      }
    }

    s_clock_ac_last_x = best_x;
    s_clock_ac_last_y = best_y;
    s_clock_ac_has_last_pos = true;

    s_draw->drawString(count_label, best_x, best_y);
  } else {
    s_clock_ac_has_last_pos = false;
  }
}

template <typename Gfx>
void drawStaticGrid(Gfx& gfx) {
  initLabelMetrics();
  const DrawScope scope(gfx);
  displayFontEnsureLoaded(gfx);
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int grid_r = radar::kGridOuterRadius;

  gfx.fillScreen(radar::kColorBackground);
  drawRings(cx, cy, grid_r);
  drawCrosshairs(cx, cy, grid_r, radar::kColorGrid);
  initPalette();
  runway::drawLargeAirportRunways(gfx);
  drawCenterDot(cx, cy);
  drawCardinalLabels();
  drawScaleLabel(cx, cy, grid_r);
  gfx.setTextDatum(textdatum_t::top_left);
}

bool ensureFrameSprite() {
  if (s_frame_ready) {
    return true;
  }
  s_frame.setColorDepth(16);
  if (!s_frame.createSprite(radar::kSize, radar::kSize)) {
    Serial.println("radar: frame sprite alloc failed");
    return false;
  }
  s_frame_ready = true;
  return true;
}

// Double-buffered frame: composite the grid AND aircraft into the off-screen
// sprite, then blit it to the panel in a single pushSprite. Because the panel
// is updated in one pass, labels never show an erase/redraw gap — no flicker.
void renderFrame() {
  const size_t aircraft_count = services::adsb::aircraftCount();
  const bool radar_only_mode = radar::radarOnlyModeEnabled();
  const bool clock_only_mode = radar::clockOnlyModeEnabled() && !radar_only_mode;
  const uint8_t minute_window_sec = radar::clockMinuteWindowSec();
  const bool time_synced = timeSync_isSynced();
  const time_t local_time = time_synced
                                ? tzCalc_getLocalTimeFromCache(timeSync_getUnixTime())
                                : static_cast<time_t>(12 * 60 * 60);
  struct tm* tm_info = localtime(&local_time);
  const bool minute_clock_window =
      (time_synced && minute_window_sec > 0 && tm_info != nullptr &&
       tm_info->tm_sec < minute_window_sec);
  const bool show_clock = !radar_only_mode &&
                          (clock_only_mode || (aircraft_count == 0) || minute_clock_window);
  
  if (show_clock) {
    // Show clock when no aircraft or during the configured minute window.
    s_show_clock_mode = true;
    
    // Draw clock background
    s_frame.fillScreen(radar::kColorBackground);
    
    // Check if this is a new second for animation
    static time_t last_second = 0;
    bool tick_second = (local_time != last_second);
    if (tick_second) {
      last_second = local_time;
    }
    
    {
      const DrawScope scope(s_frame);
      drawClockFace(radar::kCenterX, radar::kCenterY, radar::kGridOuterRadius, 
                    local_time, tick_second, aircraft_count);
    }
  } else {
    // Show radar when aircraft are present
    s_show_clock_mode = false;
    drawStaticGrid(s_frame);  // opens its own DrawScope(s_frame)
    {
      const DrawScope scope(s_frame);
      drawAircraft();
      drawTimeEdgeMarkers(radar::kCenterX, radar::kCenterY, radar::kGridOuterRadius);
    }
  }
  
  s_frame.pushSprite(0, 0);
  tft.setTextDatum(textdatum_t::top_left);
}

}  // namespace

void radarDisplayDraw() {
  initPalette();
  initLabelMetrics();

  if (ensureFrameSprite()) {
    renderFrame();
    return;
  }

  // Fallback when the sprite can't be allocated: draw straight to the panel.
  const DrawScope scope(tft);
  drawStaticGrid(tft);
  drawAircraft();
  drawTimeEdgeMarkers(radar::kCenterX, radar::kCenterY, radar::kGridOuterRadius);
  tft.setTextDatum(textdatum_t::top_left);
}

void radarDisplayRefreshAircraft() {
  initPalette();

  if (ensureFrameSprite()) {
    renderFrame();
    return;
  }

  radarDisplayDraw();
}

}  // namespace ui
