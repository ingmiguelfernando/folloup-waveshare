#ifndef BOOK_PAGE_COORDINATOR_H_
#define BOOK_PAGE_COORDINATOR_H_

#include <cstddef>
#include <string>
#include <vector>

#include "book_service.h"
#include "epaper_ui/reader_page.h"

// Owns reader state for the .txt book page: pagination and saved position.
// A page is a fixed stack of wrapped lines; there are no chapters, so page
// turns stop at the ends of the file.
class BookPageCoordinator {
public:
    // Loads `filename` from SD and reopens the saved position when it matches.
    void Open(const std::string& filename);

    bool available() const { return available_; }
    const std::string& filename() const { return book_.filename; }
    const std::string& error_text() const { return error_text_; }
    size_t size_bytes() const { return book_.buffer_size; }
    epaper_ui::ReaderTextSize text_size() const { return text_size_; }
    int page() const { return position_.page; }
    int page_count() const { return static_cast<int>(pages_.size()); }

    bool NextPage();
    bool PreviousPage();
    bool GoToPage(int page);
    // Jumps to the page containing `percent` of the book (0..100), e.g. 50
    // lands on the first page of the second half. False when the book is empty.
    bool GoToPercent(int percent);
    void SetTextSize(epaper_ui::ReaderTextSize size);

    epaper_ui::ReaderPageState BuildState() const;

private:
    struct Cursor {
        size_t paragraph = 0;
        size_t sub = 0;
    };

    void Paginate();
    std::vector<epaper_ui::ReaderPageLine> ParagraphLines(size_t paragraph_index) const;
    void SavePosition();

    bool available_ = false;
    book_service::Book book_ = {};
    book_service::Position position_ = {};
    epaper_ui::ReaderTextSize text_size_ = epaper_ui::ReaderTextSize::kMedium;
    std::vector<Cursor> pages_ = {};
    int lines_per_page_ = 1;
    int text_width_ = 0;
    std::string error_text_ = {};
};

#endif  // BOOK_PAGE_COORDINATOR_H_
