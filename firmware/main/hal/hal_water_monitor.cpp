/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "board/config.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <driver/i2c_master.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mooncake_log.h>
#include <nvs.h>

static const std::string_view _tag = "HAL-Water";

static constexpr uint8_t kMiniScalesAddress = 0x26;
static constexpr uint8_t kWeightFloatReg    = 0x10;
static constexpr uint8_t kFirmwareReg       = 0xFE;
static constexpr int kI2cTimeoutMs          = 1000;
static constexpr const char* kNvsNamespace  = "water_monitor";
static constexpr const char* kBaselineKey   = "baseline_cg";

static i2c_master_bus_handle_t _port_a_bus = nullptr;
static i2c_master_dev_handle_t _scale_dev  = nullptr;
static SemaphoreHandle_t _status_mutex     = nullptr;
static WaterMonitorStatus_t _status;

static void _lock_status()
{
    if (_status_mutex) {
        xSemaphoreTake(_status_mutex, portMAX_DELAY);
    }
}

static void _unlock_status()
{
    if (_status_mutex) {
        xSemaphoreGive(_status_mutex);
    }
}

static void _set_scale_unready(uint32_t now_ms)
{
    _lock_status();
    _status.scaleReady   = false;
    _status.lastUpdateMs = now_ms;
    _unlock_status();
}

static void _update_weight(float weight_g, uint32_t now_ms)
{
    _lock_status();
    _status.scaleReady   = true;
    _status.weightGrams  = weight_g;
    _status.consumedMl   = _status.baselineSet ? std::max(_status.baselineGrams - weight_g, 0.0f) : 0.0f;
    _status.lastUpdateMs = now_ms;
    _unlock_status();
}

static void _load_baseline()
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        mclog::tagInfo(_tag, "no saved water baseline");
        return;
    }

    int32_t baseline_cg = 0;
    err                 = nvs_get_i32(handle, kBaselineKey, &baseline_cg);
    nvs_close(handle);

    if (err == ESP_OK && baseline_cg >= 0) {
        _lock_status();
        _status.baselineSet   = true;
        _status.baselineGrams = static_cast<float>(baseline_cg) / 100.0f;
        _unlock_status();
        mclog::tagInfo(_tag, "loaded baseline: {:.2f} g", static_cast<float>(baseline_cg) / 100.0f);
    } else {
        mclog::tagInfo(_tag, "no saved water baseline");
    }
}

static bool _save_baseline(float baseline_g)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        mclog::tagError(_tag, "failed to open nvs: {}", esp_err_to_name(err));
        return false;
    }

    int32_t baseline_cg = static_cast<int32_t>(std::lroundf(baseline_g * 100.0f));
    err                 = nvs_set_i32(handle, kBaselineKey, baseline_cg);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        mclog::tagError(_tag, "failed to save baseline: {}", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool _read_scale_weight(float& weight_g)
{
    if (_scale_dev == nullptr) {
        return false;
    }

    uint8_t reg     = kWeightFloatReg;
    uint8_t data[4] = {};
    esp_err_t err   = i2c_master_transmit_receive(_scale_dev, &reg, 1, data, sizeof(data), kI2cTimeoutMs);
    if (err != ESP_OK) {
        return false;
    }

    float value = 0.0f;
    std::memcpy(&value, data, sizeof(value));
    if (!std::isfinite(value) || value < -10000.0f || value > 100000.0f) {
        return false;
    }

    weight_g = value;
    return true;
}

static void _water_monitor_task(void* param)
{
    uint32_t last_error_log_ms = 0;

    while (true) {
        float weight_g = 0.0f;
        uint32_t now   = GetHAL().millis();
        if (_read_scale_weight(weight_g)) {
            _update_weight(weight_g, now);
        } else {
            _set_scale_unready(now);
            if (now - last_error_log_ms > 10000) {
                last_error_log_ms = now;
                mclog::tagWarn(_tag, "Mini Scales not detected on Port A address 0x{:02X}", kMiniScalesAddress);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void Hal::waterMonitorInit()
{
    mclog::tagInfo(_tag, "init Mini Scales on Port A");

    if (_status_mutex == nullptr) {
        _status_mutex = xSemaphoreCreateMutex();
    }

    _load_baseline();

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port                = I2C_NUM_0;
    bus_cfg.sda_io_num              = PORT_A_I2C_SDA_PIN;
    bus_cfg.scl_io_num              = PORT_A_I2C_SCL_PIN;
    bus_cfg.clk_source              = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt       = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &_port_a_bus);
    if (err != ESP_OK) {
        mclog::tagError(_tag, "failed to create Port A i2c bus: {}", esp_err_to_name(err));
        _set_scale_unready(millis());
        return;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length     = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address      = kMiniScalesAddress;
    dev_cfg.scl_speed_hz        = 100000;

    err = i2c_master_bus_add_device(_port_a_bus, &dev_cfg, &_scale_dev);
    if (err != ESP_OK) {
        mclog::tagError(_tag, "failed to add Mini Scales i2c device: {}", esp_err_to_name(err));
        _set_scale_unready(millis());
        return;
    }

    err = i2c_master_probe(_port_a_bus, kMiniScalesAddress, kI2cTimeoutMs);
    if (err == ESP_OK) {
        uint8_t reg     = kFirmwareReg;
        uint8_t version = 0;
        if (i2c_master_transmit_receive(_scale_dev, &reg, 1, &version, 1, kI2cTimeoutMs) == ESP_OK) {
            mclog::tagInfo(_tag, "Mini Scales firmware version: {}", version);
        } else {
            mclog::tagInfo(_tag, "Mini Scales detected");
        }
    } else {
        mclog::tagWarn(_tag, "Mini Scales probe failed: {}", esp_err_to_name(err));
    }

    xTaskCreate(_water_monitor_task, "water_monitor", 4096, nullptr, 2, nullptr);
}

WaterMonitorStatus_t Hal::getWaterMonitorStatus()
{
    _lock_status();
    auto copy = _status;
    _unlock_status();
    return copy;
}

bool Hal::setWaterRefillBaseline()
{
    float weight_g = 0.0f;
    uint32_t now   = millis();
    if (!_read_scale_weight(weight_g)) {
        _set_scale_unready(now);
        mclog::tagWarn(_tag, "cannot set baseline because Mini Scales read failed");
        return false;
    }

    _lock_status();
    _status.scaleReady    = true;
    _status.baselineSet   = true;
    _status.weightGrams   = weight_g;
    _status.baselineGrams = weight_g;
    _status.consumedMl    = 0.0f;
    _status.lastUpdateMs  = now;
    _unlock_status();

    if (!_save_baseline(weight_g)) {
        return false;
    }

    mclog::tagInfo(_tag, "water refill baseline set to {:.2f} g", weight_g);
    return true;
}
