#ifndef BOOK_PAGE_RUNTIME_H_
#define BOOK_PAGE_RUNTIME_H_

#include <string>

#include "display_service.h"
#include "esp_err.h"
#include "page_action_result.h"

namespace book_page_runtime {

// Loads the book from SD and publishes the page state; call before showing.
esp_err_t Prepare(const std::string& filename);
esp_err_t UpdateDisplayState();
esp_err_t UpdateDisplayStateAndRequestRefresh(
    display_service::RefreshMode refresh_mode = display_service::RefreshMode::kPartial);

// UP/DOWN turn pages and stop at the ends of the file.
page_actions::FocusMoveOutcome MoveFocus(int delta);
// OK opens the reader menu (select modal).
bool OpenMenu();

// Returns true when the submitted select modal belonged to the book reader.
bool HandleSelectModalSubmit(int selected_index);
void ClearPendingSelectModal();

// "Exit" is deferred so the screen change happens after input dispatch returns.
bool ConsumePendingExit();

}  // namespace book_page_runtime

#endif  // BOOK_PAGE_RUNTIME_H_
