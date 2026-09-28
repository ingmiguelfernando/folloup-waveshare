#ifndef SHTC3_H
#define SHTC3_H

#include <cstdint>

#include "i2c_device.h"

// Sensirion SHTC3 temperature/humidity sensor on the shared sensor I2C bus.
//
// Unlike the register-oriented chips this driver wraps, the SHTC3 speaks in
// 16-bit commands (MSB first) answered by raw byte payloads, so everything goes
// through I2cDevice::WriteBytes/ReadBytes. Command and sequence facts follow
// section 9.2 of docs/waveshare-epaper-hardware-spec.md (Sensirion datasheet).
class Shtc3 : public I2cDevice {
public:
    static constexpr uint8_t kDefaultAddress = 0x70;

    explicit Shtc3(i2c_master_bus_handle_t i2c_bus, uint8_t addr = kDefaultAddress);

    // Wakeup, soft reset and identity read. An unexpected identity is logged
    // but not fatal (the ID mask is a datasheet interpretation).
    esp_err_t Initialize(bool log_failures = true);
    esp_err_t ReadIdentity(uint16_t* out_id, bool log_failures = true);
    // One normal-mode measurement (temperature first): wakes the sensor,
    // measures, then puts it back to sleep. Writes degrees C and %RH.
    esp_err_t ReadMeasurement(float* out_temperature_c, float* out_humidity_rh);

private:
    esp_err_t SendCommand(uint16_t command);
    esp_err_t WakeUp();
    esp_err_t Sleep();
    esp_err_t ReadRawMeasurement(uint16_t* out_raw_temperature, uint16_t* out_raw_humidity);
};

#endif  // SHTC3_H
