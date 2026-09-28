#pragma once

#include <cstdint>
#include <string>

#include "esp_err.h"

namespace ota_service {

enum class State : uint8_t {
    kIdle = 0,
    kChecking,
    kUpToDate,
    kUpdateAvailable,
    kDownloading,
    kInstalled,
    kFailed,
};

struct Snapshot {
    State state = State::kIdle;
    std::string running_version = {};
    std::string available_version = {};
    int progress_percent = 0;
    // True when the running image was just installed by OTA and is not yet confirmed.
    bool pending_verify = false;
    std::string error_message = {};
};

struct Event {
    Snapshot snapshot = {};
};

using EventHandler = void (*)(const Event& event, void* context);

esp_err_t Init();
void SetEventHandler(EventHandler handler, void* context);
Snapshot GetSnapshot();
bool IsBusy();

// The first connection confirms a freshly installed image (cancelling rollback) and
// schedules an update check; later connections re-check at most once a day.
void SetNetworkConnected(bool connected);

bool CheckForUpdate();
// Downloads the release announced by the last check, then reboots into it.
bool StartUpdate();

const char* StateName(State state);

}  // namespace ota_service
