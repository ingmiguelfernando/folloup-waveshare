#include "ota_service.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "followup_task_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace ota_service {
namespace {

constexpr const char* kTag = "OtaService";
constexpr const char* kVersionAsset = "/version.txt";
constexpr const char* kFirmwareAsset = "/followup-app.bin";
constexpr size_t kMaxVersionLength = 64;
constexpr int kHttpTimeoutMs = 30000;
// GitHub release downloads redirect to a signed URL several hundred bytes long.
constexpr int kHttpBufferSize = 2048;
// Flash writes need an internal-RAM stack, and the TLS handshake needs a deep one.
constexpr uint32_t kWorkerStackBytes = 8192;
constexpr int kProgressStepPercent = 20;
constexpr uint64_t kFirstCheckDelayUs = 30ULL * 1000 * 1000;
constexpr int64_t kRecheckIntervalUs = 24LL * 60 * 60 * 1000 * 1000;
constexpr TickType_t kRestartDelay = pdMS_TO_TICKS(3000);

enum class Job : uint8_t {
    kCheck,
    kUpdate,
};

std::mutex s_mutex;
bool s_initialized = false;
Snapshot s_snapshot = {};
EventHandler s_event_handler = nullptr;
void* s_event_context = nullptr;
bool s_worker_running = false;
bool s_image_confirmed = false;
int64_t s_last_check_us = 0;
esp_timer_handle_t s_check_timer = nullptr;

void Notify()
{
    EventHandler handler = nullptr;
    void* context = nullptr;
    Event event = {};
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        handler = s_event_handler;
        context = s_event_context;
        event.snapshot = s_snapshot;
    }
    if (handler != nullptr) {
        handler(event, context);
    }
}

void SetState(State state, const std::string& error_message = {})
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.state = state;
        s_snapshot.error_message = error_message;
    }
    Notify();
}

std::string BaseUrl()
{
    std::string base = CONFIG_FOLLOWUP_OTA_URL_BASE;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base;
}

std::string Trim(const std::string& text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

esp_err_t VersionHttpEventHandler(esp_http_client_event_t* event)
{
    auto* body = static_cast<std::string*>(event->user_data);
    if (body == nullptr) {
        return ESP_OK;
    }
    if (event->event_id == HTTP_EVENT_REDIRECT) {
        body->clear();
    } else if (event->event_id == HTTP_EVENT_ON_DATA && event->data != nullptr &&
               event->data_len > 0 && esp_http_client_get_status_code(event->client) == 200) {
        const size_t room = kMaxVersionLength - std::min(body->size(), kMaxVersionLength);
        body->append(static_cast<const char*>(event->data),
                     std::min(room, static_cast<size_t>(event->data_len)));
    }
    return ESP_OK;
}

void RunCheck()
{
    SetState(State::kChecking);

    const std::string url = BaseUrl() + kVersionAsset;
    std::string body;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = kHttpBufferSize;
    config.buffer_size_tx = kHttpBufferSize;
    config.event_handler = &VersionHttpEventHandler;
    config.user_data = &body;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        SetState(State::kFailed, "HTTP client init failed");
        return;
    }
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_last_check_us = esp_timer_get_time();
    }

    const std::string available = Trim(body);
    if (err != ESP_OK || status != 200 || available.empty()) {
        ESP_LOGW(kTag, "Version check failed: err=%s http=%d url=%s", esp_err_to_name(err),
                 status, url.c_str());
        SetState(State::kFailed, "Version check failed");
        return;
    }

    std::string running;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.available_version = available;
        running = s_snapshot.running_version;
    }
    const bool update_available = available != running;
    ESP_LOGI(kTag, "Version check: running=%s available=%s update=%d", running.c_str(),
             available.c_str(), update_available ? 1 : 0);
    SetState(update_available ? State::kUpdateAvailable : State::kUpToDate);
}

void RunUpdate()
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.progress_percent = 0;
    }
    SetState(State::kDownloading);

    const std::string url = BaseUrl() + kFirmwareAsset;
    esp_http_client_config_t http_config = {};
    http_config.url = url.c_str();
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    http_config.timeout_ms = kHttpTimeoutMs;
    http_config.buffer_size = kHttpBufferSize;
    http_config.buffer_size_tx = kHttpBufferSize;
    http_config.keep_alive_enable = true;

    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &http_config;

    esp_https_ota_handle_t handle = nullptr;
    esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "OTA begin failed: %s url=%s", esp_err_to_name(err), url.c_str());
        SetState(State::kFailed, "Download failed");
        return;
    }

    esp_app_desc_t new_desc = {};
    err = esp_https_ota_get_img_desc(handle, &new_desc);
    const esp_app_desc_t* running_desc = esp_app_get_description();
    if (err != ESP_OK ||
        strncmp(new_desc.project_name, running_desc->project_name,
                sizeof(new_desc.project_name)) != 0) {
        ESP_LOGE(kTag, "OTA image rejected: err=%s project=%.*s", esp_err_to_name(err),
                 static_cast<int>(sizeof(new_desc.project_name)), new_desc.project_name);
        esp_https_ota_abort(handle);
        SetState(State::kFailed, "Wrong firmware image");
        return;
    }
    ESP_LOGI(kTag, "Installing firmware %.*s", static_cast<int>(sizeof(new_desc.version)),
             new_desc.version);

    const int image_size = esp_https_ota_get_image_size(handle);
    int last_reported = 0;
    while (true) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        if (image_size > 0) {
            const int percent = static_cast<int>(
                (static_cast<int64_t>(esp_https_ota_get_image_len_read(handle)) * 100) /
                image_size);
            if (percent - last_reported >= kProgressStepPercent) {
                last_reported = percent - (percent % kProgressStepPercent);
                {
                    std::lock_guard<std::mutex> lock(s_mutex);
                    s_snapshot.progress_percent = last_reported;
                }
                Notify();
            }
        }
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(kTag, "OTA download failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        SetState(State::kFailed, "Download failed");
        return;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "OTA finish failed: %s", esp_err_to_name(err));
        SetState(State::kFailed, err == ESP_ERR_OTA_VALIDATE_FAILED ? "Image validation failed"
                                                                     : "Install failed");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_snapshot.progress_percent = 100;
    }
    ESP_LOGI(kTag, "OTA installed; restarting");
    SetState(State::kInstalled);
    vTaskDelay(kRestartDelay);
    esp_restart();
}

void WorkerTask(void* arg)
{
    const Job job = static_cast<Job>(reinterpret_cast<uintptr_t>(arg));
    if (job == Job::kCheck) {
        RunCheck();
    } else {
        RunUpdate();
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_worker_running = false;
    }
    vTaskDelete(nullptr);
}

bool StartWorker(Job job)
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_initialized || s_worker_running) {
            return false;
        }
        if (job == Job::kUpdate && s_snapshot.state != State::kUpdateAvailable) {
            return false;
        }
        s_worker_running = true;
    }

    const BaseType_t created = xTaskCreatePinnedToCore(
        WorkerTask, job == Job::kCheck ? "ota_check" : "ota_update", kWorkerStackBytes,
        reinterpret_cast<void*>(static_cast<uintptr_t>(job)),
        followup_task_config::kPriorityGemini, nullptr, followup_task_config::kSystemCore);
    if (created != pdPASS) {
        ESP_LOGE(kTag, "Failed to start OTA worker");
        std::lock_guard<std::mutex> lock(s_mutex);
        s_worker_running = false;
        return false;
    }
    return true;
}

void CheckTimerCallback(void*)
{
    (void)StartWorker(Job::kCheck);
}

void ConfirmRunningImage()
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_image_confirmed || !s_snapshot.pending_verify) {
            s_image_confirmed = true;
            return;
        }
        s_image_confirmed = true;
        s_snapshot.pending_verify = false;
    }
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(kTag, "Confirmed new firmware after network connect: %s", esp_err_to_name(err));
}

}  // namespace

esp_err_t Init()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_initialized) {
        return ESP_OK;
    }

    const esp_app_desc_t* desc = esp_app_get_description();
    s_snapshot.running_version = desc->version;

    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    s_snapshot.pending_verify = running != nullptr &&
                                esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
                                ota_state == ESP_OTA_IMG_PENDING_VERIFY;

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = &CheckTimerCallback;
    timer_args.name = "ota_check";
    const esp_err_t err = esp_timer_create(&timer_args, &s_check_timer);
    if (err != ESP_OK) {
        return err;
    }

    s_initialized = true;
    ESP_LOGI(kTag, "Running firmware %s (%s) pending_verify=%d url=%s", desc->version,
             running != nullptr ? running->label : "?", s_snapshot.pending_verify ? 1 : 0,
             CONFIG_FOLLOWUP_OTA_URL_BASE);
    return ESP_OK;
}

void SetEventHandler(EventHandler handler, void* context)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_event_handler = handler;
    s_event_context = context;
}

Snapshot GetSnapshot()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_snapshot;
}

bool IsBusy()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_worker_running;
}

void SetNetworkConnected(bool connected)
{
    if (!connected) {
        return;
    }
    ConfirmRunningImage();

    bool schedule = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_initialized || s_worker_running || esp_timer_is_active(s_check_timer)) {
            return;
        }
        schedule = s_last_check_us == 0 ||
                   esp_timer_get_time() - s_last_check_us >= kRecheckIntervalUs;
    }
    if (schedule) {
        // Let Gemini auth and SNTP finish first so TLS sessions don't pile up.
        (void)esp_timer_start_once(s_check_timer, kFirstCheckDelayUs);
    }
}

bool CheckForUpdate()
{
    return StartWorker(Job::kCheck);
}

bool StartUpdate()
{
    return StartWorker(Job::kUpdate);
}

const char* StateName(State state)
{
    switch (state) {
        case State::kIdle:
            return "idle";
        case State::kChecking:
            return "checking";
        case State::kUpToDate:
            return "up_to_date";
        case State::kUpdateAvailable:
            return "update_available";
        case State::kDownloading:
            return "downloading";
        case State::kInstalled:
            return "installed";
        case State::kFailed:
            return "failed";
    }
    return "unknown";
}

}  // namespace ota_service
