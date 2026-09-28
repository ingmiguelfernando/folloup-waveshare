#ifndef BOOK_SERVICE_H
#define BOOK_SERVICE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "esp_err.h"

// Reads plain-text books (.txt) from /sdcard/books/ for the reader page. The
// file is loaded whole into a PSRAM buffer and split into paragraph spans, and
// the last reading position persists in NVS ("book").
namespace book_service {

struct BookEntry {
    std::string filename = {};  // e.g. "quijote.txt"
    std::string title = {};     // filename without extension
    size_t size_bytes = 0;
};

struct Paragraph {
    uint32_t offset = 0;
    uint32_t length = 0;
};

// One loaded book; paragraph text lives in a single PSRAM buffer to keep
// internal RAM free (same policy as bible_service chapters).
struct Book {
    std::string filename = {};
    std::vector<Paragraph> paragraphs = {};
    std::shared_ptr<char> buffer = {};
    size_t buffer_size = 0;

    std::string_view TextOf(const Paragraph& paragraph) const;
};

struct Position {
    std::string filename = {};
    int page = 0;
    int text_size = 1;  // epaper_ui::BookTextSize ordinal
};

// Scans /sdcard/books/ for *.txt sorted by name. Call again to refresh.
esp_err_t Load();
bool IsLoaded();
const std::vector<BookEntry>& Books();

// Reads one book into a PSRAM buffer. Files that are not valid UTF-8 are
// transcoded from CP1252/Latin-1 so Spanish accents and typographic
// punctuation survive. .epub files are unzipped and their chapters are
// stripped to plain text. Rejects books larger than the size cap.
esp_err_t LoadBook(const std::string& filename, Book* out_book);

// Reading positions persist per book (keyed by a hash of the filename), so
// every book remembers where the reader left off, not just the last one.
Position LoadPosition(const std::string& filename);
void SavePosition(const Position& position);
// Marks `filename` as the most recently opened book (for the reader menu).
void SaveLastOpened(const std::string& filename);
std::string LoadLastOpened();

}  // namespace book_service

#endif  // BOOK_SERVICE_H
