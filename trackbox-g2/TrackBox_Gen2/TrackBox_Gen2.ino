#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <NimBLEDevice.h>
#include <esp_ota_ops.h>
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>

// Parsed by GitHub Actions only; intentionally not used at runtime.
static const char* TB_FW_VERSION =
  "2026.10.27.05-START-UI-F997-TEST";

// ============================================================
// TrackBox Gen2 - Stoneridge SE5000 diagnostic tester
// ESP32-S3 + SH1106 128x64
//
// TEST:
//   BLE -> SE5000
//   Diagnostics Service
//   FIFO + Credits
//   ReadDataByIdentifier F912
//   HighResolutionTotalVehicleDistance
//
// F912 = 4 bytes
// 1 bit = 5 m = 0.005 km
// ============================================================


// ============================================================
// PINOUT - USTALONY, NIE ZMIENIAMY
// ============================================================

#define PIN_CONFIRM   38
#define PIN_SDA       39
#define PIN_SCL       40
#define PIN_ENC_PUSH  41
#define PIN_ENC_A     42
#define PIN_ENC_B      2
#define PIN_BACK       1


// ============================================================
// OLED
// ============================================================

U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(
  U8G2_R0,
  U8X8_PIN_NONE
);


// ============================================================
// BLE
// ============================================================

static const char* LOCAL_NAME =
  "TrackBox-G2-Rumcajs";

static const char* TACHO_PREFIX =
  "SE5000-";


// Download service - ten UUID widzielismy juz w reklamie SE5000
static const char* UUID_DOWNLOAD_SERVICE =
  "eef90782-55dd-4388-b80b-695aba7a69b5";


// Oficjalna usluga diagnostyczna Smart Tacho V2
static const char* UUID_DIAG_SERVICE =
  "fa213def-aef4-475c-bcea-0a8d69073efc";

// Diagnostics FIFO
static const char* UUID_DIAG_FIFO =
  "e413960c-75ba-4ca9-8a67-99bc052a1b13";

// Diagnostics Credits
static const char* UUID_DIAG_CREDITS =
  "e168d1a6-304f-42b4-ab96-4cd1d4efebd9";


// ============================================================
// KWP / DIAGNOSTYKA
// ============================================================

// Fizyczny adres VU wg Appendix 8
static const uint8_t VU_ADDRESS = 0xEE;

// Tester address.
// F0 jest uzywany w oficjalnych przykladach.
// VU powinien odpowiadac na dowolny adres testera.
static const uint8_t TESTER_ADDRESS = 0xF0;


// ReadDataByIdentifier
static const uint8_t SID_READ_DATA = 0x22;

// Positive response
static const uint8_t SID_READ_DATA_POS = 0x62;

// Negative response
static const uint8_t SID_NEGATIVE = 0x7F;


// F912
static const uint16_t DID_ODOMETER = 0xF912;
static const uint16_t DID_SPEED = 0xF902; // ISO 16844-7: 2 bytes / 256 km/h


// ============================================================
// BLE OBIEKTY
// ============================================================

NimBLEClient* client = nullptr;

NimBLERemoteService* diagService = nullptr;
NimBLERemoteCharacteristic* diagFifo = nullptr;
NimBLERemoteCharacteristic* diagCredits = nullptr;


// ============================================================
// STAN
// ============================================================

enum State
{
  ST_BOOT,
  ST_SCAN,
  ST_CONNECT,
  ST_SECURITY,
  ST_DIAG_SETUP,
  ST_DIAG_READY,
  ST_WAIT_ODOMETER,
  ST_WAIT_STARTCOMM,
  ST_ODOMETER_OK,
  ST_WAIT_SPEED,
  ST_WAIT_TIME,
  ST_WAIT_DAILY,
  ST_WAIT_REST,
  ST_ERROR
};

State state = ST_BOOT;


// ============================================================
// TACHOGRAF
// ============================================================

String tachoName = "";
String tachoAddress = "";

uint8_t tachoAddressType = 0;

// Scan diagnostics and explicit ESP bond-reset availability.
bool scanNotFound = false;
bool blePairingFailed = false;
uint32_t lastScanElapsedMs = 0;
uint32_t lastScanDevices = 0;
uint8_t lastScanRounds = 0;

volatile int tachoCredits = 0;

bool transportConfigured = false;
bool firstReadSent = false;

bool fallbackStartCommUsed = false;


// ============================================================
// ODOMETER
// ============================================================

uint32_t odometerRaw = 0;
double odometerKm = 0.0;

uint8_t lastOdoBytes[4] =
{
  0, 0, 0, 0
};


// ============================================================
// CZASY
// ============================================================

uint32_t requestStartMs = 0;

uint32_t diagReadyMs = 0;

bool retryOdometerPending = false;
uint32_t retryOdometerAt = 0;

// F902 is enabled only after F912 succeeds; disable on errors.
static const uint32_t SPEED_POLL_MS = 3000;
static const uint32_t SPEED_TIMEOUT_MS = 2500;
// Retry F902 3 s after a timeout; keep the current BLE connection and F912.
static const uint32_t SPEED_RETRY_MS = 3000;
uint32_t speedRetryAt = 0;
bool speedRetryPending = false;
uint32_t speedRetryAttempts = 0;
static const uint32_t SPEED_STALE_MS = 6000;

// Automatic F912 refresh: 2-second cadence without interrupting F902.
// On periodic failure retry later; never send StartCommunication as fallback.
static const uint32_t ODO_POLL_MS = 15000;
static const uint32_t ODO_RETRY_MS = 6000;
static const uint32_t ODO_PERIODIC_TIMEOUT_MS = 3500;
uint32_t nextOdoRequestAt = 0;
bool periodicOdoInFlight = false;
bool hasOdometer = false;
bool speedPollingEnabled = false;
bool speedValid = false;
bool diagnosticsPaused = false;
bool expectedDisconnect = false;
bool speedTimedOut = false;
bool speedPayloadInvalid = false;
bool manualOdoPending = false;
bool speedStaleDrawn = false;
uint8_t speedLastNrc = 0;
uint16_t speedRaw = 0;
float speedKmh = 0.0f;
uint32_t speedLastOkMs = 0;
uint32_t nextSpeedRequestAt = 0;
uint32_t speedReplies = 0;

// Optional ISO 16844-7 DID F99A: driver 1 current daily driving minutes.
// Poll ONLY while a fresh tachograph speed sample is > 0 km/h;
// never more than once in 5 minutes and never while other DID is in flight.
// If unsupported, refused or timed out, disable F99A for this BLE session.
static const uint32_t DAILY_DRIVE_POLL_MS = 300000UL;
static const uint32_t DAILY_DRIVE_TIMEOUT_MS = 3500UL;
bool dailyDriveEnabled = true;
bool dailyDriveValid = false;
uint16_t dailyDriveMinutes = 0;
uint32_t nextDailyDriveAtMs = 0;
uint32_t dailyDriveTxCount = 0;
uint32_t dailyDriveRxCount = 0;

// F997 = end of last daily rest. On this SE5000 it matched the driver's
// tachograph work-start time on the tested day, but is not a separate work
// start record. Display its local HH:MM as a tentative START on the dashboard.
// Never loop F997. One auto read per secured BLE session after a settling
// interval, only when stationary; encoder PUSH remains a manual override.
static const uint32_t REST_PROBE_TIMEOUT_MS = 3500UL;
static const uint32_t REST_AUTO_SETTLE_MS = 20000UL;
static const int64_t REST_START_MAX_AGE_SEC = 18LL * 3600LL;
uint32_t restProbeDisplayUntilMs = 0;
bool restProbePending = false; // manual PUSH queued until diagnostics idle
bool restAutoAttempted = false;
bool restManualInFlight = false;
bool restStartValid = false;
char restStartLocalText[6] = "--:--";

// Passive F902 diagnosis: no additional requests, and no automatic retry.
// Only F902 TIMEOUT uses a 1.5 s receive-only grace period before clean BLE close.
static const uint32_t F902_PASSIVE_GRACE_MS = 1500UL;
uint32_t f902GraceUntilMs = 0;
uint32_t f902TimeoutLastValidAgeMs = 0;
uint32_t traceF902TxOk = 0;
uint32_t traceF902RxOk = 0;
uint32_t traceF902Late = 0;
uint32_t traceF902ChecksumBad = 0;
uint32_t traceF902Unexpected = 0;
uint32_t traceF902LastRttMs = 0;
uint32_t traceF902MaxRttMs = 0;

// F90B TimeDate is queried independently, only when F902/F912 are idle.
// RTC holds local time while power remains on; no Wi-Fi is used in run mode.
static const uint32_t TIME_SYNC_PERIOD_MS = 3600000UL;
static const uint32_t TIME_RETRY_MS = 600000UL;
static const uint32_t TIME_TIMEOUT_MS = 1800UL;
bool clockValid = false;
uint32_t nextTimeSyncAtMs = 0;
char clockText[6] = "--:--";
int32_t clockMinuteStamp = -1;

// OLED-only overspeed timer: starts above 90 km/h, resets at <= 90 km/h.
// Uses real elapsed milliseconds rather than counting F902 responses.
// No extra tachograph queries, LEDs, buzzer, Wi-Fi or BLE changes.
static const uint16_t OVERSPEED_LIMIT_RAW = 90U * 256U;
bool overspeedActive = false;
uint32_t overspeedStartedAtMs = 0;
uint32_t overspeedLastShownSec = 0xFFFFFFFFUL;


// ============================================================
// BLE RX FIFO
// ============================================================

portMUX_TYPE rxMux =
  portMUX_INITIALIZER_UNLOCKED;

// Credits granted at startup must match available RX packet slots.
// Preserve 23.11's proven initial grant of 32, with 32 real slots.
static const uint8_t RX_CAPACITY = 32;
static const size_t RX_PACKET_MAX = 128;
struct RxPacket { uint8_t data[RX_PACKET_MAX]; uint8_t size; };
RxPacket rxQueue[RX_CAPACITY];
volatile uint8_t rxHead = 0, rxTail = 0, rxCount = 0;
volatile bool rxQueueOverflow = false;

// Credit returned only AFTER dequeue + parser handling, never in callback.
volatile uint8_t creditsToReturn = 0;


// ============================================================
// CREDIT CALLBACK
// ============================================================

portMUX_TYPE creditMux =
  portMUX_INITIALIZER_UNLOCKED;

// Tachograph grants are additive. One indication holds 0..254;
// 0xFF is a protocol-level close/reject, NOT +255 credits.
volatile uint32_t creditReceivedPending = 0;
volatile bool creditCounterOverflow = false;
volatile bool creditRejected = false;


// ============================================================
// BLE FLAGS
// ============================================================

volatile bool disconnected = false;

volatile bool passkeyShown = false;
volatile uint32_t pairingPasskey = 0;

volatile bool passkeyEntryRequested = false;

// Instrumentacja RX: tylko liczniki i OLED przy bledzie.
// Bez zmian w ramkach, subskrypcjach i sekwencji F912.
volatile uint32_t traceFifoCb = 0;
volatile uint32_t traceCreditCb = 0;
uint32_t traceParseCount = 0;
uint32_t traceBadChecksum = 0;
uint32_t traceF912TxOk = 0;
uint32_t traceStartTxOk = 0;
bool traceSecureOk = false;



// ============================================================
// BUTTON DEBOUNCE
// ============================================================

const uint32_t DEBOUNCE_MS = 40;

bool lastConfirm = HIGH;
bool lastBack = HIGH;
bool lastEncoderPush = HIGH;

uint32_t confirmChangeMs = 0;
uint32_t backChangeMs = 0;
uint32_t encoderPushChangeMs = 0;


// ============================================================
// POMOCNICZE - OLED
// ============================================================

void drawHeader(const char* txt)
{
  oled.clearBuffer();

  oled.setFont(u8g2_font_6x10_tf);

  oled.setCursor(0, 10);
  oled.print(txt);

  oled.drawHLine(0, 13, 128);
}


void showMessage(
  const char* title,
  const String& line1 = "",
  const String& line2 = "",
  const String& line3 = "",
  const String& line4 = ""
)
{
  drawHeader(title);

  if (line1.length())
  {
    oled.setCursor(0, 25);
    oled.print(line1);
  }

  if (line2.length())
  {
    oled.setCursor(0, 36);
    oled.print(line2);
  }

  if (line3.length())
  {
    oled.setCursor(0, 47);
    oled.print(line3);
  }

  if (line4.length())
  {
    oled.setCursor(0, 60);
    oled.print(line4);
  }

  oled.sendBuffer();
}


void showDiagReady()
{
  drawHeader("TrackBox G2 DIAG");

  oled.setCursor(0, 26);
  oled.print("DIAG READY");

  oled.setCursor(0, 39);
  oled.print("Credit Tacho: ");
  oled.print(tachoCredits);

  oled.setCursor(0, 52);
  oled.print("F912: gotowy");

  oled.setCursor(0, 63);
  oled.print("CONFIRM=odczyt");

  oled.sendBuffer();
}


void showOdometer()
{
  drawHeader("TrackBox G2 ODO");

  oled.setCursor(0, 25);
  oled.print("F912 OK");

  oled.setFont(u8g2_font_7x14B_tf);

  oled.setCursor(0, 45);

  // przebieg z trzema miejscami
  oled.print(odometerKm, 1);

  oled.setFont(u8g2_font_6x10_tf);

  oled.setCursor(0, 58);
  oled.print("km");

  oled.setCursor(30, 58);
  oled.print("Crd:");
  oled.print(tachoCredits);

  oled.sendBuffer();
}

// ============================================================
// TACHOGRAPH CLOCK: F90B is 8 bytes per EU 2016/799, Table 40.
// Fields: quarter-seconds, minutes, hours, month, quarter-days,
// years since 1985, local minute/hour offsets. Time is UTC.
// Convert UTC to Norway's local time using automatic CET/CEST.
// ============================================================

static bool isLeapYear(int year)
{
  return (year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0));
}

static uint8_t daysInMonth(int year, uint8_t month)
{
  static const uint8_t days[] =
    {31,28,31,30,31,30,31,31,30,31,30,31};
  if (month < 1 || month > 12) return 0;
  return (month == 2 && isLeapYear(year)) ? 29 : days[month - 1];
}

// UTC Gregorian date -> Unix days (independent of host TZ).
static int64_t unixDaysFromCivil(int y, unsigned m, unsigned d)
{
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static bool refreshClockText()
{
  if (!clockValid) return false;

  const time_t utcNow = time(nullptr);
  struct tm localNow;
  if (localtime_r(&utcNow, &localNow) == nullptr) return false;

  const int32_t minuteStamp =
    (int32_t)localNow.tm_year * 527040L +
    (int32_t)localNow.tm_yday * 1440L +
    localNow.tm_hour * 60 + localNow.tm_min;

  if (minuteStamp == clockMinuteStamp) return false;
  clockMinuteStamp = minuteStamp;
  snprintf(clockText, sizeof(clockText), "%02d:%02d",
           localNow.tm_hour, localNow.tm_min);
  return true;
}

static void timeSyncFailed(const char* reason, bool unsupported = false)
{
  Serial.printf("F90B sync skipped: %s\n", reason);
  state = ST_ODOMETER_OK;
  nextTimeSyncAtMs = millis() +
    (unsupported ? TIME_SYNC_PERIOD_MS : TIME_RETRY_MS);
  // Keep any previously synchronized clock running; never invent a time.
}

static void handleTimeDateF90B(const uint8_t* bytes)
{
  if (state != ST_WAIT_TIME) return;

  const unsigned sec = bytes[0] / 4U;
  const unsigned minute = bytes[1];
  const unsigned hour = bytes[2];
  const unsigned month = bytes[3];
  const unsigned day = bytes[4] / 4U;
  const int year = 1985 + bytes[5];

  if (year < 2020 || year > 2100 || sec >= 60 || minute >= 60 ||
      hour >= 24 || month < 1 || month > 12 || day < 1 ||
      day > daysInMonth(year, month))
  {
    timeSyncFailed("invalid TimeDate fields");
    return;
  }

  const int64_t epoch =
    unixDaysFromCivil(year, month, day) * 86400LL +
    hour * 3600LL + minute * 60LL + sec;
  struct timeval tv;
  tv.tv_sec = (time_t)epoch;
  tv.tv_usec = (suseconds_t)(bytes[0] % 4U) * 250000;

  if (settimeofday(&tv, nullptr) != 0)
  {
    timeSyncFailed("settimeofday failed");
    return;
  }

  clockValid = true;
  clockMinuteStamp = -1;
  nextTimeSyncAtMs = millis() + TIME_SYNC_PERIOD_MS;
  state = ST_ODOMETER_OK;
  Serial.printf("F90B UTC: %04d-%02u-%02u %02u:%02u:%02u\n",
                year, month, day, hour, minute, sec);
  showSpeedAndOdometer();
}

// Overspeed display: only a large elapsed-seconds counter on the SH1106.
// The digits are centered, and we redraw only when the second changes.
void showOverspeedSeconds()
{
  if (!overspeedActive)
    return;

  const uint32_t elapsedSeconds =
    (uint32_t)(millis() - overspeedStartedAtMs) / 1000UL;

  if (elapsedSeconds == overspeedLastShownSec)
    return;

  overspeedLastShownSec = elapsedSeconds;

  char digits[16];
  if (elapsedSeconds < 100UL)
    snprintf(digits, sizeof(digits), "%02lu", (unsigned long)elapsedSeconds);
  else
    snprintf(digits, sizeof(digits), "%lu", (unsigned long)elapsedSeconds);

  oled.clearBuffer();
  oled.setFont(u8g2_font_logisoso42_tn);
  if (oled.getStrWidth(digits) > 124)
    oled.setFont(u8g2_font_logisoso32_tn);
  if (oled.getStrWidth(digits) > 124)
    oled.setFont(u8g2_font_logisoso24_tn);

  const int16_t width = oled.getStrWidth(digits);
  const int16_t x = (128 - width) / 2;
  const int16_t y = (64 + oled.getAscent() + oled.getDescent()) / 2;
  oled.drawStr(x > 0 ? x : 0, y, digits);
  oled.sendBuffer();
}

// Large speed; odometer and HH:MM below. Diagnostic strings remain
// available on errors/Serial but are hidden in the driving view.
void showSpeedAndOdometer()
{
  // Preserve the diagnostic screen for 15s while BLE parsing continues.
  if (restProbeDisplayUntilMs != 0 &&
      (int32_t)(millis() - restProbeDisplayUntilMs) < 0)
    return;
  if (overspeedActive)
  {
    showOverspeedSeconds();
    return;
  }

  refreshClockText();
  oled.clearBuffer();

  char speedText[16];
  if (speedValid && (uint32_t)(millis() - speedLastOkMs) <= SPEED_STALE_MS)
    snprintf(speedText, sizeof(speedText), "%.1f", speedKmh);
  else
    snprintf(speedText, sizeof(speedText), "--.-");

  // Reserve the upper-right column for a large 7x14 local clock.
  // Use the measured clock width to keep 3-digit speed clear of the clock.
  oled.setFont(u8g2_font_7x14B_tf);
  const int16_t clockWidth = oled.getStrWidth(clockText);
  const int16_t clockX = 128 - clockWidth;
  const int16_t speedMaxWidth = clockX - 3;
  oled.drawStr(clockX, 14, clockText);

  oled.setFont(u8g2_font_logisoso32_tn);
  if (oled.getStrWidth(speedText) > speedMaxWidth)
    oled.setFont(u8g2_font_logisoso24_tn);
  if (oled.getStrWidth(speedText) > speedMaxWidth)
    oled.setFont(u8g2_font_7x14B_tf);
  oled.drawStr(0, 34, speedText);

  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(99, 34, "km/h");
  oled.drawHLine(0, 37, 128);

  // Existing F912 conversion and precision, no units or value changes.
  oled.setCursor(0, 49);
  if (hasOdometer)
    oled.print(odometerKm, 1);
  else
    oled.print("------.-");
  oled.print(" km");

  // START left; J daily driving time in a larger bold 7x14 right-hand area.
  // 7x14 at y=63 starts below odometer's bottom row (y=49).
  oled.setFont(u8g2_font_5x7_tf);
  oled.setCursor(0, 61);
  oled.print("Start:");
  oled.print(restStartValid ? restStartLocalText : "--:--");

  char driveText[16];
  if (dailyDriveValid)
    snprintf(driveText, sizeof(driveText), "J:%02u:%02u",
             (unsigned)(dailyDriveMinutes / 60U),
             (unsigned)(dailyDriveMinutes % 60U));
  else
    snprintf(driveText, sizeof(driveText), "J:--:--");
  oled.setFont(u8g2_font_7x14B_tf);
  const int16_t driveWidth = oled.getStrWidth(driveText);
  oled.drawStr(128 - driveWidth, 63, driveText);

  oled.sendBuffer();
}


// ITS transport closure per JRC/CORTE 4.3: 0xFF on credits characteristic.
// Only when there is an established diagnostic transport. This does not
// issue any more KWP diagnostic data requests.
void closeItsSession()
{
  if (!transportConfigured || client == nullptr ||
      !client->isConnected() || diagCredits == nullptr)
    return;
  const uint8_t closeCommand = 0xFF;
  bool ok = diagCredits->writeValue(&closeCommand, 1, true);
  Serial.printf("ITS close 0xFF sent: %s\\n", ok ? "YES" : "FAILED");
  transportConfigured = false;
}

void showF902Trace()
{
  // Four concise lines fit SH1106 128x64 and can be photographed.
  showMessage("F902 TIMEOUT",
              String("TX:") + traceF902TxOk + " OK:" + traceF902RxOk,
              String("CS:") + traceF902ChecksumBad + " LATE:" + traceF902Late,
              String("BAD:") + traceF902Unexpected +
                " AGE:" + (f902TimeoutLastValidAgeMs / 1000UL) + "s",
              "STOP / BACK");
}

void stopDiagnosticPolling(const char* reason)
{
  if (diagnosticsPaused) return;
  diagnosticsPaused = true;
  speedPollingEnabled = false;
  speedRetryPending = false;
  retryOdometerPending = false;
  periodicOdoInFlight = false;
  manualOdoPending = false;
  overspeedActive = false;
  speedValid = false;
  state = ST_ERROR;
  uint32_t fifoFrames=0, creditEvents=0;
  portENTER_CRITICAL(&rxMux);
  fifoFrames = traceFifoCb;
  portEXIT_CRITICAL(&rxMux);
  portENTER_CRITICAL(&creditMux);
  creditEvents = traceCreditCb;
  portEXIT_CRITICAL(&creditMux);
  Serial.printf("RXPROTECT STOP %s fifo=%lu parsed=%lu credits=%lu F912=%lu\\n",
      reason, (unsigned long)fifoFrames,
      (unsigned long)traceParseCount, (unsigned long)creditEvents,
      (unsigned long)traceF912TxOk);
  // On F902 timeout, stop transmitting IMMEDIATELY but listen passively
  // for 1.5 seconds, to distinguish a late F902 response from a lost one.
  // The session is closed gracefully after the passive interval.
  if (strcmp(reason, "F902 TIMEOUT") == 0)
  {
    f902TimeoutLastValidAgeMs =
      speedLastOkMs ? (uint32_t)(millis() - speedLastOkMs) : 0UL;
    f902GraceUntilMs = millis() + F902_PASSIVE_GRACE_MS;
    Serial.printf("F902 diagnosis TX=%lu OK=%lu CS=%lu LATE=%lu BAD=%lu lastRTT=%lu maxRTT=%lu age=%lu ms FIFO=%lu PAR=%lu\\n",
       (unsigned long)traceF902TxOk, (unsigned long)traceF902RxOk,
       (unsigned long)traceF902ChecksumBad, (unsigned long)traceF902Late,
       (unsigned long)traceF902Unexpected,
       (unsigned long)traceF902LastRttMs, (unsigned long)traceF902MaxRttMs,
       (unsigned long)f902TimeoutLastValidAgeMs, (unsigned long)fifoFrames,
       (unsigned long)traceParseCount);
    showF902Trace();
    return;
  }
  // Any other fault: close ITS and BLE immediately, as in 26.21.
  expectedDisconnect = true;
  closeItsSession();
  if (client != nullptr && client->isConnected())
    client->disconnect();
  showMessage("TrackBox RX SAFE", String(reason),
              String("FIFO:")+fifoFrames+" PAR:"+traceParseCount,
              String("CR:")+creditEvents+" TX:"+traceF912TxOk,
              "STOP / BACK");
}

// Optional F99A failures must never take down working F902/F912 reads.
void dailyDriveUnavailable(const char* reason)
{
  Serial.printf("Optional F99A disabled for session: %s\\n", reason);
  dailyDriveEnabled = false;
  dailyDriveValid = false;
  // Re-establish idle state without sending an automatic retry.
  state = ST_ODOMETER_OK;
  // Wait before regular F902 to let any late F99A data arrive and be ignored.
  nextSpeedRequestAt = millis() + 3000UL;
  if (!overspeedActive) showSpeedAndOdometer();
}

// F997 is a read-only, optional DID. Show its data separately from START.
// The normal driving dashboard is never populated with this timestamp.
void finishRestProbe(const char* header, const String& line1,
                     const String& line2, const String& line3,
                     const String& line4)
{
  state = ST_ODOMETER_OK;
  if (restManualInFlight)
  {
    restProbeDisplayUntilMs = millis() + 15000UL;
    showMessage(header, line1, line2, line3, line4);
  }
  else if (!diagnosticsPaused)
  {
    // Automatic read is silent; only dashboard START is updated.
    showSpeedAndOdometer();
  }
}

void restProbeReply(const uint8_t* bytes)
{
  if (state != ST_WAIT_REST)
  {
    Serial.println("Late F997 ignored");
    return;
  }
  Serial.print("F997 raw:");
  for (unsigned i = 0; i < 8; ++i)
    Serial.printf(" %02X", bytes[i]);
  Serial.println();

  // ISO 16844-7: 8-byte SAE J1939 TimeDate structure, UTC.
  const unsigned sec = bytes[0] / 4U;
  const unsigned minute = bytes[1];
  const unsigned hour = bytes[2];
  const unsigned month = bytes[3];
  const unsigned day = bytes[4] / 4U;
  const int year = 1985 + bytes[5];
  if (year < 2020 || year > 2100 || sec >= 60 || minute >= 60 ||
      hour >= 24 || month < 1 || month > 12 || day < 1 ||
      day > daysInMonth(year, month))
  {
    char h1[32], h2[32];
    snprintf(h1, sizeof(h1), "%02X %02X %02X %02X",
             bytes[0], bytes[1], bytes[2], bytes[3]);
    snprintf(h2, sizeof(h2), "%02X %02X %02X %02X",
             bytes[4], bytes[5], bytes[6], bytes[7]);
    finishRestProbe("F997 RAW / ERROR", h1, h2,
                    "Nieznany czas", "START bez zmian");
    return;
  }
  const time_t utcEpoch = (time_t)(
      unixDaysFromCivil(year, month, day) * 86400LL +
      hour * 3600LL + minute * 60LL + sec);
  struct tm localRest;
  if (localtime_r(&utcEpoch, &localRest) == nullptr)
  {
    finishRestProbe("F997 CONVERT ERR", "Brak konwersji",
                    "UTC->czas Norwegii", "START bez zmian", "");
    return;
  }
  char utcText[32], localText[32];
  snprintf(utcText, sizeof(utcText), "UTC %02u.%02u %02u:%02u",
           day, month, hour, minute);
  snprintf(localText, sizeof(localText), "NO  %02d.%02d %02d:%02d",
           localRest.tm_mday, localRest.tm_mon+1,
           localRest.tm_hour, localRest.tm_min);
  Serial.printf("F997 LAST REST END UTC %04d-%02u-%02u %02u:%02u:%02u; Norway %04d-%02d-%02d %02d:%02d\\n",
      year, month, day, hour, minute, sec,
      localRest.tm_year+1900, localRest.tm_mon+1, localRest.tm_mday,
      localRest.tm_hour, localRest.tm_min);

  // A real F90B UTC clock is mandatory. Never reuse yesterday's last rest.
  // Keep the timestamp within a plausible current shift (up to 18h old).
  const int64_t ageSeconds = (int64_t)time(nullptr) - (int64_t)utcEpoch;
  if (clockValid && ageSeconds >= 0 &&
      ageSeconds <= REST_START_MAX_AGE_SEC)
  {
    snprintf(restStartLocalText, sizeof(restStartLocalText), "%02d:%02d",
             localRest.tm_hour, localRest.tm_min);
    restStartValid = true;
    Serial.printf("F997 START proxy accepted: %s age=%lld seconds\\n",
                  restStartLocalText, (long long)ageSeconds);
  }
  else
  {
    restStartValid = false;
    strcpy(restStartLocalText, "--:--");
    Serial.printf("F997 START proxy NOT fresh: clock=%d age=%lld seconds\\n",
                  (int)clockValid, (long long)ageSeconds);
  }

  finishRestProbe("F997 KONIEC REST", utcText, localText,
                  restStartValid ? "START~F997 OK" : "ZBYT STARE/BEZ CLK",
                  "PUSH=powtorz");
}

void dailyDriveReply(uint8_t hi, uint8_t lo)
{
  if (state != ST_WAIT_DAILY)
  {
    Serial.println("Late F99A ignored");
    return;
  }
  const uint16_t raw = ((uint16_t)hi << 8) | lo; // 1 minute / bit
  // 0xFB00..0xFFFF are special/invalid numerical indications.
  if (raw >= 0xFB00U)
  {
    dailyDriveUnavailable("F99A special value");
    return;
  }
  dailyDriveMinutes = raw;
  dailyDriveValid = true;
  dailyDriveRxCount++;
  state = ST_ODOMETER_OK;
  Serial.printf("F99A daily drive=%u min (%u:%02u) TX=%lu OK=%lu\\n",
                (unsigned)raw, (unsigned)(raw/60U), (unsigned)(raw%60U),
                (unsigned long)dailyDriveTxCount,
                (unsigned long)dailyDriveRxCount);
  showSpeedAndOdometer();
}

void onOdometerReadOk()
{
  // Preserve the F902 polling state and cadence on subsequent F912 reads.
  const bool firstOdo = !hasOdometer;
  hasOdometer = true;
  periodicOdoInFlight = false;
  nextOdoRequestAt = millis() + ODO_POLL_MS;

  if (firstOdo)
  {
    speedPollingEnabled = true;
    speedLastNrc = 0;
    speedTimedOut = false;
    speedPayloadInvalid = false;
    speedStaleDrawn = false;
    nextSpeedRequestAt = millis() + SPEED_POLL_MS;
    nextTimeSyncAtMs = millis() + 5000UL;
  }

  showSpeedAndOdometer();
}

void speedReadFailed(uint8_t nrc, bool timeout, bool invalid)
{
  speedValid = false;
  speedTimedOut = timeout;
  speedPayloadInvalid = invalid;
  speedLastNrc = nrc;
  speedStaleDrawn = false;
  stopDiagnosticPolling(timeout ? "F902 TIMEOUT" :
                        invalid ? "F902 INVALID" : "F902 NRC");
}

void handleSpeedBytes(uint8_t msb, uint8_t lsb)
{
  if (state != ST_WAIT_SPEED)
  {
    traceF902Late++;
    Serial.printf("Late/unexpected F902 positive: state=%d count=%lu\\n",
                  (int)state, (unsigned long)traceF902Late);
    return;
  }

  const uint16_t raw = ((uint16_t)msb << 8) | lsb;
  if (raw > 0xFAFF) // ISO range 0...250.996 km/h
  {
    traceF902Unexpected++;
    Serial.printf("F902 invalid raw=%04X\n", raw);
    speedReadFailed(0, false, true);
    return;
  }

  speedRaw = raw;
  speedKmh = raw / 256.0f;
  speedLastOkMs = millis();
  traceF902RxOk++;
  traceF902LastRttMs = (uint32_t)(speedLastOkMs - requestStartMs);
  if (traceF902LastRttMs > traceF902MaxRttMs)
    traceF902MaxRttMs = traceF902LastRttMs;
  speedValid = true;

  // Only a valid, actual F902 speed measurement switches modes.
  // Exactly 90.0 km/h is NOT an overspeed.
  if (raw > OVERSPEED_LIMIT_RAW)
  {
    if (!overspeedActive)
    {
      overspeedActive = true;
      overspeedStartedAtMs = millis();
      overspeedLastShownSec = 0xFFFFFFFFUL;
      Serial.println("OVERSPEED >90: elapsed seconds display ON");
    }
  }
  else if (overspeedActive)
  {
    overspeedActive = false;
    overspeedLastShownSec = 0xFFFFFFFFUL;
    Serial.println("OVERSPEED <=90: standard screen restored");
  }
  speedRetryPending = false;
  speedTimedOut = false;
  speedStaleDrawn = false;
  speedReplies++;
  state = ST_ODOMETER_OK;
  nextSpeedRequestAt = millis() + SPEED_POLL_MS;
  Serial.printf("F902 speed=%.3f km/h raw=%04X\n", speedKmh, raw);
  showSpeedAndOdometer();
}


// ============================================================
// HEX DUMP
// ============================================================

void printHex(
  const char* prefix,
  const uint8_t* data,
  size_t length
)
{
  Serial.print(prefix);

  for (size_t i = 0; i < length; i++)
  {
    if (data[i] < 0x10)
      Serial.print("0");

    Serial.print(data[i], HEX);

    if (i + 1 < length)
      Serial.print(" ");
  }

  Serial.println();
}


// ============================================================
// CHECKSUM KWP2000
//
// ISO 14230:
// checksum = suma wszystkich poprzednich bajtow mod 256
// ============================================================

uint8_t kwpChecksum(
  const uint8_t* data,
  size_t length
)
{
  uint16_t sum = 0;

  for (size_t i = 0; i < length; i++)
    sum += data[i];

  return (uint8_t)(sum & 0xFF);
}


bool checkKwpChecksum(
  const uint8_t* data,
  size_t length
)
{
  if (length < 2)
    return false;

  uint8_t expected =
    kwpChecksum(data, length - 1);

  return expected == data[length - 1];
}


// ============================================================
// CREDIT -> TACHO
// ============================================================

bool giveCreditsToTacho(uint8_t count)
{
  if (diagCredits == nullptr)
    return false;

  Serial.print("TX Credits -> Tacho: ");
  Serial.println(count);

  return diagCredits->writeValue(
    &count,
    1,
    true
  );
}


// ============================================================
// TX FIFO
// ============================================================

bool sendDiagPacket(
  const uint8_t* data,
  size_t length
)
{
  if (client == nullptr ||
      !client->isConnected())
  {
    Serial.println("TX ERROR: BLE not connected");
    return false;
  }

  if (diagFifo == nullptr)
  {
    Serial.println("TX ERROR: FIFO null");
    return false;
  }

  if (tachoCredits <= 0)
  {
    Serial.println("TX ERROR: no tacho credits");

    showMessage(
      "TrackBox G2",
      "Brak credit",
      "od tachografu"
    );

    return false;
  }

  // Smart Tachograph V2 ITS Transport Protocol, sec. 4.6.4:
  // First BLE packet: [totalPackets=1][packetNumber=1][diagnostic data].
  // Our F912 and StartComm requests each fit one minimum-MTU packet.
  // Min ATT_MTU=23, ATT overhead=3 => 20 value bytes;
  // the 2 packet-info bytes leave up to 18 diagnostic bytes.
  if (data == nullptr || length == 0 || length > 18)
  {
    Serial.println("TX ERROR: payload outside single-packet limit");
    return false;
  }

  uint8_t framed[20] = {0};
  framed[0] = 0x01;
  framed[1] = 0x01;
  memcpy(&framed[2], data, length);
  const size_t framedLength = length + 2;

  printHex("TX APP: ", data, length);
  printHex("TX FIFO: ", framed, framedLength);

  bool ok =
    diagFifo->writeValue(
      framed,
      framedLength,
      true
    );

  if (ok)
  {
    tachoCredits--;

    Serial.print(
      "Remaining tacho credits: "
    );

    Serial.println(tachoCredits);
  }
  else
  {
    Serial.println("FIFO WRITE FAILED");
  }

  return ok;
}


// ============================================================
// RDBI F912
//
// Oficjalna ramka Appendix 8:
//
// 80 EE F0 03 22 F9 12 CS
//
// Dla F0 checksum wychodzi 8E.
// Liczymy go programowo.
// ============================================================

bool sendOdometerRequest()
{
  uint8_t msg[8];

  msg[0] = 0x80;          // physical addressing
  msg[1] = VU_ADDRESS;    // EE
  msg[2] = TESTER_ADDRESS;// F0
  msg[3] = 0x03;          // payload length
  msg[4] = SID_READ_DATA; // 22
  msg[5] = 0xF9;
  msg[6] = 0x12;

  msg[7] =
    kwpChecksum(msg, 7);

  Serial.println();
  Serial.println(
    "=== READ F912 / ODOMETER ==="
  );

  Serial.printf(
    "Checksum: 0x%02X\n",
    msg[7]
  );

  if (!sendDiagPacket(msg, sizeof(msg)))
    return false;

  traceF912TxOk++;
  state = ST_WAIT_ODOMETER;

  requestStartMs = millis();

  // Never flash a temporary screen on automatic background F912 refresh.
  if (!periodicOdoInFlight)
  {
    showMessage(
      "TrackBox G2 ODO",
      "Czytam F912...",
      "czekaj"
    );
  }

  return true;
}


// ============================================================
// F902 TVS: same proven single-packet KWP/ITS transport.
// Frame: 80 EE F0 03 22 F9 02 CS, ITS prefix [01 01].
// ============================================================
// Optional F99A RDBI. Same proven KWP/ITS [01 01] outgoing framing.
bool sendDailyDriveRequest()
{
  if (diagnosticsPaused || !dailyDriveEnabled || !hasOdometer ||
      state != ST_ODOMETER_OK || tachoCredits <= 0 ||
      !speedValid || speedRaw == 0 ||
      (uint32_t)(millis() - speedLastOkMs) > SPEED_STALE_MS)
    return false;

  uint8_t msg[8] = {0x80, VU_ADDRESS, TESTER_ADDRESS,
                    0x03, SID_READ_DATA, 0xF9, 0x9A, 0};
  msg[7] = kwpChecksum(msg, 7);
  // Set next eligibility BEFORE sending: at most 1 per 5 minutes.
  nextDailyDriveAtMs = millis() + DAILY_DRIVE_POLL_MS;
  if (!sendDiagPacket(msg, sizeof(msg)))
  {
    dailyDriveEnabled = false; // never retry optional traffic
    Serial.println("F99A TX failed - disabling optional reads");
    return false;
  }
  state = ST_WAIT_DAILY;
  requestStartMs = millis();
  dailyDriveTxCount++;
  Serial.printf("F99A daily-driving-time request %lu\\n",
                (unsigned long)dailyDriveTxCount);
  return true;
}

// Manual PUSH or a single delayed auto request per BLE session.
// Always only when stopped and between existing F902/F912/F90B requests.
bool sendRestProbeRequest(bool manual)
{
  if (diagnosticsPaused || !hasOdometer || state != ST_ODOMETER_OK ||
      tachoCredits <= 0 || client == nullptr || !client->isConnected() ||
      !speedValid || speedRaw != 0 ||
      (uint32_t)(millis() - speedLastOkMs) > SPEED_STALE_MS)
    return false;
  uint8_t msg[8] = {0x80, VU_ADDRESS, TESTER_ADDRESS,
                    0x03, SID_READ_DATA, 0xF9, 0x97, 0};
  msg[7] = kwpChecksum(msg, 7);
  restManualInFlight = manual;
  // A successful manual test needn't be repeated automatically this session.
  if (!manual || clockValid) restAutoAttempted = true;
  if (!sendDiagPacket(msg, sizeof(msg)))
  {
    Serial.println("F997 one-shot TX failed; optional query, do not stop BLE");
    finishRestProbe("F997 TX ERROR", "Nie wyslano",
                    "START bez zmian", "", "");
    return false;
  }
  state = ST_WAIT_REST;
  requestStartMs = millis();
  if (manual)
  {
    restProbeDisplayUntilMs = millis() + 15000UL;
    showMessage("F997 REST TEST", "Czytam 1 raz...",
                "Tylko na postoju", "START bez zmian", "");
  }
  Serial.printf("F997 one-shot (%s) last-rest-end\\n",
                manual ? "PUSH" : "AUTO");
  return true;
}

bool sendSpeedRequest()
{
  if (!hasOdometer || !speedPollingEnabled ||
      state != ST_ODOMETER_OK || tachoCredits <= 0)
    return false;

  uint8_t msg[8] = {0x80, VU_ADDRESS, TESTER_ADDRESS,
                    0x03, SID_READ_DATA, 0xF9, 0x02, 0};
  msg[7] = kwpChecksum(msg, 7);
  if (!sendDiagPacket(msg, sizeof(msg)))
  {
    Serial.println("F902 TX failed; polling paused");
    speedReadFailed(0, true, false);
    return false;
  }

  state = ST_WAIT_SPEED;
  requestStartMs = millis();
  traceF902TxOk++;
  Serial.printf("F902 request %lu sent\\n",
                (unsigned long)traceF902TxOk);
  return true;
}

// Separate F90B request. Never overlap F902/F912 requests.
bool sendTimeRequest()
{
  if (!hasOdometer || state != ST_ODOMETER_OK || tachoCredits <= 0)
    return false;

  uint8_t msg[8] = {0x80, VU_ADDRESS, TESTER_ADDRESS,
                    0x03, SID_READ_DATA, 0xF9, 0x0B, 0};
  msg[7] = kwpChecksum(msg, 7);
  if (!sendDiagPacket(msg, sizeof(msg)))
  {
    Serial.println("F90B TX failed; retry later");
    nextTimeSyncAtMs = millis() + TIME_RETRY_MS;
    return false;
  }
  state = ST_WAIT_TIME;
  requestStartMs = millis();
  Serial.println("F90B TimeDate request sent");
  return true;
}

// ============================================================
// START COMMUNICATION
//
// Tylko jako fallback, jezeli F912 nie odpowie bezposrednio.
//
// Oficjalny przyklad:
// 81 EE F0 81 E0
// ============================================================

bool sendStartCommunication()
{
  uint8_t msg[5];

  msg[0] = 0x81;
  msg[1] = VU_ADDRESS;
  msg[2] = TESTER_ADDRESS;
  msg[3] = 0x81;

  msg[4] =
    kwpChecksum(msg, 4);

  Serial.println();
  Serial.println(
    "=== START COMMUNICATION fallback ==="
  );

  if (!sendDiagPacket(msg, sizeof(msg)))
    return false;

  traceStartTxOk++;
  state = ST_WAIT_STARTCOMM;

  requestStartMs = millis();

  showMessage(
    "TrackBox G2 DIAG",
    "StartComm...",
    "czekaj"
  );

  return true;
}


// ============================================================
// BLE FIFO CALLBACK
// ============================================================

void fifoCallback(NimBLERemoteCharacteristic* chr,
                  uint8_t* data, size_t length, bool isNotify)
{
  if (!data || !length) return;
  portENTER_CRITICAL(&rxMux);
  traceFifoCb++;
  if (length > RX_PACKET_MAX || rxCount >= RX_CAPACITY)
    rxQueueOverflow = true;
  else
  {
    RxPacket& packet = rxQueue[rxHead];
    memcpy(packet.data, data, length);
    packet.size = (uint8_t)length;
    rxHead = (uint8_t)((rxHead + 1) % RX_CAPACITY);
    rxCount++;
  }
  portEXIT_CRITICAL(&rxMux);
}

// ============================================================
// BLE CREDIT CALLBACK
// ============================================================

void creditCallback(NimBLERemoteCharacteristic* chr,
                    uint8_t* data, size_t length, bool isNotify)
{
  if (!data || !length) return;
  portENTER_CRITICAL(&creditMux);
  traceCreditCb++;
  if (data[0] == 0xFF)
    creditRejected = true;
  else if (creditReceivedPending > (2147483647UL - (uint32_t)data[0]))
    creditCounterOverflow = true;
  else
    creditReceivedPending += data[0];
  portEXIT_CRITICAL(&creditMux);
}

// ============================================================
// CLIENT CALLBACKS
// ============================================================

class TachoClientCallbacks
  : public NimBLEClientCallbacks
{
  void onConnect(
    NimBLEClient* pClient
  ) override
  {
    Serial.println(
      "*** BLE CONNECTED ***"
    );
  }


  void onDisconnect(
    NimBLEClient* pClient,
    int reason
  ) override
  {
    Serial.printf(
      "*** BLE DISCONNECTED reason=%d ***\n",
      reason
    );

    disconnected = true;
  }


  void onConfirmPasskey(
    NimBLEConnInfo& connInfo,
    uint32_t pin
  ) override
  {
    pairingPasskey = pin;

    passkeyShown = true;

    Serial.println();
    Serial.println(
      "=== NUMERIC COMPARISON ==="
    );

    Serial.printf(
      "PIN: %06lu\n",
      (unsigned long)pin
    );

    Serial.println(
      "Potwierdz ten sam kod na tachografie."
    );

    // Poniewaz to my uruchomilismy polaczenie
    // do znalezionego SE5000, lokalnie akceptujemy.
    NimBLEDevice::injectConfirmPasskey(
      connInfo,
      true
    );
  }


  void onPassKeyEntry(
    NimBLEConnInfo& connInfo
  ) override
  {
    passkeyEntryRequested = true;

    Serial.println();
    Serial.println(
      "TACHO WYMAGA WPISANIA PIN DO ESP."
    );
  }


  void onAuthenticationComplete(
    NimBLEConnInfo& connInfo
  ) override
  {
    Serial.println();
    Serial.println(
      "*** AUTH COMPLETE ***"
    );

    Serial.print("Encrypted: ");

    Serial.println(
      connInfo.isEncrypted()
      ? "YES"
      : "NO"
    );
  }
};


TachoClientCallbacks clientCallbacks;


// ============================================================
// SCAN SE5000
// ============================================================

bool findTachograph()
{
  state = ST_SCAN;
  scanNotFound = false;
  lastScanDevices = 0;
  lastScanElapsedMs = 0;
  lastScanRounds = 0;
  tachoName = "";
  tachoAddress = "";

  NimBLEScan* scan = NimBLEDevice::getScan();
  if (!scan)
  {
    Serial.println("BLE scan NULL");
    scanNotFound = true;
    return false;
  }

  scan->stop();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(90);
  scan->setMaxResults(80);
  scan->setDuplicateFilter(0);

  const NimBLEUUID downloadUuid(UUID_DOWNLOAD_SERVICE);
  const NimBLEUUID diagUuid(UUID_DIAG_SERVICE);

  for (uint8_t round = 1; round <= 3; ++round)
  {
    lastScanRounds = round;
    showMessage(
      "TrackBox BLE SCAN",
      String("Proba ") + round + "/3",
      "Szukam SE5000...",
      "Pairing w tacho"
    );

    scan->stop();
    scan->clearResults();
    uint32_t started = millis();
    NimBLEScanResults found = scan->getResults(8000, false);
    uint32_t elapsed = (uint32_t)(millis() - started);
    lastScanElapsedMs += elapsed;
    lastScanDevices += found.getCount();
    Serial.printf("SCAN %u/3: %d devices, %lu ms\n",
                  round, found.getCount(), (unsigned long)elapsed);

    String uuidAddr = "";
    String uuidName = "";
    uint8_t uuidType = 0;
    int uuidRssi = -128;
    for (int k = 0; k < found.getCount(); ++k)
    {
      const NimBLEAdvertisedDevice* d = found.getDevice(k);
      if (!d) continue;
      String name = d->haveName() ? String(d->getName().c_str()) : String("");
      String upper = name;
      upper.toUpperCase();
      bool byName = upper.indexOf("SE5000") >= 0;
      bool byService = d->isAdvertisingService(downloadUuid) ||
                       d->isAdvertisingService(diagUuid);
      Serial.printf("BLE %d name='%s' addr=%s RSSI=%d service=%d\n",
                    k, name.c_str(), d->getAddress().toString().c_str(),
                    d->getRSSI(), byService);

      if (byName)
      {
        tachoName = name;
        tachoAddress = String(d->getAddress().toString().c_str());
        tachoAddressType = d->getAddressType();
        Serial.printf("SE5000 FOUND by NAME: %s\n", tachoAddress.c_str());
        return true;
      }
      if (byService && d->isConnectable() && d->getRSSI() > uuidRssi)
      {
        uuidAddr = String(d->getAddress().toString().c_str());
        uuidName = name.length() ? name : "SE5000 UUID";
        uuidType = d->getAddressType();
        uuidRssi = d->getRSSI();
      }
    }
    if (uuidAddr.length())
    {
      tachoName = uuidName;
      tachoAddress = uuidAddr;
      tachoAddressType = uuidType;
      Serial.printf("SE5000 FOUND by UUID: %s\n", tachoAddress.c_str());
      return true;
    }

    showMessage(
      "TrackBox BLE SCAN",
      String("Proba ") + round + "/3 BRAK",
      String("BLE: ") + found.getCount(),
      String("Czas: ") + elapsed + "ms",
      "Ponawiam..."
    );
    delay(400);
  }
  scanNotFound = true;
  Serial.printf("SE5000 NOT FOUND: scans=%u devices=%lu elapsed=%lu ms\n",
                lastScanRounds, (unsigned long)lastScanDevices,
                (unsigned long)lastScanElapsedMs);
  return false;
}

// ============================================================
// CONNECT
// ============================================================

bool connectTachograph()
{
  state = ST_CONNECT;

  showMessage(
    "TrackBox G2",
    "SE5000 znaleziony",
    "Laczenie...",
    tachoName
  );

  if (client != nullptr)
  {
    if (client->isConnected())
      client->disconnect();

    NimBLEDevice::deleteClient(
      client
    );

    client = nullptr;
  }

  client =
    NimBLEDevice::createClient();

  if (client == nullptr)
    return false;

  client->setClientCallbacks(
    &clientCallbacks,
    false
  );

  client->setConnectionParams(
    12,
    24,
    0,
    200
  );

  client->setConnectTimeout(
    10000
  );

  NimBLEAddress address(
    tachoAddress.c_str(),
    tachoAddressType
  );

  Serial.println();
  Serial.println(
    "Connecting SE5000..."
  );

  if (!client->connect(address))
  {
    Serial.println(
      "CONNECT FAILED"
    );

    return false;
  }

  Serial.println(
    "BLE CONNECT OK"
  );

  Serial.printf(
    "RSSI: %d dBm\n",
    client->getRssi()
  );

  // ========================================================
  // SECURITY / BONDING
  // ========================================================

  state = ST_SECURITY;

  showMessage(
    "TrackBox G2",
    "BLE polaczone",
    "Security..."
  );

  Serial.println(
    "Securing BLE..."
  );

  bool secure =
    client->secureConnection();
  traceSecureOk = secure;

  Serial.print(
    "secureConnection: "
  );

  Serial.println(
    secure
    ? "OK"
    : "FAILED"
  );

  if (!secure)
  {
    Serial.println(
      "Security nie przeszlo."
    );

    return false;
  }

  Serial.print(
    "Bonded: "
  );

  Serial.println(
    NimBLEDevice::isBonded(
      client->getPeerAddress()
    )
    ? "YES"
    : "NO"
  );

  return true;
}


// ============================================================
// SETUP DIAGNOSTIC TRANSPORT
// ============================================================

bool setupDiagnosticTransport()
{
  state = ST_DIAG_SETUP;

  showMessage(
    "TrackBox G2 DIAG",
    "Szukam uslugi...",
    "Diagnostics"
  );

  Serial.println();
  Serial.println(
    "=== DIAGNOSTICS SERVICE ==="
  );

  diagService =    client->getService(
      UUID_DIAG_SERVICE
    );

  if (diagService == nullptr)
  {
    Serial.println(
      "DIAG SERVICE NOT FOUND"
    );

    return false;
  }

  Serial.println(
    "Diagnostics service OK"
  );


  diagFifo =
    diagService->getCharacteristic(
      UUID_DIAG_FIFO
    );

  diagCredits =
    diagService->getCharacteristic(
      UUID_DIAG_CREDITS
    );


  if (diagFifo == nullptr)
  {
    Serial.println(
      "DIAG FIFO NOT FOUND"
    );

    return false;
  }


  if (diagCredits == nullptr)
  {
    Serial.println(
      "DIAG CREDITS NOT FOUND"
    );

    return false;
  }


  Serial.println(
    "FIFO characteristic OK"
  );

  Serial.println(
    "Credits characteristic OK"
  );


  // ========================================================
  // Tachograf -> ESP przez INDICATION
  // false = indications, nie notifications
  // ========================================================

  if (!diagFifo->subscribe(
        false,
        fifoCallback,
        true))
  {
    Serial.println(
      "FIFO subscribe FAILED"
    );

    return false;
  }


  if (!diagCredits->subscribe(
        false,
        creditCallback,
        true))
  {
    Serial.println(
      "Credits subscribe FAILED"
    );

    return false;
  }


  Serial.println(
    "INDICATIONS enabled"
  );


  // ========================================================
  // MY DAJEMY TACHOGRAFOWI 32 CREDIT
  // ========================================================

  delay(100);

  if (!giveCreditsToTacho(32))
  {
    Serial.println(
      "Credit write FAILED"
    );

    return false;
  }


  transportConfigured = true;

  showMessage(
    "TrackBox G2 DIAG",
    "FIFO + Credits OK",
    "Czekam tacho..."
  );


  return true;
}


// ============================================================
// RX PARSER
// ============================================================

void handleKwpFrame(
  const uint8_t* data,
  size_t length
)
{
  traceParseCount++;
  // During passive F902 timeout diagnosis NO responses may affect the
  // application state. Inspect only the F902 positive reply bytes.
  if (diagnosticsPaused)
  {
    if (f902GraceUntilMs != 0)
    {
      if (length >= 5 && data[0] == SID_READ_DATA_POS &&
          data[1] == 0xF9 && data[2] == 0x02)
        traceF902Late++;
      else if (length >= 10 && data[0] == 0x80 &&
               data[4] == SID_READ_DATA_POS &&
               data[5] == 0xF9 && data[6] == 0x02)
      {
        if (checkKwpChecksum(data, length))
          traceF902Late++;
        else
        {
          traceBadChecksum++;
          traceF902ChecksumBad++;
        }
      }
      Serial.printf("Passive RX: bytes=%u F902late=%lu CS=%lu\\n",
                    (unsigned)length, (unsigned long)traceF902Late,
                    (unsigned long)traceF902ChecksumBad);
    }
    return;
  }
  Serial.println();

  printHex(
    "RX FIFO: ",
    data,
    length
  );


  // --------------------------------------------------------
  // OPCJONALNIE RAW UDS:
  // 62 F9 12 xx xx xx xx
  // --------------------------------------------------------

  if (length >= 7 &&
      data[0] == 0x62 &&
      data[1] == 0xF9 &&
      data[2] == 0x12)
  {
    // Ignore unsolicited/late F912. During startup a late reply may
    // still belong to the first F912 while StartComm is in flight.
    if (state != ST_WAIT_ODOMETER &&
        !(state == ST_WAIT_STARTCOMM && !hasOdometer))
    {
      Serial.println("Late RAW F912 ignored");
      return;
    }
    memcpy(
      lastOdoBytes,
      &data[3],
      4
    );

    odometerRaw =
      ((uint32_t)data[3] << 24) |
      ((uint32_t)data[4] << 16) |
      ((uint32_t)data[5] << 8) |
      ((uint32_t)data[6]);

    odometerKm =
      odometerRaw / 200.0;

    state = ST_ODOMETER_OK;

    onOdometerReadOk();

    return;
  }


  // Optional RAW UDS positive: 62 F9 9A [daily minutes MSB LSB].
  if (length >= 5 && data[0] == SID_READ_DATA_POS &&
      data[1] == 0xF9 && data[2] == 0x9A)
  {
    dailyDriveReply(data[3], data[4]);
    return;
  }

  // RAW UDS negative when optional F99A was requested.
  if (state == ST_WAIT_DAILY && length >= 3 &&
      data[0] == SID_NEGATIVE && data[1] == SID_READ_DATA)
  {
    dailyDriveUnavailable("F99A NRC");
    return;
  }

  // RAW UDS positive: 62 F9 97 [8-byte end of last daily rest].
  if (length >= 11 && data[0] == SID_READ_DATA_POS &&
      data[1] == 0xF9 && data[2] == 0x97)
  {
    restProbeReply(data + 3);
    return;
  }
  if (state == ST_WAIT_REST && length >= 3 &&
      data[0] == SID_NEGATIVE && data[1] == SID_READ_DATA)
  {
    Serial.printf("F997 raw NRC %02X\\n", data[2]);
    char msg[24];
    snprintf(msg, sizeof(msg), "NRC 0x%02X", data[2]);
    finishRestProbe("F997 ODRZUCONE", msg,
                    "Tacho nie podalo", "START bez zmian", "");
    return;
  }

  // RAW UDS positive: 62 F9 0B [8-byte TimeDate].
  if (length >= 11 && data[0] == SID_READ_DATA_POS &&
      data[1] == 0xF9 && data[2] == 0x0B)
  {
    handleTimeDateF90B(data + 3);
    return;
  }

  // RAW UDS positive: 62 F9 02 HI LO
  if (length >= 5 && data[0] == SID_READ_DATA_POS &&
      data[1] == 0xF9 && data[2] == 0x02)
  {
    handleSpeedBytes(data[3], data[4]);
    return;
  }
  if (length >= 3 && state == ST_WAIT_TIME &&
      data[0] == SID_NEGATIVE && data[1] == SID_READ_DATA)
  {
    timeSyncFailed("negative response", true);
    return;
  }
  // RAW UDS negative: 7F 22 NRC
  if (length >= 3 && state == ST_WAIT_SPEED &&
      data[0] == SID_NEGATIVE && data[1] == SID_READ_DATA)
  {
    speedReadFailed(data[2], false, false);
    return;
  }
  // --------------------------------------------------------
  // KWP FRAME
  // --------------------------------------------------------

  if (length < 6)
  {
    if (state == ST_WAIT_SPEED) traceF902Unexpected++;
    Serial.println(
      "RX too short"
    );

    return;
  }


  if (!checkKwpChecksum(data, length))
  {
    traceBadChecksum++;
    if (length >= 10 && data[0] == 0x80 &&
        data[4] == SID_READ_DATA_POS &&
        data[5] == 0xF9 && data[6] == 0x02)
      traceF902ChecksumBad++;
    Serial.println(
      "WARNING: checksum incorrect"
    );

    Serial.printf(
      "Received CS=%02X, expected=%02X\n",
      data[length - 1],
      kwpChecksum(
        data,
        length - 1
      )
    );

    // Logujemy dalej, ale nie liczymy przebiegu.
    return;
  }


  Serial.println(
    "Checksum OK"
  );


  // Standard frame:
  // 80 TARGET SOURCE LEN SID ...

  if (data[0] != 0x80)
  {
    if (state == ST_WAIT_SPEED) traceF902Unexpected++;
    Serial.printf(
      "Unknown FMT: %02X\n",
      data[0]
    );

    return;
  }


  uint8_t sid = data[4];

  // Optional positive F99A KWP: 80 tgt src 05 62 F9 9A HI LO CS.
  // Checked KWP checksum and address format above, as for F902.
  if (sid == SID_READ_DATA_POS && length >= 10 &&
      data[5] == 0xF9 && data[6] == 0x9A)
  {
    dailyDriveReply(data[7], data[8]);
    return;
  }

  // Positive F997 KWP: 80 tgt src 0B 62 F9 97 [8 bytes] CS.
  if (sid == SID_READ_DATA_POS && length >= 16 &&
      data[5] == 0xF9 && data[6] == 0x97)
  {
    restProbeReply(data + 7);
    return;
  }

  // Positive F90B KWP: 80 tgt src 0B 62 F9 0B [8 bytes] CS.
  if (sid == SID_READ_DATA_POS && length >= 16 &&
      data[5] == 0xF9 && data[6] == 0x0B)
  {
    handleTimeDateF90B(data + 7);
    return;
  }

  // Positive F902 KWP: 80 tgt src 05 62 F9 02 HI LO CS.
  if (sid == SID_READ_DATA_POS && length >= 10 &&
      data[5] == 0xF9 && data[6] == 0x02)
  {
    handleSpeedBytes(data[7], data[8]);
    return;
  }

  // ========================================================
  // POSITIVE F912
  // ========================================================

  if (sid == SID_READ_DATA_POS &&
      length >= 12 &&
      data[5] == 0xF9 &&
      data[6] == 0x12)
  {
    if (state != ST_WAIT_ODOMETER &&
        !(state == ST_WAIT_STARTCOMM && !hasOdometer))
    {
      Serial.println("Late KWP F912 ignored");
      return;
    }
    memcpy(
      lastOdoBytes,
      &data[7],
      4
    );

    Serial.printf(
      "F912 raw data: %02X %02X %02X %02X\n",
      data[7],
      data[8],
      data[9],
      data[10]
    );


    // Specjalne wskazniki zaczynaja sie FB..FF
    if (data[7] >= 0xFB)
    {
      Serial.printf(
        "F912 special/error indicator: %02X\n",
        data[7]
      );

      stopDiagnosticPolling("F912 INVALID");


      return;
    }


    // Unsigned 32 bit, MSB first
    odometerRaw =
      ((uint32_t)data[7]  << 24) |
      ((uint32_t)data[8]  << 16) |
      ((uint32_t)data[9]  << 8)  |
      ((uint32_t)data[10]);


    // 5 m = 0.005 km = 1/200 km
    odometerKm =
      odometerRaw / 200.0;


    // Dodatkowo liczymy odwrotny endian
    // TYLKO do kontroli w Serial Monitorze.
    uint32_t rawLE =
      ((uint32_t)data[10] << 24) |
      ((uint32_t)data[9]  << 16) |
      ((uint32_t)data[8]  << 8)  |
      ((uint32_t)data[7]);


    double kmLE =
      rawLE / 200.0;


    Serial.println();
    Serial.println(
      "*** F912 SUCCESS ***"
    );

    Serial.printf(
      "RAW BE: %lu\n",
      (unsigned long)odometerRaw
    );

    Serial.printf(
      "ODOMETER BE: %.3f km\n",
      odometerKm
    );

    Serial.printf(
      "ODOMETER LE check: %.3f km\n",
      kmLE
    );


    state = ST_ODOMETER_OK;

    onOdometerReadOk();

    return;
  }


  // ========================================================
  // START COMMUNICATION POSITIVE
  // ========================================================

  if (sid == 0xC1)
  {
    if (state != ST_WAIT_STARTCOMM)
    {
      Serial.println("Unsolicited StartComm ignored");
      return;
    }
    Serial.println(
      "StartCommunication POSITIVE"
    );


    retryOdometerPending = true;

    retryOdometerAt =
      millis() + 150;


    state = ST_DIAG_READY;

    showMessage(
      "TrackBox G2 DIAG",
      "StartComm OK",
      "F912 ponownie..."
    );

    return;
  }


  // ========================================================
  // NEGATIVE RESPONSE
  // ========================================================

  if (sid == SID_NEGATIVE &&
      length >= 8)
  {
    uint8_t rejectedService =
      data[5];

    uint8_t nrc =
      data[6];


    Serial.printf(
      "NEGATIVE RESPONSE service=%02X NRC=%02X\n",
      rejectedService,
      nrc
    );


    // F99A is optional and requires driver consent. A refusal must not
    // interrupt the tachograph speed/odometer session.
    if (state == ST_WAIT_DAILY)
    {
      dailyDriveUnavailable("F99A refused / consent");
      return;
    }

    // The manual F997 experiment never stops the normal data session.
    if (state == ST_WAIT_REST)
    {
      char msg[24];
      snprintf(msg, sizeof(msg), "NRC 0x%02X", nrc);
      finishRestProbe("F997 ODRZUCONE", msg,
                      "Tacho nie podalo", "START bez zmian", "");
      return;
    }

    // F90B is optional; if unsupported, do not disturb F902 or F912.
    if (state == ST_WAIT_TIME)
    {
      timeSyncFailed("tachograph refused F90B", true);
      return;
    }

    // First confirmed error in regular background F912 stops ALL polls.
    if (state == ST_WAIT_ODOMETER && periodicOdoInFlight)
    {
      stopDiagnosticPolling("F912 NRC");
      return;
    }

    // F902 NRC must not invoke F912 StartComm recovery.
    if (state == ST_WAIT_SPEED)
    {
      speedReadFailed(nrc, false, false);
      return;
    }

    // Jezeli pierwsze F912 dostanie
    // ConditionsNotCorrect 0x22,
    // sprobojemy StartCommunication.
    if (rejectedService == 0x22 &&
        nrc == 0x22 &&
        !fallbackStartCommUsed)
    {
      fallbackStartCommUsed = true;

      sendStartCommunication();

      return;
    }


    char reason[24];
    snprintf(reason, sizeof(reason), "NRC %02X/%02X",
             rejectedService, nrc);
    stopDiagnosticPolling(reason);

    return;
  }


  if (state == ST_WAIT_SPEED) traceF902Unexpected++;
  Serial.printf(
    "Other response SID=0x%02X\n",
    sid
  );
}


// ============================================================
// PROCESS RX
// ============================================================

void processRx()
{
  uint8_t local[RX_PACKET_MAX];
  size_t len=0;
  bool pending=false;
  portENTER_CRITICAL(&rxMux);
  if (rxCount)
  {
    const RxPacket& packet = rxQueue[rxTail];
    len = packet.size;
    memcpy(local, packet.data, len);
    rxTail = (uint8_t)((rxTail + 1) % RX_CAPACITY);
    rxCount--;
    pending=true;
  }
  portEXIT_CRITICAL(&rxMux);
  // On a F902 timeout, remain receive-only during the short grace period.
  // No returned credits and no diagnostic request are permitted in this mode.
  if (!pending || (diagnosticsPaused && f902GraceUntilMs == 0)) return;

  // Preserve the known working 23.11 single-packet [01 01] decoder
  // and its raw KWP fallback. Never silently accept a continuation.
  if (len >= 3 && local[0] == 0x01 && local[1] == 0x01)
  {
    Serial.println("RX ITS: single packet, remove [01 01]");
    handleKwpFrame(local + 2, len - 2);
  }
  else if (len >= 2 && local[0] == 0x00 && local[1] >= 0x02)
  {
    stopDiagnosticPolling("ITS MULTIPACKET");
  }
  else
  {
    Serial.println("RX ITS: raw or unsupported packet header");
    handleKwpFrame(local, len);
  }

  // Each parsed packet frees one slot; only now return ONE credit.
  if (!diagnosticsPaused)
  {
    portENTER_CRITICAL(&rxMux);
    if (creditsToReturn < RX_CAPACITY) creditsToReturn++;
    portEXIT_CRITICAL(&rxMux);
  }
}

// ============================================================
// PROCESS CREDIT
// ============================================================

void processCredits()
{
  uint32_t count=0;
  bool rejected=false, overflow=false;
  portENTER_CRITICAL(&creditMux);
  count = creditReceivedPending;
  creditReceivedPending = 0;
  rejected = creditRejected;
  creditRejected = false;
  overflow = creditCounterOverflow;
  creditCounterOverflow = false;
  portEXIT_CRITICAL(&creditMux);
  if (diagnosticsPaused) return;

  // Never cap the additive balance at 255: only the individual credit
  // characteristic value is one byte. Stop on genuine int32 overflow.
  if (rejected || overflow || tachoCredits < 0 ||
      count > (2147483647UL - (uint32_t)tachoCredits))
  {
    stopDiagnosticPolling(rejected ? "CREDIT CLOSED" : "CREDIT OVERFLOW");
    return;
  }
  if (count)
  {
    tachoCredits += (int32_t)count;
    Serial.printf("RX credits +%lu total=%d\\n",
                  (unsigned long)count, tachoCredits);
    if (transportConfigured && tachoCredits > 0 && !firstReadSent)
    {
      state = ST_DIAG_READY;
      diagReadyMs = millis();
      showDiagReady();
    }
  }
}

// ============================================================
// RETURN CREDIT
// ============================================================

void processCreditReturn()
{
  if (diagnosticsPaused || !transportConfigured) return;
  uint8_t count=0;
  portENTER_CRITICAL(&rxMux);
  count = creditsToReturn;
  creditsToReturn = 0;
  portEXIT_CRITICAL(&rxMux);
  if (count && client != nullptr && client->isConnected())
  {
    if (!giveCreditsToTacho(count))
      stopDiagnosticPolling("CREDIT TX FAIL");
  }
}

// ============================================================
// BUTTON
// ============================================================

bool pressed(
  uint8_t pin,
  bool& lastState,
  uint32_t& changeMs
)
{
  bool nowState =
    digitalRead(pin);

  uint32_t now =
    millis();


  if (nowState != lastState &&
      now - changeMs >= DEBOUNCE_MS)
  {
    changeMs = now;

    lastState =
      nowState;


    if (nowState == LOW)
      return true;
  }


  return false;
}


// ============================================================
// COMPLETE START
// ============================================================

bool startEverything()
{
  // Jedna proba polaczenia = jeden zestaw licznikow.
  portENTER_CRITICAL(&rxMux);
  traceFifoCb = 0;
  rxHead = rxTail = rxCount = 0;
  rxQueueOverflow = false;
  creditsToReturn = 0;
  portEXIT_CRITICAL(&rxMux);
  portENTER_CRITICAL(&creditMux);
  traceCreditCb = 0;
  creditReceivedPending = 0;
  creditCounterOverflow = false;
  creditRejected = false;
  portEXIT_CRITICAL(&creditMux);
  disconnected = false;
  expectedDisconnect = false;
  diagnosticsPaused = false;
  traceParseCount = 0;
  traceBadChecksum = 0;
  traceF902TxOk = 0;
  traceF902RxOk = 0;
  traceF902Late = 0;
  traceF902ChecksumBad = 0;
  traceF902Unexpected = 0;
  traceF902LastRttMs = 0;
  traceF902MaxRttMs = 0;
  f902GraceUntilMs = 0;
  f902TimeoutLastValidAgeMs = 0;
  traceF912TxOk = 0;
  traceStartTxOk = 0;
  traceSecureOk = false;
  hasOdometer = false;
  speedPollingEnabled = false;
  speedValid = false;
  speedTimedOut = false;
  speedPayloadInvalid = false;
  manualOdoPending = false;
  speedStaleDrawn = false;
  speedLastNrc = 0;
  speedRaw = 0;
  speedKmh = 0.0f;
  speedLastOkMs = 0;
  nextSpeedRequestAt = 0;
  speedReplies = 0;
  dailyDriveEnabled = true;
  dailyDriveValid = false;
  dailyDriveMinutes = 0;
  nextDailyDriveAtMs = 0;
  dailyDriveTxCount = dailyDriveRxCount = 0;
  restProbeDisplayUntilMs = 0;
  restProbePending = false;
  restAutoAttempted = false;
  restManualInFlight = false;
  restStartValid = false;
  strcpy(restStartLocalText, "--:--");
  speedRetryPending = false;
  speedRetryAt = 0;
  speedRetryAttempts = 0;
  overspeedActive = false;
  overspeedStartedAtMs = 0;
  overspeedLastShownSec = 0xFFFFFFFFUL;
  periodicOdoInFlight = false;
  nextOdoRequestAt = 0;
  nextTimeSyncAtMs = 0;

  tachoCredits = 0;

  transportConfigured = false;
  scanNotFound = false;
  blePairingFailed = false;
  lastScanElapsedMs = 0;
  lastScanDevices = 0;
  lastScanRounds = 0;

  firstReadSent = false;

  fallbackStartCommUsed = false;

  retryOdometerPending = false;


  if (!findTachograph())
  {
    state = ST_ERROR;
    showMessage(
      "SE5000 NIE ZNAL.",
      String("Skan ") + lastScanRounds +
        "/3: " + (lastScanElapsedMs / 1000) + "s",
      String("BLE dev: ") + lastScanDevices,
      "CONF=clear bonds",
      "BACK=retry"
    );
    return false;
  }


  if (!connectTachograph())
  {
    state = ST_ERROR;
    blePairingFailed = true;

    showMessage(
      "TrackBox G2",
      "BLE / Security",
      "FAILED",
      "CONF=clear bonds",
      "BACK=retry"
    );
    return false;
  }


  if (!setupDiagnosticTransport())
  {
    state = ST_ERROR;

    showMessage(
      "TrackBox G2",
      "DIAG transport",
      "FAILED",
      "BACK=retry"
    );

    return false;
  }


  return true;
}


// ============================================================
// RESTART CONNECTION
// ============================================================

void restartConnection()
{
  Serial.println();
  Serial.println(
    "=== RESTART CONNECTION ==="
  );


  if (client != nullptr)
  {
    if (client->isConnected())
    {
      // Gracefully close the ITS diagnostic transport BEFORE BLE drop.
      expectedDisconnect = true;
      closeItsSession();
      client->disconnect();
    }

    delay(200);

    NimBLEDevice::deleteClient(
      client
    );

    client = nullptr;
  }


  diagService = nullptr;
  diagFifo = nullptr;
  diagCredits = nullptr;


  delay(300);

  startEverything();
}


// ============================================================
// SETUP
// ============================================================

void setup()
{
  Serial.begin(115200);

  delay(500);


  // ========================================================
  // GPIO
  // ========================================================

  pinMode(
    PIN_CONFIRM,
    INPUT_PULLUP
  );

  pinMode(
    PIN_BACK,
    INPUT_PULLUP
  );

  pinMode(
    PIN_ENC_PUSH,
    INPUT_PULLUP
  );

  pinMode(
    PIN_ENC_A,
    INPUT_PULLUP
  );

  pinMode(
    PIN_ENC_B,
    INPUT_PULLUP
  );


  // ========================================================
  // OLED
  // ========================================================

  Wire.setPins(
    PIN_SDA,
    PIN_SCL
  );

  Wire.begin();

  Wire.setClock(100000);


  oled.setI2CAddress(
    0x3C << 1
  );

  oled.begin();

  // Norway: standard time UTC+1, daylight-saving time UTC+2.
  // No network is needed for the timezone conversion.
  setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
  tzset();

  // Minimal recovery path only. Holding encoder PUSH during RESET
  // switches back to the previous OTA-capable app partition.
  // Normal startup executes the historical BLE/F912 code unchanged.
  if (digitalRead(PIN_ENC_PUSH) == LOW)
  {
    showMessage(
      "TrackBox RECOVERY",
      "Powrot do OTA...",
      "pusc PUSH"
    );

    delay(500);

    const esp_partition_t* otherApp =
      esp_ota_get_next_update_partition(nullptr);

    if (otherApp != nullptr &&
        esp_ota_set_boot_partition(otherApp) == ESP_OK)
    {
      delay(300);
      ESP.restart();
    }

    showMessage(
      "TrackBox RECOVERY",
      "Blad partycji",
      "RESET = test"
    );

    while (true)
      delay(100);
  }


  // Diagnostic build identity shown before executing the exact F912 sequence.
  // No Wi-Fi or new diagnostic identifiers are initialized in normal mode.
  showMessage(
    "TrackBox G2 F912",
    "LOCALCLOCK 23.11",
    "F90B CLOCK TEST",
    "DAILY 5MIN 27.01"
  );
  delay(1200);

  showMessage(
    "TrackBox G2 ODO",
    "Start..."
  );


  // ========================================================
  // BLE
  // ========================================================

  Serial.println();
  Serial.println(
    "======================================"
  );

  Serial.println(
    " TrackBox Gen2 - SE5000 F912 TEST"
  );

  Serial.println(
    "======================================"
  );


  NimBLEDevice::init(
    LOCAL_NAME
  );


  NimBLEDevice::setMTU(
    247
  );


  NimBLEDevice::setPower(
    3
  );


  // Bonding + Secure Connections
  NimBLEDevice::setSecurityAuth(
    true,
    false,
    true
  );


  // Numeric comparison mozliwe
  NimBLEDevice::setSecurityIOCap(
    BLE_HS_IO_DISPLAY_YESNO
  );


  Serial.print(
    "NimBLE version: "
  );

  Serial.println(
    NimBLEDevice::getVersion()
  );


  Serial.print(
    "Local BLE name: "
  );

  Serial.println(
    LOCAL_NAME
  );


  delay(300);


  startEverything();
}


// ============================================================
// LOOP
// ============================================================

void loop()
{
  // ========================================================
  // BLE EVENTS
  // ========================================================

  if (rxQueueOverflow && !diagnosticsPaused)
    stopDiagnosticPolling("RX QUEUE FULL");
  processCredits();
  processRx();
  processCreditReturn();

  // Run a queued manual F997 test only between other diagnostic requests.
  if (restProbePending && !diagnosticsPaused && state == ST_ODOMETER_OK)
  {
    restProbePending = false;
    sendRestProbeRequest(true); // validates a fresh standstill once more
  }

  // Finish F902 receive-only grace after timeout, with zero additional
  // diagnostic requests. Display final counters including late arrivals.
  if (diagnosticsPaused && f902GraceUntilMs != 0 &&
      (int32_t)(millis() - f902GraceUntilMs) >= 0)
  {
    f902GraceUntilMs = 0;
    expectedDisconnect = true;
    closeItsSession();
    if (client != nullptr && client->isConnected())
      client->disconnect();
    showF902Trace();
    Serial.printf("F902 trace FINAL TX:%lu RX:%lu CRC:%lu LATE:%lu BAD:%lu RTT:%lu/%lu ms\\n",
                  (unsigned long)traceF902TxOk,
                  (unsigned long)traceF902RxOk,
                  (unsigned long)traceF902ChecksumBad,
                  (unsigned long)traceF902Late,
                  (unsigned long)traceF902Unexpected,
                  (unsigned long)traceF902LastRttMs,
                  (unsigned long)traceF902MaxRttMs);
  }

  // Restore normal dashboard after the F997 diagnostic display expires.
  if (restProbeDisplayUntilMs != 0 &&
      (int32_t)(millis() - restProbeDisplayUntilMs) >= 0)
  {
    restProbeDisplayUntilMs = 0;
    if (hasOdometer && !diagnosticsPaused) showSpeedAndOdometer();
  }

  // Update the overspeed clock every second even between F902 responses.
  if (overspeedActive)
    showOverspeedSeconds();


  // ========================================================
  // AUTOMATYCZNY PIERWSZY F912
  // ========================================================

  if (state == ST_DIAG_READY &&
      !diagnosticsPaused && !firstReadSent &&
      tachoCredits > 0 &&
      millis() - diagReadyMs > 700)
  {
    firstReadSent = true;

    if (!sendOdometerRequest()) stopDiagnosticPolling("FIRST F912 TX");
  }


  // ========================================================
  // RETRY F912 PO START COMMUNICATION
  // ========================================================

  if (retryOdometerPending && !diagnosticsPaused &&
      millis() >= retryOdometerAt)
  {
    retryOdometerPending = false;

    if (!sendOdometerRequest()) stopDiagnosticPolling("RETRY F912 TX");
  }


  // Poll F902 only after successful F912. Never overlap queries.
  if (manualOdoPending && !diagnosticsPaused &&
      state == ST_ODOMETER_OK &&
      client != nullptr && client->isConnected() && tachoCredits > 0)
  {
    manualOdoPending = false;
    fallbackStartCommUsed = false;
    firstReadSent = true;
    sendOdometerRequest();
  }
  // Do not automatically retry a failed F902; stop all polling on fault.
  // Clock requests are optional and run between F912 and F902 polling.
  // A failure affects only the clock; never the established data session.
  if (hasOdometer && !diagnosticsPaused &&
      state == ST_ODOMETER_OK &&
      nextTimeSyncAtMs != 0 &&
      !manualOdoPending && tachoCredits > 0 &&
      (int32_t)(millis() - nextTimeSyncAtMs) >= 0)
  {
    if (!sendTimeRequest()) stopDiagnosticPolling("F90B TX FAIL");
  }

  if (state == ST_WAIT_TIME &&
      (uint32_t)(millis() - requestStartMs) > TIME_TIMEOUT_MS)
  {
    stopDiagnosticPolling("F90B TIMEOUT");
  }

  // Update the clock only when local HH:MM changes; speed redraws may
  // still refresh the shared OLED buffer every speed sample.
  if (!overspeedActive && hasOdometer && !diagnosticsPaused &&
      refreshClockText())
    showSpeedAndOdometer();

  // After 20s on a secured BLE diagnostics session: only ONE silent
  // automatic F997 attempt, and only when fresh F902 reports standstill.
  // This does not run during BLE pairing or when driving. Any refusal or
  // timeout is optional and never disconnects the session.
  if (!restAutoAttempted && clockValid && !diagnosticsPaused &&
      (uint32_t)(millis() - diagReadyMs) >= REST_AUTO_SETTLE_MS &&
      hasOdometer && !manualOdoPending && !restProbePending &&
      state == ST_ODOMETER_OK && tachoCredits > 0 &&
      client != nullptr && client->isConnected() &&
      speedValid && speedRaw == 0 &&
      (uint32_t)(millis() - speedLastOkMs) <= SPEED_STALE_MS)
  {
    sendRestProbeRequest(false);
  }

  // Service a due odometer request ahead of the next speed query.
  // One diagnostic request in flight at any time.
  if (hasOdometer && !manualOdoPending && !diagnosticsPaused &&
      state == ST_ODOMETER_OK &&
      client != nullptr && client->isConnected() &&
      tachoCredits > 0 &&
      (int32_t)(millis() - nextOdoRequestAt) >= 0)
  {
    periodicOdoInFlight = true;
    if (!sendOdometerRequest())
    {
      stopDiagnosticPolling("F912 TX FAIL");
    }
  }

  // Read optional F99A only when we have a fresh nonzero speed.
  // Place between existing F912 refresh and F902; no two queries overlap.
  // On standstill (raw speed 0), never request daily-driving time.
  if (hasOdometer && dailyDriveEnabled && !diagnosticsPaused &&
      state == ST_ODOMETER_OK && speedValid && speedRaw > 0 &&
      (uint32_t)(millis() - speedLastOkMs) <= SPEED_STALE_MS &&
      tachoCredits > 0 &&
      (int32_t)(millis() - nextDailyDriveAtMs) >= 0)
  {
    sendDailyDriveRequest();
  }

  if (!diagnosticsPaused && state == ST_WAIT_DAILY &&
      (uint32_t)(millis() - requestStartMs) > DAILY_DRIVE_TIMEOUT_MS)
  {
    dailyDriveUnavailable("F99A timeout"); // no retry or BLE reset
  }

  if (state == ST_ODOMETER_OK && speedPollingEnabled &&
      !diagnosticsPaused &&
      !manualOdoPending && tachoCredits > 0 &&
      (int32_t)(millis() - nextSpeedRequestAt) >= 0)
  {
    sendSpeedRequest();
  }
  if (!diagnosticsPaused && state == ST_WAIT_SPEED &&
      (uint32_t)(millis() - requestStartMs) > SPEED_TIMEOUT_MS)
  {
    Serial.println("F902 TIMEOUT; keep F912 operational");
    speedReadFailed(0, true, false);
  }

  if (!diagnosticsPaused && state == ST_WAIT_REST &&
      (uint32_t)(millis() - requestStartMs) > REST_PROBE_TIMEOUT_MS)
  {
    Serial.println("F997 one-shot TIMEOUT; no retry");
    finishRestProbe("F997 TIMEOUT", "Brak odpowiedzi",
                    "Bez powtarzania", "START bez zmian", "");
  }

  // If incoming F902 updates stop, remove the last speed from the screen.
  // Redraw once, not continuously.
  if (speedValid && !speedStaleDrawn &&
      (uint32_t)(millis() - speedLastOkMs) > SPEED_STALE_MS)
  {
    speedStaleDrawn = true;
    showSpeedAndOdometer();
  }

  // ========================================================
  // TIMEOUT F912
  // ========================================================

  if (state == ST_WAIT_ODOMETER &&
      (uint32_t)(millis() - requestStartMs) >
        (hasOdometer ? ODO_PERIODIC_TIMEOUT_MS : 1500UL))
  {
    if (hasOdometer)
      stopDiagnosticPolling("F912 TIMEOUT");
    else if (!fallbackStartCommUsed)
    {
      // Preserve the tested 23.11 first-read StartCommunication fallback.
      fallbackStartCommUsed = true;
      Serial.println("F912 startup timeout -> StartCommunication");
      if (!sendStartCommunication())
        stopDiagnosticPolling("STARTCOMM TX");
    }
    else
      stopDiagnosticPolling("FIRST F912");
  }

  // ========================================================
  // TIMEOUT START COMM
  // ========================================================

  if (!diagnosticsPaused && state == ST_WAIT_STARTCOMM &&
      (uint32_t)(millis() - requestStartMs) > 1500UL)
  {
    stopDiagnosticPolling("STARTCOMM TIMEOUT");
  }


  // ========================================================
  // ========================================================
  // DISCONNECT
  // ========================================================

  if (disconnected)
  {
    disconnected = false;
    overspeedActive = false;
    overspeedLastShownSec = 0xFFFFFFFFUL;
    if (expectedDisconnect || diagnosticsPaused)
    {
      expectedDisconnect = false;
      // Retain RX SAFE fault diagnostics, do not replace with BLE lost.
    }
    else
    {
      diagnosticsPaused = true; // no further TX on unexpected loss
      state = ST_ERROR;
      showMessage("TrackBox G2", "BLE rozlaczone", "BACK=connect");
    }
  }


  // ========================================================
  // PAIRING INFO
  // ========================================================

  if (passkeyShown)
  {
    passkeyShown = false;

    char buf[16];

    snprintf(
      buf,
      sizeof(buf),
      "%06lu",
      (unsigned long)
        pairingPasskey
    );

    showMessage(
      "BLE PAIRING",
      "Kod:",
      String(buf),
      "Potwierdz w tacho"
    );
  }

  if (passkeyEntryRequested)
  {
    passkeyEntryRequested = false;

    showMessage(
      "BLE PAIRING",
      "Tacho chce PIN",
      "do wpisania",
      "zatrzymaj test"
    );
  }


  // ========================================================
  // CONFIRM
  // ========================================================

  // Encoder PUSH is otherwise unused during normal running.
  // Send exactly one F997 only on a fresh zero-speed tachograph reading.
  if (pressed(PIN_ENC_PUSH, lastEncoderPush, encoderPushChangeMs) &&
      !diagnosticsPaused && hasOdometer && speedValid && speedRaw == 0 &&
      (uint32_t)(millis() - speedLastOkMs) <= SPEED_STALE_MS &&
      client != nullptr && client->isConnected())
    restProbePending = true;

  if (pressed(
        PIN_CONFIRM,
        lastConfirm,
        confirmChangeMs))
  {
    // The tachograph erased pairing; ESP32 may retain old keys.
    // Never erase silently: CONFIRM does it only on scan failure.
    if (state == ST_ERROR && (scanNotFound || blePairingFailed))
    {
      bool ok = NimBLEDevice::deleteAllBonds();
      Serial.printf("ESP bonds erased: %s\n", ok ? "YES" : "NO");
      showMessage(
        "TrackBox BLE RESET",
        ok ? "Bonds cleared" : "Bonds failure",
        "Ponowny skan..."
      );
      delay(600);
      restartConnection();
    }
    else if (!diagnosticsPaused &&
             (state == ST_DIAG_READY ||
         state == ST_ODOMETER_OK ||
         state == ST_WAIT_SPEED ||
         state == ST_ERROR) &&
        client != nullptr &&
        client->isConnected())
    {
      // A pending F902 response must not overlap an F912 query.
      if (state == ST_WAIT_SPEED || tachoCredits <= 0)
        manualOdoPending = true;
      else
      {
        manualOdoPending = false;
        fallbackStartCommUsed = false;
        firstReadSent = true;
        if (!sendOdometerRequest()) stopDiagnosticPolling("MANUAL F912 TX");
      }
    }
  }


  // ========================================================
  // BACK - PELNE PONOWNE POLACZENIE
  // ========================================================

  if (pressed(
        PIN_BACK,
        lastBack,
        backChangeMs))
  {
    restartConnection();
  }


  delay(2);
}