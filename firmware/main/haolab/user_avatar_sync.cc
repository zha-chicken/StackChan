#include "user_avatar_sync.h"

#include <board.h>
#include <display/lvgl_display/lvgl_image.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal/board/stackchan_display.h>
#include <http.h>
#include <lvgl.h>

#include <atomic>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <string>

namespace haolab {
namespace {

constexpr char TAG[] = "UserAvatarSync";
constexpr size_t kAvatarWidth = 172;
constexpr size_t kAvatarHeight = 172;
constexpr size_t kRgb565Bytes = kAvatarWidth * kAvatarHeight * 2;
constexpr size_t kMaxAvatarBytes = 150 * 1024;
constexpr int kAvatarStride = kAvatarWidth * 2;

std::atomic<bool> g_sync_in_flight{false};

struct AvatarBuffer {
    uint8_t* data = nullptr;
    size_t size = 0;
};

void FreeAvatarBuffer(AvatarBuffer& buffer)
{
    if (buffer.data != nullptr) {
        heap_caps_free(buffer.data);
        buffer.data = nullptr;
    }
    buffer.size = 0;
}

void* AllocAvatarBytes(size_t size)
{
#if CONFIG_SPIRAM
    void* ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr != nullptr) {
        return ptr;
    }
#endif
    return heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

std::string NormalizeAvatarUrl(const std::string& url)
{
    if (url.find("/api/uploads/avatars/") == std::string::npos) {
        return url;
    }

    const auto query_pos = url.find('?');
    const auto base = query_pos == std::string::npos ? url : url.substr(0, query_pos);
    return base + "?w=172&fmt=rgb565";
}

bool ReadKnownLength(Http* http, size_t content_length, AvatarBuffer& out)
{
    if (content_length == 0) {
        ESP_LOGW(TAG, "Avatar response has no Content-Length; falling back to ReadAll");
        std::string body = http->ReadAll();
        if (body.empty()) {
            ESP_LOGE(TAG, "Empty avatar body");
            return false;
        }
        if (body.size() > kMaxAvatarBytes) {
            ESP_LOGE(TAG, "Avatar body too large: %u bytes", static_cast<unsigned>(body.size()));
            return false;
        }
        out.data = static_cast<uint8_t*>(AllocAvatarBytes(body.size()));
        if (out.data == nullptr) {
            ESP_LOGE(TAG, "OOM allocating %u avatar bytes", static_cast<unsigned>(body.size()));
            return false;
        }
        memcpy(out.data, body.data(), body.size());
        out.size = body.size();
        return true;
    }

    if (content_length > kMaxAvatarBytes) {
        ESP_LOGE(TAG, "Avatar body too large: %u bytes", static_cast<unsigned>(content_length));
        return false;
    }

    out.data = static_cast<uint8_t*>(AllocAvatarBytes(content_length));
    if (out.data == nullptr) {
        ESP_LOGE(TAG, "OOM allocating %u avatar bytes", static_cast<unsigned>(content_length));
        return false;
    }

    size_t total_read = 0;
    while (total_read < content_length) {
        int ret = http->Read(reinterpret_cast<char*>(out.data + total_read), content_length - total_read);
        if (ret < 0) {
            ESP_LOGE(TAG, "Failed to read avatar body: %d", ret);
            return false;
        }
        if (ret == 0) {
            break;
        }
        total_read += static_cast<size_t>(ret);
    }

    if (total_read != content_length) {
        ESP_LOGE(TAG, "Avatar body truncated: %u/%u bytes",
                 static_cast<unsigned>(total_read),
                 static_cast<unsigned>(content_length));
        return false;
    }

    out.size = total_read;
    return true;
}

bool DownloadAvatar(const std::string& url, AvatarBuffer& out)
{
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGE(TAG, "No network interface");
        return false;
    }

    auto http = network->CreateHttp(3);
    http->SetTimeout(15000);
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open avatar URL: %s", url.c_str());
        return false;
    }

    const int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "Avatar fetch HTTP %d: %s", status_code, url.c_str());
        http->Close();
        return false;
    }

    const size_t content_length = http->GetBodyLength();
    const bool ok = ReadKnownLength(http.get(), content_length, out);
    http->Close();
    return ok;
}

bool ApplyAvatar(AvatarBuffer& buffer)
{
    auto display = dynamic_cast<StackChanAvatarDisplay*>(Board::GetInstance().GetDisplay());
    if (display == nullptr) {
        ESP_LOGW(TAG, "Current display does not support user avatar");
        return false;
    }

    std::unique_ptr<LvglImage> image;
    if (buffer.size == kRgb565Bytes) {
        image = std::make_unique<LvglAllocatedImage>(
            buffer.data,
            buffer.size,
            static_cast<int>(kAvatarWidth),
            static_cast<int>(kAvatarHeight),
            kAvatarStride,
            LV_COLOR_FORMAT_RGB565);
        buffer.data = nullptr;
    } else {
        ESP_LOGW(TAG, "Avatar body is %u bytes, trying LVGL decoder fallback",
                 static_cast<unsigned>(buffer.size));
        try {
            image = std::make_unique<LvglAllocatedImage>(buffer.data, buffer.size);
            buffer.data = nullptr;
        } catch (const std::exception& error) {
            ESP_LOGE(TAG, "LVGL failed to decode avatar: %s", error.what());
            return false;
        }
    }

    display->SetUserAvatar(std::move(image));
    ESP_LOGI(TAG, "Avatar slot updated");
    return true;
}

void AvatarSyncTask(void* arg)
{
    std::unique_ptr<std::string> url(static_cast<std::string*>(arg));
    vTaskDelay(pdMS_TO_TICKS(2500));

    AvatarBuffer buffer;
    if (DownloadAvatar(*url, buffer)) {
        ESP_LOGI(TAG, "Downloaded %u avatar bytes from %s",
                 static_cast<unsigned>(buffer.size),
                 url->c_str());
        ApplyAvatar(buffer);
    }
    FreeAvatarBuffer(buffer);

    g_sync_in_flight.store(false);
    vTaskDelete(nullptr);
}

}  // namespace

void UserAvatarSync::SyncFromUrl(const std::string& url)
{
    if (url.empty()) {
        return;
    }
    if (g_sync_in_flight.exchange(true)) {
        ESP_LOGW(TAG, "Avatar sync already in flight; skipping");
        return;
    }

    auto normalized_url = std::make_unique<std::string>(NormalizeAvatarUrl(url));
    ESP_LOGI(TAG, "Scheduling avatar sync: %s", normalized_url->c_str());

    auto* task_arg = normalized_url.release();
    BaseType_t ret = xTaskCreate(AvatarSyncTask, "avatar_sync", 8192, task_arg, 2, nullptr);
    if (ret != pdPASS) {
        delete task_arg;
        g_sync_in_flight.store(false);
        ESP_LOGE(TAG, "Failed to create avatar sync task");
    }
}

}  // namespace haolab
