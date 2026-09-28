#include "epaper_ui/climate_status.h"

#include <algorithm>
#include <string>

#include "epaper_ui/font_renderer.h"
#include "render_utils.h"

namespace epaper_ui {
namespace {

constexpr auto kLabelRole = design::TypographyRole::kLabelSmallBlack;
constexpr auto kValueRole = design::TypographyRole::kBody;
constexpr const char* kLabel = "Environment";
constexpr const char* kUnavailableText = "Unavailable";

std::string ResolveValueText(const ClimateStatusState& state)
{
    if (!state.available) {
        return kUnavailableText;
    }

    std::string text = state.temperature_text;
    if (!state.humidity_text.empty()) {
        if (!text.empty()) {
            text += "  ·  ";
        }
        text += state.humidity_text;
    }
    return text.empty() ? std::string(kUnavailableText) : text;
}

}  // namespace

UiRect ClimateStatusBounds(int origin_x,
                           int origin_y,
                           const ClimateStatusState& state,
                           const ClimateStatusStyle& style)
{
    (void)state;
    const int width = ClampPositive(style.max_width);
    const int height = std::max(LineHeight(kLabelRole), LineHeight(kValueRole));
    return {origin_x, origin_y, width, height};
}

void DrawClimateStatus(uint8_t* framebuffer,
                       int raw_width,
                       int raw_height,
                       int portrait_width,
                       int portrait_height,
                       int origin_x,
                       int origin_y,
                       const ClimateStatusState& state,
                       const ClimateStatusStyle& style)
{
    const UiRect bounds = ClimateStatusBounds(origin_x, origin_y, state, style);
    if (bounds.IsEmpty()) {
        return;
    }

    DrawTypographyText(framebuffer, raw_width, raw_height, portrait_width, portrait_height,
                       origin_x, origin_y + (bounds.height - LineHeight(kLabelRole)) / 2, kLabel,
                       kLabelRole, design::color::kBlack);

    const std::string value_text = ResolveValueText(state);
    const int value_width = MeasureText(kValueRole, value_text);
    const int value_x = origin_x + std::max(0, bounds.width - value_width);
    DrawTypographyText(framebuffer, raw_width, raw_height, portrait_width, portrait_height, value_x,
                       origin_y + (bounds.height - LineHeight(kValueRole)) / 2, value_text,
                       kValueRole, design::color::kBlack);
}

}  // namespace epaper_ui
