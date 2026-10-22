#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <NimBLEDevice.h>
#include <esp_ota_ops.h>

// ============================================================
// TrackBox Gen2 - FRESH BLE DIAGNOSTIC PROBE
// 2026-10-22
//
// Cel:
//   1. sprawdzic czy SE5000 wysyla Credits do ESP,
//   2. sprawdzic czy SE5000 wysyla jakikolwiek pakiet FIFO,
//   3. NIE uzywac F912,
//   4. odczytac dwa inne, oficjalne recordDataIdentifier:
//        F90B = CurrentDateTime
//        F190 = VehicleIdentificationNumber
//
// Ten plik zostal napisany od nowa jako niezalezny tester.
// Normalny runtime nie zawiera Wi-Fi / WebServer / Preferences.
//
// Recovery:
//   trzymaj PUSH enkodera podczas RESET -> powrot do poprzedniej
//   partycji OTA.
// ============================================================

static const char* TB_FW_VERSION =
  "2026.10.22.8-PROBE";

// ============================================================
// PINOUT - ZAMROZONY
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
  const String& l1 = "",
  const String& l2 = "",
  const String& l3 = "",
  const String& l4 = ""
)
{
  drawHeader(title);

  if (l1.length()) { oled.setCursor(0, 25); oled.print(l1); }
  if (l2.length()) { oled.setCursor(0, 36); oled.print(l2); }
  if (l3.length()) { oled.setCursor(0, 47); oled.print(l3); }
  if (l4.length()) { oled.setCursor(0, 60); oled.print(l4); }

  oled.sendBuffer();
}

// ============================================================
// BLE / SE5000
// ============================================================

static const char* LOCAL_NAME =
  "TrackBox-G2-Rumcajs";

static const char* TACHO_PREFIX =
  "SE5000-";

static const char* UUID_DIAG_SERVICE =
  "fa213def-aef4-475c-bcea-0a8d69073efc";

static const char* UUID_DIAG_FIFO =
  "e413960c-75ba-4ca9-8a67-99bc052a1b13";

static const char* UUID_DIAG_CREDITS =
  "e168d1a6-304f-42b4-ab96-4cd1d4efebd9";

static const uint8_t VU_ADDRESS = 0xEE;
static const uint8_t TESTER_ADDRESS = 0xF0;
static const uint8_t SID_READ_DATA = 0x22;

NimBLEClient* client = nullptr;
NimBLERemoteService* diagService = nullptr;
NimBLERemoteCharacteristic* diagFifo = nullptr;
NimBLERemoteCharacteristic* diagCredits = nullptr;

String tachoName;
String tachoAddress;
uint8_t tachoAddressType = 0;

// ============================================================
// EVENTS / COUNTERS
// ============================================================

portMUX_TYPE eventMux = portMUX_INITIALIZER_UNLOCKED;

volatile bool creditEvent = false;
volatile uint8_t creditValue = 0;
volatile bool creditRejected = false;

volatile bool fifoEvent = false;
volatile size_t fifoLength = 0;
uint8_t fifoBuffer[96];

volatile bool disconnected = false;
volatile int disconnectReason = 0;

uint16_t totalCreditsRx = 0;
uint16_t totalFifoRx = 0;
uint16_t creditsForTx = 0;

bool transportReady = false;
bool autoProbeSent = false;

uint32_t transportReadyMs = 0;
uint32_t requestMs = 0;

enum ProbeKind
{
  PROBE_NONE,
  PROBE_TIME_F90B,
  PROBE_VIN_F190
};

ProbeKind waitingProbe = PROBE_NONE;

// ============================================================
// HELPERS
// ============================================================

uint8_t kwpChecksum(const uint8_t* data, size_t len)
{
  uint8_t sum = 0;
  for (size_t i = 0; i < len; ++i)
    sum = (uint8_t)(sum + data[i]);
  return sum;
}

String hexByte(uint8_t v)
{
  char b[4];
  snprintf(b, sizeof(b), "%02X", v);
  return String(b);
}

String rawLine(const uint8_t* data, size_t len, size_t start, size_t count)
{
  String s = "";
  size_t end = start + count;
  if (end > len) end = len;

  for (size_t i = start; i < end; ++i)
  {
    if (s.length()) s += " ";
    s += hexByte(data[i]);
  }

  return s;
}

bool buttonPressed(uint8_t pin)
{
  static uint32_t lastConfirm = 0;
  static uint32_t lastPush = 0;
  static uint32_t lastBack = 0;

  uint32_t* stamp = nullptr;

  if (pin == PIN_CONFIRM) stamp = &lastConfirm;
  if (pin == PIN_ENC_PUSH) stamp = &lastPush;
  if (pin == PIN_BACK) stamp = &lastBack;

  if (stamp == nullptr)
    return false;

  if (digitalRead(pin) == LOW &&
      millis() - *stamp > 350)
  {
    *stamp = millis();

    while (digitalRead(pin) == LOW)
      delay(5);

    return true;
  }

  return false;
}

// ============================================================
// CALLBACKS
// ============================================================

void fifoCallback(
  NimBLERemoteCharacteristic* chr,
  uint8_t* data,
  size_t length,
  bool isNotify
)
{
  if (data == nullptr || length == 0)
    return;

  if (length > sizeof(fifoBuffer))
    length = sizeof(fifoBuffer);

  portENTER_CRITICAL(&eventMux);

  memcpy(fifoBuffer, data, length);
  fifoLength = length;
  fifoEvent = true;

  portEXIT_CRITICAL(&eventMux);
}

void creditCallback(
  NimBLERemoteCharacteristic* chr,
  uint8_t* data,
  size_t length,
  bool isNotify
)
{
  if (data == nullptr || length < 1)
    return;

  portENTER_CRITICAL(&eventMux);

  if (data[0] == 0xFF)
    creditRejected = true;
  else
  {
    creditValue = data[0];
    creditEvent = true;
  }

  portEXIT_CRITICAL(&eventMux);
}

class ProbeClientCallbacks : public NimBLEClientCallbacks
{
  void onConnect(NimBLEClient* c) override
  {
    Serial.println("BLE CONNECTED");
  }

  void onDisconnect(
    NimBLEClient* c,
    int reason
  ) override
  {
    disconnectReason = reason;
    disconnected = true;
  }

  void onConfirmPasskey(
    NimBLEConnInfo& info,
    uint32_t pin
  ) override
  {
    char p[16];
    snprintf(p, sizeof(p), "%06lu", (unsigned long)pin);

    showMessage(
      "BLE PAIRING",
      "Kod:",
      String(p),
      "Potwierdz w tacho"
    );

    NimBLEDevice::injectConfirmPasskey(info, true);
  }

  void onPassKeyEntry(
    NimBLEConnInfo& info
  ) override
  {
    showMessage(
      "BLE PAIRING",
      "Tacho chce PIN",
      "Brak klawiatury"
    );
  }

  void onAuthenticationComplete(
    NimBLEConnInfo& info
  ) override
  {
    Serial.print("AUTH encrypted=");
    Serial.println(info.isEncrypted() ? "YES" : "NO");
  }
};

ProbeClientCallbacks clientCallbacks;

// ============================================================
// SCAN
// ============================================================

bool findTacho()
{
  showMessage(
    "DIAG PROBE",
    "1/5 SCAN",
    "Szukam SE5000..."
  );

  NimBLEScan* scan = NimBLEDevice::getScan();

  scan->stop();
  scan->clearResults();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(90);
  scan->setMaxResults(40);

  NimBLEScanResults results =
    scan->getResults(8000, false);

  for (int i = 0; i < results.getCount(); ++i)
  {
    const NimBLEAdvertisedDevice* dev =
      results.getDevice(i);

    if (dev == nullptr || !dev->haveName())
      continue;

    String name(dev->getName().c_str());

    if (!name.startsWith(TACHO_PREFIX))
      continue;

    tachoName = name;
    tachoAddress =
      String(dev->getAddress().toString().c_str());
    tachoAddressType =
      dev->getAddressType();

    showMessage(
      "DIAG PROBE",
      "SE5000 FOUND",
      tachoName,
      String("RSSI ") + dev->getRSSI()
    );

    delay(500);
    return true;
  }

  showMessage(
    "DIAG PROBE ERROR",
    "SE5000",
    "NIE ZNALEZIONY",
    "BACK=retry"
  );

  return false;
}

// ============================================================
// CONNECT + SECURITY
// ============================================================

bool connectTacho()
{
  showMessage(
    "DIAG PROBE",
    "2/5 CONNECT",
    tachoName
  );

  client = NimBLEDevice::createClient();

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

  client->setConnectTimeout(10000);

  NimBLEAddress addr(
    tachoAddress.c_str(),
    tachoAddressType
  );

  if (!client->connect(addr))
  {
    showMessage(
      "DIAG PROBE ERROR",
      "CONNECT FAIL",
      "BACK=retry"
    );

    return false;
  }

  showMessage(
    "DIAG PROBE",
    "3/5 SECURITY",
    "BLE link: OK"
  );

  bool secure =
    client->secureConnection();

  if (!secure)
  {
    showMessage(
      "DIAG PROBE ERROR",
      "SECURITY FAIL",
      "BACK=retry"
    );

    return false;
  }

  showMessage(
    "DIAG PROBE",
    "SECURITY OK",
    NimBLEDevice::isBonded(
      client->getPeerAddress()
    ) ? "Bond:Y" : "Bond:N",
    "Szukam DIAG..."
  );

  delay(400);
  return true;
}

// ============================================================
// TRANSPORT SETUP
// ============================================================

bool giveCreditsToTacho(uint8_t count)
{
  if (diagCredits == nullptr)
    return false;

  return diagCredits->writeValue(
    &count,
    1,
    true
  );
}

bool setupTransport()
{
  showMessage(
    "DIAG PROBE",
    "4/5 SERVICE",
    "Diagnostics..."
  );

  diagService =
    client->getService(UUID_DIAG_SERVICE);

  if (diagService == nullptr)
  {
    showMessage(
      "DIAG PROBE ERROR",
      "NO DIAG SERVICE",
      "BACK=retry"
    );

    return false;
  }

  diagFifo =
    diagService->getCharacteristic(
      UUID_DIAG_FIFO
    );

  diagCredits =
    diagService->getCharacteristic(
      UUID_DIAG_CREDITS
    );

  if (diagFifo == nullptr ||
      diagCredits == nullptr)
  {
    showMessage(
      "DIAG PROBE ERROR",
      "CHAR MISSING",
      diagFifo ? "FIFO:Y" : "FIFO:N",
      diagCredits ? "CREDIT:Y" : "CREDIT:N"
    );

    return false;
  }

  bool fifoSub =
    diagFifo->subscribe(
      false,
      fifoCallback,
      true
    );

  bool creditSub =
    diagCredits->subscribe(
      false,
      creditCallback,
      true
    );

  if (!fifoSub || !creditSub)
  {
    showMessage(
      "DIAG PROBE ERROR",
      "SUBSCRIBE FAIL",
      fifoSub ? "FIFO:Y" : "FIFO:N",
      creditSub ? "CREDIT:Y" : "CREDIT:N"
    );

    return false;
  }

  delay(120);

  bool grant =
    giveCreditsToTacho(32);

  if (!grant)
  {
    showMessage(
      "DIAG PROBE ERROR",
      "TX CREDIT FAIL",
      "BACK=retry"
    );

    return false;
  }

  transportReady = true;
  transportReadyMs = millis();

  showMessage(
    "DIAG PROBE",
    "5/5 WAIT RX",
    "TX grant:32",
    "Czekam Credits...",
    "z tachografu"
  );

  return true;
}

// ============================================================
// RDBI PROBES
// ============================================================

bool sendRdbi(uint16_t did, ProbeKind kind)
{
  if (client == nullptr ||
      !client->isConnected() ||
      diagFifo == nullptr)
    return false;

  if (creditsForTx == 0)
    return false;

  uint8_t msg[8];

  msg[0] = 0x80;
  msg[1] = VU_ADDRESS;
  msg[2] = TESTER_ADDRESS;
  msg[3] = 0x03;
  msg[4] = SID_READ_DATA;
  msg[5] = (uint8_t)(did >> 8);
  msg[6] = (uint8_t)(did & 0xFF);
  msg[7] = kwpChecksum(msg, 7);

  Serial.print("TX RDBI: ");
  for (size_t i = 0; i < sizeof(msg); ++i)
  {
    Serial.print(hexByte(msg[i]));
    Serial.print(" ");
  }
  Serial.println();

  bool ok =
    diagFifo->writeValue(
      msg,
      sizeof(msg),
      true
    );

  if (!ok)
    return false;

  creditsForTx--;
  waitingProbe = kind;
  requestMs = millis();

  if (kind == PROBE_TIME_F90B)
  {
    showMessage(
      "DIAG PROBE F90B",
      "TX CurrentDateTime",
      "Czekam FIFO...",
      String("CrdRX:") + totalCreditsRx,
      "PUSH=VIN"
    );
  }
  else
  {
    showMessage(
      "DIAG PROBE F190",
      "TX VIN",
      "Czekam FIFO...",
      String("CrdRX:") + totalCreditsRx
    );
  }

  return true;
}

// ============================================================
// PROCESS RX
// ============================================================

void showProbePacket(
  const uint8_t* data,
  size_t len
)
{
  String lineA = rawLine(data, len, 0, 6);
  String lineB = rawLine(data, len, 6, 6);

  if (len >= 7 &&
      data[0] == 0x80 &&
      data[4] == 0x62)
  {
    uint16_t did =
      ((uint16_t)data[5] << 8) |
      data[6];

    String name = "RDBI RESPONSE";

    if (did == 0xF90B)
      name = "F90B TIME RX";
    else if (did == 0xF190)
      name = "F190 VIN RX";

    showMessage(
      "TACHO -> ESP OK",
      name,
      String("FIFO packets:") + totalFifoRx,
      lineA,
      lineB
    );
  }
  else if (len >= 7 &&
           data[0] == 0x80 &&
           data[4] == 0x7F)
  {
    showMessage(
      "TACHO NEGATIVE RX",
      String("SID:") + hexByte(data[5]),
      String("NRC:") + hexByte(data[6]),
      lineA,
      lineB
    );
  }
  else
  {
    showMessage(
      "TACHO FIFO RX",
      String("Len:") + len,
      String("Packets:") + totalFifoRx,
      lineA,
      lineB
    );
  }
}

void processEvents()
{
  bool haveCredit = false;
  uint8_t credit = 0;
  bool rejected = false;

  bool haveFifo = false;
  size_t len = 0;
  uint8_t local[96];

  portENTER_CRITICAL(&eventMux);

  if (creditEvent)
  {
    haveCredit = true;
    credit = creditValue;
    creditEvent = false;
  }

  if (creditRejected)
  {
    rejected = true;
    creditRejected = false;
  }

  if (fifoEvent)
  {
    haveFifo = true;
    len = fifoLength;
    memcpy(local, fifoBuffer, len);
    fifoEvent = false;
  }

  portEXIT_CRITICAL(&eventMux);

  if (rejected)
  {
    showMessage(
      "DIAG PROBE ERROR",
      "RX CREDIT = FF",
      "Tacho odrzucil"
    );
  }

  if (haveCredit)
  {
    totalCreditsRx += credit;
    creditsForTx += credit;

    showMessage(
      "TACHO -> ESP OK",
      "CREDIT RX",
      String("Now:") + credit,
      String("Total:") + totalCreditsRx,
      "To juz jest RX BLE"
    );

    Serial.print("RX CREDIT=");
    Serial.println(credit);
  }

  if (haveFifo)
  {
    totalFifoRx++;

    // Każdy pakiet FIFO odebrany od tachografu zużywa jeden
    // przyznany przez nas credit. Oddajemy jeden credit.
    giveCreditsToTacho(1);

    waitingProbe = PROBE_NONE;

    Serial.print("RX FIFO len=");
    Serial.println(len);

    showProbePacket(local, len);
  }
}

// ============================================================
// START / RESTART
// ============================================================

void cleanupClient()
{
  transportReady = false;
  autoProbeSent = false;
  waitingProbe = PROBE_NONE;

  totalCreditsRx = 0;
  totalFifoRx = 0;
  creditsForTx = 0;

  diagService = nullptr;
  diagFifo = nullptr;
  diagCredits = nullptr;

  if (client != nullptr)
  {
    if (client->isConnected())
      client->disconnect();

    delay(150);

    NimBLEDevice::deleteClient(client);
    client = nullptr;
  }
}

void startProbe()
{
  cleanupClient();

  if (!findTacho())
    return;

  if (!connectTacho())
    return;

  if (!setupTransport())
    return;
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
  Serial.begin(115200);
  delay(400);

  pinMode(PIN_CONFIRM, INPUT_PULLUP);
  pinMode(PIN_BACK, INPUT_PULLUP);
  pinMode(PIN_ENC_PUSH, INPUT_PULLUP);
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);

  Wire.setPins(PIN_SDA, PIN_SCL);
  Wire.begin();
  Wire.setClock(100000);

  oled.setI2CAddress(0x3C << 1);
  oled.begin();

  // Recovery do poprzedniej partycji OTA.
  if (digitalRead(PIN_ENC_PUSH) == LOW)
  {
    showMessage(
      "TrackBox RECOVERY",
      "Powrot do OTA...",
      "pusc PUSH"
    );

    delay(500);

    const esp_partition_t* other =
      esp_ota_get_next_update_partition(nullptr);

    if (other != nullptr &&
        esp_ota_set_boot_partition(other) == ESP_OK)
    {
      delay(300);
      ESP.restart();
    }

    showMessage(
      "RECOVERY ERROR",
      "Brak partycji"
    );

    while (true)
      delay(100);
  }

  showMessage(
    "FRESH DIAG PROBE",
    "NO F912",
    "Test F90B + F190",
    "FW .8"
  );

  delay(1200);

  NimBLEDevice::init(LOCAL_NAME);
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setPower(3);

  NimBLEDevice::setSecurityAuth(
    true,
    false,
    true
  );

  NimBLEDevice::setSecurityIOCap(
    BLE_HS_IO_DISPLAY_YESNO
  );

  delay(250);

  startProbe();
}

// ============================================================
// LOOP
// ============================================================

void loop()
{
  processEvents();

  if (disconnected)
  {
    disconnected = false;

    showMessage(
      "DIAG PROBE ERROR",
      "BLE DISCONNECT",
      String("Reason:") + disconnectReason,
      "BACK=retry"
    );
  }

  // Po odebraniu Credits od tachografu wysylamy automatycznie
  // pierwszy test: F90B CurrentDateTime.
  if (transportReady &&
      !autoProbeSent &&
      creditsForTx > 0 &&
      millis() - transportReadyMs > 700)
  {
    autoProbeSent = true;
    sendRdbi(0xF90B, PROBE_TIME_F90B);
  }

  if (waitingProbe != PROBE_NONE &&
      millis() - requestMs > 3000)
  {
    ProbeKind timedOut = waitingProbe;
    waitingProbe = PROBE_NONE;

    showMessage(
      "DIAG PROBE TIMEOUT",
      timedOut == PROBE_TIME_F90B
        ? "F90B brak FIFO"
        : "F190 brak FIFO",
      String("CrdRX:") + totalCreditsRx,
      String("FIFO RX:") + totalFifoRx,
      "PUSH=VIN CONF=TIME"
    );
  }

  // CONFIRM -> ponow F90B
  if (buttonPressed(PIN_CONFIRM))
  {
    if (creditsForTx > 0)
      sendRdbi(0xF90B, PROBE_TIME_F90B);
    else
      showMessage(
        "DIAG PROBE",
        "Brak TX credit",
        String("CrdRX:") + totalCreditsRx,
        String("FIFO:") + totalFifoRx
      );
  }

  // PUSH -> test VIN F190
  if (buttonPressed(PIN_ENC_PUSH))
  {
    if (creditsForTx > 0)
      sendRdbi(0xF190, PROBE_VIN_F190);
    else
      showMessage(
        "DIAG PROBE",
        "Brak TX credit",
        String("CrdRX:") + totalCreditsRx,
        String("FIFO:") + totalFifoRx
      );
  }

  // BACK -> pelne ponowne polaczenie
  if (buttonPressed(PIN_BACK))
  {
    startProbe();
  }

  delay(2);
}
