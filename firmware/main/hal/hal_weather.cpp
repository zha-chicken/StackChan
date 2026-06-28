/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"

#include <board.h>
#include <cJSON.h>
#include <http.h>
#define LODEPNG_NO_COMPILE_CPP
#include <libs/lodepng/lodepng.h>
#include <mooncake_log.h>
#include <settings.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

static const std::string_view _tag = "HAL-Weather";

static constexpr const char* kWeatherSettingsNs          = "weather";
static constexpr const char* kWeatherApiKeyKey           = "api_key";
static constexpr const char* kWeatherDefaultLocationKey  = "location";
static constexpr const char* kWeatherHostKey             = "weather_host";
static constexpr const char* kWeatherGeoHostKey          = "geo_host";
static constexpr const char* kWeatherLangKey             = "lang";
static constexpr const char* kWeatherUnitKey             = "unit";
static constexpr const char* kDefaultWeatherHost         = "devapi.qweather.com";
static constexpr const char* kDefaultGeoHost             = "geoapi.qweather.com";
static constexpr const char* kDefaultLang                = "zh";
static constexpr const char* kDefaultUnit                = "m";

static std::string _json_string(cJSON* object)
{
    char* text = cJSON_PrintUnformatted(object);
    std::string result = text ? text : "{}";
    cJSON_free(text);
    return result;
}

static std::string _error_json(const char* error, const char* setup_required = nullptr)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", error);
    if (setup_required) {
        cJSON_AddStringToObject(root, "setup_required", setup_required);
    }
    std::string result = _json_string(root);
    cJSON_Delete(root);
    return result;
}

static bool _is_unreserved_url_char(unsigned char c)
{
    return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

static std::string _url_encode(std::string_view value)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if (_is_unreserved_url_char(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

static std::string _trim(std::string value)
{
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

static bool _looks_like_direct_qweather_location(std::string_view location)
{
    if (location.empty()) {
        return false;
    }
    bool all_digits = true;
    for (unsigned char c : location) {
        if (!std::isdigit(c)) {
            all_digits = false;
            break;
        }
    }
    return all_digits || location.find(',') != std::string_view::npos;
}

static bool _skip_gzip_zero_terminated_field(std::string_view data, size_t& pos)
{
    while (pos < data.size() && data[pos] != '\0') {
        ++pos;
    }
    if (pos >= data.size()) {
        return false;
    }
    ++pos;
    return true;
}

static bool _gunzip_if_needed(std::string& response, std::string& error)
{
    if (response.size() < 2 || static_cast<unsigned char>(response[0]) != 0x1F ||
        static_cast<unsigned char>(response[1]) != 0x8B) {
        return true;
    }
    if (response.size() < 18 || static_cast<unsigned char>(response[2]) != 8) {
        error = "invalid gzip QWeather response";
        return false;
    }

    const std::string_view data(response.data(), response.size());
    const unsigned char flags = static_cast<unsigned char>(data[3]);
    size_t pos = 10;
    if (flags & 0x04) {
        if (pos + 2 > data.size()) {
            error = "truncated gzip extra field";
            return false;
        }
        const size_t extra_len = static_cast<unsigned char>(data[pos]) |
                                 (static_cast<unsigned char>(data[pos + 1]) << 8);
        pos += 2 + extra_len;
        if (pos > data.size()) {
            error = "truncated gzip extra data";
            return false;
        }
    }
    if ((flags & 0x08) && !_skip_gzip_zero_terminated_field(data, pos)) {
        error = "truncated gzip filename";
        return false;
    }
    if ((flags & 0x10) && !_skip_gzip_zero_terminated_field(data, pos)) {
        error = "truncated gzip comment";
        return false;
    }
    if (flags & 0x02) {
        pos += 2;
    }
    if (pos + 8 > data.size()) {
        error = "truncated gzip payload";
        return false;
    }

    unsigned char* out = nullptr;
    size_t out_size    = 0;
    LodePNGDecompressSettings settings;
    lodepng_decompress_settings_init(&settings);
    settings.max_output_size = 32768;

    const size_t compressed_size = data.size() - pos - 8;
    const unsigned inflate_error =
        lodepng_inflate(&out, &out_size, reinterpret_cast<const unsigned char*>(data.data() + pos), compressed_size,
                        &settings);
    if (inflate_error != 0 || out == nullptr) {
        if (out) {
            std::free(out);
        }
        error = "failed to decompress QWeather gzip response";
        return false;
    }

    response.assign(reinterpret_cast<const char*>(out), out_size);
    std::free(out);
    return true;
}

static void _normalize_weather_options(WeatherConfig_t& config)
{
    config.lang = _trim(config.lang);
    config.unit = _trim(config.unit);
    if (config.lang == "zh-cn" || config.lang == "zh-CN" || config.lang == "zh_cn" || config.lang == "zh_CN") {
        config.lang = "zh";
    }
    if (config.lang.empty()) {
        config.lang = kDefaultLang;
    }
    if (config.unit.empty()) {
        config.unit = kDefaultUnit;
    }
}

static bool _request_json(std::string_view url, std::string_view api_key, std::string& response, std::string& error)
{
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        error = "network is not ready";
        return false;
    }

    auto http = network->CreateHttp(0);
    if (!http) {
        error = "failed to create HTTP client";
        return false;
    }

    http->SetTimeout(12000);
    http->SetHeader("X-QW-Api-Key", std::string(api_key));
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Accept-Encoding", "identity");

    if (!http->Open("GET", std::string(url))) {
        error = "failed to connect to QWeather";
        return false;
    }

    const int status_code = http->GetStatusCode();
    response = http->ReadAll();
    http->Close();
    if (status_code != 200) {
        error = "QWeather HTTP " + std::to_string(status_code);
        return false;
    }
    if (response.empty()) {
        error = "empty QWeather response";
        return false;
    }
    if (!_gunzip_if_needed(response, error)) {
        return false;
    }
    return true;
}

static std::string _get_string(cJSON* object, const char* key)
{
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

static std::string _lookup_location_id(const WeatherConfig_t& config, std::string_view query, cJSON* output_location)
{
    const std::string url = "https://" + config.geoHost + "/geo/v2/city/lookup?location=" + _url_encode(query) +
                            "&lang=" + _url_encode(config.lang) + "&number=1";
    std::string response;
    std::string error;
    if (!_request_json(url, config.apiKey, response, error)) {
        cJSON_AddStringToObject(output_location, "lookup_error", error.c_str());
        return "";
    }

    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) {
        cJSON_AddStringToObject(output_location, "lookup_error", "failed to parse QWeather Geo response");
        return "";
    }

    std::string id;
    cJSON* locations = cJSON_GetObjectItemCaseSensitive(root, "location");
    cJSON* first = cJSON_IsArray(locations) ? cJSON_GetArrayItem(locations, 0) : nullptr;
    if (first && cJSON_IsObject(first)) {
        id = _get_string(first, "id");
        cJSON_AddStringToObject(output_location, "id", id.c_str());
        cJSON_AddStringToObject(output_location, "name", _get_string(first, "name").c_str());
        cJSON_AddStringToObject(output_location, "country", _get_string(first, "country").c_str());
        cJSON_AddStringToObject(output_location, "adm1", _get_string(first, "adm1").c_str());
        cJSON_AddStringToObject(output_location, "adm2", _get_string(first, "adm2").c_str());
        cJSON_AddStringToObject(output_location, "lat", _get_string(first, "lat").c_str());
        cJSON_AddStringToObject(output_location, "lon", _get_string(first, "lon").c_str());
    } else {
        cJSON_AddStringToObject(output_location, "lookup_error", "location not found");
    }

    cJSON_Delete(root);
    return id;
}

WeatherConfig_t Hal::getWeatherConfig()
{
    Settings settings(kWeatherSettingsNs, false);

    WeatherConfig_t config;
    config.apiKey          = settings.GetString(kWeatherApiKeyKey, "");
    config.defaultLocation = settings.GetString(kWeatherDefaultLocationKey, "");
    config.weatherHost     = settings.GetString(kWeatherHostKey, kDefaultWeatherHost);
    config.geoHost         = settings.GetString(kWeatherGeoHostKey, kDefaultGeoHost);
    config.lang            = settings.GetString(kWeatherLangKey, kDefaultLang);
    config.unit            = settings.GetString(kWeatherUnitKey, kDefaultUnit);

    if (config.weatherHost.empty()) {
        config.weatherHost = kDefaultWeatherHost;
    }
    if (config.geoHost.empty()) {
        config.geoHost = kDefaultGeoHost;
    }
    if (config.lang.empty()) {
        config.lang = kDefaultLang;
    }
    if (config.unit.empty()) {
        config.unit = kDefaultUnit;
    }
    _normalize_weather_options(config);

    return config;
}

std::string Hal::getCurrentWeatherJson(std::string_view location, std::string_view lang, std::string_view unit)
{
    WeatherConfig_t config = getWeatherConfig();
    if (config.apiKey.empty()) {
        return _error_json("QWeather API key is not configured on this device.", "qweather_api_key");
    }

    std::string requested_location = _trim(std::string(location));
    if (requested_location.empty()) {
        requested_location = config.defaultLocation;
    }
    if (requested_location.empty()) {
        return _error_json("Default weather location is not configured on this device.", "qweather_location");
    }
    if (!lang.empty()) {
        config.lang = _trim(std::string(lang));
    }
    if (!unit.empty()) {
        config.unit = _trim(std::string(unit));
    }
    _normalize_weather_options(config);

    cJSON* location_json = cJSON_CreateObject();
    std::string qweather_location = requested_location;
    if (!_looks_like_direct_qweather_location(requested_location)) {
        qweather_location = _lookup_location_id(config, requested_location, location_json);
        if (qweather_location.empty()) {
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", false);
            cJSON_AddStringToObject(root, "error", "Failed to resolve location through QWeather Geo API.");
            cJSON_AddStringToObject(root, "requested_location", requested_location.c_str());
            cJSON_AddItemToObject(root, "location", location_json);
            std::string result = _json_string(root);
            cJSON_Delete(root);
            return result;
        }
    } else {
        cJSON_AddStringToObject(location_json, "query", requested_location.c_str());
    }

    const std::string url = "https://" + config.weatherHost + "/v7/weather/now?location=" +
                            _url_encode(qweather_location) + "&lang=" + _url_encode(config.lang) +
                            "&unit=" + _url_encode(config.unit);

    std::string response;
    std::string error;
    if (!_request_json(url, config.apiKey, response, error)) {
        cJSON_Delete(location_json);
        return _error_json(error.c_str());
    }

    cJSON* qweather = cJSON_Parse(response.c_str());
    if (!qweather) {
        cJSON_Delete(location_json);
        return _error_json("failed to parse QWeather weather response");
    }

    const std::string code = _get_string(qweather, "code");
    if (code != "200") {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "QWeather returned a non-success code.");
        cJSON_AddStringToObject(root, "code", code.c_str());
        cJSON_AddStringToObject(root, "requested_location", requested_location.c_str());
        cJSON_AddItemToObject(root, "location", location_json);
        std::string result = _json_string(root);
        cJSON_Delete(root);
        cJSON_Delete(qweather);
        return result;
    }

    cJSON* now = cJSON_GetObjectItemCaseSensitive(qweather, "now");
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "provider", "QWeather");
    cJSON_AddStringToObject(root, "requested_location", requested_location.c_str());
    cJSON_AddStringToObject(root, "qweather_location", qweather_location.c_str());
    cJSON_AddStringToObject(root, "lang", config.lang.c_str());
    cJSON_AddStringToObject(root, "unit", config.unit.c_str());
    cJSON_AddStringToObject(root, "update_time", _get_string(qweather, "updateTime").c_str());
    cJSON_AddStringToObject(root, "fx_link", _get_string(qweather, "fxLink").c_str());
    cJSON_AddItemToObject(root, "location", location_json);

    cJSON* current = cJSON_CreateObject();
    if (now && cJSON_IsObject(now)) {
        cJSON_AddStringToObject(current, "obs_time", _get_string(now, "obsTime").c_str());
        cJSON_AddStringToObject(current, "text", _get_string(now, "text").c_str());
        cJSON_AddStringToObject(current, "icon", _get_string(now, "icon").c_str());
        cJSON_AddStringToObject(current, "temp", _get_string(now, "temp").c_str());
        cJSON_AddStringToObject(current, "feels_like", _get_string(now, "feelsLike").c_str());
        cJSON_AddStringToObject(current, "humidity_percent", _get_string(now, "humidity").c_str());
        cJSON_AddStringToObject(current, "precip", _get_string(now, "precip").c_str());
        cJSON_AddStringToObject(current, "pressure_hpa", _get_string(now, "pressure").c_str());
        cJSON_AddStringToObject(current, "visibility", _get_string(now, "vis").c_str());
        cJSON_AddStringToObject(current, "wind_dir", _get_string(now, "windDir").c_str());
        cJSON_AddStringToObject(current, "wind_scale", _get_string(now, "windScale").c_str());
        cJSON_AddStringToObject(current, "wind_speed", _get_string(now, "windSpeed").c_str());
        cJSON_AddStringToObject(current, "cloud_percent", _get_string(now, "cloud").c_str());
        cJSON_AddStringToObject(current, "dew", _get_string(now, "dew").c_str());
    }
    cJSON_AddItemToObject(root, "now", current);

    std::string result = _json_string(root);
    cJSON_Delete(root);
    cJSON_Delete(qweather);
    mclog::tagInfo(_tag, "weather.get_current location={} ok=true", requested_location);
    return result;
}
