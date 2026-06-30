/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <memory>
#include <cstdint>
#include <string>
#include <lvgl.h>
#include <functional>
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <smooth_lvgl.hpp>
#include <array>
#include <lvgl_image.h>
#include <string_view>

/**
 * @brief
 *
 */
enum class HeadPetGesture { None, Press, Release, SwipeForward, SwipeBackward };

/**
 * @brief
 *
 */
enum class WsSignalSource {
    Local = 0,
    Remote,
};

/**
 * @brief
 *
 */
struct WsTextMessage_t {
    std::string name;
    std::string content;
};

/**
 * @brief
 *
 */
enum class ImuMotionEvent {
    None = 0,
    Shake,
    PickUp,
};

/**
 * @brief
 *
 */
enum class AppConfigEvent {
    None = 0,
    AppConnected,
    AppDisconnected,
    TryWifiConnect,
    WifiConnectFailed,
    WifiConnected,
};

/**
 * @brief
 *
 */
enum class CommonLogLevel {
    Info = 0,
    Warning,
    Error,
};

/**
 * @brief
 *
 */
namespace app_center {

struct AppInfo_t {
    std::string name;
    std::string iconUrl;
    std::string description;
    std::string firmwareUrl;
};

using AppInfoList_t = std::vector<AppInfo_t>;

};  // namespace app_center

/**
 * @brief
 *
 */
enum class WifiStatus {
    None = 0,
    Low,
    Medium,
    High,
};

/**
 * @brief
 *
 */
struct UserAccountInfo_t {
    std::string username;
    std::string deviceName;
};

/**
 * @brief
 *
 */
struct XiaozhiConfig_t {
    uint32_t idleShutdownTimeSeconds = 600;
    bool allowShutdownWhenCharging   = false;
    uint8_t idleRandomMovementLevel  = 0;
    bool startAiAgentOnBoot          = true;
};

/**
 * @brief Current local water monitor state from the Mini Scales unit on Port A.
 *
 */
struct WaterMonitorStatus_t {
    bool scaleReady       = false;
    bool cupPresent       = false;
    bool emptyCupSet      = false;
    bool baselineSet      = false;
    bool dailyGoalMet     = false;
    float weightGrams     = 0.0f;
    float emptyCupGrams   = 0.0f;
    float waterMl         = 0.0f;
    float baselineGrams   = 0.0f;
    float baselineWaterMl = 0.0f;
    float consumedMl      = 0.0f;
    float todayConsumedMl = 0.0f;
    float remainingGoalMl = 0.0f;
    int dailyGoalMl       = 1500;
    int todayYmd          = 0;
    std::uint32_t lastUpdateMs = 0;
};

/**
 * @brief Local onboarding state used to personalize the M5 Agent.
 *
 */
struct OnboardingProfileStatus_t {
    bool active       = false;
    bool complete     = false;
    int step          = 0;
    std::string preferredName;
    std::string communicationStyle;
    std::string focusTopics;
    std::string relationshipStyle;
    std::string reminderStyle;
    std::string summary;
};

/**
 * @brief Local QWeather configuration. The API key is stored only in NVS.
 *
 */
struct WeatherConfig_t {
    std::string apiKey;
    std::string defaultLocation;
    std::string weatherHost = "devapi.qweather.com";
    std::string geoHost     = "geoapi.qweather.com";
    std::string lang        = "zh";
    std::string unit        = "m";
};

/**
 * @brief Approximate device location derived from public IP geolocation.
 *
 */
struct LocationStatus_t {
    bool valid = false;
    bool stale = false;
    std::string source;
    std::string provider;
    std::string publicIp;
    std::string city;
    std::string region;
    std::string country;
    std::string countryCode;
    std::string latitude;
    std::string longitude;
    std::string timezone;
    std::string accuracy;
    int updatedUnix = 0;
};

struct MemoItem_t {
    int id = -1;
    std::string title;
    std::string content;
    int createdUnix = 0;
    int updatedUnix = 0;
};

/**
 * @brief
 *
 */
enum class MicTestStatus {
    Starting = 0,
    Recording,
    Playing,
    Done,
    Failed,
};

/**
 * @brief
 *
 */
class BootLogo {
public:
    BootLogo()
    {
        _panel = std::make_unique<uitk::lvgl_cpp::Container>(lv_screen_active());
        _panel->setSize(320, 240);
        _panel->setAlign(LV_ALIGN_CENTER);
        _panel->setBorderWidth(0);
        _panel->setBgOpa(0);
        _panel->setPaddingAll(0);

        _label_logo = std::make_unique<uitk::lvgl_cpp::Label>(_panel->get());
        _label_logo->setTextFont(&lv_font_montserrat_24);
        _label_logo->setTextColor(lv_color_hex(0xFFFFFF));
        _label_logo->align(LV_ALIGN_CENTER, 0, -14);
        _label_logo->setText("STACKCHAN");

        _label_msg = std::make_unique<uitk::lvgl_cpp::Label>(_panel->get());
        _label_msg->setTextFont(&lv_font_montserrat_16);
        _label_msg->setTextColor(lv_color_hex(0xBFBFBF));
        _label_msg->align(LV_ALIGN_CENTER, 0, 14);
        _label_msg->setText("Starting up ...");

        _label_version = std::make_unique<uitk::lvgl_cpp::Label>(_panel->get());
        _label_version->setTextFont(&lv_font_montserrat_14);
        _label_version->setTextColor(lv_color_hex(0x8B8B8B));
        _label_version->align(LV_ALIGN_BOTTOM_RIGHT, -7, -6);
        _label_version->setText("V" FIRMWARE_VERSION);
    }

private:
    std::unique_ptr<uitk::lvgl_cpp::Container> _panel;
    std::unique_ptr<uitk::lvgl_cpp::Label> _label_logo;
    std::unique_ptr<uitk::lvgl_cpp::Label> _label_msg;
    std::unique_ptr<uitk::lvgl_cpp::Label> _label_version;
};

/**
 * @brief
 *
 */
class Hal {
public:
    void init();

    /* --------------------------------- System --------------------------------- */
    void delay(std::uint32_t ms);
    std::uint32_t millis();
    void feedTheDog();
    std::array<uint8_t, 6> getFactoryMac();
    std::string getFactoryMacString(std::string divider = "");
    void reboot();
    void updateHeapStatusLog();
    uint8_t getBatteryLevel();
    bool isBatteryCharging();
    void factoryReset();

    /* --------------------------------- Display -------------------------------- */
    lv_indev_t* lvTouchpad = nullptr;
    std::unique_ptr<BootLogo> bootLogo;
    void lvglLock();
    void lvglUnlock();
    void setBackLightBrightness(uint8_t brightness, bool permanent = false);
    uint8_t getBackLightBrightness();

    /* --------------------------------- Xiaozhi -------------------------------- */
    void requestXiaozhiStart()
    {
        _xiaozhi_start_requested = true;
    }
    bool isXiaozhiStartRequested()
    {
        return _xiaozhi_start_requested;
    }
    void startXiaozhi();
    XiaozhiConfig_t getXiaozhiConfig();
    void setXiaozhiConfig(XiaozhiConfig_t config);

    /* ------------------------------ Water Monitor ---------------------------- */
    WaterMonitorStatus_t getWaterMonitorStatus();
    bool setWaterEmptyCupWeight();
    bool setWaterRefillBaseline();
    bool setWaterDailyGoal(int goalMl);
    void waterMonitorUpdateReminder();

    /* ------------------------------ Onboarding ------------------------------- */
    OnboardingProfileStatus_t getOnboardingProfileStatus();
    std::string getOnboardingProfileJson();
    std::string startOnboarding();
    std::string recordOnboardingAnswer(std::string_view answer);
    std::string resetOnboardingProfile();

    /* -------------------------------- Weather -------------------------------- */
    WeatherConfig_t getWeatherConfig();
    std::string getCurrentWeatherJson(std::string_view location = "", std::string_view lang = "",
                                      std::string_view unit = "");

    /* -------------------------------- Location -------------------------------- */
    LocationStatus_t getLocationStatus();
    std::string getCurrentLocationJson(bool refresh = false);
    std::string setManualLocationJson(std::string_view city, std::string_view latitude = "",
                                      std::string_view longitude = "", std::string_view region = "",
                                      std::string_view country = "", std::string_view timezone = "");

    /* ---------------------------------- Memo ---------------------------------- */
    std::vector<MemoItem_t> getMemoItems();
    std::string getMemoListJson();
    std::string createMemoJson(std::string_view title, std::string_view content);
    std::string updateMemoJson(int id, std::string_view title = "", std::string_view content = "");
    std::string deleteMemoJson(int id);
    std::string clearMemoJson();
    void showMemoOverlay();

    /* ----------------------------------- BLE ---------------------------------- */
    uitk::Signal<const char*> onBleMotionData;
    uitk::Signal<const char*> onBleAvatarData;
    uitk::Signal<const char*> onBleConfigData;
    uitk::Signal<const char*> onBleRgbData;
    uitk::Signal<AppConfigEvent> onAppConfigEvent;

    void startBleServer();
    bool isBleConnected();
    void startAppConfigServer();
    bool isAppConfiged();
    void resetAppConfiged();

    /* --------------------------------- HeadPet -------------------------------- */
    uitk::Signal<HeadPetGesture> onHeadPetGesture;

    /* ----------------------------------- RGB ---------------------------------- */
    void setRgbColor(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
    void showRgbColor(uint8_t r, uint8_t g, uint8_t b);
    void refreshRgb();

    /* ---------------------------------- Power --------------------------------- */
    void setServoPowerEnabled(bool enabled);

    /* -------------------------------- Websocket ------------------------------- */
    uitk::Signal<std::string_view> onWsMotionData;
    uitk::Signal<std::string_view> onWsAvatarData;
    uitk::Signal<std::string> onWsCallRequest;
    uitk::Signal<bool> onWsCallResponse;
    uitk::Signal<WsSignalSource> onWsCallEnd;
    uitk::Signal<const WsTextMessage_t&> onWsTextMessage;
    uitk::Signal<bool> onWsVideoModeChange;
    uitk::Signal<std::shared_ptr<LvglImage>> onWsVideoFrame;
    uitk::Signal<std::string_view> onWsDanceData;
    uitk::Signal<CommonLogLevel, std::string_view> onWsLog;

    void startWebSocketAvatarService(std::function<void(std::string_view)> onStartLog);

    /* ----------------------------------- IMU ---------------------------------- */
    uitk::Signal<ImuMotionEvent> onImuMotionEvent;

    /* ---------------------------------- Time ---------------------------------- */
    void syncRtcTimeToSystem();
    void syncSystemTimeToRtc();
    void setTimezone(std::string_view tz);
    std::string getTimezone();

    /* --------------------------------- EspNow --------------------------------- */
    uitk::Signal<const std::vector<uint8_t>&> onEspNowData;
    void startEspNow(int channel);
    bool espNowSend(const std::vector<uint8_t>& data, const uint8_t* destAddr = nullptr);
    void setLaserEnabled(bool enabled);

    /* ------------------------------- Warm Reboot ------------------------------ */
    void requestWarmReboot(int appIndex);
    int getWarmRebootTarget();
    void clearWarmRebootRequest();

    /* --------------------------------- Network -------------------------------- */
    void startNetwork(std::function<void(std::string_view)> onLog);
    WifiStatus getWifiStatus();
    void startSntp();

    /* -------------------------------- App center ------------------------------- */
    app_center::AppInfoList_t fetchAppList();
    void launchApp(std::string_view url, std::function<void(int)> onProgress);

    /* --------------------------------- EzData --------------------------------- */
    void startEzDataService(std::function<void(std::string_view)> onStartLog);
    uitk::Signal<std::string_view> onEzdataPairCode;

    /* ------------------------------- User Acount ------------------------------ */
    UserAccountInfo_t getUserAccountInfo();
    bool updateAccountInfo(std::function<void(std::string_view)> onLog);
    bool unbindAccount(std::function<void(std::string_view)> onLog);

    /* ----------------------------------- OTA ---------------------------------- */
    bool updateFirmware(std::function<void(std::string_view)> onLog);

    /* ---------------------------------- Audio --------------------------------- */
    void setSpeakerVolume(uint8_t volume, bool permanent = false);
    uint8_t getSpeakerVolume();
    std::string startMicTest(std::function<void(MicTestStatus)> onStatusUpdate);
    void getMicWaveformFrame(std::vector<int16_t>& data);
    void clearupMicTest();

private:
    bool _xiaozhi_start_requested = false;

    void xiaozhi_board_init();
    void onboardingInit();
    void waterMonitorInit();
    void lvgl_init();
    void xiaozhi_mcp_init();
    void ble_init(bool useAltUuid);
    void servo_init();
    void head_touch_init();
    void io_expander_init();
    void imu_init();
    void rtc_init();
};

Hal& GetHAL();

/**
 * @brief
 *
 */
class LvglLockGuard {
public:
    LvglLockGuard()
    {
        GetHAL().lvglLock();
    }
    ~LvglLockGuard()
    {
        GetHAL().lvglUnlock();
    }
};
