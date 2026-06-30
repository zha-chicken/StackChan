/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "apps/common/reminder/reminder.h"
#include "board/hal_bridge.h"

#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lvgl.h>
#include <lvgl_theme.h>
#include <mooncake_log.h>
#include <settings.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

static const std::string_view _tag = "HAL-Memo";

static constexpr const char* kMemoSettingsNs = "memo";
static constexpr const char* kMemoItemsKey   = "items";
static constexpr const char* kMemoNextIdKey  = "next_id";
static constexpr int kMaxMemoCount           = 8;
static constexpr size_t kMaxTitleBytes       = 48;
static constexpr size_t kMaxContentBytes     = 280;
static constexpr size_t kMaxStorageBytes     = 3600;
static constexpr uint32_t kRecentUserMemoMaxAgeMs = 30000;

static SemaphoreHandle_t _memo_mutex = nullptr;
static lv_obj_t* _memo_overlay       = nullptr;

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

static const lv_font_t* _memo_text_font()
{
    auto* theme = LvglThemeManager::GetInstance().GetTheme("dark");
    if (theme != nullptr && theme->text_font() != nullptr && theme->text_font()->font() != nullptr) {
        return theme->text_font()->font();
    }
    return &BUILTIN_TEXT_FONT;
}

static void _lock_memo()
{
    if (_memo_mutex) {
        xSemaphoreTake(_memo_mutex, portMAX_DELAY);
    }
}

static void _unlock_memo()
{
    if (_memo_mutex) {
        xSemaphoreGive(_memo_mutex);
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

static bool _starts_with(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

static void _strip_leading_separator(std::string& value)
{
    value = _trim(value);
    bool changed = true;
    while (changed && !value.empty()) {
        changed = false;
        constexpr std::string_view separators[] = {
            ":", "：", ",", "，", ".", "。", ";", "；", "-", "—", " ", "\t", "\"", "'", "“", "”",
        };
        for (const auto separator : separators) {
            if (_starts_with(value, separator)) {
                value.erase(0, separator.size());
                value = _trim(value);
                changed = true;
                break;
            }
        }
    }
}

static void _strip_wake_prefixes(std::string& value)
{
    value = _trim(value);
    bool changed = true;
    while (changed && !value.empty()) {
        changed = false;
        constexpr std::string_view prefixes[] = {
            "墨狐", "墨湖", "小智", "你好", "您好", "hello", "Hello", "hi", "Hi", "hey", "Hey",
        };
        for (const auto prefix : prefixes) {
            if (_starts_with(value, prefix)) {
                value.erase(0, prefix.size());
                _strip_leading_separator(value);
                changed = true;
                break;
            }
        }
    }
}

static std::string _after_marker(std::string_view input, std::string_view marker)
{
    const auto pos = input.find(marker);
    if (pos == std::string_view::npos) {
        return "";
    }
    std::string value(input.substr(pos + marker.size()));
    _strip_leading_separator(value);
    return value;
}

static std::string _clean_memo_text(std::string_view input, bool strip_wake_prefix = false)
{
    std::string value = _trim(input);
    if (strip_wake_prefix) {
        _strip_wake_prefixes(value);
    }

    bool changed = true;
    while (changed && !value.empty()) {
        changed = false;

        constexpr std::string_view prefixes[] = {
            "帮我记一下",       "帮我记下",       "帮我记录一下",       "帮我记录",
            "帮我保存一下",     "帮我保存",       "帮我创建一个备忘录", "帮我创建备忘录",
            "帮我新增一个备忘录", "帮我新增备忘录", "帮我添加一个备忘录", "帮我添加备忘录",
            "创建一个备忘录",   "创建备忘录",     "新增一个备忘录",     "新增备忘录",
            "添加一个备忘录",   "添加备忘录",     "加一条备忘录",       "备忘录内容就是",
            "备忘录内容是",     "备忘录内容为",   "内容就是",           "内容是",
            "内容为",           "内容写成",       "内容写",             "就写",
            "写成",             "写下",           "记一下",             "记下",
            "记录一下",         "记录",           "保存一下",           "保存",
            "备忘一下",         "备忘录",         "备忘",               "我已帮你记下",
            "已帮你记下",       "我帮你记下",     "我帮你记一下",       "我会记住",
            "已经记下",         "记下了",         "我再帮你加一条",     "提醒你",
            "remember that",     "remember to",    "remember",           "note that",
            "take a note",       "save a note",    "memo",
        };

        for (const auto prefix : prefixes) {
            if (_starts_with(value, prefix)) {
                value.erase(0, prefix.size());
                _strip_leading_separator(value);
                changed = true;
                break;
            }
        }
    }

    return value;
}

static std::string _extract_memo_text_from_user_message(std::string_view input)
{
    std::string value = _trim(input);
    _strip_wake_prefixes(value);
    if (value.empty()) {
        return "";
    }

    const std::string cleaned = _clean_memo_text(value);
    if (!cleaned.empty() && cleaned != value) {
        return cleaned;
    }

    constexpr std::string_view markers[] = {
        "备忘录内容就是", "备忘录内容是", "备忘录内容为", "内容就是", "内容是", "内容为",
        "内容写成",       "内容写",       "就写",         "写成",     "写下",   "记为",
        "记成",           "记录为",       "记录成",       "保存为",   "改成",   "改为",
        "修改成",         "修改为",
    };
    for (const auto marker : markers) {
        std::string candidate = _after_marker(value, marker);
        candidate = _clean_memo_text(candidate);
        if (!candidate.empty()) {
            return candidate;
        }
    }

    return "";
}

static std::string _choose_memo_content(std::string_view mcp_content)
{
    const std::string recent_user_message = hal_bridge::get_recent_user_message(kRecentUserMemoMaxAgeMs);
    const std::string user_content        = _extract_memo_text_from_user_message(recent_user_message);
    if (!user_content.empty()) {
        mclog::tagInfo(_tag, "using recent user transcript for memo content: transcript_len={} content_len={}",
                       recent_user_message.size(), user_content.size());
        return user_content;
    }
    return _clean_memo_text(mcp_content);
}

static std::string _truncate_utf8(std::string value, size_t max_bytes)
{
    if (value.size() <= max_bytes) {
        return value;
    }

    size_t end = 0;
    for (size_t i = 0; i < value.size();) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        size_t char_len       = 1;
        if ((c & 0x80) == 0) {
            char_len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            char_len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            char_len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            char_len = 4;
        }
        if (i + char_len > max_bytes) {
            break;
        }
        end = i + char_len;
        i += char_len;
    }
    value.resize(end);
    return value;
}

static int _now_unix()
{
    const time_t now = std::time(nullptr);
    return now >= 1700000000 ? static_cast<int>(now) : 0;
}

static std::string _json_string(cJSON* object)
{
    char* text = cJSON_PrintUnformatted(object);
    std::string result = text ? text : "{}";
    cJSON_free(text);
    return result;
}

static std::string _get_json_string(cJSON* object, const char* key)
{
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

static std::vector<MemoItem_t> _load_memos_locked()
{
    Settings settings(kMemoSettingsNs, false);
    const std::string stored = settings.GetString(kMemoItemsKey, "[]");

    std::vector<MemoItem_t> memos;
    cJSON* root = cJSON_Parse(stored.c_str());
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return memos;
    }

    const int count = std::min(cJSON_GetArraySize(root), kMaxMemoCount);
    memos.reserve(count);
    for (int i = 0; i < count; ++i) {
        cJSON* item = cJSON_GetArrayItem(root, i);
        if (!cJSON_IsObject(item)) {
            continue;
        }
        cJSON* id = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (!cJSON_IsNumber(id) || id->valueint <= 0) {
            continue;
        }

        MemoItem_t memo;
        memo.id          = id->valueint;
        memo.title       = _get_json_string(item, "title");
        memo.content     = _get_json_string(item, "content");
        cJSON* created   = cJSON_GetObjectItemCaseSensitive(item, "created_unix");
        cJSON* updated   = cJSON_GetObjectItemCaseSensitive(item, "updated_unix");
        memo.createdUnix = cJSON_IsNumber(created) ? created->valueint : 0;
        memo.updatedUnix = cJSON_IsNumber(updated) ? updated->valueint : memo.createdUnix;
        memos.push_back(std::move(memo));
    }
    cJSON_Delete(root);
    return memos;
}

static bool _save_memos_locked(const std::vector<MemoItem_t>& memos)
{
    cJSON* root = cJSON_CreateArray();
    for (const auto& memo : memos) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", memo.id);
        cJSON_AddStringToObject(item, "title", memo.title.c_str());
        cJSON_AddStringToObject(item, "content", memo.content.c_str());
        cJSON_AddNumberToObject(item, "created_unix", memo.createdUnix);
        cJSON_AddNumberToObject(item, "updated_unix", memo.updatedUnix);
        cJSON_AddItemToArray(root, item);
    }

    const std::string payload = _json_string(root);
    cJSON_Delete(root);
    if (payload.size() > kMaxStorageBytes) {
        mclog::tagError(_tag, "memo payload too large: {}", payload.size());
        return false;
    }

    Settings settings(kMemoSettingsNs, true);
    settings.SetString(kMemoItemsKey, payload);
    return true;
}

static std::string _memos_json(const std::vector<MemoItem_t>& memos, const char* event, bool ok = true,
                               const char* error = nullptr, int changed_id = -1)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "event", event);
    cJSON_AddNumberToObject(root, "count", memos.size());
    cJSON_AddNumberToObject(root, "max_count", kMaxMemoCount);
    if (changed_id > 0) {
        cJSON_AddNumberToObject(root, "id", changed_id);
    }
    if (error) {
        cJSON_AddStringToObject(root, "error", error);
    }
    cJSON_AddStringToObject(root, "assistant_instruction",
                            "Use these memos as the device-local memo list. For edits, keep ids stable and call the "
                            "create/update/delete memo tools instead of inventing state.");

    cJSON* array = cJSON_CreateArray();
    for (const auto& memo : memos) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", memo.id);
        cJSON_AddStringToObject(item, "title", memo.title.c_str());
        cJSON_AddStringToObject(item, "content", memo.content.c_str());
        cJSON_AddNumberToObject(item, "created_unix", memo.createdUnix);
        cJSON_AddNumberToObject(item, "updated_unix", memo.updatedUnix);
        cJSON_AddItemToArray(array, item);
    }
    cJSON_AddItemToObject(root, "memos", array);

    std::string result = _json_string(root);
    cJSON_Delete(root);
    return result;
}

static void _log_memos(const char* context, const std::vector<MemoItem_t>& memos)
{
    mclog::tagInfo(_tag, "{} count={}", context, memos.size());
    for (const auto& memo : memos) {
        mclog::tagInfo(_tag, "{} id={} title_len={} content_len={}", context, memo.id, memo.title.size(),
                       memo.content.size());
    }
}

std::vector<MemoItem_t> Hal::getMemoItems()
{
    if (_memo_mutex == nullptr) {
        _memo_mutex = xSemaphoreCreateMutex();
    }
    _lock_memo();
    auto memos = _load_memos_locked();
    _unlock_memo();
    return memos;
}

std::string Hal::getMemoListJson()
{
    auto memos = getMemoItems();
    _log_memos("list", memos);
    return _memos_json(memos, "list");
}

std::string Hal::createMemoJson(std::string_view title, std::string_view content)
{
    if (_memo_mutex == nullptr) {
        _memo_mutex = xSemaphoreCreateMutex();
    }

    const std::string clean_content = _truncate_utf8(_choose_memo_content(content), kMaxContentBytes);
    if (clean_content.empty()) {
        return _memos_json(getMemoItems(), "create", false, "content is required");
    }

    _lock_memo();
    auto memos = _load_memos_locked();
    if (memos.size() >= kMaxMemoCount) {
        auto result = _memos_json(memos, "create", false, "memo list is full");
        _unlock_memo();
        return result;
    }

    Settings settings(kMemoSettingsNs, true);
    int next_id = std::max<int>(settings.GetInt(kMemoNextIdKey, 1), 1);
    for (const auto& memo : memos) {
        if (memo.id >= next_id) {
            next_id = memo.id + 1;
        }
    }

    MemoItem_t memo;
    memo.id      = next_id;
    memo.title   = _truncate_utf8(_trim(title), kMaxTitleBytes);
    memo.content = clean_content;
    if (memo.title.empty()) {
        memo.title = _truncate_utf8(clean_content, kMaxTitleBytes);
    }
    memo.createdUnix = _now_unix();
    memo.updatedUnix = memo.createdUnix;
    memos.push_back(memo);

    if (!_save_memos_locked(memos)) {
        auto result = _memos_json(memos, "create", false, "failed to save memo");
        _unlock_memo();
        return result;
    }
    settings.SetInt(kMemoNextIdKey, next_id + 1);
    _log_memos("create", memos);
    auto result = _memos_json(memos, "create", true, nullptr, memo.id);
    _unlock_memo();
    return result;
}

std::string Hal::updateMemoJson(int id, std::string_view title, std::string_view content)
{
    if (_memo_mutex == nullptr) {
        _memo_mutex = xSemaphoreCreateMutex();
    }
    if (id <= 0) {
        return _memos_json(getMemoItems(), "update", false, "valid memo id is required");
    }

    _lock_memo();
    auto memos = _load_memos_locked();
    auto it = std::find_if(memos.begin(), memos.end(), [id](const MemoItem_t& memo) { return memo.id == id; });
    if (it == memos.end()) {
        auto result = _memos_json(memos, "update", false, "memo not found");
        _unlock_memo();
        return result;
    }

    const std::string clean_title   = _truncate_utf8(_trim(title), kMaxTitleBytes);
    const std::string clean_content = _truncate_utf8(content.empty() ? "" : _choose_memo_content(content),
                                                     kMaxContentBytes);
    if (clean_title.empty() && clean_content.empty()) {
        auto result = _memos_json(memos, "update", false, "title or content is required");
        _unlock_memo();
        return result;
    }
    if (!clean_title.empty()) {
        it->title = clean_title;
    }
    if (!clean_content.empty()) {
        it->content = clean_content;
    }
    it->updatedUnix = _now_unix();

    if (!_save_memos_locked(memos)) {
        auto result = _memos_json(memos, "update", false, "failed to save memo");
        _unlock_memo();
        return result;
    }
    _log_memos("update", memos);
    auto result = _memos_json(memos, "update", true, nullptr, id);
    _unlock_memo();
    return result;
}

std::string Hal::deleteMemoJson(int id)
{
    if (_memo_mutex == nullptr) {
        _memo_mutex = xSemaphoreCreateMutex();
    }
    if (id == 0) {
        return clearMemoJson();
    }
    if (id <= 0) {
        return _memos_json(getMemoItems(), "delete", false, "valid memo id is required");
    }

    _lock_memo();
    auto memos = _load_memos_locked();
    const auto old_size = memos.size();
    memos.erase(std::remove_if(memos.begin(), memos.end(), [id](const MemoItem_t& memo) { return memo.id == id; }),
                memos.end());
    if (memos.size() == old_size) {
        auto result = _memos_json(memos, "delete", false, "memo not found");
        _unlock_memo();
        return result;
    }
    if (!_save_memos_locked(memos)) {
        auto result = _memos_json(memos, "delete", false, "failed to save memo");
        _unlock_memo();
        return result;
    }
    _log_memos("delete", memos);
    auto result = _memos_json(memos, "delete", true, nullptr, id);
    _unlock_memo();
    return result;
}

std::string Hal::clearMemoJson()
{
    if (_memo_mutex == nullptr) {
        _memo_mutex = xSemaphoreCreateMutex();
    }

    _lock_memo();
    std::vector<MemoItem_t> memos;
    if (!_save_memos_locked(memos)) {
        auto result = _memos_json(memos, "clear", false, "failed to clear memos");
        _unlock_memo();
        return result;
    }
    Settings settings(kMemoSettingsNs, true);
    settings.SetInt(kMemoNextIdKey, 1);
    _log_memos("clear", memos);
    auto result = _memos_json(memos, "clear");
    _unlock_memo();
    return result;
}

static void _memo_overlay_delete_cb(lv_event_t* event)
{
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        _memo_overlay = nullptr;
    }
}

static void _memo_overlay_close_cb(lv_event_t* event)
{
    (void)event;
    if (_memo_overlay != nullptr) {
        lv_obj_delete(_memo_overlay);
    }
}

static std::string _memo_display_text(const MemoItem_t& memo)
{
    std::string text = "#";
    text += std::to_string(memo.id);
    text += " ";
    text += memo.content;
    return text;
}

static std::string _dashboard_clock_text()
{
    const time_t now = std::time(nullptr);
    if (now < 1700000000) {
        return "未校时";
    }

    struct tm timeinfo = {};
    localtime_r(&now, &timeinfo);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d  %02d/%02d", timeinfo.tm_hour, timeinfo.tm_min,
                  timeinfo.tm_mon + 1, timeinfo.tm_mday);
    return buffer;
}

static std::string _ml_text(const char* label, float value)
{
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%s %.0f ml", label, value);
    return buffer;
}

static lv_obj_t* _dashboard_label(lv_obj_t* parent, const lv_font_t* font, const char* text, uint32_t color = 0xF4F4F4,
                                  lv_coord_t width = 284)
{
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_line_space(label, 2, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t* _dashboard_section(lv_obj_t* parent, const lv_font_t* font, const char* title)
{
    lv_obj_t* section = lv_obj_create(parent);
    lv_obj_set_width(section, 292);
    lv_obj_set_height(section, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(section, lv_color_hex(0x151515), 0);
    lv_obj_set_style_border_color(section, lv_color_hex(0x303030), 0);
    lv_obj_set_style_border_width(section, 1, 0);
    lv_obj_set_style_radius(section, 6, 0);
    lv_obj_set_style_pad_all(section, 7, 0);
    lv_obj_set_style_pad_row(section, 5, 0);
    lv_obj_set_flex_flow(section, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(section, LV_OBJ_FLAG_SCROLLABLE);

    _dashboard_label(section, font, title, 0xFFFFFF, 274);
    return section;
}

static void _add_water_section(lv_obj_t* parent, const lv_font_t* font)
{
    const WaterMonitorStatus_t status = GetHAL().getWaterMonitorStatus();
    lv_obj_t* section                 = _dashboard_section(parent, font, "饮水");

    if (!status.scaleReady) {
        _dashboard_label(section, font, "Mini Scales 未响应", 0xE8C170, 274);
        return;
    }

    char summary[96];
    std::snprintf(summary, sizeof(summary), "%.0f / %d ml  剩余 %.0f ml", status.todayConsumedMl,
                  status.dailyGoalMl, status.remainingGoalMl);
    _dashboard_label(section, font, summary, status.dailyGoalMet ? 0x72E58B : 0xF4F4F4, 274);

    lv_obj_t* bar = lv_bar_create(section);
    lv_obj_set_size(bar, 274, 10);
    lv_bar_set_range(bar, 0, std::max(status.dailyGoalMl, 1));
    lv_bar_set_value(bar, std::min(static_cast<int>(status.todayConsumedMl), status.dailyGoalMl), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x303030), 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(status.dailyGoalMet ? 0x39D86A : 0x37A2FF), LV_PART_INDICATOR);

    std::string water_line = _ml_text("杯中", status.waterMl);
    char raw[64];
    std::snprintf(raw, sizeof(raw), "  原始 %.0f g", status.weightGrams);
    water_line += raw;
    _dashboard_label(section, font, water_line.c_str(), 0xD8D8D8, 274);

    if (!status.emptyCupSet) {
        _dashboard_label(section, font, "空杯重量未记录", 0xE8C170, 274);
    } else if (!status.baselineSet) {
        _dashboard_label(section, font, "补水基准未设置", 0xE8C170, 274);
    }
}

static void _add_location_section(lv_obj_t* parent, const lv_font_t* font)
{
    const LocationStatus_t location = GetHAL().getLocationStatus();
    lv_obj_t* section               = _dashboard_section(parent, font, "位置");

    if (!location.valid) {
        _dashboard_label(section, font, "暂未获取", 0xE8C170, 274);
        return;
    }

    std::string text = location.city.empty() ? "当前位置" : location.city;
    if (!location.region.empty() && location.region != location.city) {
        text += " · ";
        text += location.region;
    }
    if (location.stale) {
        text += " · 缓存";
    }
    _dashboard_label(section, font, text.c_str(), 0xD8D8D8, 274);
}

static void _add_reminder_section(lv_obj_t* parent, const lv_font_t* font)
{
    const auto reminders = tools::get_active_reminders();
    lv_obj_t* section    = _dashboard_section(parent, font, "提醒");

    if (reminders.empty()) {
        _dashboard_label(section, font, "暂无", 0xA8A8A8, 274);
        return;
    }

    char count[32];
    std::snprintf(count, sizeof(count), "%d 个进行中", static_cast<int>(reminders.size()));
    _dashboard_label(section, font, count, 0xD8D8D8, 274);

    const size_t shown = std::min<size_t>(reminders.size(), 2);
    for (size_t i = 0; i < shown; ++i) {
        std::string text = "#";
        text += std::to_string(reminders[i].id);
        text += " ";
        text += reminders[i].message;
        _dashboard_label(section, font, text.c_str(), 0xF4F4F4, 274);
    }
}

static void _add_memo_section(lv_obj_t* parent, const lv_font_t* font, const std::vector<MemoItem_t>& memos)
{
    char title[32];
    std::snprintf(title, sizeof(title), "备忘录 (%d/%d)", static_cast<int>(memos.size()), kMaxMemoCount);
    lv_obj_t* section = _dashboard_section(parent, font, title);

    if (memos.empty()) {
        _dashboard_label(section, font, "暂无", 0xA8A8A8, 274);
        return;
    }

    for (const auto& memo : memos) {
        const std::string text = _memo_display_text(memo);
        _dashboard_label(section, font, text.c_str(), 0xF4F4F4, 274);
    }
}

void Hal::showMemoOverlay()
{
    if (_memo_overlay != nullptr) {
        lv_obj_move_foreground(_memo_overlay);
        return;
    }

    const auto memos = getMemoItems();
    _log_memos("overlay", memos);
    const lv_font_t* memo_font = _memo_text_font();
    mclog::tagInfo(_tag, "overlay font line_height={} base_line={}", memo_font ? memo_font->line_height : 0,
                   memo_font ? memo_font->base_line : 0);

    _memo_overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(_memo_overlay, 320, 240);
    lv_obj_set_pos(_memo_overlay, 0, 0);
    lv_obj_set_style_bg_color(_memo_overlay, lv_color_hex(0x050505), 0);
    lv_obj_set_style_border_width(_memo_overlay, 0, 0);
    lv_obj_set_style_radius(_memo_overlay, 0, 0);
    lv_obj_set_style_pad_all(_memo_overlay, 8, 0);
    lv_obj_clear_flag(_memo_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(_memo_overlay, _memo_overlay_delete_cb, LV_EVENT_DELETE, nullptr);

    lv_obj_t* title = lv_label_create(_memo_overlay);
    lv_obj_set_style_text_font(title, memo_font, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(title, "今日中心");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 2);

    const std::string clock_text = _dashboard_clock_text();
    lv_obj_t* clock              = lv_label_create(_memo_overlay);
    lv_obj_set_style_text_font(clock, memo_font, 0);
    lv_obj_set_style_text_color(clock, lv_color_hex(0xD8D8D8), 0);
    lv_label_set_text(clock, clock_text.c_str());
    lv_obj_align(clock, LV_ALIGN_TOP_LEFT, 96, 2);

    lv_obj_t* close = lv_button_create(_memo_overlay);
    lv_obj_set_size(close, 42, 30);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -2, 0);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x303030), 0);
    lv_obj_set_style_radius(close, 6, 0);
    lv_obj_add_event_cb(close, _memo_overlay_close_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* close_label = lv_label_create(close);
    lv_obj_set_style_text_color(close_label, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(close_label, "X");
    lv_obj_center(close_label);

    lv_obj_t* list = lv_obj_create(_memo_overlay);
    lv_obj_set_size(list, 304, 190);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x050505), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_all(list, 2, 0);
    lv_obj_set_style_pad_row(list, 7, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    _add_water_section(list, memo_font);
    _add_location_section(list, memo_font);
    _add_reminder_section(list, memo_font);
    _add_memo_section(list, memo_font, memos);
}
