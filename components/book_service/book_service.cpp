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
constexpr const char* kFileKey = "file";   // most recently opened book
constexpr const char* kPageKey = "page";   // per-book: "<hash>p"
constexpr const char* kSizeKey = "size";   // per-book: "<hash>s"
// Full-file reads live in PSRAM; the cap keeps a multi-megabyte import from
// starving the recording clips, which are PSRAM-backed too.
constexpr size_t kMaxBookBytes = 2 * 1024 * 1024;
constexpr uint8_t kUtf8Bom[] = {0xEF, 0xBB, 0xBF};

std::mutex s_mutex;
bool s_loaded = false;
std::vector<BookEntry> s_books;

// FNV-1a 32-bit: NVS keys are capped at 15 chars, so per-book state keys are
// "<hash>p"/"<hash>s" instead of the filename itself.
uint32_t HashFilename(const std::string& filename)
{
    uint32_t hash = 2166136261u;
    for (const char c : filename) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 16777619u;
    }
    return hash;
}

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
        if (!EndsWithIgnoreCase(filename, ".txt") && !EndsWithIgnoreCase(filename, ".epub")) {
            continue;
        }
        size_t size_bytes = 0;
        struct stat file_stat = {};
        if (stat(JoinPath(root, filename).c_str(), &file_stat) == 0) {
            size_bytes = static_cast<size_t>(file_stat.st_size);
        }
        const size_t base_len = filename.size() - 4;
        context->books.push_back({.filename = filename,
                                  .title = filename.substr(0, base_len),
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

esp_err_t ExtractEpub(Book* book);  // defined below; unzips stored XHTML chapters

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

    if (EndsWithIgnoreCase(context->filename, ".epub")) {
        ESP_LOGI(kTag, "Extracting EPUB %s", context->filename.c_str());
        esp_err_t err = ExtractEpub(&book);
        if (err != ESP_OK) {
            return err;
        }
    } else if (!IsValidUtf8(reinterpret_cast<const uint8_t*>(raw), raw_size)) {
        ESP_LOGI(kTag, "Transcoding %s from CP1252/Latin-1 to UTF-8", context->filename.c_str());
        if (!TranscodeToUtf8(&book)) {
            return ESP_ERR_NO_MEM;
        }
    }

    ParseParagraphs(&book);
    *context->out = std::move(book);
    return ESP_OK;
}

// --- EPUB support -----------------------------------------------------------
//
// An EPUB is a ZIP archive whose OPF lists the reading order (spine) as XHTML
// files. This reader handles STORE (uncompressed) entries only -- the deflate
// path would need a whole inflate implementation. Calibre can re-export with
// "compression level 0 (store)" and many EPUBs (notably those produced by
// Project Gutenberg tools) already ship stored text chapters. Anything deflated
// is skipped with a warning rather than failing the whole book.

constexpr uint32_t kEpubLocalHeaderSignature = 0x04034B50u;

#pragma pack(push, 1)
struct EpubLocalHeader {
    uint32_t signature;
    uint16_t version_needed;
    uint16_t flags;
    uint16_t method;
    uint16_t mod_time;
    uint16_t mod_date;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t name_length;
    uint16_t extra_length;
};
#pragma pack(pop)
static_assert(sizeof(EpubLocalHeader) == 30, "unexpected ZIP local header layout");

// Little-endian read for the ZIP local header signature (be explicit).
uint32_t ReadLe32(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

// Grows the extraction buffer by doubling; PSRAM-backed via heap_caps.
bool AppendBytes(char** out, size_t* capacity, size_t* used, const char* data, size_t length)
{
    if (*used + length + 1 > *capacity) {
        size_t new_capacity = *capacity == 0 ? 4096 : *capacity;
        while (*used + length + 1 > new_capacity) {
            new_capacity *= 2;
        }
        char* grown = static_cast<char*>(heap_caps_realloc(
            *out, new_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (grown == nullptr) {
            return false;
        }
        *out = grown;
        *capacity = new_capacity;
    }
    std::memcpy(*out + *used, data, length);
    *used += length;
    return true;
}

// Strips XHTML tags to plain text: <tag> removed, entities decoded, block ends
// become newlines so ParseParagraphs keeps the chapter rhythm.
std::string StripXhtml(const std::string& input)
{
    std::string out;
    out.reserve(input.size() + input.size() / 8);
    bool in_tag = false;
    for (size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (in_tag) {
            if (c == '>') {
                in_tag = false;
            }
            continue;
        }
        if (c == '<') {
            // A block-level boundary: close the current line.
            if (!out.empty() && out.back() != '\n') {
                out.push_back('\n');
            }
            in_tag = true;
            continue;
        }
        if (c == '&') {
            if (input.compare(i, 6, "&quot;") == 0) {
                out.push_back('"');
                i += 5;
            } else if (input.compare(i, 6, "&apos;") == 0) {
                out.push_back('\'');
                i += 5;
            } else if (input.compare(i, 4, "&lt;") == 0) {
                out.push_back('<');
                i += 3;
            } else if (input.compare(i, 4, "&gt;") == 0) {
                out.push_back('>');
                i += 3;
            } else if (input.compare(i, 5, "&amp;") == 0) {
                out.push_back('&');
                i += 4;
            } else if (input.compare(i, 6, "&nbsp;") == 0) {
                out.push_back(' ');
                i += 5;
            } else if (input.compare(i, 8, "&#8217;") == 0) {
                // Right single quote: the fonts carry it via Latin-1 range? It is
                // U+2019 (3-byte UTF-8); emit directly so rendering can fall back.
                out += "\xE2\x80\x99";
                i += 7;
            } else if (input.compare(i, 8, "&#8220;") == 0) {
                out += "\xE2\x80\x9C";
                i += 7;
            } else if (input.compare(i, 8, "&#8221;") == 0) {
                out += "\xE2\x80\x9D";
                i += 7;
            } else if (input.compare(i, 6, "&#160;") == 0) {
                out.push_back(' ');
                i += 5;
            } else {
                out.push_back('&');
            }
            continue;
        }
        out.push_back(c);
    }
    return out;
}

// Case-insensitive attribute lookup within one <tag ...> span.
std::string TagAttribute(const std::string& tag, const char* attribute)
{
    // tag includes the angle brackets.
    const size_t key_length = std::strlen(attribute);
    size_t search = 0;
    while (true) {
        const size_t found = tag.find(attribute, search);
        if (found == std::string::npos) {
            return {};
        }
        const size_t eq = found + key_length;
        size_t cursor = eq;
        while (cursor < tag.size() && (tag[cursor] == ' ' || tag[cursor] == '\t' ||
                                       tag[cursor] == '\r' || tag[cursor] == '\n')) {
            ++cursor;
        }
        if (cursor < tag.size() && tag[cursor] == '=' ) {
            ++cursor;
            while (cursor < tag.size() && (tag[cursor] == ' ' || tag[cursor] == '\t')) {
                ++cursor;
            }
            if (cursor < tag.size() && (tag[cursor] == '"' || tag[cursor] == '\'')) {
                const char quote = tag[cursor++];
                const size_t end = tag.find(quote, cursor);
                if (end != std::string::npos) {
                    return tag.substr(cursor, end - cursor);
                }
            }
        }
        search = found + 1;
    }
}

// Unzips the stored entries named by the OPF spine (in order) into one text
// buffer. Returns ESP_ERR_NOT_SUPPORTED when the archive has no usable stored
// XHTML (all deflated or no spine) so the caller can report a clear error.
esp_err_t ExtractEpub(Book* book)
{
    const auto* zip = reinterpret_cast<const uint8_t*>(book->buffer.get());
    const size_t zip_size = book->buffer_size;
    if (zip_size < sizeof(EpubLocalHeader) || ReadLe32(zip) != kEpubLocalHeaderSignature) {
        return ESP_ERR_INVALID_STATE;
    }

    // Pass 1: index stored entries. Parallel vectors keep it allocation-light.
    struct ZipEntry {
        std::string name;
        size_t offset = 0;  // data offset in the archive
        size_t size = 0;
    };
    std::vector<ZipEntry> entries;
    size_t cursor = 0;
    while (cursor + sizeof(EpubLocalHeader) <= zip_size) {
        if (ReadLe32(zip + cursor) != kEpubLocalHeaderSignature) {
            ++cursor;
            continue;
        }
        EpubLocalHeader header = {};
        std::memcpy(&header, zip + cursor, sizeof(header));
        const size_t name_offset = cursor + sizeof(header);
        if (name_offset + header.name_length > zip_size) {
            break;
        }
        ZipEntry entry;
        entry.name.assign(reinterpret_cast<const char*>(zip + name_offset), header.name_length);
        const size_t data_offset = name_offset + header.name_length + header.extra_length;
        if (header.method == 0 && header.uncompressed_size == header.compressed_size &&
            data_offset + header.uncompressed_size <= zip_size) {
            entry.offset = data_offset;
            entry.size = header.uncompressed_size;
            entries.push_back(std::move(entry));
        }
        cursor = data_offset + header.compressed_size;
        if (header.compressed_size == 0) {
            // Data-descriptor entries (flag bit 3): sizes live in the trailer.
            // Without parsing central directory we cannot know the length, so
            // stop scanning rather than misparse random bytes.
            break;
        }
    }

    // Pass 2: read the container -> OPF -> spine order.
    std::string opf_path;
    std::vector<std::string> spine;
    for (const ZipEntry& entry : entries) {
        if (entry.name == "META-INF/container.xml") {
            const std::string xml(reinterpret_cast<const char*>(zip + entry.offset), entry.size);
            const size_t rootfile = xml.find("full-path=");
            if (rootfile != std::string::npos) {
                const char quote = xml[rootfile + 10];
                const size_t start = rootfile + 11;
                const size_t end = xml.find(quote, start);
                if (end != std::string::npos) {
                    opf_path = xml.substr(start, end - start);
                }
            }
        }
    }
    if (opf_path.empty()) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const std::string opf_dir = opf_path.find('/') == std::string::npos
                                    ? std::string()
                                    : opf_path.substr(0, opf_path.rfind('/') + 1);
    for (const ZipEntry& entry : entries) {
        if (entry.name != opf_path) {
            continue;
        }
        const std::string opf(reinterpret_cast<const char*>(zip + entry.offset), entry.size);
        // Spine itemrefs in document order.
        size_t spine_pos = opf.find("<spine");
        if (spine_pos == std::string::npos) {
            continue;
        }
        spine_pos = opf.find('>', spine_pos) + 1;
        const size_t spine_end = opf.find("</spine>", spine_pos);
        if (spine_end == std::string::npos) {
            continue;
        }
        const std::string spine_block = opf.substr(spine_pos, spine_end - spine_pos);
        // Manifest id -> href map.
        std::vector<std::pair<std::string, std::string>> manifest;
        size_t manifest_pos = opf.find("<manifest");
        if (manifest_pos != std::string::npos) {
            const size_t manifest_end = opf.find("</manifest>", manifest_pos);
            const std::string manifest_block =
                manifest_end == std::string::npos
                    ? opf.substr(manifest_pos)
                    : opf.substr(manifest_pos, manifest_end - manifest_pos);
            size_t item_pos = 0;
            while (true) {
                const size_t item_start = manifest_block.find("<item", item_pos);
                if (item_start == std::string::npos) {
                    break;
                }
                const size_t item_end = manifest_block.find('>', item_start);
                if (item_end == std::string::npos) {
                    break;
                }
                const std::string item = manifest_block.substr(item_start, item_end - item_start);
                const std::string id = TagAttribute(item, "id");
                const std::string href = TagAttribute(item, "href");
                if (!id.empty() && !href.empty()) {
                    manifest.emplace_back(id, href);
                }
                item_pos = item_end;
            }
        }
        size_t idref_pos = 0;
        while (true) {
            const size_t idref_start = spine_block.find("<itemref", idref_pos);
            if (idref_start == std::string::npos) {
                break;
            }
            const size_t idref_end = spine_block.find('>', idref_start);
            if (idref_end == std::string::npos) {
                break;
            }
            const std::string idref =
                TagAttribute(spine_block.substr(idref_start, idref_end - idref_start), "idref");
            for (const auto& item : manifest) {
                if (item.first == idref) {
                    spine.push_back(item.second);
                    break;
                }
            }
            idref_pos = idref_end;
        }
    }
    if (spine.empty()) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Pass 3: strip each spine chapter in order into the extraction buffer.
    char* text = nullptr;
    size_t capacity = 0;
    size_t used = 0;
    size_t chapters = 0;
    for (const std::string& href : spine) {
        // Resolve relative hrefs against the OPF directory.
        std::string resolved = href;
        if (!opf_dir.empty() && href.find("://") == std::string::npos &&
            href.front() != '/') {
            resolved = opf_dir + href;
        }
        // Percent-decoding skipped: EPUB chapter names with spaces are rare and
        // container paths must be ASCII per spec.
        bool found = false;
        for (const ZipEntry& entry : entries) {
            if (entry.name != resolved) {
                continue;
            }
            found = true;
            std::string chapter(reinterpret_cast<const char*>(zip + entry.offset), entry.size);
            const std::string stripped = StripXhtml(chapter);
            if (!AppendBytes(&text, &capacity, &used, stripped.data(), stripped.size())) {
                std::free(text);
                return ESP_ERR_NO_MEM;
            }
            ++chapters;
            break;
        }
        if (!found) {
            ESP_LOGW(kTag, "EPUB spine chapter missing: %s", resolved.c_str());
        }
    }
    if (text == nullptr || used == 0) {
        std::free(text);
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(kTag, "EPUB extracted: %u chapter(s), %u bytes", static_cast<unsigned>(chapters),
             static_cast<unsigned>(used));

    book->buffer = std::shared_ptr<char>(text, std::free);
    book->buffer_size = used;
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
        ESP_LOGW(kTag, "No .txt/.epub books found under /sdcard/books: %s",
                 esp_err_to_name(err));
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

// Per-book NVS keys: "<hash>p" (page) and "<hash>s" (text size). Keys hold the
// position of every book; kFileKey records the most recently opened one.
void PositionKeys(const std::string& filename, char* page_key, size_t page_key_size,
                  char* size_key, size_t size_key_size)
{
    const uint32_t hash = HashFilename(filename);
    std::snprintf(page_key, page_key_size, "%08Xp", static_cast<unsigned>(hash));
    std::snprintf(size_key, size_key_size, "%08Xs", static_cast<unsigned>(hash));
}

Position LoadPosition(const std::string& filename)
{
    Position position = {};
    position.filename = filename;

    char page_key[16] = {};
    char size_key[16] = {};
    PositionKeys(filename, page_key, sizeof(page_key), size_key, sizeof(size_key));

    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return position;
    }

    // nvs_get_i32 wants int32_t*, which is `long` on Xtensa: read through a
    // temporary like bible_service does.
    int32_t value = 0;
    if (nvs_get_i32(handle, page_key, &value) == ESP_OK) {
        position.page = static_cast<int>(value);
    }
    if (nvs_get_i32(handle, size_key, &value) == ESP_OK) {
        position.text_size = static_cast<int>(value);
    }
    nvs_close(handle);
    return position;
}

void SavePosition(const Position& position)
{
    if (position.filename.empty()) {
        return;
    }

    char page_key[16] = {};
    char size_key[16] = {};
    PositionKeys(position.filename, page_key, sizeof(page_key), size_key, sizeof(size_key));

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to open NVS for book position: %s", esp_err_to_name(err));
        return;
    }

    if (err == ESP_OK) {
        err = nvs_set_i32(handle, page_key, position.page);
    }
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, size_key, position.text_size);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to save book position: %s", esp_err_to_name(err));
    }
}

void SaveLastOpened(const std::string& filename)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return;
    }
    err = nvs_set_str(handle, kFileKey, filename.c_str());
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to save last opened book: %s", esp_err_to_name(err));
    }
}

std::string LoadLastOpened()
{
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return {};
    }
    char file[64] = {};
    size_t length = sizeof(file);
    std::string filename;
    if (nvs_get_str(handle, kFileKey, file, &length) == ESP_OK) {
        filename = file;
    }
    nvs_close(handle);
    return filename;
}

}  // namespace book_service
