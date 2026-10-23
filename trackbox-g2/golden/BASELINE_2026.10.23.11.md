# TrackBox Gen2 — confirmed development baseline

Version: `2026.10.23.11-LOCALCLOCK-TEST` (23 October 2026)
Status: **selected by the user as the starting point for further TrackBox Gen2 development.**

## User-confirmed on real hardware
- ESP32-S3 firmware successfully uploaded through automatic GitHub OTA.
- BLE connection with own Stoneridge SE5000 restored and working following tachograph power reset.
- F902 tachograph speed displayed with bigger digits on SH1106 128×64 OLED.
- F912 odometer displayed beneath speed.
- F90B time successfully retrieved; HH:MM shows correct local time (Norway, Europe/Oslo).

## Implemented but NOT user-tested
- >90 km/h: full-screen seconds elapsed timer.
- <=90 km/h: return to normal dashboard; counter resets on next overspeed.
- On F902 timeout: overspeed timer ends; automatic F902 retry at 3 seconds.
- Long-term BLE stability and CET/CEST time transitions need observation.

## Locked baseline / rollback
- Verified 23.11 source snapshot: `trackbox-g2/golden/TrackBox_Gen2_2026.10.23.11_LOCALCLOCK_USER_VERIFIED.ino`.
- Binary at immutable published commit:
  `https://raw.githubusercontent.com/Rumcajs201/media/9c272b35bf83cec005bede1eff64ae1dc54f3884/trackbox-g2/firmware/latest.bin`
- SHA256 of 23.11 binary: `a716eac3579b39e5d52632c9b2b5575109b47bc11b20714d052eba2eca08cab3`.
- Known working earlier versions: 23.10 source `trackbox-g2/golden/TrackBox_Gen2_2026.10.23.10_BLESCANPAIR_PRE_CLOCK.ino`; 23.8 source `trackbox-g2/golden/TrackBox_Gen2_2026.10.23.8_OVERSPEED90_BEFORE_SCAN_REPAIR.ino`, plus published binaries in respective pinned git commits.
- Do not rewrite this archived baseline when developing future versions; modify only active source and compile/publish new version.

## Hardware: current test setup
ESP32-S3, SH1106 128x64, GPIO: SDA 39, SCL 40, encoder PUSH 41 / A 42 / B 2, CONFIRM 38, BACK 1.
Target tachograph advertised name: `SE5000-3185059840`.
First stable data source is Stoneridge ITS BLE Diagnostic FIFO (single-packet [01 01] header) reading F912, F902, F90B.
Future hardware migration to TFT/touch/GPS/LED and mmWave presence sensor is a separate stage.
