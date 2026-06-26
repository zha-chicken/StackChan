/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mooncake_log.h>

#include "board/hal_bridge.h"

static const std::string_view _tag = "HAL-VisualAttention";

static constexpr uint32_t kSampleIntervalMs  = 500;
static constexpr uint32_t kLookTriggerMs     = 3000;
static constexpr uint32_t kLostDebounceMs    = 1000;
static constexpr uint32_t kCooldownMs        = 30000;
static constexpr int kBaselineWarmupSamples  = 3;
static constexpr float kCenterDiffThreshold  = 16.0f;
static constexpr float kDetailScoreThreshold = 0.34f;
static constexpr float kCenterEdgeThreshold  = 5.5f;
static constexpr float kMirrorMinThreshold   = 0.32f;

enum class AttentionState {
    Idle,
    Validating,
    Cooldown,
};

static AttentionState _attention_state = AttentionState::Idle;
static uint32_t _state_start_ms        = 0;
static uint32_t _last_seen_ms          = 0;
static uint32_t _last_sample_ms        = 0;
static uint32_t _last_trigger_ms       = 0;
static bool _pending_prompt            = false;
static int _baseline_samples           = 0;
static std::array<float, VisualAttentionSample::kGridSize> _baseline_grid{};

static bool _baseline_ready()
{
    return _baseline_samples >= kBaselineWarmupSamples;
}

static void _update_baseline(const VisualAttentionSample& sample)
{
    const float alpha = _baseline_samples == 0 ? 1.0f : 0.12f;
    for (size_t i = 0; i < sample.lumaGrid.size(); i++) {
        _baseline_grid[i] = _baseline_grid[i] * (1.0f - alpha) + static_cast<float>(sample.lumaGrid[i]) * alpha;
    }
    if (_baseline_samples < kBaselineWarmupSamples) {
        _baseline_samples++;
    }
}

static float _center_baseline_diff(const VisualAttentionSample& sample)
{
    constexpr size_t center_left   = 4;
    constexpr size_t center_right  = 12;
    constexpr size_t center_top    = 2;
    constexpr size_t center_bottom = 10;

    float diff = 0.0f;
    int count  = 0;
    for (size_t gy = center_top; gy < center_bottom; gy++) {
        for (size_t gx = center_left; gx < center_right; gx++) {
            const size_t index = gy * VisualAttentionSample::kGridWidth + gx;
            diff += std::abs(static_cast<float>(sample.lumaGrid[index]) - _baseline_grid[index]);
            count++;
        }
    }
    return count > 0 ? diff / count : 0.0f;
}

static bool _is_attention_candidate(const VisualAttentionSample& sample, float& center_diff)
{
    if (!sample.valid || !_baseline_ready()) {
        center_diff = 0.0f;
        return false;
    }

    center_diff       = _center_baseline_diff(sample);
    const bool detail = sample.detailScore >= kDetailScoreThreshold &&
                        sample.centerEdge >= kCenterEdgeThreshold &&
                        sample.mirrorSimilarity >= kMirrorMinThreshold;
    const bool foreground = center_diff >= kCenterDiffThreshold;
    return detail && foreground;
}

static void _invoke_prompt_or_queue()
{
    static constexpr const char* kPrompt =
        "<detect>User has looked toward StackChan for more than 3 seconds. Ask exactly one short, friendly Chinese "
        "question to see whether they need help.</detect>";

    if (!hal_bridge::is_xiaozhi_mode()) {
        _pending_prompt = true;
        GetHAL().requestXiaozhiStart();
        mclog::tagInfo(_tag, "attention trigger queued, requesting xiaozhi start");
        return;
    }

    if (!hal_bridge::is_xiaozhi_ready() || !hal_bridge::is_xiaozhi_idle()) {
        _pending_prompt = true;
        mclog::tagInfo(_tag, "attention trigger queued, waiting for xiaozhi idle");
        return;
    }

    _pending_prompt = false;
    hal_bridge::invoke_xiaozhi_wake_word(kPrompt);
    mclog::tagInfo(_tag, "attention prompt invoked");
}

static void _flush_pending_prompt()
{
    if (!_pending_prompt) {
        return;
    }
    if (hal_bridge::is_xiaozhi_mode() && hal_bridge::is_xiaozhi_ready() && hal_bridge::is_xiaozhi_idle()) {
        _invoke_prompt_or_queue();
    }
}

void Hal::visualAttentionInit()
{
    mclog::tagInfo(_tag, "init");
    xTaskCreatePinnedToCore(
        [](void*) {
            while (true) {
                GetHAL().visualAttentionUpdate();
                vTaskDelay(pdMS_TO_TICKS(kSampleIntervalMs));
            }
        },
        "visual_attention", 4096, nullptr, 2, nullptr, 1);
}

void Hal::visualAttentionUpdate()
{
    const uint32_t now = millis();
    if (now - _last_sample_ms < kSampleIntervalMs) {
        _flush_pending_prompt();
        return;
    }
    _last_sample_ms = now;
    _flush_pending_prompt();

    auto camera = hal_bridge::board_get_camera();
    if (camera == nullptr) {
        return;
    }

    VisualAttentionSample sample;
    if (!camera->SampleVisualAttention(sample) || !sample.valid) {
        return;
    }

    float center_diff          = 0.0f;
    const bool candidate       = _is_attention_candidate(sample, center_diff);
    const bool allow_retrigger = _last_trigger_ms == 0 || (now - _last_trigger_ms) >= kCooldownMs;

    if (!candidate && _attention_state == AttentionState::Idle) {
        _update_baseline(sample);
    }

    switch (_attention_state) {
        case AttentionState::Idle:
            if (candidate && allow_retrigger) {
                _attention_state = AttentionState::Validating;
                _state_start_ms  = now;
                _last_seen_ms    = now;
                mclog::tagInfo(_tag, "attention validating: diff={:.1f}, detail={:.2f}, mirror={:.2f}",
                               center_diff, sample.detailScore, sample.mirrorSimilarity);
            }
            break;

        case AttentionState::Validating:
            if (candidate) {
                _last_seen_ms = now;
                if (now - _state_start_ms >= kLookTriggerMs) {
                    _last_trigger_ms  = now;
                    _attention_state  = AttentionState::Cooldown;
                    _state_start_ms   = now;
                    _baseline_samples = 0;
                    mclog::tagInfo(_tag, "attention trigger: diff={:.1f}, edge={:.1f}, variance={:.1f}", center_diff,
                                   sample.centerEdge, sample.centerVariance);
                    _invoke_prompt_or_queue();
                }
            } else if (now - _last_seen_ms >= kLostDebounceMs) {
                _attention_state = AttentionState::Idle;
                _state_start_ms  = now;
                _update_baseline(sample);
                mclog::tagInfo(_tag, "attention validation cancelled");
            }
            break;

        case AttentionState::Cooldown:
            if (!candidate) {
                _update_baseline(sample);
            }
            if (!candidate && now - _state_start_ms >= kCooldownMs) {
                _attention_state = AttentionState::Idle;
                _state_start_ms  = now;
                mclog::tagInfo(_tag, "attention cooldown complete");
            }
            break;
    }
}
