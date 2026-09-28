#ifndef EPAPER_UI_CLIMATE_STATUS_H_
#define EPAPER_UI_CLIMATE_STATUS_H_

#include <string>

#include "design_tokens.h"
#include "epaper_ui/overlay_geometry.h"

namespace epaper_ui {

// Read-only environment row (SHTC3 temperature/humidity) used by the Settings
// page, modelled on SdStatusState. The row is label left, value right.
struct ClimateStatusState {
    bool available = false;
    std::string temperature_text = {};  // e.g. "23.4 °C"
    std::string humidity_text = {};     // e.g. "41 % RH"
};

struct ClimateStatusStyle {
    int max_width = design::climate_status::kMaxWidth;
    int value_gap = design::climate_status::kValueGap;
};

UiRect ClimateStatusBounds(int origin_x,
                           int origin_y,
                           const ClimateStatusState& state,
                           const ClimateStatusStyle& style);
void DrawClimateStatus(uint8_t* framebuffer,
                       int raw_width,
                       int raw_height,
                       int portrait_width,
                       int portrait_height,
                       int origin_x,
                       int origin_y,
                       const ClimateStatusState& state,
                       const ClimateStatusStyle& style);

}  // namespace epaper_ui

#endif  // EPAPER_UI_CLIMATE_STATUS_H_
