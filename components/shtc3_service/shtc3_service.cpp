#include "shtc3_service.h"

#include <mutex>
#include <new>

#include "esp_log.h"
#include "esp_timer.h"
#include "followup_task_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "shtc3.h"
#include "waveshare_board.h"
#include "waveshare_board_config.h"

namespace shtc3_service {
namespace {

constexpr const char* kTag = "Shtc3Service";
// Board spec section 9.2: poll slowly and cache; UI must never do synchronous I2C.
constexpr uint32_t kPollIntervalMs = 30 * 1000;
constexpr uint32_t kPollTaskStackWords = 3072;
// The sensor sits next to the ESP32-S3 and the charger, so readings run warm
// while Wi-Fi is active or the battery charges; calibrated via Kconfig.
constexpr float kTemperatureOffsetC =
    static_cast<float>(CONFIG_FOLLOWUP_SHTC3_TEMPERATURE_OFFSET_TENTHS) / 10.0f;

i2c_master_bus_handle_t s_sensor_bus = nullptr;
Shtc3* s_sensor = nullptr;
bool s_initialized = false;
std::mutex s_mutex;
Reading s_reading = {};
TaskHandle_t s_poll_task = nullptr;

void PublishReading(float temperature_c, float humidity_rh)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_reading.temperature_c = temperature_c + kTemperatureOffsetC;
    s_reading.humidity_rh = humidity_rh;
    s_reading.valid = true;
    s_reading.timestamp_us = esp_timer_get_time();
    s_reading.consecutive_errors = 0;
}

void PollTask(void*)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));

        float temperature_c = 0.0f;
        float humidity_rh = 0.0f;
        const esp_err_t err = s_sensor->ReadMeasurement(&temperature_c, &humidity_rh);
        if (err != ESP_OK) {
            std::lock_guard<std::mutex> lock(s_mutex);
            ++s_reading.consecutive_errors;
            continue;
        }
        PublishReading(temperature_c, humidity_rh);
    }
}

}  // namespace

esp_err_t Init()
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(kTag, "Initializing SHTC3: addr=0x%02X bus=%d scl=GPIO%d sda=GPIO%d",
             WAVESHARE_SHTC3_I2C_ADDR, static_cast<int>(WAVESHARE_SENSOR_I2C_PORT),
             static_cast<int>(WAVESHARE_SENSOR_I2C_SCL_PIN),
             static_cast<int>(WAVESHARE_SENSOR_I2C_SDA_PIN));

    esp_err_t err = waveshare_board::EnsureSensorI2cBus(&s_sensor_bus);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Sensor I2C bus unavailable: %s", esp_err_to_name(err));
        return err;
    }

    s_sensor = new (std::nothrow) Shtc3(s_sensor_bus, WAVESHARE_SHTC3_I2C_ADDR);
    if (s_sensor == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    err = s_sensor->Initialize(true);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "SHTC3 init failed: %s", esp_err_to_name(err));
        delete s_sensor;
        s_sensor = nullptr;
        return err;
    }

    if (xTaskCreatePinnedToCore(PollTask,
                                "shtc3_poll",
                                kPollTaskStackWords,
                                nullptr,
                                followup_task_config::kPrioritySensorPoll,
                                &s_poll_task,
                                followup_task_config::kSystemCore) != pdPASS) {
        // Non-fatal: the service still answers LogDebugStatus/ReadReading, the
        // cache just stays at whatever the boot reading produced.
        s_poll_task = nullptr;
        ESP_LOGW(kTag, "SHTC3 poll task create failed");
    }

    s_initialized = true;
    ESP_LOGI(kTag, "SHTC3 service initialized");
    return ESP_OK;
}

bool IsInitialized()
{
    return s_initialized;
}

esp_err_t ReadReading(Reading* out_reading)
{
    if (out_reading == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    std::lock_guard<std::mutex> lock(s_mutex);
    *out_reading = s_reading;
    return s_reading.valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void LogDebugStatus()
{
    if (!s_initialized || s_sensor == nullptr) {
        ESP_LOGW(kTag, "SHTC3 unavailable");
        return;
    }

    float temperature_c = 0.0f;
    float humidity_rh = 0.0f;
    const esp_err_t err = s_sensor->ReadMeasurement(&temperature_c, &humidity_rh);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "First reading failed: %s", esp_err_to_name(err));
        return;
    }
    PublishReading(temperature_c, humidity_rh);

    Reading reading = {};
    (void)ReadReading(&reading);
    ESP_LOGI(kTag, "Environment temp=%.2fC humidity=%.1f%%", reading.temperature_c,
             reading.humidity_rh);
}

}  // namespace shtc3_service
