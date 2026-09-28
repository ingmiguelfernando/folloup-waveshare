#include "bible_service.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <mutex>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "storage_service.h"

namespace bible_service {
namespace {

constexpr const char* kTag = "BibleService";
constexpr const char* kRootDir = "bible";
constexpr const char* kNvsNamespace = "bible";
constexpr size_t kMaxChapterBytes = 256 * 1024;

std::mutex s_mutex;
bool s_loaded = false;
std::string s_dir = {};
std::string s_title = {};
std::string s_copyright = {};
std::vector<Book> s_books = {};

std::string JoinPath(const std::string& a, const std::string& b)
{
    return a.empty() || a.back() == '/' ? a + b : a + "/" + b;
}

std::vector<std::string> SplitTabs(const std::string& line)
{
    std::vector<std::string> fields;
    size_t start = 0;
    while (true) {
        const size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            fields.push_back(line.substr(start));
            return fields;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
}

bool ReadLines(const std::string& path, std::vector<std::string>* lines)
{
    FILE* file = std::fopen(path.c_str(), "r");
    if (file == nullptr) {
        return false;
    }
    char buffer[512];
    std::string current;
    while (std::fgets(buffer, sizeof(buffer), file) != nullptr) {
        current += buffer;
        if (!current.empty() && current.back() == '\n') {
            while (!current.empty() && (current.back() == '\n' || current.back() == '\r')) {
                current.pop_back();
            }
            lines->push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) {
        lines->push_back(current);
    }
    std::fclose(file);
    return true;
}

struct LoadContext {
    std::string dir;
    std::string title;
    std::string copyright;
    std::vector<Book> books;
};

esp_err_t LoadOnMountedFilesystem(const char* mount_point, void* raw_context)
{
    auto* context = static_cast<LoadContext*>(raw_context);
    const std::string root = JoinPath(mount_point, kRootDir);
    DIR* dir = opendir(root.c_str());
    if (dir == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }

    std::vector<std::string> index_lines;
    while (dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        const std::string candidate = JoinPath(root, entry->d_name);
        index_lines.clear();
        if (ReadLines(JoinPath(candidate, "index.tsv"), &index_lines) && !index_lines.empty()) {
            context->dir = candidate;
            break;
        }
    }
    closedir(dir);
    if (context->dir.empty()) {
        return ESP_ERR_NOT_FOUND;
    }

    for (const std::string& line : index_lines) {
        const std::vector<std::string> fields = SplitTabs(line);
        if (fields.size() < 3) {
            continue;
        }
        const int chapters = std::atoi(fields[2].c_str());
        if (chapters <= 0) {
            continue;
        }
        context->books.push_back({.usfm = fields[0], .name = fields[1], .chapter_count = chapters});
    }

    std::vector<std::string> meta_lines;
    (void)ReadLines(JoinPath(context->dir, "meta.txt"), &meta_lines);
    for (const std::string& line : meta_lines) {
        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, equals);
        if (key == "title") {
            context->title = line.substr(equals + 1);
        } else if (key == "copyright") {
            context->copyright = line.substr(equals + 1);
        }
    }
    return context->books.empty() ? ESP_ERR_NOT_FOUND : ESP_OK;
}

struct ChapterContext {
    std::string dir;
    std::string usfm;
    int chapter = 0;
    Chapter* out = nullptr;
};

bool FindChapterRange(const std::string& idx_path, int chapter, long* start, long* end)
{
    std::vector<std::string> lines;
    if (!ReadLines(idx_path, &lines)) {
        return false;
    }
    *start = -1;
    *end = -1;
    for (const std::string& line : lines) {
        const std::vector<std::string> fields = SplitTabs(line);
        if (fields.size() < 2) {
            continue;
        }
        const int number = std::atoi(fields[0].c_str());
        const long offset = std::atol(fields[1].c_str());
        if (*start >= 0) {
            *end = offset;
            return true;
        }
        if (number == chapter) {
            *start = offset;
        }
    }
    return *start >= 0;
}

void ParseChapter(Chapter* chapter)
{
    char* data = chapter->buffer.get();
    size_t line_start = 0;
    while (line_start < chapter->buffer_size) {
        size_t line_end = line_start;
        while (line_end < chapter->buffer_size && data[line_end] != '\n') {
            ++line_end;
        }
        const size_t length = line_end - line_start;
        const char* line = data + line_start;
        if (length > 2 && line[1] == '\t' && (line[0] == 'H' || line[0] == 'V')) {
            Item item = {};
            size_t text_start = line_start + 2;
            if (line[0] == 'H') {
                item.kind = ItemKind::kHeading;
            } else {
                // V <label> <paragraph flag> <text>
                const char* label_end =
                    static_cast<const char*>(std::memchr(line + 2, '\t', length - 2));
                if (label_end == nullptr) {
                    line_start = line_end + 1;
                    continue;
                }
                item.label.assign(line + 2, label_end);
                const size_t after_label = static_cast<size_t>(label_end - data) + 1;
                const char* flag_end = static_cast<const char*>(
                    std::memchr(data + after_label, '\t', line_end - after_label));
                text_start = flag_end != nullptr ? static_cast<size_t>(flag_end - data) + 1
                                                 : after_label;
            }
            item.text_offset = static_cast<uint32_t>(text_start);
            item.text_length = static_cast<uint32_t>(line_end > text_start ? line_end - text_start
                                                                            : 0);
            chapter->items.push_back(std::move(item));
        }
        line_start = line_end + 1;
    }
}

esp_err_t LoadChapterOnMountedFilesystem(const char* /*mount_point*/, void* raw_context)
{
    auto* context = static_cast<ChapterContext*>(raw_context);
    long start = -1;
    long end = -1;
    if (!FindChapterRange(JoinPath(context->dir, context->usfm + ".idx"), context->chapter, &start,
                          &end)) {
        return ESP_ERR_NOT_FOUND;
    }

    FILE* file = std::fopen(JoinPath(context->dir, context->usfm + ".txt").c_str(), "rb");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    if (end < 0) {
        std::fseek(file, 0, SEEK_END);
        end = std::ftell(file);
    }
    const size_t size = end > start ? static_cast<size_t>(end - start) : 0;
    if (size == 0 || size > kMaxChapterBytes) {
        std::fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    char* raw = static_cast<char*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (raw == nullptr) {
        raw = static_cast<char*>(std::malloc(size));
    }
    if (raw == nullptr) {
        std::fclose(file);
        return ESP_ERR_NO_MEM;
    }
    std::fseek(file, start, SEEK_SET);
    const size_t read = std::fread(raw, 1, size, file);
    std::fclose(file);

    Chapter* chapter = context->out;
    chapter->buffer = std::shared_ptr<char>(raw, [](char* p) { heap_caps_free(p); });
    chapter->buffer_size = read;
    chapter->items.clear();
    ParseChapter(chapter);
    return chapter->items.empty() ? ESP_ERR_NOT_FOUND : ESP_OK;
}

}  // namespace

esp_err_t Load()
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_loaded) {
            return ESP_OK;
        }
    }
    LoadContext context = {};
    const esp_err_t err =
        storage_service::RunWithMountedFilesystem(LoadOnMountedFilesystem, &context);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "No Bible found on SD (%s)", esp_err_to_name(err));
        return err;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    s_dir = std::move(context.dir);
    s_title = std::move(context.title);
    s_copyright = std::move(context.copyright);
    s_books = std::move(context.books);
    s_loaded = true;
    ESP_LOGI(kTag, "Loaded %s (%u books) from %s", s_title.c_str(),
             static_cast<unsigned>(s_books.size()), s_dir.c_str());
    return ESP_OK;
}

bool IsLoaded()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_loaded;
}

std::string Title()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_title;
}

std::string Copyright()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_copyright;
}

std::vector<Book> Books()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_books;
}

esp_err_t LoadChapter(int book_index, int chapter, Chapter* out)
{
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    ChapterContext context = {};
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_loaded || book_index < 0 || book_index >= static_cast<int>(s_books.size()) ||
            chapter < 1 || chapter > s_books[static_cast<size_t>(book_index)].chapter_count) {
            return ESP_ERR_INVALID_ARG;
        }
        context.dir = s_dir;
        context.usfm = s_books[static_cast<size_t>(book_index)].usfm;
    }
    context.chapter = chapter;
    context.out = out;
    const esp_err_t err =
        storage_service::RunWithMountedFilesystem(LoadChapterOnMountedFilesystem, &context);
    if (err == ESP_OK) {
        out->book_index = book_index;
        out->chapter = chapter;
    } else {
        ESP_LOGW(kTag, "Load %s %d failed: %s", context.usfm.c_str(), chapter,
                 esp_err_to_name(err));
    }
    return err;
}

Position LoadPosition()
{
    Position position = {};
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return position;
    }
    int32_t value = 0;
    if (nvs_get_i32(handle, "book", &value) == ESP_OK) {
        position.book_index = value;
    }
    if (nvs_get_i32(handle, "chapter", &value) == ESP_OK) {
        position.chapter = value;
    }
    if (nvs_get_i32(handle, "page", &value) == ESP_OK) {
        position.page = value;
    }
    if (nvs_get_i32(handle, "size", &value) == ESP_OK) {
        position.text_size = value;
    }
    nvs_close(handle);
    return position;
}

void SavePosition(const Position& position)
{
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    (void)nvs_set_i32(handle, "book", position.book_index);
    (void)nvs_set_i32(handle, "chapter", position.chapter);
    (void)nvs_set_i32(handle, "page", position.page);
    (void)nvs_set_i32(handle, "size", position.text_size);
    (void)nvs_commit(handle);
    nvs_close(handle);
}

}  // namespace bible_service
