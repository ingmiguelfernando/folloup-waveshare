#ifndef BIBLE_PAGE_COORDINATOR_H_
#define BIBLE_PAGE_COORDINATOR_H_

#include <cstddef>
#include <string>
#include <vector>

#include "bible_service.h"
#include "epaper_ui/bible_page.h"

// Owns reader state for the Bible page: current chapter, pagination and saved position.
class BiblePageCoordinator {
public:
    // Loads the library from SD (once) and reopens the saved position.
    void Open();

    bool available() const { return available_; }
    const std::vector<bible_service::Book>& books() const { return books_; }
    int book_index() const { return position_.book_index; }
    int chapter() const { return position_.chapter; }
    epaper_ui::BibleTextSize text_size() const { return text_size_; }

    bool NextPage();
    bool PreviousPage();
    bool NextChapter();
    bool PreviousChapter();
    bool GoTo(int book_index, int chapter);
    void SetTextSize(epaper_ui::BibleTextSize size);

    epaper_ui::BiblePageState BuildState() const;

private:
    struct Cursor {
        size_t item = 0;
        size_t sub = 0;
    };

    bool LoadChapter(int book_index, int chapter, bool last_page);
    std::vector<epaper_ui::BiblePageLine> ItemLines(size_t item_index) const;
    void Paginate();
    void SavePosition();

    bool available_ = false;
    std::vector<bible_service::Book> books_ = {};
    bible_service::Chapter chapter_ = {};
    bible_service::Position position_ = {};
    epaper_ui::BibleTextSize text_size_ = epaper_ui::BibleTextSize::kMedium;
    std::vector<Cursor> pages_ = {};
    int lines_per_page_ = 1;
    int text_width_ = 0;
    std::string error_text_ = {};
};

#endif  // BIBLE_PAGE_COORDINATOR_H_
