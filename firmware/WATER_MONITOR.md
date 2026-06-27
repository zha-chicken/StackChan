# Water Monitor Build

This firmware variant turns a StackChan K151 / CoreS3 into a standalone water monitor when an M5Stack Unit Mini Scales is connected to Port A.

## Hardware

- StackChan K151 / CoreS3
- M5Stack Unit Mini Scales connected to StackChan Port A
- Port A I2C pins:
  - SDA: GPIO2
  - SCL: GPIO1
- Mini Scales I2C address: `0x26`

GPIO2 is reserved for Port A SDA in this build, so the original laser GPIO behavior is disabled.

## Firmware Behavior

- The firmware reads Mini Scales weight once per second.
- Water is estimated as `1 g = 1 ml`.
- The empty cup/bottle weight is saved to NVS under namespace `water_monitor`, key `empty_cup_cg`.
- Current water is calculated as `current_weight_g - empty_cup_g`.
- The refill baseline is saved to NVS under namespace `water_monitor`, key `baseline_cg`.
- Daily consumed water is accumulated from stable decreases in current water while the cup/bottle is present.
- The default daily goal is `1500 ml`.
- If today's consumed water is below the daily goal, the device creates a reminder every hour.
- The custom firmware disables firmware OTA updates so the device does not overwrite this build after Wi-Fi setup.

The local MCP tools exposed to the Agent are:

- `self.water.get_status`
- `self.water.set_empty_cup_weight`
- `self.water.set_refill_baseline`
- `self.water.set_daily_goal`

At the start of the first water-related conversation, call `self.water.get_status`.

If `empty_cup_set` is false:

1. Ask the user to place the empty cup or bottle on the Mini Scales.
2. Wait until the user confirms it is empty and stable.
3. Call `self.water.set_empty_cup_weight`.

Use `self.water.set_refill_baseline` when the user says they refilled water or wants to reset current-container tracking. Use `self.water.get_status` when the user asks how much water was consumed, how much water is currently in the cup/bottle, or whether they have reached the daily goal.

## Hao Lab Body Connection

This build uses the upstream xiaozhi OTA and WebSocket protocol expected by Hao Lab body connection.

- Default OTA endpoint: `https://wexiyi.com/agents/api/ota/`
- The OTA response provides the device WebSocket URL, token, and protocol version.
- The firmware sends the required WebSocket headers: `Authorization`, `Device-Id`, `Client-Id`, and `Protocol-Version`.
- If an older device has `wifi/ota_url` set to the legacy tenclass or overseas endpoint in NVS, the firmware ignores that value and uses the compiled body endpoint instead.
- Pairing codes from OTA activation or WebSocket `alert` messages are shown in the StackChan speech bubble.

To connect:

1. Flash this firmware without erasing NVS if you want to keep Wi-Fi and water calibration.
2. Open `AI.AGENT` on the StackChan launcher, or enable `Start AI Agent on boot` in Setup for a dedicated water monitor.
3. Open the body connection page at `https://wexiyi.com/agents/dashboard/ta/body`.
4. Enter the 6-digit pairing code shown on the StackChan screen.

For the overseas deployment, create `firmware/sdkconfig.defaults.local` with:

```ini
CONFIG_OTA_URL="https://haolab.ai/agents/api/ota/"
```

## Build And Flash

Fetch dependencies first if this is a fresh clone:

```bash
cd firmware
python3 ./fetch_repos.py
```

Build with ESP-IDF v5.5.4:

```bash
cd firmware
idf.py build
```

Flash a connected StackChan:

```bash
cd firmware
idf.py -p /dev/cu.usbmodem3101 flash
```

Use the serial port that matches the target machine, for example `/dev/cu.usbmodem3101` on macOS.

## Validation

After flashing, open the monitor:

```bash
cd firmware
idf.py -p /dev/cu.usbmodem3101 monitor
```

Expected boot logs include:

```text
[HAL-Water] init Mini Scales on Port A
[HAL-Water] loaded empty cup weight: ...
[HAL-Water] Mini Scales firmware version: 4
[HAL-MCP] add water.get_status tool
[HAL-MCP] add water.set_empty_cup_weight tool
[HAL-MCP] add water.set_refill_baseline tool
[HAL-MCP] add water.set_daily_goal tool
```

Basic user test:

1. Place the empty cup or bottle on the Mini Scales.
2. Tell the M5 Agent: "This is my empty cup. Record the empty cup weight."
3. Fill the cup or bottle, place it back on the scale, and say: "I refilled my water. Reset my water baseline."
4. Ask: "How much water is in my cup?"
5. Drink some water and place the same cup or bottle back on the scale.
6. Ask: "How much water have I consumed today?"
7. The reported daily consumed amount should roughly equal the stable weight drop in milliliters.
