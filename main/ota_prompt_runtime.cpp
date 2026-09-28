#include "ota_prompt_runtime.h"

#include <mutex>
#include <string>

#include "epaper_ui/select_modal.h"
#include "epaper_ui/toast.h"
#include "esp_log.h"
#include "overlay_runtime.h"
#include "project_assets.h"
#include "recording_session_service.h"
#include "transcription_service.h"

namespace ota_prompt_runtime {
namespace {

constexpr const char* kTag = "OtaPrompt";
constexpr int kInstallIndex = 0;
constexpr uint32_t kResultToastMs = 3000;

std::mutex s_mutex;
bool s_prompt_active = false;
bool s_install_requested = false;
// The version the user postponed; it is not offered again until a newer one appears.
std::string s_declined_version = {};

epaper_ui::ToastState BuildToast(const std::string& text, EmbeddedIconId icon)
{
    epaper_ui::ToastState toast = {};
    toast.visible = true;
    toast.body_text = text;
    toast.leading_icon = project_assets::GetIcon(icon);
    return toast;
}

bool IsRecordingIdle()
{
    const recording_session_service::Phase phase = recording_session_service::GetSnapshot().phase;
    return (phase == recording_session_service::Phase::kIdle ||
            phase == recording_session_service::Phase::kComplete ||
            phase == recording_session_service::Phase::kFailed) &&
           !transcription_service::GetSnapshot().request_in_flight;
}

void MaybeShowPrompt(const std::string& version)
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_prompt_active || version == s_declined_version) {
            return;
        }
    }
    // Never stack on top of another modal or interrupt a recording; the next check retries.
    if (overlay_runtime::IsInputCaptured() || !IsRecordingIdle()) {
        ESP_LOGI(kTag, "Update %s available; prompt deferred", version.c_str());
        return;
    }

    epaper_ui::SelectModalState state = {};
    state.visible = true;
    state.title_text = "Update to " + version + "?";
    state.selected_index = kInstallIndex;
    state.items.push_back({.label_text = "Install now"});
    state.items.push_back({.label_text = "Later"});
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_prompt_active = true;
    }
    const esp_err_t err = overlay_runtime::ShowSelectModal(state);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Show update prompt failed: %s", esp_err_to_name(err));
        std::lock_guard<std::mutex> lock(s_mutex);
        s_prompt_active = false;
    }
}

}  // namespace

void HandleOtaEvent(const ota_service::Event& event)
{
    const ota_service::Snapshot& snapshot = event.snapshot;
    switch (snapshot.state) {
        case ota_service::State::kUpdateAvailable:
            MaybeShowPrompt(snapshot.available_version);
            return;
        case ota_service::State::kDownloading:
            (void)overlay_runtime::ShowToast(
                BuildToast("Updating " + std::to_string(snapshot.progress_percent) + "%",
                           EmbeddedIconId::kTranscribe));
            return;
        case ota_service::State::kInstalled:
            (void)overlay_runtime::ShowToast(
                BuildToast("Update installed. Restarting", EmbeddedIconId::kCheck));
            return;
        case ota_service::State::kFailed: {
            // Background check failures stay in the log; only a failed install is surfaced.
            bool install_requested = false;
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                install_requested = s_install_requested;
                s_install_requested = false;
            }
            if (install_requested) {
                (void)overlay_runtime::ShowToastForDuration(
                    BuildToast("Update failed: " + snapshot.error_message,
                               EmbeddedIconId::kClose),
                    kResultToastMs);
            }
            return;
        }
        default:
            return;
    }
}

bool HandleSelectModalSubmit(int selected_index)
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_prompt_active) {
            return false;
        }
        s_prompt_active = false;
    }
    const std::string available_version = ota_service::GetSnapshot().available_version;

    if (selected_index != kInstallIndex) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_declined_version = available_version;
        ESP_LOGI(kTag, "Update %s postponed", available_version.c_str());
        return true;
    }

    if (!IsRecordingIdle()) {
        (void)overlay_runtime::ShowToastForDuration(
            BuildToast("Finish the recording first", EmbeddedIconId::kClose), kResultToastMs);
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_install_requested = true;
    }
    if (!ota_service::StartUpdate()) {
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_install_requested = false;
        }
        (void)overlay_runtime::ShowToastForDuration(
            BuildToast("Update could not start", EmbeddedIconId::kClose), kResultToastMs);
    }
    return true;
}

void ClearPendingSelectModal()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_prompt_active = false;
}

}  // namespace ota_prompt_runtime
