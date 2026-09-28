#ifndef OTA_PROMPT_RUNTIME_H_
#define OTA_PROMPT_RUNTIME_H_

#include "ota_service.h"

// Owns the on-device UI for firmware updates: the "install now / later" prompt and the
// progress/result toasts. ota_service stays UI-free.
namespace ota_prompt_runtime {

void HandleOtaEvent(const ota_service::Event& event);
// Returns true when the submitted select modal was the update prompt.
bool HandleSelectModalSubmit(int selected_index);
void ClearPendingSelectModal();

}  // namespace ota_prompt_runtime

#endif  // OTA_PROMPT_RUNTIME_H_
