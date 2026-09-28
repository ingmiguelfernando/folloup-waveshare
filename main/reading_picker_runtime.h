#ifndef READING_PICKER_RUNTIME_H_
#define READING_PICKER_RUNTIME_H_

#include <string>

#include "esp_err.h"

namespace reading_picker_runtime {

struct Selection {
    bool open_bible = false;
    std::string book_filename = {};  // non-empty: open this .txt book
};

// Opens the unified "Read" selector: the Bible first, then the .txt books on
// the SD card. Refreshes the book list on every open.
bool Open();

// Returns true when the submitted select modal belonged to the picker.
bool HandleSelectModalSubmit(int selected_index);
void ClearPendingSelectModal();

// Pending choice; drained by app_shell after input dispatch returns.
bool ConsumePendingSelection(Selection* out_selection);

}  // namespace reading_picker_runtime

#endif  // READING_PICKER_RUNTIME_H_
