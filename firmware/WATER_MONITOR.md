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
- The refill baseline is saved to NVS under namespace `water_monitor`, key `baseline_cg`.
- The custom firmware disables firmware OTA updates so the device does not overwrite this build after Wi-Fi setup.

The local MCP tools exposed to the Agent are:

- `self.water.get_status`
- `self.water.set_refill_baseline`

Use `self.water.set_refill_baseline` when the user says they refilled water or wants to reset tracking. Use `self.water.get_status` when the user asks how much water was consumed or what the current bottle/cup weight is.

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
[HAL-Water] Mini Scales firmware version: 4
[HAL-MCP] add water.get_status tool
[HAL-MCP] add water.set_refill_baseline tool
```

Basic user test:

1. Place the filled cup or bottle on the Mini Scales.
2. Tell the M5 Agent: "I refilled my water. Reset my water baseline."
3. Ask: "How much water have I consumed?"
4. Drink some water and place the same cup or bottle back on the scale.
5. Ask again. The reported consumed amount should roughly equal the weight difference in milliliters.
