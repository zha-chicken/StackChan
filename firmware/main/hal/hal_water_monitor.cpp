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
#include <ctime>
#include <driver/i2c_master.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mooncake_log.h>
#include <nvs.h>
#include <apps/common/common.h>

static const std::string_view _tag = "HAL-Water";

static constexpr uint8_t kMiniScalesAddress = 0x26;
static constexpr uint8_t kWeightFloatReg    = 0x10;
static constexpr uint8_t kFirmwareReg       = 0xFE;
static constexpr int kI2cTimeoutMs          = 1000;

static constexpr const char* kNvsNamespace       = "water_monitor";
static constexpr const char* kBaselineKey        = "baseline_cg";
static constexpr const char* kEmptyCupKey        = "empty_cup_cg";
static constexpr const char* kDailyGoalKey       = "daily_goal_ml";
static constexpr const char* kTodayKey           = "today_ymd";
static constexpr const char* kTodayConsumedKey   = "today_ml";
static constexpr const char* kLastStableWaterKey = "last_water_cg";

static constexpr int kDefaultDailyGoalMl          = 1500;
static constexpr int32_t kCupPresentToleranceCg   = 500;   // 5 g
static constexpr int32_t kStableWaterDeltaCg      = 200;   // 2 g
static constexpr int kStableSamplesRequired       = 3;
static constexpr uint32_t kHydrationReminderMs    = 60 * 60 * 1000;
static constexpr uint32_t kEmptyCupSetupDelayMs   = 45 * 1000;

static i2c_master_bus_handle_t _port_a_bus = nullptr;
static i2c_master_dev_handle_t _scale_dev  = nullptr;
static SemaphoreHandle_t _status_mutex     = nullptr;
static WaterMonitorStatus_t _status;

static int32_t _candidate_water_cg     = -1;
static int _candidate_stable_samples   = 0;
static int32_t _last_stable_water_cg   = -1;
static bool _last_stable_water_loaded  = false;
static uint32_t _last_hydration_reminder_ms = 0;
static bool _empty_cup_setup_prompt_shown   = false;

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

static int _today_ymd()
{
    time_t now = std::time(nullptr);
    if (now < 1700000000) {
        return 0;
    }

    struct tm local_tm = {};
    localtime_r(&now, &local_tm);
    return (local_tm.tm_year + 1900) * 10000 + (local_tm.tm_mon + 1) * 100 + local_tm.tm_mday;
}

static bool _save_i32(const char* key, int32_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        mclog::tagError(_tag, "failed to open nvs for {}: {}", key, esp_err_to_name(err));
        return false;
    }

    err = nvs_set_i32(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        mclog::tagError(_tag, "failed to save {}: {}", key, esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool _erase_key(const char* key)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return false;
    }

    err = nvs_erase_key(handle, key);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

static int32_t _grams_to_cg(float grams)
{
    return static_cast<int32_t>(std::lroundf(grams * 100.0f));
}

static void _recalculate_status_locked()
{
    if (_status.dailyGoalMl <= 0) {
        _status.dailyGoalMl = kDefaultDailyGoalMl;
    }

    _status.cupPresent = false;
    _status.waterMl    = 0.0f;

    if (_status.emptyCupSet && _status.scaleReady) {
        const float present_threshold_g =
            std::max((_status.emptyCupGrams * 100.0f - static_cast<float>(kCupPresentToleranceCg)) / 100.0f, 0.0f);
        _status.cupPresent = _status.weightGrams >= present_threshold_g;
        if (_status.cupPresent) {
            _status.waterMl = std::max(_status.weightGrams - _status.emptyCupGrams, 0.0f);
        }
    }

    if (_status.baselineSet) {
        if (_status.emptyCupSet) {
            _status.baselineWaterMl = std::max(_status.baselineGrams - _status.emptyCupGrams, 0.0f);
            _status.consumedMl      = std::max(_status.baselineWaterMl - _status.waterMl, 0.0f);
        } else {
            _status.baselineWaterMl = _status.baselineGrams;
            _status.consumedMl      = std::max(_status.baselineGrams - _status.weightGrams, 0.0f);
        }
    } else {
        _status.baselineWaterMl = 0.0f;
        _status.consumedMl      = 0.0f;
    }

    _status.remainingGoalMl = std::max(static_cast<float>(_status.dailyGoalMl) - _status.todayConsumedMl, 0.0f);
    _status.dailyGoalMet    = _status.todayConsumedMl >= static_cast<float>(_status.dailyGoalMl);
}

static void _set_scale_unready(uint32_t now_ms)
{
    _lock_status();
    _status.scaleReady   = false;
    _status.cupPresent   = false;
    _status.lastUpdateMs = now_ms;
    _recalculate_status_locked();
    _unlock_status();
}

static void _reset_daily_if_needed(int today_ymd, uint32_t now_ms)
{
    if (today_ymd == 0) {
        return;
    }

    bool changed = false;
    _lock_status();
    if (_status.todayYmd != today_ymd) {
        _status.todayYmd         = today_ymd;
        _status.todayConsumedMl  = 0.0f;
        _last_stable_water_cg    = -1;
        _last_stable_water_loaded = false;
        _candidate_water_cg      = -1;
        _candidate_stable_samples = 0;
        _last_hydration_reminder_ms = now_ms;
        _recalculate_status_locked();
        changed = true;
    }
    _unlock_status();

    if (changed) {
        _save_i32(kTodayKey, today_ymd);
        _save_i32(kTodayConsumedKey, 0);
        _erase_key(kLastStableWaterKey);
        mclog::tagInfo(_tag, "new water day: {}", today_ymd);
    }
}

static void _save_last_stable_water(int32_t water_cg)
{
    _last_stable_water_cg     = water_cg;
    _last_stable_water_loaded = true;
    _save_i32(kLastStableWaterKey, water_cg);
}

static void _track_stable_water(int32_t current_water_cg, uint32_t now_ms)
{
    _reset_daily_if_needed(_today_ymd(), now_ms);

    if (_candidate_water_cg < 0 || std::abs(current_water_cg - _candidate_water_cg) > kStableWaterDeltaCg) {
        _candidate_water_cg       = current_water_cg;
        _candidate_stable_samples = 1;
        return;
    }

    if (_candidate_stable_samples < kStableSamplesRequired) {
        _candidate_stable_samples++;
    }
    if (_candidate_stable_samples < kStableSamplesRequired) {
        return;
    }

    const int32_t stable_water_cg = _candidate_water_cg;
    if (!_last_stable_water_loaded) {
        _save_last_stable_water(stable_water_cg);
        return;
    }

    const int32_t delta_cg = _last_stable_water_cg - stable_water_cg;
    if (std::abs(delta_cg) <= kStableWaterDeltaCg) {
        return;
    }

    if (delta_cg > 0) {
        const float consumed_ml = static_cast<float>(delta_cg) / 100.0f;
        float today_consumed_ml = 0.0f;
        int today_ymd           = 0;

        _lock_status();
        _status.todayConsumedMl += consumed_ml;
        today_consumed_ml = _status.todayConsumedMl;
        today_ymd         = _status.todayYmd;
        _recalculate_status_locked();
        _unlock_status();

        _save_i32(kTodayKey, today_ymd);
        _save_i32(kTodayConsumedKey, static_cast<int32_t>(std::lroundf(today_consumed_ml)));
        mclog::tagInfo(_tag, "tracked drink: +{:.1f} ml today={:.1f} ml", consumed_ml, today_consumed_ml);
    } else {
        mclog::tagInfo(_tag, "water level increased by {:.1f} ml; treating as refill", static_cast<float>(-delta_cg) / 100.0f);
    }

    _save_last_stable_water(stable_water_cg);
}

static void _update_weight(float weight_g, uint32_t now_ms)
{
    bool should_track = false;
    int32_t water_cg  = 0;

    _lock_status();
    _status.scaleReady   = true;
    _status.weightGrams  = weight_g;
    _status.lastUpdateMs = now_ms;
    _recalculate_status_locked();
    should_track = _status.emptyCupSet && _status.cupPresent;
    water_cg     = _grams_to_cg(_status.waterMl);
    _unlock_status();

    if (should_track) {
        _track_stable_water(water_cg, now_ms);
    }
}

static void _load_state()
{
    int32_t empty_cup_cg      = -1;
    int32_t baseline_cg       = -1;
    int32_t daily_goal_ml     = kDefaultDailyGoalMl;
    int32_t today_ymd         = 0;
    int32_t today_consumed_ml = 0;
    int32_t last_water_cg     = -1;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        nvs_get_i32(handle, kEmptyCupKey, &empty_cup_cg);
        nvs_get_i32(handle, kBaselineKey, &baseline_cg);
        nvs_get_i32(handle, kDailyGoalKey, &daily_goal_ml);
        nvs_get_i32(handle, kTodayKey, &today_ymd);
        nvs_get_i32(handle, kTodayConsumedKey, &today_consumed_ml);
        nvs_get_i32(handle, kLastStableWaterKey, &last_water_cg);
        nvs_close(handle);
    }

    const int current_ymd = _today_ymd();
    if (current_ymd != 0 && today_ymd != current_ymd) {
        today_ymd         = current_ymd;
        today_consumed_ml = 0;
        last_water_cg     = -1;
        _save_i32(kTodayKey, today_ymd);
        _save_i32(kTodayConsumedKey, 0);
        _erase_key(kLastStableWaterKey);
    }

    _lock_status();
    if (empty_cup_cg >= 0) {
        _status.emptyCupSet    = true;
        _status.emptyCupGrams  = static_cast<float>(empty_cup_cg) / 100.0f;
    }
    if (baseline_cg >= 0) {
        _status.baselineSet    = true;
        _status.baselineGrams  = static_cast<float>(baseline_cg) / 100.0f;
    }
    _status.dailyGoalMl      = daily_goal_ml > 0 ? daily_goal_ml : kDefaultDailyGoalMl;
    _status.todayYmd         = today_ymd;
    _status.todayConsumedMl  = static_cast<float>(std::max(today_consumed_ml, static_cast<int32_t>(0)));
    _recalculate_status_locked();
    _unlock_status();

    if (last_water_cg >= 0) {
        _last_stable_water_cg     = last_water_cg;
        _last_stable_water_loaded = true;
    }
    _last_hydration_reminder_ms = GetHAL().millis();

    if (_status.emptyCupSet) {
        mclog::tagInfo(_tag, "loaded empty cup weight: {:.2f} g", _status.emptyCupGrams);
    } else {
        mclog::tagInfo(_tag, "no saved empty cup weight");
    }
    if (_status.baselineSet) {
        mclog::tagInfo(_tag, "loaded refill baseline: {:.2f} g", _status.baselineGrams);
    } else {
        mclog::tagInfo(_tag, "no saved refill baseline");
    }
    mclog::tagInfo(_tag, "daily goal: {} ml, today consumed: {:.1f} ml", _status.dailyGoalMl, _status.todayConsumedMl);
}

static bool _save_baseline(float baseline_g)
{
    return _save_i32(kBaselineKey, _grams_to_cg(baseline_g));
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

    _load_state();

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

bool Hal::setWaterEmptyCupWeight()
{
    float weight_g = 0.0f;
    uint32_t now   = millis();
    if (!_read_scale_weight(weight_g)) {
        _set_scale_unready(now);
        mclog::tagWarn(_tag, "cannot set empty cup weight because Mini Scales read failed");
        return false;
    }

    if (weight_g < 1.0f || weight_g > 5000.0f) {
        mclog::tagWarn(_tag, "ignore invalid empty cup weight: {:.2f} g", weight_g);
        return false;
    }

    _lock_status();
    _status.scaleReady     = true;
    _status.emptyCupSet    = true;
    _status.baselineSet    = false;
    _status.weightGrams    = weight_g;
    _status.emptyCupGrams  = weight_g;
    _status.baselineGrams  = 0.0f;
    _status.lastUpdateMs   = now;
    _recalculate_status_locked();
    _unlock_status();

    _candidate_water_cg       = 0;
    _candidate_stable_samples = kStableSamplesRequired;
    _save_last_stable_water(0);
    _erase_key(kBaselineKey);

    if (!_save_i32(kEmptyCupKey, _grams_to_cg(weight_g))) {
        return false;
    }

    mclog::tagInfo(_tag, "empty cup weight set to {:.2f} g", weight_g);
    return true;
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
    if (!_status.emptyCupSet) {
        _unlock_status();
        mclog::tagWarn(_tag, "cannot set refill baseline before empty cup weight is set");
        return false;
    }

    _status.scaleReady    = true;
    _status.baselineSet   = true;
    _status.weightGrams   = weight_g;
    _status.baselineGrams = weight_g;
    _status.lastUpdateMs  = now;
    _recalculate_status_locked();
    const int32_t water_cg = _grams_to_cg(_status.waterMl);
    _unlock_status();

    _candidate_water_cg       = water_cg;
    _candidate_stable_samples = kStableSamplesRequired;
    _save_last_stable_water(water_cg);

    if (!_save_baseline(weight_g)) {
        return false;
    }

    mclog::tagInfo(_tag, "water refill baseline set to {:.2f} g", weight_g);
    return true;
}

bool Hal::setWaterDailyGoal(int goalMl)
{
    if (goalMl < 250 || goalMl > 5000) {
        mclog::tagWarn(_tag, "ignore invalid daily goal: {} ml", goalMl);
        return false;
    }

    _lock_status();
    _status.dailyGoalMl = goalMl;
    _recalculate_status_locked();
    _unlock_status();

    return _save_i32(kDailyGoalKey, goalMl);
}

void Hal::waterMonitorUpdateReminder()
{
    const uint32_t now = millis();
    _reset_daily_if_needed(_today_ymd(), now);
    WaterMonitorStatus_t status = getWaterMonitorStatus();

    if (!status.emptyCupSet) {
        if (!_empty_cup_setup_prompt_shown && now > kEmptyCupSetupDelayMs) {
            _empty_cup_setup_prompt_shown = true;
            tools::create_reminder(1, "Place the empty cup on the scale, then ask me to record the empty cup weight.", false);
        }
        return;
    }

    if (_last_hydration_reminder_ms == 0) {
        _last_hydration_reminder_ms = now;
        return;
    }

    if (!status.dailyGoalMet && status.dailyGoalMl > 0 &&
        now - _last_hydration_reminder_ms >= kHydrationReminderMs) {
        _last_hydration_reminder_ms = now;
        std::string message = fmt::format("Water check: {:.0f} ml left to reach today's {} ml goal.",
                                          status.remainingGoalMl, status.dailyGoalMl);
        tools::create_reminder(1, message, false);
    }
}
