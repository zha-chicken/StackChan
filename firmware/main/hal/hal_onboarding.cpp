/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <algorithm>
#include <cJSON.h>
#include <cctype>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mooncake_log.h>
#include <settings.h>

static const std::string_view _tag = "HAL-Onboarding";

static constexpr const char* kSettingsNamespace = "onboard";
static constexpr const char* kActiveKey         = "active";
static constexpr const char* kCompleteKey       = "complete";
static constexpr const char* kStepKey           = "step";
static constexpr const char* kNameKey           = "name";
static constexpr const char* kCommKey           = "comm";
static constexpr const char* kTopicsKey         = "topics";
static constexpr const char* kRelationKey       = "relation";
static constexpr const char* kReminderKey       = "reminder";
static constexpr const char* kSummaryKey        = "summary";

static constexpr const char* kQuestions[] = {
    "你希望我怎么称呼你？",
    "你喜欢我回答简短直接，还是多解释一点？",
    "你最常想让我帮你关注哪些事？",
    "你希望我像助手、朋友，还是行动伙伴？",
    "提醒你喝水时，你喜欢温柔一点还是直接一点？",
};
static constexpr int kQuestionCount = sizeof(kQuestions) / sizeof(kQuestions[0]);

static SemaphoreHandle_t _onboarding_mutex = nullptr;
static OnboardingProfileStatus_t _profile;

static void _lock_onboarding()
{
    if (_onboarding_mutex) {
        xSemaphoreTake(_onboarding_mutex, portMAX_DELAY);
    }
}

static void _unlock_onboarding()
{
    if (_onboarding_mutex) {
        xSemaphoreGive(_onboarding_mutex);
    }
}

static std::string _trim(std::string_view input)
{
    auto begin = input.begin();
    auto end   = input.end();
    while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }
    while (begin != end && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }
    return std::string(begin, end);
}

static const char* _next_question_locked()
{
    if (!_profile.active || _profile.step < 0 || _profile.step >= kQuestionCount) {
        return "";
    }
    return kQuestions[_profile.step];
}

static void _build_summary_locked()
{
    std::string summary = "用户偏好：";
    if (!_profile.preferredName.empty()) {
        summary += "称呼用户为「" + _profile.preferredName + "」。";
    }
    if (!_profile.communicationStyle.empty()) {
        summary += "回答风格：" + _profile.communicationStyle + "。";
    }
    if (!_profile.focusTopics.empty()) {
        summary += "常关注：" + _profile.focusTopics + "。";
    }
    if (!_profile.relationshipStyle.empty()) {
        summary += "相处方式：" + _profile.relationshipStyle + "。";
    }
    if (!_profile.reminderStyle.empty()) {
        summary += "提醒偏好：" + _profile.reminderStyle + "。";
    }
    _profile.summary = summary;
}

static std::string _assistant_instruction_locked()
{
    if (_profile.complete && !_profile.summary.empty()) {
        return "Use this persistent user profile when replying. Adapt tone, detail level, reminders, and examples to it: " +
               _profile.summary;
    }
    if (_profile.active) {
        return "Continue onboarding. Ask exactly the returned next_question, wait for the user's answer, then call self.onboarding.record_answer with that answer.";
    }
    return "If the user says onboarding or asks you to personalize yourself, call self.onboarding.start.";
}

static std::string _to_json_locked(const char* event, const char* next_question = "", const char* error = nullptr)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event", event);
    cJSON_AddBoolToObject(root, "active", _profile.active);
    cJSON_AddBoolToObject(root, "complete", _profile.complete);
    cJSON_AddNumberToObject(root, "step", _profile.step);
    cJSON_AddNumberToObject(root, "total_steps", kQuestionCount);
    cJSON_AddStringToObject(root, "next_question", next_question ? next_question : "");
    cJSON_AddStringToObject(root, "preferred_name", _profile.preferredName.c_str());
    cJSON_AddStringToObject(root, "communication_style", _profile.communicationStyle.c_str());
    cJSON_AddStringToObject(root, "focus_topics", _profile.focusTopics.c_str());
    cJSON_AddStringToObject(root, "relationship_style", _profile.relationshipStyle.c_str());
    cJSON_AddStringToObject(root, "reminder_style", _profile.reminderStyle.c_str());
    cJSON_AddStringToObject(root, "summary", _profile.summary.c_str());
    auto instruction = _assistant_instruction_locked();
    cJSON_AddStringToObject(root, "assistant_instruction", instruction.c_str());
    if (error) {
        cJSON_AddStringToObject(root, "error", error);
    }

    char* json_str = cJSON_PrintUnformatted(root);
    std::string result = json_str ? json_str : "{}";
    cJSON_free(json_str);
    cJSON_Delete(root);
    return result;
}

static void _save_locked()
{
    Settings settings(kSettingsNamespace, true);
    settings.SetBool(kActiveKey, _profile.active);
    settings.SetBool(kCompleteKey, _profile.complete);
    settings.SetInt(kStepKey, _profile.step);
    settings.SetString(kNameKey, _profile.preferredName);
    settings.SetString(kCommKey, _profile.communicationStyle);
    settings.SetString(kTopicsKey, _profile.focusTopics);
    settings.SetString(kRelationKey, _profile.relationshipStyle);
    settings.SetString(kReminderKey, _profile.reminderStyle);
    settings.SetString(kSummaryKey, _profile.summary);
}

static void _load()
{
    Settings settings(kSettingsNamespace, false);
    _profile.active             = settings.GetBool(kActiveKey, false);
    _profile.complete           = settings.GetBool(kCompleteKey, false);
    _profile.step               = std::clamp(static_cast<int>(settings.GetInt(kStepKey, 0)), 0, kQuestionCount);
    _profile.preferredName      = settings.GetString(kNameKey);
    _profile.communicationStyle = settings.GetString(kCommKey);
    _profile.focusTopics        = settings.GetString(kTopicsKey);
    _profile.relationshipStyle  = settings.GetString(kRelationKey);
    _profile.reminderStyle      = settings.GetString(kReminderKey);
    _profile.summary            = settings.GetString(kSummaryKey);
    if (_profile.summary.empty() && _profile.complete) {
        _build_summary_locked();
    }
}

static void _reset_locked()
{
    _profile = OnboardingProfileStatus_t{};
}

static void _set_answer_locked(int step, const std::string& answer)
{
    switch (step) {
        case 0:
            _profile.preferredName = answer;
            break;
        case 1:
            _profile.communicationStyle = answer;
            break;
        case 2:
            _profile.focusTopics = answer;
            break;
        case 3:
            _profile.relationshipStyle = answer;
            break;
        case 4:
            _profile.reminderStyle = answer;
            break;
        default:
            break;
    }
}

void Hal::onboardingInit()
{
    mclog::tagInfo(_tag, "init");
    if (_onboarding_mutex == nullptr) {
        _onboarding_mutex = xSemaphoreCreateMutex();
    }

    _lock_onboarding();
    _load();
    _unlock_onboarding();

    mclog::tagInfo(_tag, "profile complete={}, active={}, step={}", _profile.complete, _profile.active, _profile.step);
}

OnboardingProfileStatus_t Hal::getOnboardingProfileStatus()
{
    _lock_onboarding();
    auto copy = _profile;
    _unlock_onboarding();
    return copy;
}

std::string Hal::getOnboardingProfileJson()
{
    _lock_onboarding();
    auto result = _to_json_locked("profile", _next_question_locked());
    _unlock_onboarding();
    return result;
}

std::string Hal::startOnboarding()
{
    _lock_onboarding();
    const bool previous_complete = _profile.complete;
    _reset_locked();
    _profile.active   = true;
    _profile.complete = false;
    _profile.step     = 0;
    _save_locked();

    auto base = _to_json_locked("started", _next_question_locked());
    cJSON* root = cJSON_Parse(base.c_str());
    std::string result = base;
    if (root) {
        cJSON_AddBoolToObject(root, "previous_profile_existed", previous_complete);
        char* json_str = cJSON_PrintUnformatted(root);
        result = json_str ? json_str : base;
        cJSON_free(json_str);
        cJSON_Delete(root);
    }

    _unlock_onboarding();
    mclog::tagInfo(_tag, "started");
    return result;
}

std::string Hal::recordOnboardingAnswer(std::string_view answer)
{
    auto trimmed = _trim(answer);
    _lock_onboarding();
    if (trimmed.empty()) {
        auto result = _to_json_locked("answer_rejected", _next_question_locked(), "answer is empty");
        _unlock_onboarding();
        return result;
    }

    if (!_profile.active) {
        auto result = _to_json_locked("answer_rejected", _next_question_locked(), "onboarding is not active");
        _unlock_onboarding();
        return result;
    }

    const int answered_step = _profile.step;
    _set_answer_locked(answered_step, trimmed);
    _profile.step++;

    std::string result;
    if (_profile.step >= kQuestionCount) {
        _profile.step     = kQuestionCount;
        _profile.active   = false;
        _profile.complete = true;
        _build_summary_locked();
        _save_locked();
        result = _to_json_locked("completed", "");
        mclog::tagInfo(_tag, "completed: {}", _profile.summary);
    } else {
        _save_locked();
        result = _to_json_locked("answer_recorded", _next_question_locked());
        mclog::tagInfo(_tag, "answer recorded: step={}", _profile.step);
    }

    _unlock_onboarding();
    return result;
}

std::string Hal::resetOnboardingProfile()
{
    _lock_onboarding();
    _reset_locked();
    _save_locked();
    auto result = _to_json_locked("reset", "");
    _unlock_onboarding();
    mclog::tagInfo(_tag, "reset");
    return result;
}
