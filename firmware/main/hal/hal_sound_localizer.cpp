/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"

#include <mooncake_log.h>
#include <settings.h>
#include <stackchan/stackchan.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string_view>

static const std::string_view _tag = "SoundLocalizer";

namespace {

constexpr const char* kSettingsNs      = "sound_track";
constexpr const char* kEnabledKey      = "enabled";
constexpr const char* kInvertKey       = "invert";
constexpr uint32_t kAnalyzeIntervalMs  = 30;
constexpr uint32_t kRequestIntervalMs  = 450;
constexpr uint32_t kApplyIntervalMs    = 550;
constexpr int kMinAverageAbs           = 320;
constexpr int kMinDiffPermille         = 80;
constexpr int kStableFramesRequired    = 3;
constexpr int kMaxCorrelationLag       = 4;
constexpr int kYawStepMin              = 70;
constexpr int kYawStepMax              = 180;
constexpr int kYawLimit                = 650;
constexpr int kYawMoveSpeed            = 180;

std::atomic<int> pending_yaw_step{0};

bool config_loaded = false;
bool tracking_enabled = true;
bool invert_direction = false;

uint32_t last_analyze_ms = 0;
uint32_t last_request_ms = 0;
uint32_t last_apply_ms = 0;
int stable_direction = 0;
int stable_count = 0;

void load_config_once()
{
    if (config_loaded) {
        return;
    }
    Settings settings(kSettingsNs, false);
    tracking_enabled = settings.GetBool(kEnabledKey, true);
    invert_direction = settings.GetBool(kInvertKey, false);
    config_loaded = true;
    mclog::tagInfo(_tag, "enabled={} invert={}", tracking_enabled, invert_direction);
}

int abs_i16(int16_t value)
{
    const int v = static_cast<int>(value);
    return v < 0 ? -v : v;
}

int clamp_step(int value)
{
    if (value < kYawStepMin) {
        return kYawStepMin;
    }
    if (value > kYawStepMax) {
        return kYawStepMax;
    }
    return value;
}

int estimate_correlation_direction(const int16_t* interleaved, size_t frames, int channels)
{
    int best_lag = 0;
    int64_t best_score = INT64_MIN;

    for (int lag = -kMaxCorrelationLag; lag <= kMaxCorrelationLag; ++lag) {
        const size_t left_start = lag < 0 ? static_cast<size_t>(-lag) : 0;
        const size_t right_start = lag > 0 ? static_cast<size_t>(lag) : 0;
        const size_t usable = frames - std::max(left_start, right_start);
        int64_t score = 0;

        for (size_t i = 0; i < usable; ++i) {
            const int32_t left = interleaved[(left_start + i) * channels];
            const int32_t right = interleaved[(right_start + i) * channels + 1];
            score += static_cast<int64_t>(left) * right;
        }

        if (score > best_score) {
            best_score = score;
            best_lag = lag;
        }
    }

    if (best_score <= 0 || best_lag == 0) {
        return 0;
    }

    // With this alignment, a positive lag means the right channel arrived later,
    // so the source is probably on the left. Yaw positive looks right.
    return best_lag > 0 ? -1 : 1;
}

void reset_stability()
{
    stable_direction = 0;
    stable_count = 0;
}

}  // namespace

extern "C" void stackchan_sound_localizer_feed(const int16_t* interleaved, size_t frames, int channels,
                                               int sample_rate, bool tracking_active)
{
    (void)sample_rate;
    load_config_once();

    if (!tracking_enabled || !tracking_active || interleaved == nullptr || channels < 2 || frames < 64) {
        reset_stability();
        return;
    }

    const uint32_t now = GetHAL().millis();
    if (now - last_analyze_ms < kAnalyzeIntervalMs) {
        return;
    }
    last_analyze_ms = now;

    uint64_t left_abs_sum = 0;
    uint64_t right_abs_sum = 0;
    for (size_t i = 0; i < frames; ++i) {
        left_abs_sum += abs_i16(interleaved[i * channels]);
        right_abs_sum += abs_i16(interleaved[i * channels + 1]);
    }

    const uint64_t total_abs = left_abs_sum + right_abs_sum;
    if (total_abs / (frames * 2) < kMinAverageAbs) {
        reset_stability();
        return;
    }

    const int diff_permille = static_cast<int>(
        (static_cast<int64_t>(right_abs_sum) - static_cast<int64_t>(left_abs_sum)) * 1000 /
        static_cast<int64_t>(total_abs));

    int direction = 0;
    if (std::abs(diff_permille) >= kMinDiffPermille) {
        direction = diff_permille > 0 ? 1 : -1;
    }

    const int corr_direction = estimate_correlation_direction(interleaved, frames, channels);
    if (corr_direction != 0 && (direction == 0 || direction == corr_direction)) {
        direction = corr_direction;
    }

    if (invert_direction) {
        direction = -direction;
    }

    if (direction == 0) {
        reset_stability();
        return;
    }

    if (direction == stable_direction) {
        stable_count = std::min(stable_count + 1, kStableFramesRequired);
    } else {
        stable_direction = direction;
        stable_count = 1;
    }

    if (stable_count < kStableFramesRequired || now - last_request_ms < kRequestIntervalMs ||
        pending_yaw_step.load() != 0) {
        return;
    }

    const int strength = std::min(std::abs(diff_permille), 450);
    const int step = direction * clamp_step(kYawStepMin + strength / 4);
    pending_yaw_step.store(step);
    last_request_ms = now;
}

extern "C" void stackchan_sound_localizer_update_motion()
{
    const int yaw_step = pending_yaw_step.exchange(0);
    if (yaw_step == 0) {
        return;
    }

    const uint32_t now = GetHAL().millis();
    if (now - last_apply_ms < kApplyIntervalMs) {
        return;
    }

    auto& stackchan = GetStackChan();
    if (!stackchan.hasAvatar()) {
        return;
    }

    auto& motion = stackchan.motion();
    if (motion.isModifyLocked() || motion.isMoving()) {
        return;
    }

    const int current_yaw = motion.getCurrentYawAngle();
    const int current_pitch = motion.getCurrentPitchAngle();
    const int target_yaw = std::clamp(current_yaw + yaw_step, -kYawLimit, kYawLimit);
    if (std::abs(target_yaw - current_yaw) < kYawStepMin / 2) {
        return;
    }

    motion.moveWithSpeed(target_yaw, current_pitch, kYawMoveSpeed);
    last_apply_ms = now;
    mclog::tagInfo(_tag, "move yaw {} -> {} step={}", current_yaw, target_yaw, yaw_step);
}
