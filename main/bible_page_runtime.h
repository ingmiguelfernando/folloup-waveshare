#ifndef BIBLE_PAGE_RUNTIME_H_
#define BIBLE_PAGE_RUNTIME_H_

#include "display_service.h"
#include "esp_err.h"
#include "page_action_result.h"

namespace bible_page_runtime {

// Loads the Bible from SD (first time) and publishes the page state; call before showing.
esp_err_t Prepare();
esp_err_t UpdateDisplayState();
esp_err_t UpdateDisplayStateAndRequestRefresh(
    display_service::RefreshMode refresh_mode = display_service::RefreshMode::kPartial);

// UP/DOWN turn pages, crossing into the neighbouring chapter at either end.
page_actions::FocusMoveOutcome MoveFocus(int delta);
// OK opens the reader menu (select modal).
bool OpenMenu();

// Returns true when the submitted select modal belonged to the Bible reader.
bool HandleSelectModalSubmit(int selected_index);
void ClearPendingSelectModal();

// "Exit" is deferred so the screen change happens after input dispatch returns.
bool ConsumePendingExit();

}  // namespace bible_page_runtime

#endif  // BIBLE_PAGE_RUNTIME_H_
