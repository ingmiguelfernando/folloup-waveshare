#include "bible_page_runtime.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>

#include "bible_page_coordinator.h"
#include "bible_service.h"
#include "epaper_ui/select_modal.h"
#include "epaper_ui/toast.h"
#include "overlay_runtime.h"
#include "ui_refresh_runtime.h"

namespace bible_page_runtime {
namespace {

// Partial refreshes accumulate ghosting; flush with a full refresh every few page turns.
constexpr int kFullRefreshEveryTurns = 6;
constexpr uint32_t kAboutToastMs = 6000;

enum class MenuMode : uint8_t {
    kNone = 0,
    kMain,
    kBook,
    kChapter,
    kTextSize,
};

enum MainMenuItem : int {
    kNextChapter = 0,
    kPreviousChapter,
    kGoToBook,
    kGoToChapter,
    kTextSize,
    kAbout,
    kExit,
};

std::mutex s_mutex;
BiblePageCoordinator s_coordinator = {};
MenuMode s_menu_mode = MenuMode::kNone;
int s_pending_book = -1;
int s_turns_since_full = 0;
std::atomic<bool> s_pending_exit{false};

display_service::RefreshMode NextTurnRefreshModeLocked()
{
    if (++s_turns_since_full >= kFullRefreshEveryTurns) {
        s_turns_since_full = 0;
        return display_service::RefreshMode::kFull;
    }
    return display_service::RefreshMode::kPartial;
}

epaper_ui::SelectModalState MakeModalLocked(MenuMode mode, const std::string& title)
{
    s_menu_mode = mode;
    epaper_ui::SelectModalState state = {};
    state.visible = true;
    state.title_text = title;
    return state;
}

epaper_ui::SelectModalState MainMenuLocked()
{
    epaper_ui::SelectModalState state = MakeModalLocked(MenuMode::kMain, "Bible");
    for (const char* label : {"Next chapter", "Previous chapter", "Go to book", "Go to chapter",
                              "Text size", "About", "Exit"}) {
        state.items.push_back({.label_text = label});
    }
    return state;
}

epaper_ui::SelectModalState BookMenuLocked()
{
    epaper_ui::SelectModalState state = MakeModalLocked(MenuMode::kBook, "Book");
    for (const bible_service::Book& book : s_coordinator.books()) {
        state.items.push_back({.label_text = book.name});
    }
    state.selected_index = s_coordinator.book_index();
    return state;
}

std::optional<epaper_ui::SelectModalState> ChapterMenuLocked(int book_index)
{
    const auto& books = s_coordinator.books();
    if (book_index < 0 || book_index >= static_cast<int>(books.size())) {
        return std::nullopt;
    }
    s_pending_book = book_index;
    const bible_service::Book& book = books[static_cast<size_t>(book_index)];
    epaper_ui::SelectModalState state = MakeModalLocked(MenuMode::kChapter, book.name);
    for (int chapter = 1; chapter <= book.chapter_count; ++chapter) {
        state.items.push_back({.label_text = "Chapter " + std::to_string(chapter)});
    }
    state.selected_index =
        book_index == s_coordinator.book_index() ? s_coordinator.chapter() - 1 : 0;
    return state;
}

epaper_ui::SelectModalState TextSizeMenuLocked()
{
    epaper_ui::SelectModalState state = MakeModalLocked(MenuMode::kTextSize, "Text size");
    for (const char* label : {"Small", "Medium", "Large"}) {
        state.items.push_back({.label_text = label});
    }
    state.selected_index = static_cast<int>(s_coordinator.text_size());
    return state;
}

void ShowAboutToast()
{
    epaper_ui::ToastState toast = {};
    toast.visible = true;
    const std::string title = bible_service::Title();
    const std::string copyright = bible_service::Copyright();
    toast.body_text = copyright.empty() ? title : title + ". " + copyright;
    (void)overlay_runtime::ShowToastForDuration(toast, kAboutToastMs);
}

}  // namespace

esp_err_t Prepare()
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_coordinator.Open();
        s_turns_since_full = 0;
        s_menu_mode = MenuMode::kNone;
    }
    return UpdateDisplayState();
}

esp_err_t UpdateDisplayState()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return display_service::SetBiblePageState(s_coordinator.BuildState());
}

esp_err_t UpdateDisplayStateAndRequestRefresh(display_service::RefreshMode refresh_mode)
{
    return ui_refresh_runtime::Schedule(ui_refresh_runtime::SurfaceKey::kBiblePage,
                                        &UpdateDisplayState,
                                        display_service::RefreshRequest{.refresh_mode = refresh_mode});
}

page_actions::FocusMoveOutcome MoveFocus(int delta)
{
    page_actions::FocusMoveOutcome outcome = {};
    display_service::RefreshMode refresh_mode = display_service::RefreshMode::kPartial;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        const bool moved = delta > 0 ? s_coordinator.NextPage() : s_coordinator.PreviousPage();
        // Consume the press even at the ends so it doesn't fall through to other handlers.
        outcome.handled = true;
        if (!moved) {
            return outcome;
        }
        outcome.play_navigation_cue = true;
        refresh_mode = NextTurnRefreshModeLocked();
    }
    (void)UpdateDisplayStateAndRequestRefresh(refresh_mode);
    return outcome;
}

bool OpenMenu()
{
    epaper_ui::SelectModalState state = {};
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_coordinator.available()) {
            // Nothing to navigate without a Bible; OK leaves the page.
            s_pending_exit.store(true, std::memory_order_relaxed);
            return true;
        }
        state = MainMenuLocked();
    }
    return overlay_runtime::ShowSelectModal(state) == ESP_OK;
}

bool HandleSelectModalSubmit(int selected_index)
{
    bool page_changed = false;
    bool show_about = false;
    std::optional<epaper_ui::SelectModalState> next_modal;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        const MenuMode mode = s_menu_mode;
        s_menu_mode = MenuMode::kNone;
        switch (mode) {
            case MenuMode::kNone:
                return false;
            case MenuMode::kMain:
                switch (selected_index) {
                    case kNextChapter:
                        page_changed = s_coordinator.NextChapter();
                        break;
                    case kPreviousChapter:
                        page_changed = s_coordinator.PreviousChapter();
                        break;
                    case kGoToBook:
                        next_modal = BookMenuLocked();
                        break;
                    case kGoToChapter:
                        next_modal = ChapterMenuLocked(s_coordinator.book_index());
                        break;
                    case kTextSize:
                        next_modal = TextSizeMenuLocked();
                        break;
                    case kAbout:
                        show_about = true;
                        break;
                    case kExit:
                        s_pending_exit.store(true, std::memory_order_relaxed);
                        break;
                    default:
                        break;
                }
                break;
            case MenuMode::kBook:
                next_modal = ChapterMenuLocked(selected_index);
                break;
            case MenuMode::kChapter:
                page_changed = s_coordinator.GoTo(s_pending_book, selected_index + 1);
                break;
            case MenuMode::kTextSize:
                s_coordinator.SetTextSize(
                    static_cast<epaper_ui::BibleTextSize>(std::clamp(selected_index, 0, 2)));
                page_changed = true;
                break;
        }
        if (page_changed) {
            s_turns_since_full = 0;
        }
    }
    if (next_modal) {
        (void)overlay_runtime::ShowSelectModal(*next_modal);
    }
    if (show_about) {
        ShowAboutToast();
    }
    if (page_changed) {
        (void)UpdateDisplayStateAndRequestRefresh(display_service::RefreshMode::kFull);
    }
    return true;
}

void ClearPendingSelectModal()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_menu_mode = MenuMode::kNone;
}

bool ConsumePendingExit()
{
    return s_pending_exit.exchange(false, std::memory_order_relaxed);
}

}  // namespace bible_page_runtime
