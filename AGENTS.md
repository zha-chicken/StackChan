# Repository Guidelines

## Project Structure & Module Organization

This repository contains the full StackChan stack. `firmware/` is the ESP-IDF firmware; HAL, water monitor, MCP, display, audio, and motion code live under `firmware/main/`. Local XiaoZhi integration patches are in `firmware/patches/`. `app/` is the Flutter app (`lib/`, `test/`, `assets/`). `server/` is the Go backend (`api/`, `internal/`, `manifest/`, `check_list/`). `remote/` contains the ESP-NOW remote controller firmware.

## Build, Test, and Development Commands

- `cd firmware && python3 ./fetch_repos.py`: fetch firmware dependencies.
- `cd firmware && idf.py build`: build ESP32-S3 firmware.
- `cd firmware && idf.py flash`: flash the connected device.
- `cd firmware && cmake -S tests -B build-host-tests && cmake --build build-host-tests && ctest --test-dir build-host-tests --output-on-failure`: run host-side C++ tests.
- `cd app && flutter pub get && flutter run`: install Flutter dependencies and run the app.
- `cd app && flutter test && flutter analyze`: run Flutter tests and static analysis.
- `cd server && go run .`: run the backend locally.
- `cd server && go build -o stackChan .`: build the server binary.

## Coding Style & Naming Conventions

C/C++ code uses the checked-in `.clang-format` Google-derived style: 4-space indentation and 120-column limit. Dart follows standard Flutter style: `camelCase` variables/functions, `PascalCase` classes, and `snake_case` JSON keys. Go code should be formatted with `gofmt`; keep package names short and lowercase.

## Testing Guidelines

Place firmware host tests in `firmware/tests/`, Flutter tests in `app/test/`, and Go tests beside the package under test using `*_test.go`. Prefer focused tests for protocol parsing, hardware-independent math, API handlers, and state transitions. Run the relevant test command before submitting changes.

## M5 Wi-Fi Configuration

Prefer the app/BLE setup flow. When changing Wi-Fi over USB, never erase full NVS; it also stores pairing tokens, water calibration, onboarding memory, volume, and device identity. First back up NVS:

```bash
esptool.py --chip esp32s3 --port /dev/cu.usbmodem3101 read-flash 0x9000 0x4000 nvs-backup.bin
```

Parse the backup with ESP-IDF `nvs_tool.py`, regenerate an image preserving all namespaces, and edit only `wifi` keys (`ssid`, `password`, optional numbered fallbacks). Verify the image, then write it back:

```bash
esptool.py --chip esp32s3 --port /dev/cu.usbmodem3101 write-flash 0x9000 nvs-updated.bin
```

After reset, confirm logs show `Connected to WiFi: <ssid>` and that websocket activation still succeeds. Never commit real SSIDs, passwords, or NVS dumps.

## Commit & Pull Request Guidelines

Commit subjects in this repo are short imperative phrases, for example `Add Hao Lab body connection support` or `Enable Mo Hu custom wake word`. Keep commits scoped to one concern. Pull requests should describe the affected module, list commands run, and mention hardware tested for firmware changes. Include screenshots or recordings for app UI changes and avoid committing secrets, local Wi-Fi credentials, generated build output, or private config.

## Related Repository Workflow

Any code, documentation, configuration, or script change in `haolab.ai` must go through a pull request. Do not commit directly on `main` or push local changes straight to `main`; create a feature branch, push it, and open a PR first.
