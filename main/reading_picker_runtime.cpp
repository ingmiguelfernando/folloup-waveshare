#include "reading_picker_runtime.h"

#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "book_service.h"
#include "epaper_ui/select_modal.h"
#include "overlay_runtime.h"

namespace reading_picker_runtime {
namespace {

std::mutex s_mutex;
bool s_menu_open = false;
std::vector<book_service::BookEntry> s_entries = {};
bool s_shows_continue = false;
std::string s_last_opened = {};
bool s_pending = false;
Selection s_pending_selection = {};

}  // namespace

bool Open()
{
    // Refresh the list so books added to the SD since boot show up.
    (void)book_service::Load();

    std::vector<book_service::BookEntry> entries = book_service::Books();
    const std::string last_opened = book_service::LoadLastOpened();

    epaper_ui::SelectModalState state = {};
    state.visible = true;
    state.title_text = "Read";
    // "Continue" jumps straight back into the last opened book (when it is
    // still on the card); otherwise the row is hidden.
    bool shows_continue = false;
    std::string continue_title;
    if (!last_opened.empty()) {
        for (const book_service::BookEntry& entry : entries) {
            if (entry.filename == last_opened) {
                shows_continue = true;
                continue_title = entry.title;
                break;
            }
        }
    }
    if (shows_continue) {
        state.items.push_back({.label_text = "Continue " + continue_title});
    }
    state.items.push_back({.label_text = "Bible"});
    for (const book_service::BookEntry& entry : entries) {
        state.items.push_back({.label_text = entry.title});
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_entries = std::move(entries);
        s_shows_continue = shows_continue;
        s_last_opened = last_opened;
        s_menu_open = true;
    }
    return overlay_runtime::ShowSelectModal(state) == ESP_OK;
}

bool HandleSelectModalSubmit(int selected_index)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_menu_open) {
        return false;
    }
    s_menu_open = false;

    Selection selection = {};
    size_t row = static_cast<size_t>(selected_index);
    if (s_shows_continue) {
        if (row == 0) {
            selection.book_filename = s_last_opened;
            s_pending_selection = std::move(selection);
            s_pending = true;
            return true;
        }
        --row;
    }
    if (row == 0) {
        selection.open_bible = true;
    } else {
        const size_t entry_index = row - 1;
        if (entry_index >= s_entries.size()) {
            return true;  // consumed, but an out-of-range row selects nothing
        }
        selection.book_filename = s_entries[entry_index].filename;
    }
    s_pending_selection = std::move(selection);
    s_pending = true;
    return true;
}

void ClearPendingSelectModal()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_menu_open = false;
}

bool ConsumePendingSelection(Selection* out_selection)
{
    if (out_selection == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_pending) {
        return false;
    }
    s_pending = false;
    *out_selection = s_pending_selection;
    s_pending_selection = {};
    return true;
}

}  // namespace reading_picker_runtime
