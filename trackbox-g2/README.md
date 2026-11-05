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
Hold encoder PUSH, press RESET, release RESET, and keep PUSH held for about 1–2 seconds to enter OTA mode. Once Wi-Fi is connected, TrackBox checks `version.txt`; when a newer version is present, it downloads `latest.bin`, flashes it and restarts automatically. The web OTA page remains available as a fallback.

## Diagnostic BLE-only preflight 27.09 (temporary)
- The default OTA file in main may temporarily point to this diagnostic image.
- On normal boot Wi-Fi is disabled and the program does **not** automatically connect to SE5000.
- On vehicle **standstill only**, press BACK once: scan, connect, then secure BLE using the historical 27.06 parameters.
- If authentication succeeds, the unit displays `AUTH OK`, holds BLE for 15 seconds and disconnects once. No automatic reconnection.
- **No ITS, FIFO, Credits or KWP queries are performed in 27.09.** Speed, odometer, clock and daily-driving data are intentionally unavailable.
- A failure shows `SCAN BRAK`, `CONNECT FAIL` or `AUTH FAIL`, with host error code. A prior 517 (0x205) means authentication failure.
- No bond records are erased automatically. Do not press CONF to clear bonds unless prompted to perform supervised re-pairing.
- Existing state-machine host tests and source preflight checks compile on GitHub Actions, but do not simulate the actual SE5000 firmware.
- Preserve 27.06/27.08 in git history. Do not label this preflight as field-proven.
