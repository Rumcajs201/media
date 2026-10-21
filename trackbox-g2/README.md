# TrackBox Gen2

ESP32-S3 WROOM-1 N16R8 + SH1106 128x64.

## Frozen pinout
- CONFIRM: GPIO38
- SDA: GPIO39
- SCL: GPIO40
- encoder push: GPIO41
- encoder A: GPIO42
- encoder B: GPIO2
- BACK: GPIO1

## Firmware flow
Source: `trackbox-g2/TrackBox_Gen2/TrackBox_Gen2.ino`

GitHub Actions builds the sketch and publishes:
- `trackbox-g2/firmware/latest.bin`
- `trackbox-g2/firmware/version.txt`

Normal boot keeps Wi-Fi off and runs the tachograph BLE/F912 baseline.
Boot with BACK+CONFIRM enters OTA mode. Once Wi-Fi is connected, TrackBox checks `version.txt`; when a newer version is present, it downloads `latest.bin`, flashes it and restarts automatically. The web OTA page remains available as a fallback.
