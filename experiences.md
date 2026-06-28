# StackChan M5 / XiaoZhi Integration Experiences

This note captures practical lessons from bringing up a StackChan M5 as a water-monitoring XiaoZhi voice agent. It is intended for another coding agent working on a second StackChan M5.

## Repository Shape

- Main firmware work happens under `firmware/main/`.
- XiaoZhi integration is bridged through `firmware/main/hal/board/hal_bridge.*` and initialized from `firmware/main/hal/hal.cpp`.
- Device MCP tools live in `firmware/main/hal/hal_mcp.cpp`.
- Water monitor logic lives in `firmware/main/hal/hal_water_monitor.cpp`.
- Weather/location helpers live in `firmware/main/hal/hal_weather.cpp` and `firmware/main/hal/hal_location.cpp`.
- Local XiaoZhi source changes should be reflected in `firmware/patches/xiaozhi-esp32.patch`.

## Build And Flash

Use the existing ESP-IDF/PlatformIO toolchain. A known-good build path is:

```bash
IDF_PATH=/Users/benjamin/.platformio/packages/framework-espidf PATH=/Users/benjamin/.platformio/penv/.espidf-5.5.4/bin:/Users/benjamin/.platformio/packages/tool-ninja:/Users/benjamin/.platformio/packages/toolchain-xtensa-esp-elf/bin:/usr/bin:/bin:/usr/sbin:/sbin /Users/benjamin/.platformio/packages/tool-cmake/bin/cmake --build firmware/build
```

Flash only the app partition when NVS/pairing data must be preserved:

```bash
/Users/benjamin/.platformio/penv/.espidf-5.5.4/bin/python /Users/benjamin/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3 --port /dev/cu.usbmodem3101 write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x20000 firmware/build/stack-chan.bin
```

Never erase full flash unless the user explicitly accepts losing pairing, Wi-Fi, water calibration, onboarding memory, volume, and identity data.

## Wi-Fi And Pairing

Prefer BLE/app provisioning. If changing Wi-Fi over USB, back up NVS first and edit only Wi-Fi keys. Do not commit real SSIDs, passwords, NVS dumps, pairing codes, API keys, or copied production URLs with tokens.

When pairing to wexiyi/haolab body connection, the device gets OTA config first, then connects to the websocket. A pairing error can be caused by an expired code, stale OTA data, or server-side state. A successful bind does not automatically prove avatar/TTS is healthy.

Any change in the `haolab.ai` repository must go through a pull request. Do not push directly to `main`.

## MCP Tools

MCP is the right extension point for device facts and actions:

- `self.water.get_status`: read Mini Scales and water state.
- `self.water.set_empty_cup_weight`: save empty cup weight in NVS.
- `self.water.set_refill_baseline`: reset baseline after refill.
- `self.water.set_daily_goal`: change daily goal.
- `self.weather.get_current`: query QWeather.
- `self.location.get_current` / `self.location.set_manual`: get or correct approximate location.
- `self.time.get_current`: return device time and local date/time.

Tool descriptions matter. Make them explicit about when the model should call the tool. However, MCP alone cannot fully force the hosted Agent to call a tool before speaking; if strict behavior is needed, enforce it in the server/tool-routing policy.

Return compact JSON strings. Include an `assistant_instruction` field when the tool result should shape the reply.

## Water Monitor Logic

The Mini Scales is connected on Port A and is read as raw total weight. The important distinction:

- `weight_g`: raw scale reading.
- `empty_cup_g`: saved empty cup/bottle weight.
- `water_ml`: `max(weight_g - empty_cup_g, 0)`, assuming 1 g = 1 ml.
- `today_consumed_ml`: stable decreases in water amount during the current day.

First conversation should guide the user to put an empty cup on the scale, wait for a stable reading, then call `self.water.set_empty_cup_weight`. Keep the empty cup weight in NVS for long-term accuracy. Default daily goal is 1500 ml; proactive reminders are scheduled if the goal is not met.

If `scale_ready=false`, check Port A wiring and I2C address before debugging higher-level Agent behavior.

## Time, Weather, And Location

Do not hardcode Shanghai. The device can have `system_timezone_posix` as `GMT0`, while location may report `Asia/Shanghai` and a city such as Hangzhou. `self.time.get_current` should choose the location timezone when known.

IP-based location is approximate and can be wrong behind proxies, VPNs, campus/company NAT, or carrier networks. If the user corrects the location, save it with `self.location.set_manual`.

QWeather credentials must stay in device settings/NVS or local private config. Do not print or commit the key. If weather sometimes returns data then says unavailable, inspect tool JSON and model behavior separately; the API may succeed while the Agent wording is wrong.

## Avatar And Display

Large avatars can cause lag or restarts. Use small downloaded/converted assets, prefer RGB565 where possible, and avoid decoding large PNGs on the device. For circular web avatars on the square M5 screen, mask the avatar circle and fill the outside with black.

When changing local face assets, compress or resize the supplied images. Do not regenerate or reinterpret user-provided artwork unless explicitly asked.

Avatar sync is independent from TTS. If sound effects play but speech does not, first check server TTS status and websocket state before blaming the display/avatar code.

## Audio, Wake Word, And Servo

Wake-word detection should stay short; do not send long text through `detect`. Custom wake word was set to "墨狐".

Idle random servo motion was disabled because it was distracting. Sound localization using two microphones is possible but must be conservative: keep math cheap, avoid extra allocations in the audio path, and move servos only when XiaoZhi is not busy speaking/listening in a way that would disrupt interaction.

If the device appears stuck in `speaking`, confirm whether TTS packets arrive. If only notification sounds work, the audio codec path is likely fine and the failure is usually upstream TTS or websocket state.

## Debugging Checklist

1. Build first, then flash only the app partition.
2. Watch serial logs for `Guru`, `panic`, repeated `rst:`, websocket disconnects, and free SRAM.
3. Confirm SNTP sync before testing time-sensitive questions.
4. For MCP behavior, ask a direct question and watch for `HAL-MCP` tool logs.
5. If the Agent answers from memory before calling MCP, improve tool descriptions or fix server-side tool routing.
6. Keep an eye on memory after avatar sync; image decoding and audio buffers are common pressure points.
7. Treat server outages separately from device bugs, especially TTS.
