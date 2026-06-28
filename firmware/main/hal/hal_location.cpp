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
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

static const std::string_view _tag = "HAL-Location";

static constexpr const char* kLocationSettingsNs = "location";
static constexpr const char* kWeatherSettingsNs  = "weather";
static constexpr const char* kWeatherApiKeyKey   = "api_key";
static constexpr const char* kWeatherGeoHostKey  = "geo_host";
static constexpr const char* kWeatherLangKey     = "lang";
static constexpr const char* kDefaultGeoHost     = "geoapi.qweather.com";
static constexpr const char* kDefaultLang        = "zh";
static constexpr const char* kIpifyUrl           = "https://api.ipify.org?format=json";
static constexpr const char* kIpWhoIsBaseUrl     = "https://ipwho.is/";
static constexpr int kCacheTtlSeconds            = 24 * 60 * 60;

static std::string _json_string(cJSON* object)
{
    char* text = cJSON_PrintUnformatted(object);
    std::string result = text ? text : "{}";
    cJSON_free(text);
    return result;
}

static std::string _error_json(const char* error)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", error);
    std::string result = _json_string(root);
    cJSON_Delete(root);
    return result;
}

static std::string _get_string(cJSON* object, const char* key)
{
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

static std::string _trim(std::string value)
{
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
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

static std::string _format_double(double value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6f", value);
    std::string out(buffer);
    while (!out.empty() && out.back() == '0') {
        out.pop_back();
    }
    if (!out.empty() && out.back() == '.') {
        out.pop_back();
    }
    return out;
}

static std::string _get_number_string(cJSON* object, const char* key)
{
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsNumber(item)) {
        return _format_double(item->valuedouble);
    }
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

static bool _gunzip_if_needed(std::string& response, std::string& error);

static bool _request_json(std::string_view url, std::string& response, std::string& error,
                          std::string_view api_key = "")
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
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Accept-Encoding", "identity");
    http->SetHeader("User-Agent", "StackChan-M5/1.0");
    if (!api_key.empty()) {
        http->SetHeader("X-QW-Api-Key", std::string(api_key));
    }

    if (!http->Open("GET", std::string(url))) {
        error = "failed to open HTTP request";
        return false;
    }

    const int status_code = http->GetStatusCode();
    response = http->ReadAll();
    http->Close();

    if (status_code != 200) {
        error = "HTTP " + std::to_string(status_code);
        return false;
    }
    if (response.empty()) {
        error = "empty response";
        return false;
    }
    if (!_gunzip_if_needed(response, error)) {
        return false;
    }
    response = _trim(response);
    if (response.front() != '{') {
        error = "non-JSON response";
        return false;
    }
    return true;
}

static void _add_string_if_present(cJSON* object, const char* key, const std::string& value)
{
    if (!value.empty()) {
        cJSON_AddStringToObject(object, key, value.c_str());
    }
}

static void _add_number_string_if_present(cJSON* object, const char* key, const std::string& value)
{
    if (!value.empty()) {
        cJSON_AddNumberToObject(object, key, std::strtod(value.c_str(), nullptr));
    }
}

static bool _is_cache_fresh(int updated_unix)
{
    if (updated_unix <= 0) {
        return false;
    }
    const time_t now = std::time(nullptr);
    if (now < 1700000000) {
        return true;
    }
    return now - updated_unix < kCacheTtlSeconds;
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
        error = "invalid gzip response";
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
        error = "failed to decompress gzip response";
        return false;
    }

    response.assign(reinterpret_cast<const char*>(out), out_size);
    std::free(out);
    return true;
}

static std::string _status_json(const LocationStatus_t& status, bool refreshed, const std::string& refresh_error = "")
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", status.valid);
    cJSON_AddBoolToObject(root, "refreshed", refreshed);
    cJSON_AddBoolToObject(root, "stale", status.stale);
    _add_string_if_present(root, "source", status.source);
    _add_string_if_present(root, "provider", status.provider);
    _add_string_if_present(root, "public_ip", status.publicIp);
    _add_string_if_present(root, "city", status.city);
    _add_string_if_present(root, "region", status.region);
    _add_string_if_present(root, "country", status.country);
    _add_string_if_present(root, "country_code", status.countryCode);
    _add_number_string_if_present(root, "latitude", status.latitude);
    _add_number_string_if_present(root, "longitude", status.longitude);
    _add_string_if_present(root, "timezone", status.timezone);
    _add_string_if_present(root, "accuracy", status.accuracy);
    if (status.updatedUnix > 0) {
        cJSON_AddNumberToObject(root, "updated_unix", status.updatedUnix);
    }
    if (!refresh_error.empty()) {
        cJSON_AddStringToObject(root, "refresh_error", refresh_error.c_str());
    }
    std::string result = _json_string(root);
    cJSON_Delete(root);
    return result;
}

static LocationStatus_t _load_cached_location()
{
    Settings settings(kLocationSettingsNs, false);

    LocationStatus_t status;
    status.source      = settings.GetString("source", "");
    status.provider    = settings.GetString("provider", "");
    status.publicIp    = settings.GetString("ip", "");
    status.city        = settings.GetString("city", "");
    status.region      = settings.GetString("region", "");
    status.country     = settings.GetString("country", "");
    status.countryCode = settings.GetString("country_code", "");
    status.latitude    = settings.GetString("lat", "");
    status.longitude   = settings.GetString("lon", "");
    status.timezone    = settings.GetString("timezone", "");
    status.accuracy    = settings.GetString("accuracy", "");
    status.updatedUnix = settings.GetInt("updated_unix", 0);
    status.valid       = !status.latitude.empty() && !status.longitude.empty();
    status.stale       = status.valid && status.source != "manual" && !_is_cache_fresh(status.updatedUnix);
    return status;
}

static void _save_location(const LocationStatus_t& status)
{
    Settings settings(kLocationSettingsNs, true);
    settings.SetString("source", status.source);
    settings.SetString("provider", status.provider);
    settings.SetString("ip", status.publicIp);
    settings.SetString("city", status.city);
    settings.SetString("region", status.region);
    settings.SetString("country", status.country);
    settings.SetString("country_code", status.countryCode);
    settings.SetString("lat", status.latitude);
    settings.SetString("lon", status.longitude);
    settings.SetString("timezone", status.timezone);
    settings.SetString("accuracy", status.accuracy);
    settings.SetInt("updated_unix", status.updatedUnix);

    if (!status.latitude.empty() && !status.longitude.empty()) {
        Settings weather(kWeatherSettingsNs, true);
        weather.SetString("location", status.longitude + "," + status.latitude);
    }
}

static bool _resolve_city_with_qweather(std::string_view city, LocationStatus_t& status, std::string& error)
{
    Settings settings(kWeatherSettingsNs, false);
    const std::string api_key = settings.GetString(kWeatherApiKeyKey, "");
    if (api_key.empty()) {
        error = "QWeather API key is not configured";
        return false;
    }

    std::string geo_host = settings.GetString(kWeatherGeoHostKey, kDefaultGeoHost);
    if (geo_host.empty()) {
        geo_host = kDefaultGeoHost;
    }
    std::string lang = _trim(settings.GetString(kWeatherLangKey, kDefaultLang));
    if (lang == "zh-cn" || lang == "zh-CN" || lang == "zh_cn" || lang == "zh_CN") {
        lang = "zh";
    }
    if (lang.empty()) {
        lang = kDefaultLang;
    }

    const std::string url = "https://" + geo_host + "/geo/v2/city/lookup?location=" + _url_encode(city) +
                            "&lang=" + _url_encode(lang) + "&number=1";
    std::string response;
    if (!_request_json(url, response, error, api_key)) {
        return false;
    }

    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) {
        error = "failed to parse QWeather Geo response";
        return false;
    }

    const std::string code = _get_string(root, "code");
    cJSON* locations       = cJSON_GetObjectItemCaseSensitive(root, "location");
    cJSON* first           = cJSON_IsArray(locations) ? cJSON_GetArrayItem(locations, 0) : nullptr;
    if (code != "200" || first == nullptr) {
        cJSON_Delete(root);
        error = "QWeather Geo location not found";
        return false;
    }

    status.city        = _get_string(first, "name");
    status.region      = _get_string(first, "adm1");
    status.country     = _get_string(first, "country");
    status.countryCode = "";
    status.latitude    = _get_string(first, "lat");
    status.longitude   = _get_string(first, "lon");
    status.timezone    = _get_string(first, "tz");
    if (status.timezone.empty()) {
        status.timezone = "Asia/Shanghai";
    }
    cJSON_Delete(root);

    if (status.latitude.empty() || status.longitude.empty()) {
        error = "QWeather Geo did not return coordinates";
        return false;
    }
    return true;
}

static bool _fetch_public_ip(std::string& ip, std::string& error)
{
    std::string response;
    if (!_request_json(kIpifyUrl, response, error)) {
        return false;
    }

    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) {
        error = "failed to parse ipify response";
        return false;
    }
    ip = _get_string(root, "ip");
    cJSON_Delete(root);

    if (ip.empty()) {
        error = "ipify response did not contain an IP";
        return false;
    }
    return true;
}

static bool _fetch_ip_location(const std::string& ip, LocationStatus_t& status, std::string& error)
{
    std::string response;
    const std::string url = std::string(kIpWhoIsBaseUrl) + ip;
    if (!_request_json(url, response, error)) {
        return false;
    }

    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) {
        error = "failed to parse IP geolocation response";
        return false;
    }

    cJSON* success = cJSON_GetObjectItemCaseSensitive(root, "success");
    if (cJSON_IsBool(success) && !cJSON_IsTrue(success)) {
        std::string message = _get_string(root, "message");
        cJSON_Delete(root);
        error = message.empty() ? "IP geolocation failed" : message;
        return false;
    }

    cJSON* connection = cJSON_GetObjectItemCaseSensitive(root, "connection");
    cJSON* timezone   = cJSON_GetObjectItemCaseSensitive(root, "timezone");

    status.valid       = true;
    status.stale       = false;
    status.source      = "ip";
    status.provider    = "ipwho.is";
    status.publicIp    = ip;
    status.city        = _get_string(root, "city");
    status.region      = _get_string(root, "region");
    status.country     = _get_string(root, "country");
    status.countryCode = _get_string(root, "country_code");
    status.latitude    = _get_number_string(root, "latitude");
    status.longitude   = _get_number_string(root, "longitude");
    status.timezone    = _get_string(timezone, "id");
    status.accuracy    = "city";
    status.updatedUnix = static_cast<int>(std::time(nullptr));
    if (status.updatedUnix < 1700000000) {
        status.updatedUnix = 0;
    }

    const std::string isp = _get_string(connection, "isp");
    if (!isp.empty()) {
        status.provider += "/" + isp;
    }

    cJSON_Delete(root);
    if (status.latitude.empty() || status.longitude.empty()) {
        error = "IP geolocation did not return coordinates";
        return false;
    }
    return true;
}

LocationStatus_t Hal::getLocationStatus()
{
    return _load_cached_location();
}

std::string Hal::setManualLocationJson(std::string_view city, std::string_view latitude, std::string_view longitude,
                                       std::string_view region, std::string_view country,
                                       std::string_view timezone)
{
    LocationStatus_t status;
    status.valid       = true;
    status.stale       = false;
    status.source      = "manual";
    status.provider    = "user";
    status.city        = _trim(std::string(city));
    status.region      = _trim(std::string(region));
    status.country     = _trim(std::string(country));
    status.latitude    = _trim(std::string(latitude));
    status.longitude   = _trim(std::string(longitude));
    status.timezone    = _trim(std::string(timezone));
    status.accuracy    = "manual";
    status.updatedUnix = static_cast<int>(std::time(nullptr));
    if (status.updatedUnix < 1700000000) {
        status.updatedUnix = 0;
    }

    if (status.latitude.empty() || status.longitude.empty()) {
        if (status.city.empty()) {
            return _error_json("city or latitude/longitude is required");
        }
        std::string error;
        if (!_resolve_city_with_qweather(status.city, status, error)) {
            return _error_json(error.c_str());
        }
        status.source   = "manual";
        status.provider = "user/QWeather Geo";
        status.accuracy = "manual-city";
    }
    if (status.city.empty()) {
        status.city = "manual";
    }
    if (status.country.empty()) {
        status.country = "China";
    }
    if (status.timezone.empty()) {
        status.timezone = "Asia/Shanghai";
    }

    _save_location(status);
    mclog::tagInfo(_tag, "manual location saved: city={} country={}", status.city, status.country);
    return _status_json(status, true);
}

std::string Hal::getCurrentLocationJson(bool refresh)
{
    LocationStatus_t cached = _load_cached_location();
    if (cached.valid && !refresh && !cached.stale) {
        return _status_json(cached, false);
    }

    std::string ip;
    std::string error;
    if (!_fetch_public_ip(ip, error)) {
        if (cached.valid) {
            cached.stale = true;
            return _status_json(cached, false, error);
        }
        return _error_json(error.c_str());
    }

    LocationStatus_t refreshed;
    if (!_fetch_ip_location(ip, refreshed, error)) {
        if (cached.valid) {
            cached.stale = true;
            cached.publicIp = ip;
            return _status_json(cached, false, error);
        }
        return _error_json(error.c_str());
    }

    _save_location(refreshed);
    mclog::tagInfo(_tag, "location updated: city={} country={} ip_len={}", refreshed.city, refreshed.country,
                   refreshed.publicIp.size());
    return _status_json(refreshed, true);
}
