#ifndef EPAPER_UI_READER_PAGE_H_
#define EPAPER_UI_READER_PAGE_H_

#include <string>
#include <vector>

#include "epaper_ui/bible_page.h"

// Shared full-screen reader rendering. The implementation lives in bible_page.*;
// these aliases give the .txt book reader the exact same pagination language
// (pre-wrapped lines, text sizes, page indicator) without duplicating draw code.
namespace epaper_ui {

using ReaderTextSize = BibleTextSize;
using ReaderPageLine = BiblePageLine;
using ReaderPageState = BiblePageState;

inline int ReaderPageTextWidth(int portrait_width)
{
    return BiblePageTextWidth(portrait_width);
}

inline int ReaderPageLinesPerPage(int portrait_width, int portrait_height, ReaderTextSize size)
{
    return BiblePageLinesPerPage(portrait_width, portrait_height, size);
}

inline std::vector<std::string> WrapReaderText(ReaderTextSize size,
                                               bool emphasis,
                                               const std::string& text,
                                               int max_width)
{
    return WrapBibleText(size, emphasis, text, max_width);
}

inline void DrawReaderPage(uint8_t* framebuffer,
                           int raw_width,
                           int raw_height,
                           int portrait_width,
                           int portrait_height,
                           const ReaderPageState& state,
                           const StatusBarState& status_bar_state)
{
    DrawBiblePage(framebuffer, raw_width, raw_height, portrait_width, portrait_height, state,
                  status_bar_state);
}

}  // namespace epaper_ui

#endif  // EPAPER_UI_READER_PAGE_H_
