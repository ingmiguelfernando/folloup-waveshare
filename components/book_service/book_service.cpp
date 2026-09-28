#include "book_service.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "storage_service.h"

namespace book_service {
namespace {

constexpr const char* kTag = "BookService";
constexpr const char* kRootDir = "books";
constexpr const char* kNvsNamespace = "book";
constexpr const char* kFileKey = "file";
constexpr const char* kPageKey = "page";
constexpr const char* kSizeKey = "size";
// Full-file reads live in PSRAM; the cap keeps a multi-megabyte import from
// starving the recording clips, which are PSRAM-backed too.
constexpr size_t kMaxBookBytes = 2 * 1024 * 1024;
constexpr uint8_t kUtf8Bom[] = {0xEF, 0xBB, 0xBF};

std::mutex s_mutex;
bool s_loaded = false;
std::vector<BookEntry> s_books;

std::string JoinPath(const std::string& base, const std::string& leaf)
{
    return base + "/" + leaf;
}

bool EndsWithIgnoreCase(const std::string& value, const char* suffix)
{
    const size_t suffix_len = std::strlen(suffix);
    if (value.size() < suffix_len) {
        return false;
    }
    for (size_t i = 0; i < suffix_len; ++i) {
        const unsigned char a = static_cast<unsigned char>(value[value.size() - suffix_len + i]);
        const unsigned char b = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

bool IsValidUtf8(const uint8_t* data, size_t size)
{
    size_t index = 0;
    while (index < size) {
        const uint8_t lead = data[index];
        size_t extra = 0;
        if (lead < 0x80) {
            extra = 0;
        } else if ((lead & 0xE0) == 0xC0) {
            extra = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            extra = 3;
        } else {
            return false;
        }
        if (index + extra >= size) {
            return false;
        }
        for (size_t i = 1; i <= extra; ++i) {
            if ((data[index + i] & 0xC0) != 0x80) {
                return false;
            }
        }
        index += extra + 1;
    }
    return true;
}

// Windows-1252 codepoint for a byte (the punctuation Latin-1 leaves undefined).
uint32_t Cp1252Codepoint(uint8_t byte)
{
    switch (byte) {
        case 0x80: return 0x20AC;  // euro
        case 0x82: return 0x201A;
        case 0x83: return 0x0192;
        case 0x84: return 0x201E;
        case 0x85: return 0x2026;  // ellipsis
        case 0x86: return 0x2020;
        case 0x87: return 0x2021;
        case 0x88: return 0x02C6;
        case 0x89: return 0x2030;
        case 0x8A: return 0x0160;
        case 0x8B: return 0x2039;
        case 0x8C: return 0x0152;
        case 0x8E: return 0x017D;
        case 0x91: return 0x2018;
        case 0x92: return 0x2019;
        case 0x93: return 0x201C;
        case 0x94: return 0x201D;
        case 0x95: return 0x2022;
        case 0x96: return 0x2013;
        case 0x97: return 0x2014;
        case 0x98: return 0x02DC;
        case 0x99: return 0x2122;
        case 0x9A: return 0x0161;
        case 0x9B: return 0x203A;
        case 0x9C: return 0x0153;
        case 0x9E: return 0x017E;
        case 0x9F: return 0x0178;
        default: return byte;
    }
}

size_t Utf8Length(uint32_t codepoint)
{
    if (codepoint < 0x80) {
        return 1;
    }
    if (codepoint < 0x800) {
        return 2;
    }
    return codepoint < 0x10000 ? 3 : 4;
}

size_t AppendUtf8(char* out, uint32_t codepoint)
{
    const size_t length = Utf8Length(codepoint);
    switch (length) {
        case 1:
            out[0] = static_cast<char>(codepoint);
            break;
        case 2:
            out[0] = static_cast<char>(0xC0 | (codepoint >> 6));
            out[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
            break;
        case 3:
            out[0] = static_cast<char>(0xE0 | (codepoint >> 12));
            out[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            out[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
            break;
        default:
            out[0] = static_cast<char>(0xF0 | (codepoint >> 18));
            out[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
            out[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            out[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
            break;
    }
    return length;
}

bool TranscodeToUtf8(Book* book)
{
    const auto* raw = reinterpret_cast<const uint8_t*>(book->buffer.get());
    size_t needed = 0;
    for (size_t i = 0; i < book->buffer_size; ++i) {
        needed += Utf8Length(Cp1252Codepoint(raw[i]));
    }

    char* converted =
        static_cast<char*>(heap_caps_malloc(needed, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (converted == nullptr) {
        converted = static_cast<char*>(std::malloc(needed));
    }
    if (converted == nullptr) {
        return false;
    }

    size_t written = 0;
    for (size_t i = 0; i < book->buffer_size; ++i) {
        written += AppendUtf8(converted + written, Cp1252Codepoint(raw[i]));
    }
    book->buffer = std::shared_ptr<char>(converted, std::free);
    book->buffer_size = written;
    return true;
}

void ParseParagraphs(Book* book)
{
    book->paragraphs.clear();
    const char* data = book->buffer.get();
    const size_t size = book->buffer_size;
    // Offsets stay buffer-relative; the BOM just never becomes a paragraph.
    size_t line_start = (size >= 3 && std::memcmp(data, kUtf8Bom, sizeof(kUtf8Bom)) == 0) ? 3 : 0;

    for (size_t index = line_start; index <= size; ++index) {
        if (index != size && data[index] != '\n') {
            continue;
        }
        size_t end = index;
        if (end > line_start && data[end - 1] == '\r') {
            --end;  // CRLF endings
        }
        book->paragraphs.push_back({.offset = static_cast<uint32_t>(line_start),
                                    .length = static_cast<uint32_t>(end - line_start)});
        line_start = index + 1;
    }
}

struct ScanContext {
    std::vector<BookEntry> books;
};

esp_err_t ScanBooksOnMountedFilesystem(const char* mount_point, void* raw_context)
{
    auto* context = static_cast<ScanContext*>(raw_context);
    const std::string root = JoinPath(mount_point, kRootDir);
    DIR* dir = opendir(root.c_str());
    if (dir == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }

    while (dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        const std::string filename(entry->d_name);
        if (!EndsWithIgnoreCase(filename, ".txt")) {
            continue;
        }
        size_t size_bytes = 0;
        struct stat file_stat = {};
        if (stat(JoinPath(root, filename).c_str(), &file_stat) == 0) {
            size_bytes = static_cast<size_t>(file_stat.st_size);
        }
        context->books.push_back({.filename = filename,
                                  .title = filename.substr(0, filename.size() - 4),
                                  .size_bytes = size_bytes});
    }
    closedir(dir);

    std::sort(context->books.begin(), context->books.end(),
              [](const BookEntry& a, const BookEntry& b) { return a.filename < b.filename; });
    return context->books.empty() ? ESP_ERR_NOT_FOUND : ESP_OK;
}

struct LoadBookContext {
    std::string filename;
    Book* out = nullptr;
};

esp_err_t LoadBookOnMountedFilesystem(const char* mount_point, void* raw_context)
{
    auto* context = static_cast<LoadBookContext*>(raw_context);
    const std::string path = JoinPath(JoinPath(mount_point, kRootDir), context->filename);
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }

    std::fseek(file, 0, SEEK_END);
    const long file_size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (file_size <= 0) {
        std::fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    if (static_cast<size_t>(file_size) > kMaxBookBytes) {
        std::fclose(file);
        ESP_LOGW(kTag, "Book too large: %s is %ld bytes (cap %u)", context->filename.c_str(),
                 file_size, static_cast<unsigned>(kMaxBookBytes));
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t raw_size = static_cast<size_t>(file_size);
    char* raw = static_cast<char*>(heap_caps_malloc(raw_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (raw == nullptr) {
        raw = static_cast<char*>(std::malloc(raw_size));
    }
    if (raw == nullptr) {
        std::fclose(file);
        return ESP_ERR_NO_MEM;
    }

    const size_t read_bytes = std::fread(raw, 1, raw_size, file);
    std::fclose(file);
    if (read_bytes != raw_size) {
        std::free(raw);
        return ESP_ERR_INVALID_STATE;
    }

    Book book = {};
    book.filename = context->filename;
    book.buffer = std::shared_ptr<char>(raw, std::free);
    book.buffer_size = raw_size;

    if (!IsValidUtf8(reinterpret_cast<const uint8_t*>(raw), raw_size)) {
        ESP_LOGI(kTag, "Transcoding %s from CP1252/Latin-1 to UTF-8", context->filename.c_str());
        if (!TranscodeToUtf8(&book)) {
            return ESP_ERR_NO_MEM;
        }
    }

    ParseParagraphs(&book);
    *context->out = std::move(book);
    return ESP_OK;
}

}  // namespace

std::string_view Book::TextOf(const Paragraph& paragraph) const
{
    if (buffer == nullptr || static_cast<size_t>(paragraph.offset) + paragraph.length >
                                buffer_size) {
        return {};
    }
    return std::string_view(buffer.get() + paragraph.offset, paragraph.length);
}

esp_err_t Load()
{
    ScanContext context = {};
    const esp_err_t err =
        storage_service::RunWithMountedFilesystem(ScanBooksOnMountedFilesystem, &context);
    const size_t count = context.books.size();
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_books = std::move(context.books);
        s_loaded = (err == ESP_OK);
    }
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "No .txt books found under /sdcard/books: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(kTag, "Loaded %u book(s) from /sdcard/books", static_cast<unsigned>(count));
    }
    return err;
}

bool IsLoaded()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_loaded;
}

const std::vector<BookEntry>& Books()
{
    return s_books;
}

esp_err_t LoadBook(const std::string& filename, Book* out_book)
{
    if (filename.empty() || out_book == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    LoadBookContext context = {.filename = filename, .out = out_book};
    return storage_service::RunWithMountedFilesystem(LoadBookOnMountedFilesystem, &context);
}

Position LoadPosition()
{
    Position position = {};
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return position;
    }

    char file[64] = {};
    size_t length = sizeof(file);
    if (nvs_get_str(handle, kFileKey, file, &length) == ESP_OK) {
        position.filename = file;
    }
    nvs_get_i32(handle, kPageKey, &position.page);
    nvs_get_i32(handle, kSizeKey, &position.text_size);
    nvs_close(handle);
    return position;
}

void SavePosition(const Position& position)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to open NVS for book position: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_str(handle, kFileKey, position.filename.c_str());
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, kPageKey, position.page);
    }
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, kSizeKey, position.text_size);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to save book position: %s", esp_err_to_name(err));
    }
}

}  // namespace book_service
