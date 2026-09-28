#include "book_page_runtime.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>

#include "book_page_coordinator.h"
#include "epaper_ui/select_modal.h"
#include "epaper_ui/toast.h"
#include "overlay_runtime.h"
#include "ui_refresh_runtime.h"

namespace book_page_runtime {
namespace {

// Partial refreshes accumulate ghosting; flush with a full refresh every few page turns.
constexpr int kFullRefreshEveryTurns = 6;
constexpr uint32_t kAboutToastMs = 6000;

enum class MenuMode : uint8_t {
    kNone = 0,
    kMain,
    kPage,
    kTextSize,
};

enum MainMenuItem : int {
    kGoToPage = 0,
    kTextSize,
    kAbout,
    kExit,
};

std::mutex s_mutex;
BookPageCoordinator s_coordinator = {};
MenuMode s_menu_mode = MenuMode::kNone;
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
    epaper_ui::SelectModalState state = MakeModalLocked(MenuMode::kMain, "Book");
    for (const char* label : {"Go to page", "Text size", "About", "Exit"}) {
        state.items.push_back({.label_text = label});
    }
    return state;
}

epaper_ui::SelectModalState PageMenuLocked()
{
    epaper_ui::SelectModalState state = MakeModalLocked(MenuMode::kPage, "Go to page");
    for (int page = 1; page <= s_coordinator.page_count(); ++page) {
        state.items.push_back({.label_text = "Page " + std::to_string(page)});
    }
    state.selected_index = s_coordinator.page();
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
    const long kilobytes = static_cast<long>(s_coordinator.size_bytes() / 1024);
    toast.body_text = s_coordinator.filename() + " (" + std::to_string(kilobytes) + " KB)";
    (void)overlay_runtime::ShowToastForDuration(toast, kAboutToastMs);
}

}  // namespace

esp_err_t Prepare(const std::string& filename)
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_coordinator.Open(filename);
        s_turns_since_full = 0;
        s_menu_mode = MenuMode::kNone;
    }
    return UpdateDisplayState();
}

esp_err_t UpdateDisplayState()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return display_service::SetBookPageState(s_coordinator.BuildState());
}

esp_err_t UpdateDisplayStateAndRequestRefresh(display_service::RefreshMode refresh_mode)
{
    return ui_refresh_runtime::Schedule(ui_refresh_runtime::SurfaceKey::kBookPage,
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
            // Nothing to navigate without a book; OK leaves the page.
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
                    case kGoToPage:
                        next_modal = PageMenuLocked();
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
            case MenuMode::kPage:
                page_changed = s_coordinator.GoToPage(selected_index);
                break;
            case MenuMode::kTextSize:
                s_coordinator.SetTextSize(
                    static_cast<epaper_ui::ReaderTextSize>(std::clamp(selected_index, 0, 2)));
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

}  // namespace book_page_runtime
