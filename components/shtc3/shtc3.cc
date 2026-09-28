#include "shtc3.h"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char* kTag = "Shtc3";

// Sensirion SHTC3 commands (16-bit, MSB first).
constexpr uint16_t kCommandWakeup = 0x3517;
constexpr uint16_t kCommandSleep = 0xB098;
constexpr uint16_t kCommandSoftReset = 0x805D;
constexpr uint16_t kCommandReadId = 0xEFC8;
// Normal mode (up to 12.1 ms), no clock stretching, temperature first.
constexpr uint16_t kCommandMeasureNormalTFirst = 0x7866;

constexpr uint32_t kWakeupDelayUs = 240;  // datasheet wakeup time
constexpr uint32_t kSoftResetDelayMs = 1; // datasheet reset time
constexpr uint32_t kMeasureDelayMs = 15;  // margin over the 12.1 ms worst case
constexpr int kMeasurementAttempts = 2;

// Sensirion CRC-8: polynomial 0x31, init 0xFF, no reflection, no final XOR.
uint8_t Crc8(const uint8_t* data, size_t length)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31)
                               : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

}  // namespace

Shtc3::Shtc3(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {}

esp_err_t Shtc3::SendCommand(uint16_t command)
{
    const uint8_t payload[2] = {
        static_cast<uint8_t>(command >> 8),
        static_cast<uint8_t>(command & 0xFF),
    };
    return WriteBytes(payload, sizeof(payload));
}

esp_err_t Shtc3::WakeUp()
{
    const esp_err_t err = SendCommand(kCommandWakeup);
    if (err != ESP_OK) {
        return err;
    }
    esp_rom_delay_us(kWakeupDelayUs);
    return ESP_OK;
}

esp_err_t Shtc3::Sleep()
{
    return SendCommand(kCommandSleep);
}

esp_err_t Shtc3::Initialize(bool log_failures)
{
    esp_err_t err = WakeUp();
    if (err == ESP_OK) {
        err = SendCommand(kCommandSoftReset);
    }
    if (err != ESP_OK) {
        if (log_failures) {
            ESP_LOGW(kTag, "Soft reset failed: %s", esp_err_to_name(err));
        }
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(kSoftResetDelayMs));

    uint16_t identity = 0;
    err = ReadIdentity(&identity, log_failures);
    if (err != ESP_OK) {
        return err;
    }
    // Datasheet: bits [11:5] carry the SHTC3 product code (0x0807 pattern).
    if ((identity & 0x083F) == 0x0807) {
        ESP_LOGI(kTag, "SHTC3 identified: id=0x%04X", identity);
    } else {
        // Non-fatal: the mask is an interpretation of the datasheet ID layout.
        ESP_LOGW(kTag, "Unexpected identity 0x%04X; continuing", identity);
    }
    return Sleep();
}

esp_err_t Shtc3::ReadIdentity(uint16_t* out_id, bool log_failures)
{
    if (out_id == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = WakeUp();
    if (err == ESP_OK) {
        err = SendCommand(kCommandReadId);
    }
    // The identity register answers two bytes with no CRC.
    uint8_t raw[2] = {};
    if (err == ESP_OK) {
        err = ReadBytes(raw, sizeof(raw));
    }
    if (err != ESP_OK) {
        if (log_failures) {
            ESP_LOGW(kTag, "Identity read failed: %s", esp_err_to_name(err));
        }
        return err;
    }

    *out_id = static_cast<uint16_t>((raw[0] << 8) | raw[1]);
    return ESP_OK;
}

esp_err_t Shtc3::ReadRawMeasurement(uint16_t* out_raw_temperature, uint16_t* out_raw_humidity)
{
    // Six bytes: T MSB, T LSB, CRC, RH MSB, RH LSB, CRC.
    uint8_t raw[6] = {};
    const esp_err_t err = ReadBytes(raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }
    if (Crc8(&raw[0], 2) != raw[2] || Crc8(&raw[3], 2) != raw[5]) {
        return ESP_ERR_INVALID_CRC;
    }

    if (out_raw_temperature != nullptr) {
        *out_raw_temperature = static_cast<uint16_t>((raw[0] << 8) | raw[1]);
    }
    if (out_raw_humidity != nullptr) {
        *out_raw_humidity = static_cast<uint16_t>((raw[3] << 8) | raw[4]);
    }
    return ESP_OK;
}

esp_err_t Shtc3::ReadMeasurement(float* out_temperature_c, float* out_humidity_rh)
{
    if (out_temperature_c == nullptr || out_humidity_rh == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_FAIL;
    uint16_t raw_temperature = 0;
    uint16_t raw_humidity = 0;
    for (int attempt = 0; attempt < kMeasurementAttempts; ++attempt) {
        err = WakeUp();
        if (err == ESP_OK) {
            err = SendCommand(kCommandMeasureNormalTFirst);
        }
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(kMeasureDelayMs));
            err = ReadRawMeasurement(&raw_temperature, &raw_humidity);
        }
        (void)Sleep();
        if (err == ESP_OK) {
            break;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Measurement failed: %s", esp_err_to_name(err));
        return err;
    }

    // T[°C] = -45 + 175 * raw / 65536; RH[%] = 100 * raw / 65536.
    *out_temperature_c = -45.0f + 175.0f * (static_cast<float>(raw_temperature) / 65536.0f);
    *out_humidity_rh = 100.0f * (static_cast<float>(raw_humidity) / 65536.0f);
    return ESP_OK;
}
