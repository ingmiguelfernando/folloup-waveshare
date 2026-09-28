#include "book_page_coordinator.h"

#include <algorithm>

#include "display_service.h"

namespace {

constexpr const char* kHintText = "OK: menu (Exit inside)";
constexpr const char* kLoadErrorMessage = "Could not read this book from the SD card.";
constexpr const char* kEmptyMessage = "This book has no text.";

epaper_ui::ReaderTextSize ClampTextSize(int value)
{
    return static_cast<epaper_ui::ReaderTextSize>(std::clamp(value, 0, 2));
}

std::string TitleOf(const std::string& filename)
{
    constexpr const char* kExtension = ".txt";
    if (filename.size() > 4 &&
        filename.compare(filename.size() - 4, 4, kExtension) == 0) {
        return filename.substr(0, filename.size() - 4);
    }
    return filename;
}

}  // namespace

void BookPageCoordinator::Open(const std::string& filename)
{
    book_service::Book loaded = {};
    if (book_service::LoadBook(filename, &loaded) != ESP_OK) {
        available_ = false;
        book_ = {};
        error_text_ = kLoadErrorMessage;
        return;
    }
    error_text_.clear();
    book_ = std::move(loaded);
    available_ = true;
    book_service::SaveLastOpened(filename);

    position_ = book_service::LoadPosition(filename);
    // Position keys are per-book; a page outside the freshly paginated range
    // (book changed size, first open, stale key) clamps to the last page.
    position_.filename = filename;
    text_size_ = ClampTextSize(position_.text_size);
    Paginate();
    position_.page =
        std::clamp(position_.page, 0, std::max(0, static_cast<int>(pages_.size()) - 1));
    SavePosition();
}

std::vector<epaper_ui::ReaderPageLine> BookPageCoordinator::ParagraphLines(
    size_t paragraph_index) const
{
    const book_service::Paragraph& paragraph = book_.paragraphs[paragraph_index];
    if (paragraph.length == 0) {
        // A blank source line becomes the paragraph spacer (same as the Bible's
        // heading spacer), so prose keeps its paragraph rhythm for free.
        return {{.text = {}, .emphasis = false}};
    }

    std::vector<epaper_ui::ReaderPageLine> lines;
    const std::string text(book_.TextOf(paragraph));
    for (std::string& wrapped : epaper_ui::WrapReaderText(text_size_, false, text, text_width_)) {
        lines.push_back({.text = std::move(wrapped), .emphasis = false});
    }
    return lines;
}

void BookPageCoordinator::Paginate()
{
    text_width_ = epaper_ui::ReaderPageTextWidth(display_service::PortraitWidth());
    lines_per_page_ = epaper_ui::ReaderPageLinesPerPage(display_service::PortraitWidth(),
                                                        display_service::PortraitHeight(),
                                                        text_size_);
    pages_.clear();
    bool page_open = false;
    int used = 0;
    for (size_t paragraph = 0; paragraph < book_.paragraphs.size(); ++paragraph) {
        const std::vector<epaper_ui::ReaderPageLine> lines = ParagraphLines(paragraph);
        for (size_t sub = 0; sub < lines.size(); ++sub) {
            if (!page_open) {
                // Never start a page with a spacer line.
                if (lines[sub].text.empty()) {
                    continue;
                }
                pages_.push_back({paragraph, sub});
                page_open = true;
                used = 0;
            }
            if (++used >= lines_per_page_) {
                page_open = false;
            }
        }
    }
    if (pages_.empty()) {
        pages_.push_back({});
    }
}

bool BookPageCoordinator::NextPage()
{
    if (!available_ || position_.page + 1 >= static_cast<int>(pages_.size())) {
        return false;
    }
    ++position_.page;
    SavePosition();
    return true;
}

bool BookPageCoordinator::PreviousPage()
{
    if (!available_ || position_.page <= 0) {
        return false;
    }
    --position_.page;
    SavePosition();
    return true;
}

bool BookPageCoordinator::GoToPage(int page)
{
    if (!available_ || pages_.empty()) {
        return false;
    }
    position_.page = std::clamp(page, 0, static_cast<int>(pages_.size()) - 1);
    SavePosition();
    return true;
}

bool BookPageCoordinator::GoToPercent(int percent)
{
    if (!available_ || pages_.empty()) {
        return false;
    }
    percent = std::clamp(percent, 0, 100);
    const int target = (percent * static_cast<int>(pages_.size())) / 100;
    return GoToPage(std::min(target, static_cast<int>(pages_.size()) - 1));
}

void BookPageCoordinator::SetTextSize(epaper_ui::ReaderTextSize size)
{
    if (size == text_size_) {
        return;
    }
    // Keep roughly the same reading position when the page count changes.
    const size_t anchor_paragraph =
        pages_.empty() ? 0 : pages_[static_cast<size_t>(position_.page)].paragraph;
    text_size_ = size;
    Paginate();
    int page = 0;
    for (size_t index = 0; index < pages_.size(); ++index) {
        if (pages_[index].paragraph <= anchor_paragraph) {
            page = static_cast<int>(index);
        }
    }
    position_.page = page;
    SavePosition();
}

void BookPageCoordinator::SavePosition()
{
    position_.text_size = static_cast<int>(text_size_);
    book_service::SavePosition(position_);
}

epaper_ui::ReaderPageState BookPageCoordinator::BuildState() const
{
    epaper_ui::ReaderPageState state = {};
    state.text_size = text_size_;
    state.title_text = available_ ? TitleOf(book_.filename) : "Book";
    if (!available_) {
        state.message_text = error_text_.empty() ? kLoadErrorMessage : error_text_;
        return state;
    }
    state.hint_text = kHintText;
    if (book_.paragraphs.empty()) {
        state.message_text = kEmptyMessage;
        return state;
    }

    state.position_text =
        std::to_string(position_.page + 1) + "/" + std::to_string(pages_.size());
    const Cursor start = pages_[static_cast<size_t>(position_.page)];
    for (size_t paragraph = start.paragraph;
         paragraph < book_.paragraphs.size() &&
         static_cast<int>(state.lines.size()) < lines_per_page_;
         ++paragraph) {
        const std::vector<epaper_ui::ReaderPageLine> lines = ParagraphLines(paragraph);
        for (size_t sub = paragraph == start.paragraph ? start.sub : 0;
             sub < lines.size() && static_cast<int>(state.lines.size()) < lines_per_page_; ++sub) {
            if (state.lines.empty() && lines[sub].text.empty()) {
                continue;
            }
            state.lines.push_back(lines[sub]);
        }
    }
    return state;
}
