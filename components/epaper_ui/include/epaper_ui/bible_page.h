#ifndef EPAPER_UI_BIBLE_PAGE_H_
#define EPAPER_UI_BIBLE_PAGE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "epaper_ui/status_bar.h"

namespace epaper_ui {

enum class BibleTextSize : uint8_t {
    kSmall = 0,
    kMedium,
    kLarge,
};

struct BiblePageLine {
    std::string text = {};
    bool emphasis = false;

    bool operator==(const BiblePageLine& other) const = default;
};

// Full-screen reader: title (book + chapter), one page of pre-wrapped lines, page indicator.
// This is the shared reader render: epaper_ui/reader_page.h aliases these types for the
// .txt book reader so both screens paginate and draw identically.
struct BiblePageState {
    std::string title_text = {};
    std::string position_text = {};
    std::string hint_text = {};
    // Shown instead of lines when there is nothing to read (no SD card, no Bible installed).
    std::string message_text = {};
    BibleTextSize text_size = BibleTextSize::kMedium;
    std::vector<BiblePageLine> lines = {};

    bool operator==(const BiblePageState& other) const = default;
};

int BiblePageTextWidth(int portrait_width);
int BiblePageLinesPerPage(int portrait_width, int portrait_height, BibleTextSize size);
std::vector<std::string> WrapBibleText(BibleTextSize size,
                                       bool emphasis,
                                       const std::string& text,
                                       int max_width);

void DrawBiblePage(uint8_t* framebuffer,
                   int raw_width,
                   int raw_height,
                   int portrait_width,
                   int portrait_height,
                   const BiblePageState& state,
                   const StatusBarState& status_bar_state);

}  // namespace epaper_ui

#endif  // EPAPER_UI_BIBLE_PAGE_H_
