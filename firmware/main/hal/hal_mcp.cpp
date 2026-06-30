/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <cJSON.h>
#include <mooncake_log.h>
#include <mcp_server.h>
#include <stackchan/stackchan.h>
#include <apps/common/common.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <string_view>

using namespace stackchan;

static const std::string_view _tag = "HAL-MCP";

static std::string _json_string(cJSON* object)
{
    char* text = cJSON_PrintUnformatted(object);
    std::string result = text ? text : "{}";
    cJSON_free(text);
    return result;
}

static std::string _format_tm(const tm& value, const char* format)
{
    char buffer[40];
    if (std::strftime(buffer, sizeof(buffer), format, &value) == 0) {
        return "";
    }
    return buffer;
}

static std::string _format_utc_iso(time_t value)
{
    tm utc_tm;
    gmtime_r(&value, &utc_tm);
    return _format_tm(utc_tm, "%Y-%m-%dT%H:%M:%SZ");
}

static std::string _format_offset(int offset_minutes)
{
    const char sign = offset_minutes >= 0 ? '+' : '-';
    int abs_minutes = std::abs(offset_minutes);
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%c%02d:%02d", sign, abs_minutes / 60, abs_minutes % 60);
    return buffer;
}

static bool _iana_timezone_offset_minutes(std::string_view timezone, int& offset_minutes)
{
    if (timezone == "Asia/Shanghai" || timezone == "Asia/Chongqing" || timezone == "Asia/Hong_Kong" ||
        timezone == "Asia/Taipei" || timezone == "Asia/Macau" || timezone == "Asia/Singapore") {
        offset_minutes = 8 * 60;
        return true;
    }
    if (timezone == "Asia/Tokyo" || timezone == "Asia/Seoul") {
        offset_minutes = 9 * 60;
        return true;
    }
    if (timezone == "Asia/Bangkok" || timezone == "Asia/Ho_Chi_Minh" || timezone == "Asia/Jakarta") {
        offset_minutes = 7 * 60;
        return true;
    }
    if (timezone == "UTC" || timezone == "Etc/UTC" || timezone == "GMT" || timezone == "Etc/GMT") {
        offset_minutes = 0;
        return true;
    }
    return false;
}

static bool _posix_timezone_offset_minutes(std::string_view timezone, int& offset_minutes)
{
    const auto digit = std::find_if(timezone.begin(), timezone.end(), [](unsigned char ch) {
        return ch == '+' || ch == '-' || std::isdigit(ch);
    });
    if (digit == timezone.end()) {
        return false;
    }

    int sign = 1;
    size_t pos = static_cast<size_t>(digit - timezone.begin());
    if (timezone[pos] == '+') {
        sign = 1;
        ++pos;
    } else if (timezone[pos] == '-') {
        sign = -1;
        ++pos;
    }
    if (pos >= timezone.size() || !std::isdigit(static_cast<unsigned char>(timezone[pos]))) {
        return false;
    }

    int hours = 0;
    while (pos < timezone.size() && std::isdigit(static_cast<unsigned char>(timezone[pos]))) {
        hours = hours * 10 + (timezone[pos] - '0');
        ++pos;
    }

    int minutes = 0;
    if (pos < timezone.size() && timezone[pos] == ':') {
        ++pos;
        while (pos < timezone.size() && std::isdigit(static_cast<unsigned char>(timezone[pos]))) {
            minutes = minutes * 10 + (timezone[pos] - '0');
            ++pos;
        }
    }

    // POSIX TZ uses the inverse sign: CST-8 means UTC+8.
    offset_minutes = -(sign * (hours * 60 + minutes));
    return true;
}

static void _add_local_time_fields(cJSON* root, const char* prefix, time_t now, int offset_minutes)
{
    tm local_tm;
    const time_t shifted = now + offset_minutes * 60;
    gmtime_r(&shifted, &local_tm);

    cJSON_AddStringToObject(root, (std::string(prefix) + "_datetime").c_str(),
                            _format_tm(local_tm, "%Y-%m-%d %H:%M:%S").c_str());
    cJSON_AddStringToObject(root, (std::string(prefix) + "_date").c_str(), _format_tm(local_tm, "%Y-%m-%d").c_str());
    cJSON_AddStringToObject(root, (std::string(prefix) + "_time").c_str(), _format_tm(local_tm, "%H:%M:%S").c_str());
    cJSON_AddStringToObject(root, (std::string(prefix) + "_weekday").c_str(), _format_tm(local_tm, "%A").c_str());
    cJSON_AddNumberToObject(root, (std::string(prefix) + "_weekday_index").c_str(), local_tm.tm_wday);
}

static std::string _get_current_time_json()
{
    const time_t now = std::time(nullptr);
    const bool synced = now >= 1700000000;
    const char* env_tz = std::getenv("TZ");
    const std::string system_tz = env_tz && env_tz[0] ? env_tz : GetHAL().getTimezone();
    const auto location = GetHAL().getLocationStatus();

    int chosen_offset = 0;
    std::string chosen_timezone = system_tz;
    std::string timezone_source = "system";
    bool offset_known = _posix_timezone_offset_minutes(system_tz, chosen_offset);

    int location_offset = 0;
    if (!location.timezone.empty() && _iana_timezone_offset_minutes(location.timezone, location_offset)) {
        chosen_offset = location_offset;
        chosen_timezone = location.timezone;
        timezone_source = "location";
        offset_known = true;
    }

    tm system_local_tm;
    localtime_r(&now, &system_local_tm);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", synced);
    cJSON_AddBoolToObject(root, "synced", synced);
    cJSON_AddNumberToObject(root, "unix_seconds", static_cast<double>(now));
    cJSON_AddStringToObject(root, "utc_iso", _format_utc_iso(now).c_str());
    cJSON_AddStringToObject(root, "system_timezone_posix", system_tz.c_str());
    cJSON_AddStringToObject(root, "system_local_datetime", _format_tm(system_local_tm, "%Y-%m-%d %H:%M:%S").c_str());
    cJSON_AddStringToObject(root, "system_local_date", _format_tm(system_local_tm, "%Y-%m-%d").c_str());
    cJSON_AddStringToObject(root, "system_local_time", _format_tm(system_local_tm, "%H:%M:%S").c_str());
    cJSON_AddStringToObject(root, "system_local_weekday", _format_tm(system_local_tm, "%A").c_str());
    if (!location.timezone.empty()) {
        cJSON_AddStringToObject(root, "location_timezone", location.timezone.c_str());
    }
    if (!location.city.empty()) {
        cJSON_AddStringToObject(root, "location_city", location.city.c_str());
    }
    cJSON_AddStringToObject(root, "chosen_timezone", chosen_timezone.c_str());
    cJSON_AddStringToObject(root, "timezone_source", timezone_source.c_str());
    cJSON_AddBoolToObject(root, "utc_offset_known", offset_known);
    if (offset_known) {
        cJSON_AddNumberToObject(root, "utc_offset_minutes", chosen_offset);
        cJSON_AddStringToObject(root, "utc_offset", _format_offset(chosen_offset).c_str());
        _add_local_time_fields(root, "local", now, chosen_offset);
    }
    if (synced && offset_known) {
        cJSON_AddStringToObject(
            root, "assistant_instruction",
            "Use local_datetime, local_date, local_time, local_weekday and utc_offset as the source of truth for any "
            "question about current time, today, tomorrow, tonight, or relative time such as 'in two hours'.");
    } else {
        cJSON_AddStringToObject(
            root, "assistant_instruction",
            "The device clock is not synchronized or the local timezone offset is unknown. Do not guess the current "
            "time; tell the user the device cannot provide a reliable time yet.");
    }

    std::string result = _json_string(root);
    cJSON_Delete(root);
    return result;
}

void Hal::xiaozhi_mcp_init()
{
    mclog::tagInfo(_tag, "init");

    // https://github.com/78/xiaozhi-esp32/blob/main/docs/mcp-usage.md
    auto& mcp_server = McpServer::GetInstance();

    // System Prompt：
    // You can control the robot's head. Use get_yaw and get_pitch to sense current position. Use set_yaw for horizontal
    // movement and set_pitch for vertical movement. All angles are in degrees.

    mclog::tagInfo(_tag, "add time.get_current tool");
    mcp_server.AddTool(
        "self.time.get_current",
        "Get the device's current clock and local date/time. IMPORTANT: Call this tool before answering any question "
        "about current time, date, today, tomorrow, tonight, weekday, schedules, deadlines, reminders, or relative "
        "time such as 'in two hours'. Do not say a guessed time or placeholder time while checking. Do not answer "
        "time questions from model memory. If ok=false, say the device clock is not synchronized instead of guessing.",
        std::vector<Property>{}, [this](const PropertyList& properties) -> ReturnValue {
            auto result = _get_current_time_json();
            mclog::tagInfo(_tag, "time.get_current: {}", result);
            return result;
        });

    mclog::tagInfo(_tag, "add robot.get_head_angles tool");
    mcp_server.AddTool("self.robot.get_head_angles",
                       "Returns current yaw/pitch in degrees. Neutral position is {yaw:0, pitch:0}.",
                       std::vector<Property>{}, [this](const PropertyList& properties) -> ReturnValue {
                           LvglLockGuard lock;  // StackChan motion update is under the lvgl lock

                           auto& motion      = GetStackChan().motion();
                           int current_yaw   = motion.yawServo().getCurrentAngle() / 10;
                           int current_pitch = motion.pitchServo().getCurrentAngle() / 10;

                           auto result = fmt::format(R"({{"yaw": {}, "pitch": {}}})", current_yaw, current_pitch);
                           mclog::tagInfo(_tag, "get_head_angles: {}", result);
                           return result;
                       });

    mclog::tagInfo(_tag, "add robot.set_head_angles tool");
    mcp_server.AddTool("self.robot.set_head_angles",
                       "Adjust head position. GUIDELINES: "
                       "1. For natural interaction, stay within +/- 45 degrees. "
                       "2. Only use values > 70 if the user explicitly asks to look far away/behind. "
                       "3. Max ranges: Yaw(-128 to 128, -128 as your left), Pitch(0 to 90, 90 as your up). "
                       "Speed(100-1000, 150 is natural).",
                       PropertyList({Property("yaw", kPropertyTypeInteger, -9999, -9999, 128),
                                     Property("pitch", kPropertyTypeInteger, -9999, -9999, 90),
                                     Property("speed", kPropertyTypeInteger, 150, 100, 1000)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int speed = properties["speed"].value<int>();
                           int yaw   = properties["yaw"].value<int>();
                           int pitch = properties["pitch"].value<int>();

                           mclog::tagInfo(_tag, "motion set_angles: yaw: {}, pitch: {}, speed: {}", yaw, pitch, speed);

                           LvglLockGuard lock;

                           auto& motion = GetStackChan().motion();
                           if (pitch != -9999) {
                               motion.pitchServo().moveWithSpeed(pitch * 10, speed);
                           }
                           if (yaw != -9999) {
                               motion.yawServo().moveWithSpeed(yaw * 10, speed);
                           }

                           return true;
                       });

    mclog::tagInfo(_tag, "add robot.set_led_color tool");
    mcp_server.AddTool(
        "self.robot.set_led_color",
        "Set the color of the robot's INTERNAL onboard LED. This is NOT for room lights. "
        "Values: 0-168 (safe range). Red=168,0,0; Green=0,168,0; Blue=0,0,168; White=100,100,100; Off=0,0,0.",
        PropertyList({Property("red", kPropertyTypeInteger, 0, 0, 168),
                      Property("green", kPropertyTypeInteger, 0, 0, 168),
                      Property("blue", kPropertyTypeInteger, 0, 0, 168)}),
        [this](const PropertyList& properties) -> ReturnValue {
            int r = properties["red"].value<int>();
            int g = properties["green"].value<int>();
            int b = properties["blue"].value<int>();

            mclog::tagInfo(_tag, "set_led_color: r={}, g={}, b={}", r, g, b);

            LvglLockGuard lock;

            GetStackChan().leftNeonLight().setColor(r, g, b);
            GetStackChan().rightNeonLight().setColor(r, g, b);

            return true;
        });

    mclog::tagInfo(_tag, "add onboarding.get_profile tool");
    mcp_server.AddTool("self.onboarding.get_profile",
                       "Get the persistent local onboarding profile for this user. Call this at the start of a "
                       "conversation and before answers where personalization helps. If complete is true, use the "
                       "returned summary and assistant_instruction to adapt tone, detail level, examples, water "
                       "reminders, and how you address the user.",
                       std::vector<Property>{}, [this](const PropertyList& properties) -> ReturnValue {
                           auto result = GetHAL().getOnboardingProfileJson();
                           mclog::tagInfo(_tag, "onboarding.get_profile: {}", result);
                           return result;
                       });

    mclog::tagInfo(_tag, "add onboarding.start tool");
    mcp_server.AddTool("self.onboarding.start",
                       "Start or restart local onboarding. When the user says exactly 'onboarding' or asks you to "
                       "learn their preferences, call this tool. Then ask exactly the returned next_question and wait "
                       "for the user answer.",
                       std::vector<Property>{}, [this](const PropertyList& properties) -> ReturnValue {
                           auto result = GetHAL().startOnboarding();
                           mclog::tagInfo(_tag, "onboarding.start: {}", result);
                           return result;
                       });

    mclog::tagInfo(_tag, "add onboarding.record_answer tool");
    mcp_server.AddTool("self.onboarding.record_answer",
                       "Record one user answer during onboarding. During active onboarding, call this after every user "
                       "answer with the user's exact answer text. If the result has next_question, ask exactly that "
                       "question next. If complete is true, tell the user onboarding is done and use the returned "
                       "profile summary for future replies.",
                       PropertyList({Property("answer", kPropertyTypeString)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           auto answer = properties["answer"].value<std::string>();
                           auto result = GetHAL().recordOnboardingAnswer(answer);
                           mclog::tagInfo(_tag, "onboarding.record_answer: {}", result);
                           return result;
                       });

    mclog::tagInfo(_tag, "add onboarding.reset tool");
    mcp_server.AddTool("self.onboarding.reset",
                       "Clear the saved onboarding profile and preferences. Use this only when the user asks to reset "
                       "or forget their personalization profile.",
                       std::vector<Property>{}, [this](const PropertyList& properties) -> ReturnValue {
                           auto result = GetHAL().resetOnboardingProfile();
                           mclog::tagInfo(_tag, "onboarding.reset: {}", result);
                           return result;
                       });

    mclog::tagInfo(_tag, "add memo.list tool");
    mcp_server.AddTool("self.memo.list",
                       "List all local memos saved on this device. Use this before answering questions about existing "
                       "memos or before editing/deleting a memo by id.",
                       std::vector<Property>{}, [this](const PropertyList& properties) -> ReturnValue {
                           auto result = GetHAL().getMemoListJson();
                           mclog::tagInfo(_tag, "memo.list result_len={}", result.size());
                           return result;
                       });

    mclog::tagInfo(_tag, "add memo.create tool");
    mcp_server.AddTool("self.memo.create",
                       "Create a local memo. Use when the user asks you to remember, note, save, or memo something. "
                       "IMPORTANT: content must be the exact user-requested memo fact, not your reply, summary, "
                       "guess, joke, or extra explanation. If the user says '帮我记一下 X', set content to X only.",
                       PropertyList({Property("title", kPropertyTypeString, std::string("")),
                                     Property("content", kPropertyTypeString)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           auto title   = properties["title"].value<std::string>();
                           auto content = properties["content"].value<std::string>();
                           auto result  = GetHAL().createMemoJson(title, content);
                           mclog::tagInfo(_tag, "memo.create title_len={} content_len={} result_len={}", title.size(),
                                          content.size(), result.size());
                           return result;
                       });

    mclog::tagInfo(_tag, "add memo.update tool");
    mcp_server.AddTool("self.memo.update",
                       "Update an existing local memo by id. Call self.memo.list first if the target id is unknown. "
                       "Provide title, content, or both. IMPORTANT: content must be the corrected memo fact from the "
                       "user, not your reply or a paraphrase unless the user explicitly asked you to summarize it.",
                       PropertyList({Property("id", kPropertyTypeInteger),
                                     Property("title", kPropertyTypeString, std::string("")),
                                     Property("content", kPropertyTypeString, std::string(""))}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int id       = properties["id"].value<int>();
                           auto title   = properties["title"].value<std::string>();
                           auto content = properties["content"].value<std::string>();
                           auto result  = GetHAL().updateMemoJson(id, title, content);
                           mclog::tagInfo(_tag, "memo.update id={} title_len={} content_len={} result_len={}", id,
                                          title.size(), content.size(), result.size());
                           return result;
                       });

    mclog::tagInfo(_tag, "add memo.delete tool");
    mcp_server.AddTool("self.memo.delete",
                       "Delete one local memo by id. Call self.memo.list first if the target id is unknown. Use "
                       "self.memo.clear instead when the user asks to delete or clear all memos.",
                       PropertyList({Property("id", kPropertyTypeInteger)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int id      = properties["id"].value<int>();
                           auto result = GetHAL().deleteMemoJson(id);
                           mclog::tagInfo(_tag, "memo.delete id={} result_len={}", id, result.size());
                           return result;
                       });

    mclog::tagInfo(_tag, "add memo.clear tool");
    mcp_server.AddTool("self.memo.clear", "Delete all local memos saved on this device. Use only when the user asks "
                                          "to delete all, clear all, or empty the memo list.",
                       std::vector<Property>{}, [this](const PropertyList& properties) -> ReturnValue {
                           auto result = GetHAL().clearMemoJson();
                           mclog::tagInfo(_tag, "memo.clear result_len={}", result.size());
                           return result;
                       });

    mclog::tagInfo(_tag, "add water.get_status tool");
    mcp_server.AddTool("self.water.get_status",
                       "Get local water monitor status from the Mini Scales connected to Port A. Call this at the "
                       "start of the first water-related conversation. If empty_cup_set is false, guide the user to put "
                       "the empty cup/bottle on the scale, wait for a stable reading, then call "
                       "self.water.set_empty_cup_weight. Water is estimated as 1 gram equals 1 milliliter. water_ml is "
                       "current total weight minus the saved empty cup weight. today_consumed_ml tracks stable water "
                       "drops toward the daily goal.",
                       std::vector<Property>{}, [this](const PropertyList& properties) -> ReturnValue {
                           auto status = GetHAL().getWaterMonitorStatus();
                           auto result = fmt::format(
                               R"({{"scale_ready": {}, "cup_present": {}, "empty_cup_set": {}, "baseline_set": {}, "daily_goal_met": {}, "weight_g": {:.1f}, "empty_cup_g": {:.1f}, "water_ml": {:.1f}, "baseline_g": {:.1f}, "baseline_water_ml": {:.1f}, "consumed_ml": {:.1f}, "today_consumed_ml": {:.1f}, "daily_goal_ml": {}, "remaining_goal_ml": {:.1f}, "today_ymd": {}, "last_update_ms": {}}})",
                               status.scaleReady ? "true" : "false", status.cupPresent ? "true" : "false",
                               status.emptyCupSet ? "true" : "false", status.baselineSet ? "true" : "false",
                               status.dailyGoalMet ? "true" : "false", status.weightGrams, status.emptyCupGrams,
                               status.waterMl, status.baselineGrams, status.baselineWaterMl, status.consumedMl,
                               status.todayConsumedMl, status.dailyGoalMl, status.remainingGoalMl, status.todayYmd,
                               status.lastUpdateMs);
                           if (!status.scaleReady) {
                               result =
                                   fmt::format(R"({{"scale_ready": false, "cup_present": false, "empty_cup_set": {}, "baseline_set": {}, "daily_goal_met": {}, "weight_g": {:.1f}, "empty_cup_g": {:.1f}, "water_ml": {:.1f}, "baseline_g": {:.1f}, "baseline_water_ml": {:.1f}, "consumed_ml": {:.1f}, "today_consumed_ml": {:.1f}, "daily_goal_ml": {}, "remaining_goal_ml": {:.1f}, "today_ymd": {}, "last_update_ms": {}, "error": "Mini Scales is not responding on Port A address 0x26."}})",
                                               status.emptyCupSet ? "true" : "false",
                                               status.baselineSet ? "true" : "false",
                                               status.dailyGoalMet ? "true" : "false", status.weightGrams,
                                               status.emptyCupGrams, status.waterMl, status.baselineGrams,
                                               status.baselineWaterMl, status.consumedMl, status.todayConsumedMl,
                                               status.dailyGoalMl, status.remainingGoalMl, status.todayYmd,
                                               status.lastUpdateMs);
                           } else if (!status.emptyCupSet) {
                               if (!result.empty() && result.back() == '}') {
                                   result.erase(result.size() - 1);
                               }
                               result +=
                                   R"(,"setup_required":"empty_cup_weight","assistant_instruction":"Ask the user to put the empty cup or bottle on the scale. After they confirm it is empty and stable, call self.water.set_empty_cup_weight."})";
                           }
                           mclog::tagInfo(_tag, "water.get_status: {}", result);
                           return result;
                       });

    mclog::tagInfo(_tag, "add water.set_empty_cup_weight tool");
    mcp_server.AddTool("self.water.set_empty_cup_weight",
                       "Save the current Mini Scales reading as the empty cup/bottle weight. Use this when the user has "
                       "placed the empty cup/bottle on the scale and confirmed it is empty. This value is saved in NVS "
                       "and used for all future water_ml calculations.",
                       std::vector<Property>{}, [this](const PropertyList& properties) -> ReturnValue {
                           bool ok     = GetHAL().setWaterEmptyCupWeight();
                           auto status = GetHAL().getWaterMonitorStatus();
                           auto result = ok ? fmt::format(R"({{"ok": true, "empty_cup_g": {:.1f}, "water_ml": {:.1f}}})",
                                                          status.emptyCupGrams, status.waterMl)
                                            : std::string(
                                                  R"({"ok": false, "error": "Mini Scales is not responding or the empty cup weight is invalid."})");
                           mclog::tagInfo(_tag, "water.set_empty_cup_weight: {}", result);
                           return result;
                       });

    mclog::tagInfo(_tag, "add water.set_refill_baseline tool");
    mcp_server.AddTool("self.water.set_refill_baseline",
                       "Set the water refill baseline to the current Mini Scales reading. Use this after the user says "
                       "they refilled water, placed a full bottle/cup on the scale, or wants to reset water tracking. "
                       "If empty_cup_set is false, first ask the user to place the empty cup/bottle and call "
                       "self.water.set_empty_cup_weight.",
                       std::vector<Property>{}, [this](const PropertyList& properties) -> ReturnValue {
                           bool ok     = GetHAL().setWaterRefillBaseline();
                           auto status = GetHAL().getWaterMonitorStatus();
                           auto result = ok ? fmt::format(R"({{"ok": true, "baseline_g": {:.1f}, "baseline_water_ml": {:.1f}, "water_ml": {:.1f}, "consumed_ml": 0.0}})",
                                                          status.baselineGrams, status.baselineWaterMl,
                                                          status.waterMl)
                                            : std::string(
                                                  R"({"ok": false, "error": "Mini Scales is not responding, or empty cup weight has not been set yet."})");
                           mclog::tagInfo(_tag, "water.set_refill_baseline: {}", result);
                           return result;
                       });

    mclog::tagInfo(_tag, "add water.set_daily_goal tool");
    mcp_server.AddTool("self.water.set_daily_goal",
                       "Set the daily drinking goal in milliliters. Default is 1500 ml. Use this when the user asks to "
                       "change their target water intake.",
                       PropertyList({Property("goal_ml", kPropertyTypeInteger, 1500, 250, 5000)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int goal_ml = properties["goal_ml"].value<int>();
                           bool ok     = GetHAL().setWaterDailyGoal(goal_ml);
                           auto status = GetHAL().getWaterMonitorStatus();
                           auto result = ok ? fmt::format(R"({{"ok": true, "daily_goal_ml": {}, "remaining_goal_ml": {:.1f}}})",
                                                          status.dailyGoalMl, status.remainingGoalMl)
                                            : std::string(R"({"ok": false, "error": "daily goal must be 250-5000 ml."})");
                           mclog::tagInfo(_tag, "water.set_daily_goal: {}", result);
                           return result;
                       });

    mclog::tagInfo(_tag, "add location.get_current tool");
    mcp_server.AddTool(
        "self.location.get_current",
        "Get this device's approximate current location. The device uses api.ipify.org to detect its public IP, "
        "then resolves city-level geolocation on the device and caches it locally. A manually saved location is "
        "returned unless refresh is true. Set refresh to true only when the user asks to update location or the "
        "network changed. IP-based accuracy is city-level and may be wrong behind VPN, proxy, carrier-grade NAT, "
        "cloud, company, or campus networks. If this tool returns ok=false, do not infer the device location from "
        "the weather default; tell the user location is unavailable.",
        PropertyList({Property("refresh", kPropertyTypeBoolean, false)}),
        [this](const PropertyList& properties) -> ReturnValue {
            bool refresh = properties["refresh"].value<bool>();
            auto result  = GetHAL().getCurrentLocationJson(refresh);
            mclog::tagInfo(_tag, "location.get_current refresh={} result_len={}", refresh, result.size());
            return result;
        });

    mclog::tagInfo(_tag, "add location.set_manual tool");
    mcp_server.AddTool(
        "self.location.set_manual",
        "Save a user-confirmed device location and update the default weather location. Use this when the user "
        "corrects the device location, for example says they are in Hangzhou. City alone is accepted and will be "
        "resolved through the configured QWeather Geo API; latitude and longitude may be supplied directly when "
        "known.",
        PropertyList({Property("city", kPropertyTypeString, std::string("")),
                      Property("latitude", kPropertyTypeString, std::string("")),
                      Property("longitude", kPropertyTypeString, std::string("")),
                      Property("region", kPropertyTypeString, std::string("")),
                      Property("country", kPropertyTypeString, std::string("")),
                      Property("timezone", kPropertyTypeString, std::string(""))}),
        [this](const PropertyList& properties) -> ReturnValue {
            auto city      = properties["city"].value<std::string>();
            auto latitude  = properties["latitude"].value<std::string>();
            auto longitude = properties["longitude"].value<std::string>();
            auto region    = properties["region"].value<std::string>();
            auto country   = properties["country"].value<std::string>();
            auto timezone  = properties["timezone"].value<std::string>();
            auto result = GetHAL().setManualLocationJson(city, latitude, longitude, region, country, timezone);
            mclog::tagInfo(_tag, "location.set_manual city_len={} result_len={}", city.size(), result.size());
            return result;
        });

    mclog::tagInfo(_tag, "add weather.get_current tool");
    mcp_server.AddTool("self.weather.get_current",
                       "Get current outdoor weather from QWeather. Use the configured default location when location "
                       "is empty. For a different place, pass a city name, QWeather LocationID, or longitude,latitude. "
                       "Use lang like zh/en and unit m/i only when the user asks for a specific language or unit. The "
                       "API key is stored locally on the device and is never returned.",
                       PropertyList({Property("location", kPropertyTypeString, std::string("")),
                                     Property("lang", kPropertyTypeString, std::string("")),
                                     Property("unit", kPropertyTypeString, std::string(""))}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           auto location = properties["location"].value<std::string>();
                           auto lang     = properties["lang"].value<std::string>();
                           auto unit     = properties["unit"].value<std::string>();
                           auto result   = GetHAL().getCurrentWeatherJson(location, lang, unit);
                           mclog::tagInfo(_tag, "weather.get_current location_len={} result_len={}", location.size(),
                                          result.size());
                           return result;
                       });

    mclog::tagInfo(_tag, "add robot.create_reminder tool");
    mcp_server.AddTool("self.robot.create_reminder",
                       "Create a reminder. Duration is in seconds. Message is what to say when time is up. Set repeat "
                       "to true to repeat the reminder.",
                       PropertyList({Property("duration_seconds", kPropertyTypeInteger, 60, 1, 86400),
                                     Property("message", kPropertyTypeString, std::string("Time's up!")),
                                     Property("repeat", kPropertyTypeBoolean, false)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int duration_seconds = properties["duration_seconds"].value<int>();
                           std::string message  = properties["message"].value<std::string>();
                           bool repeat          = properties["repeat"].value<bool>();

                           // Default message
                           if (message.empty()) {
                               message = "Time's up!";
                           }

                           mclog::tagInfo(_tag, "create_reminder: duration={}s, message={}, repeat={}",
                                          duration_seconds, message, repeat);

                           int id = tools::create_reminder(duration_seconds * 1000, message, repeat);

                           return id;
                       });

    mclog::tagInfo(_tag, "add robot.get_reminders tool");
    mcp_server.AddTool("self.robot.get_reminders", "Get list of active reminders.", std::vector<Property>{},
                       [this](const PropertyList& properties) -> ReturnValue {
                           mclog::tagInfo(_tag, "get_reminders");
                           auto reminders          = tools::get_active_reminders();
                           std::string result_json = "[";
                           for (size_t i = 0; i < reminders.size(); ++i) {
                               const auto& r = reminders[i];
                               result_json +=
                                   fmt::format(R"({{"id": {}, "duration_ms": {}, "message": "{}", "repeat": {}}})",
                                               r.id, r.durationMs, r.message, r.repeat ? "true" : "false");
                               if (i < reminders.size() - 1) {
                                   result_json += ", ";
                               }
                           }
                           result_json += "]";
                           mclog::tagInfo(_tag, "get_reminders result: {}", result_json);
                           return result_json;
                       });

    mclog::tagInfo(_tag, "add robot.stop_reminder tool");
    mcp_server.AddTool("self.robot.stop_reminder", "Stop a reminder by ID.",
                       PropertyList({Property("id", kPropertyTypeInteger, -1)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int id = properties["id"].value<int>();
                           mclog::tagInfo(_tag, "stop_reminder: id={}", id);
                           tools::stop_reminder(id);
                           return true;
                       });
}
