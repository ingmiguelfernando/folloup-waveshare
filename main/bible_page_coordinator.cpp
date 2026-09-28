#include "bible_page_coordinator.h"

#include <algorithm>

#include "display_service.h"

namespace {

constexpr const char* kNoBibleMessage =
    "No Bible found on the SD card. Generate the bible folder with "
    "scripts/bible_json_to_sd.py and copy it to the root of the card.";
constexpr const char* kLoadErrorMessage = "Could not read this chapter from the SD card.";
constexpr const char* kHintText = "OK: menu";

epaper_ui::BibleTextSize ClampTextSize(int value)
{
    return static_cast<epaper_ui::BibleTextSize>(std::clamp(value, 0, 2));
}

}  // namespace

void BiblePageCoordinator::Open()
{
    if (!available_) {
        available_ = bible_service::Load() == ESP_OK;
        if (!available_) {
            return;
        }
        books_ = bible_service::Books();
        position_ = bible_service::LoadPosition();
        text_size_ = ClampTextSize(position_.text_size);
    }

    const int saved_page = position_.page;
    const int book = std::clamp(position_.book_index, 0, static_cast<int>(books_.size()) - 1);
    const int max_chapter = books_[static_cast<size_t>(book)].chapter_count;
    if (chapter_.book_index != book || chapter_.chapter != position_.chapter) {
        if (LoadChapter(book, std::clamp(position_.chapter, 1, max_chapter), false)) {
            position_.page = std::clamp(saved_page, 0, static_cast<int>(pages_.size()) - 1);
        }
    }
}

bool BiblePageCoordinator::LoadChapter(int book_index, int chapter, bool last_page)
{
    bible_service::Chapter loaded = {};
    if (bible_service::LoadChapter(book_index, chapter, &loaded) != ESP_OK) {
        error_text_ = kLoadErrorMessage;
        return false;
    }
    error_text_.clear();
    chapter_ = std::move(loaded);
    position_.book_index = book_index;
    position_.chapter = chapter;
    Paginate();
    position_.page = last_page ? static_cast<int>(pages_.size()) - 1 : 0;
    SavePosition();
    return true;
}

std::vector<epaper_ui::BiblePageLine> BiblePageCoordinator::ItemLines(size_t item_index) const
{
    std::vector<epaper_ui::BiblePageLine> lines;
    const bible_service::Item& item = chapter_.items[item_index];
    const bool heading = item.kind == bible_service::ItemKind::kHeading;
    if (heading && item_index > 0) {
        lines.push_back({});
    }
    std::string text = heading ? std::string(chapter_.TextOf(item))
                               : item.label + " " + std::string(chapter_.TextOf(item));
    for (std::string& wrapped : epaper_ui::WrapBibleText(text_size_, heading, text, text_width_)) {
        lines.push_back({.text = std::move(wrapped), .emphasis = heading});
    }
    return lines;
}

void BiblePageCoordinator::Paginate()
{
    text_width_ = epaper_ui::BiblePageTextWidth(display_service::PortraitWidth());
    lines_per_page_ = epaper_ui::BiblePageLinesPerPage(display_service::PortraitWidth(),
                                                       display_service::PortraitHeight(),
                                                       text_size_);
    pages_.clear();
    bool page_open = false;
    int used = 0;
    for (size_t item = 0; item < chapter_.items.size(); ++item) {
        const std::vector<epaper_ui::BiblePageLine> lines = ItemLines(item);
        for (size_t sub = 0; sub < lines.size(); ++sub) {
            if (!page_open) {
                // Never start a page with the spacer line above a heading.
                if (lines[sub].text.empty()) {
                    continue;
                }
                pages_.push_back({item, sub});
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

bool BiblePageCoordinator::NextPage()
{
    if (!available_) {
        return false;
    }
    if (position_.page + 1 < static_cast<int>(pages_.size())) {
        ++position_.page;
        SavePosition();
        return true;
    }
    return NextChapter();
}

bool BiblePageCoordinator::PreviousPage()
{
    if (!available_) {
        return false;
    }
    if (position_.page > 0) {
        --position_.page;
        SavePosition();
        return true;
    }
    if (position_.chapter > 1) {
        return LoadChapter(position_.book_index, position_.chapter - 1, true);
    }
    if (position_.book_index > 0) {
        const int book = position_.book_index - 1;
        return LoadChapter(book, books_[static_cast<size_t>(book)].chapter_count, true);
    }
    return false;
}

bool BiblePageCoordinator::NextChapter()
{
    if (!available_) {
        return false;
    }
    const int count = books_[static_cast<size_t>(position_.book_index)].chapter_count;
    if (position_.chapter < count) {
        return LoadChapter(position_.book_index, position_.chapter + 1, false);
    }
    if (position_.book_index + 1 < static_cast<int>(books_.size())) {
        return LoadChapter(position_.book_index + 1, 1, false);
    }
    return false;
}

bool BiblePageCoordinator::PreviousChapter()
{
    if (!available_) {
        return false;
    }
    if (position_.chapter > 1) {
        return LoadChapter(position_.book_index, position_.chapter - 1, false);
    }
    if (position_.book_index > 0) {
        const int book = position_.book_index - 1;
        return LoadChapter(book, books_[static_cast<size_t>(book)].chapter_count, false);
    }
    return false;
}

bool BiblePageCoordinator::GoTo(int book_index, int chapter)
{
    if (!available_ || book_index < 0 || book_index >= static_cast<int>(books_.size())) {
        return false;
    }
    const int count = books_[static_cast<size_t>(book_index)].chapter_count;
    return LoadChapter(book_index, std::clamp(chapter, 1, count), false);
}

void BiblePageCoordinator::SetTextSize(epaper_ui::BibleTextSize size)
{
    if (size == text_size_) {
        return;
    }
    // Keep roughly the same reading position when the page count changes.
    const size_t anchor_item =
        pages_.empty() ? 0 : pages_[static_cast<size_t>(position_.page)].item;
    text_size_ = size;
    Paginate();
    int page = 0;
    for (size_t index = 0; index < pages_.size(); ++index) {
        if (pages_[index].item <= anchor_item) {
            page = static_cast<int>(index);
        }
    }
    position_.page = page;
    SavePosition();
}

void BiblePageCoordinator::SavePosition()
{
    position_.text_size = static_cast<int>(text_size_);
    bible_service::SavePosition(position_);
}

epaper_ui::BiblePageState BiblePageCoordinator::BuildState() const
{
    epaper_ui::BiblePageState state = {};
    state.text_size = text_size_;
    if (!available_) {
        state.title_text = "Bible";
        state.message_text = kNoBibleMessage;
        return state;
    }

    const bible_service::Book& book = books_[static_cast<size_t>(position_.book_index)];
    state.title_text = book.name + " " + std::to_string(position_.chapter);
    state.hint_text = kHintText;
    if (!error_text_.empty() || chapter_.items.empty()) {
        state.message_text = error_text_.empty() ? kLoadErrorMessage : error_text_;
        return state;
    }

    state.position_text = std::to_string(position_.page + 1) + "/" +
                          std::to_string(pages_.size());
    const Cursor start = pages_[static_cast<size_t>(position_.page)];
    for (size_t item = start.item;
         item < chapter_.items.size() && static_cast<int>(state.lines.size()) < lines_per_page_;
         ++item) {
        std::vector<epaper_ui::BiblePageLine> lines = ItemLines(item);
        for (size_t sub = item == start.item ? start.sub : 0;
             sub < lines.size() && static_cast<int>(state.lines.size()) < lines_per_page_; ++sub) {
            if (state.lines.empty() && lines[sub].text.empty()) {
                continue;
            }
            state.lines.push_back(std::move(lines[sub]));
        }
    }
    return state;
}
