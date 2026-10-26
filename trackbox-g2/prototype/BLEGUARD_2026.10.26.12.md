# TrackBox Gen2 26.12 BLEGUARD — diagnostic trial

Date: 2026-10-26
Firmware version: `2026.10.26.12-BLEGUARD-TEST`
Status: **TEST BUILD; not verified on a real SE5000 or in a vehicle**.
Working development baseline: `trackbox-g2/golden/TrackBox_Gen2_2026.10.23.11_LOCALCLOCK_USER_VERIFIED.ino` remains unchanged.

## Field observations, not causal proof

On Friday 2026-10-23 full vehicle power cycling restored tachograph BLE;
TrackBox then worked ~30 minutes while driving, and paired normally again
Monday 2026-10-26. During Monday driving the speed display stopped updating
after about 5–10 minutes while F912 odometer still updated; after further
connection or reset attempts the SE5000 stopped advertising to nRF Connect
as well as TrackBox. The tachograph's own functions were reported normal.

These observations do **not** prove whether the root cause is the TrackBox
request sequence, SE5000 BLE firmware, or their interaction.

## Changes from 23.11

- Normal F902 speed polling **1 second**, F912 odometer polling **2 seconds**,
  and F90B time synchronization remain unchanged.
- On F902 timeout or temporary TX failure, first auto retry after **10 seconds**,
  second after **30 seconds**. Successful F902 reply resets consecutive failures.
- On the **third consecutive F902 failure**, stop all F902/F912/F90B diagnostic
  read requests. Keep existing BLE connection rather than automatically
  reconnecting or clearing bonds; OLED shows `F902 3x timeout`, `Odczyt STOP`.
  Reconnection is manual using BACK, preferably only on a safe stop.
- Add **2.5-second diagnostic-traffic guard** after F902, F912 or F90B timeout
  to reduce interference from late responses.
- Ignore late or unsolicited **positive** F912 responses when an F912 reply
  is not the currently awaited response. F902 and F90B positive responses
  already check expected state.
- Version shown at startup changes to `BLEGUARD 26.12`.
- No hardware pin, OTA transport, display layout, overspeed logic, or
  tachograph pairing protocol changes.

## Validation

- Source changes checked for unique replacements and unchanged pin mapping.
- GitHub Actions workflow compiles ESP32-S3 (ESP32 core 3.3.7,
  NimBLE-Arduino 2.5.1, U8g2), then publishes `firmware/latest.bin`,
  `firmware/version.txt` and `firmware/latest.sha256` on success.
- Check published `version.txt` and BIN after successful workflow.
- Hardware stability is **unverified** until tested with the user's SE5000.
- This build remains experimental; tachograph functionality and compliance
  are independent of TrackBox. Never intentionally disable the tachograph
  or cut vehicle power while driving.

## Field test (on safe parking stops only)

1. OTA-update while parked; observe the OLED startup banner.
2. Drive normally if the primary tachograph itself is functioning; use
   TrackBox only as a supplemental display.
3. If speed disappears, safely stop and note approximate elapsed time,
   whether odometer/clock still work, and any BLE SAFE message.
4. On parking stop, scan the SE5000 with nRF Connect without repeated
   automatic retries and note whether it advertises.
5. If BLE stops advertising, report the observed sequence to the Scania
   service department and request SE5000 firmware/BLE diagnosis.
