#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "esp_err.h"

// Reads a Bible converted by scripts/bible_json_to_sd.py from /sdcard/bible/<abbr>/.
namespace bible_service {

struct Book {
    std::string usfm = {};
    std::string name = {};
    int chapter_count = 0;
};

enum class ItemKind : uint8_t {
    kHeading,
    kVerse,
};

struct Item {
    ItemKind kind = ItemKind::kVerse;
    std::string label = {};
    uint32_t text_offset = 0;
    uint32_t text_length = 0;
};

// One chapter; item text lives in a single PSRAM buffer to keep internal RAM free.
struct Chapter {
    int book_index = -1;
    int chapter = 0;
    std::vector<Item> items = {};
    std::shared_ptr<char> buffer = {};
    size_t buffer_size = 0;

    std::string_view TextOf(const Item& item) const
    {
        if (!buffer || item.text_offset + item.text_length > buffer_size) {
            return {};
        }
        return {buffer.get() + item.text_offset, item.text_length};
    }
};

struct Position {
    int book_index = 0;
    int chapter = 1;
    int page = 0;
    int text_size = 1;
};

// Scans the SD card for the first bible/<abbr>/index.tsv. ESP_ERR_NOT_FOUND when absent.
esp_err_t Load();
bool IsLoaded();
std::string Title();
std::string Copyright();
std::vector<Book> Books();

esp_err_t LoadChapter(int book_index, int chapter, Chapter* out);

Position LoadPosition();
void SavePosition(const Position& position);

}  // namespace bible_service
