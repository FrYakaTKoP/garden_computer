PlatformIO project for Tuttli9000 (ESP32‑S3 N16R8)

    Open this folder in VS Code with PlatformIO extension installed.
    Platform: espressif32, Framework: Arduino, Board: esp32-s3-devkitc-1.
    The project uses LittleFS for data/ files. PlatformIO will upload the data partition automatically when you run "Upload filesystem image" (see below).

Build & Upload

    Install PlatformIO extension in VS Code.
    In PlatformIO Home, open this project folder.
    Ensure your USB serial port is selected.
    Upload LittleFS data:
        PlatformIO: Project Tasks → Environment → esp32-s3-devkitc-1 → Advanced → Upload File System Image
        Or use pio command: pio run -t uploadfs
    Upload firmware:
        PlatformIO: Project Tasks → Environment → esp32-s3-devkitc-1 → Upload
        Or use: pio run -t upload

Notes & libraries

    lib_deps in platformio.ini will fetch ArduinoJson, AsyncTCP (S3-compatible fork), ESPAsyncWebServer and LittleFS_esp32.
    If AsyncTCP / ESPAsyncWebServer names fail to resolve, replace with exact git urls in lib_deps:
    e.g. lib_deps =
    https://github.com/lorol/AsyncTCP.git
    https://github.com/me-no-dev/ESPAsyncWebServer.git

Pinouts & placeholders

    Pump pins: GPIO16 (PUMP1), GPIO17 (PUMP2). Use appropriate driver/relay circuitry.
    Restart button: GPIO0 (active low).
    Modbus/RS485: Not included in main build; I can add an implementation using HardwareSerial2 and a DE/RE control pin if you want.

Troubleshooting

    If LittleFS mount fails, ensure data/ was uploaded with Upload File System Image and that partition CSV is selected in platformio.ini.
    If Async libraries cause build errors, try replacing lib_deps entries with the explicit GitHub URLs above.
