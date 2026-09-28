#ifndef SHTC3_SERVICE_H
#define SHTC3_SERVICE_H

#include <cstdint>

#include "esp_err.h"

namespace shtc3_service {

// Latest environment reading. A low-rate poll task feeds the cache so UI
// refreshes never block on sensor I2C (same policy as power_service).
struct Reading {
    float temperature_c = 0.0f;
    float humidity_rh = 0.0f;
    bool valid = false;
    int64_t timestamp_us = 0;
    uint32_t consecutive_errors = 0;
};

esp_err_t Init();
bool IsInitialized();
// Copies the cached reading; never touches the I2C bus. Returns
// ESP_ERR_INVALID_STATE until the first successful poll lands.
esp_err_t ReadReading(Reading* out_reading);
void LogDebugStatus();

}  // namespace shtc3_service

#endif  // SHTC3_SERVICE_H
