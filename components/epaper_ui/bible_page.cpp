#include "epaper_ui/bible_page.h"

#include <algorithm>

#include "render_utils.h"

namespace epaper_ui {
namespace {

constexpr int kMargin = design::spacing::k16;
constexpr int kTitleTopGap = design::spacing::k12;
constexpr int kTitleRuleGap = design::spacing::k8;
constexpr int kRuleThickness = 2;
constexpr int kRuleTextGap = design::spacing::k12;
constexpr int kTextFooterGap = design::spacing::k12;
constexpr int kFooterBottomGap = design::spacing::k16;
constexpr int kLineGap = 4;
constexpr auto kTitleRole = design::TypographyRole::kHeadingH3;
constexpr auto kFooterRole = design::TypographyRole::kLabelSmall;
constexpr auto kMessageRole = design::TypographyRole::kBodyLarge;

design::TypographyRole BodyRole(BibleTextSize size)
{
    switch (size) {
        case BibleTextSize::kSmall:
            return design::TypographyRole::kBody;
        case BibleTextSize::kLarge:
            return design::TypographyRole::kLabelLarge;
        case BibleTextSize::kMedium:
        default:
            return design::TypographyRole::kBodyLarge;
    }
}

// Same point size as BodyRole so emphasized lines keep the page's line pitch.
design::TypographyRole EmphasisRole(BibleTextSize size)
{
    switch (size) {
        case BibleTextSize::kSmall:
            return design::TypographyRole::kLabelSmallBold;
        case BibleTextSize::kLarge:
            return design::TypographyRole::kLabelLargeBold;
        case BibleTextSize::kMedium:
        default:
            return design::TypographyRole::kHeadingH3;
    }
}

struct Layout {
    int title_y = 0;
    int rule_y = 0;
    int text_top = 0;
    int text_bottom = 0;
    int footer_y = 0;
};

Layout BuildLayout(int portrait_height)
{
    Layout layout = {};
    layout.title_y = StatusBarHeight() + kTitleTopGap;
    layout.rule_y = layout.title_y + LineHeight(kTitleRole) + kTitleRuleGap;
    layout.text_top = layout.rule_y + kRuleThickness + kRuleTextGap;
    layout.footer_y = portrait_height - kFooterBottomGap - LineHeight(kFooterRole);
    layout.text_bottom = layout.footer_y - kTextFooterGap;
    return layout;
}

int LinePitch(BibleTextSize size)
{
    return LineHeight(BodyRole(size)) + kLineGap;
}

}  // namespace

int BiblePageTextWidth(int portrait_width)
{
    return std::max(0, portrait_width - (2 * kMargin));
}

int BiblePageLinesPerPage(int portrait_width, int portrait_height, BibleTextSize size)
{
    (void)portrait_width;
    const Layout layout = BuildLayout(portrait_height);
    return std::max(1, (layout.text_bottom - layout.text_top) / std::max(1, LinePitch(size)));
}

std::vector<std::string> WrapBibleText(BibleTextSize size,
                                       bool emphasis,
                                       const std::string& text,
                                       int max_width)
{
    return WrapTextToWidth(emphasis ? EmphasisRole(size) : BodyRole(size), text, max_width);
}

void DrawBiblePage(uint8_t* framebuffer,
                   int raw_width,
                   int raw_height,
                   int portrait_width,
                   int portrait_height,
                   const BiblePageState& state,
                   const StatusBarState& status_bar_state)
{
    if (framebuffer == nullptr) {
        return;
    }

    FillPortraitRect(framebuffer, raw_width, raw_height, portrait_width, portrait_height,
                     {0, 0, portrait_width, portrait_height}, design::color::kWhite);
    DrawStatusBar(framebuffer, raw_width, raw_height, portrait_width, portrait_height,
                  status_bar_state);

    const Layout layout = BuildLayout(portrait_height);
    const int text_width = BiblePageTextWidth(portrait_width);

    DrawTypographyText(framebuffer, raw_width, raw_height, portrait_width, portrait_height,
                       kMargin, layout.title_y, state.title_text, kTitleRole,
                       design::color::kBlack);
    FillPortraitRect(framebuffer, raw_width, raw_height, portrait_width, portrait_height,
                     {kMargin, layout.rule_y, text_width, kRuleThickness}, design::color::kBlack);

    if (!state.message_text.empty()) {
        int cursor_y = layout.text_top;
        for (const std::string& line : WrapTextToWidth(kMessageRole, state.message_text,
                                                       text_width)) {
            if (cursor_y + LineHeight(kMessageRole) > layout.text_bottom) {
                break;
            }
            DrawTypographyText(framebuffer, raw_width, raw_height, portrait_width,
                               portrait_height, kMargin, cursor_y, line, kMessageRole,
                               design::color::kBlack);
            cursor_y += LineHeight(kMessageRole) + kLineGap;
        }
    } else {
        const int pitch = LinePitch(state.text_size);
        int cursor_y = layout.text_top;
        for (const BiblePageLine& line : state.lines) {
            if (cursor_y + LineHeight(BodyRole(state.text_size)) > layout.text_bottom) {
                break;
            }
            if (!line.text.empty()) {
                DrawTypographyText(framebuffer, raw_width, raw_height, portrait_width,
                                   portrait_height, kMargin, cursor_y, line.text,
                                   line.emphasis ? EmphasisRole(state.text_size)
                                                 : BodyRole(state.text_size),
                                   design::color::kBlack);
            }
            cursor_y += pitch;
        }
    }

    if (!state.hint_text.empty()) {
        DrawTypographyText(framebuffer, raw_width, raw_height, portrait_width, portrait_height,
                           kMargin, layout.footer_y, state.hint_text, kFooterRole,
                           design::color::kBlack);
    }
    if (!state.position_text.empty()) {
        const int position_width = MeasureText(kFooterRole, state.position_text);
        DrawTypographyText(framebuffer, raw_width, raw_height, portrait_width, portrait_height,
                           portrait_width - kMargin - position_width, layout.footer_y,
                           state.position_text, kFooterRole, design::color::kBlack);
    }
}

}  // namespace epaper_ui
