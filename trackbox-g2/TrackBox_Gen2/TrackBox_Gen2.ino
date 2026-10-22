#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <NimBLEDevice.h>
#include <esp_ota_ops.h>

// ============================================================
// TrackBox Gen2 - DIAGNOSTIC HANDSHAKE PROBE
// 2026-10-22
//
// Cel:
//   - NIE testujemy F912.
//   - najpierw sprawdzamy transport BLE FIFO/Credits,
//   - potem wykonujemy prawidlowa kolejnosc Appendix 8:
//       1) StartCommunication  81 EE F0 81 E0
//       2) StartDiagnosticSession Standard (0x81)
//       3) ReadDataByIdentifier F90B (CurrentDateTime)
//
// Dodatkowo tester pokazuje:
//   - czy FIFO wspiera Write / Write Without Response / Indication,
//   - czy zapis do FIFO zwrocil OK,
//   - ile Credits i FIFO rzeczywiscie przyszlo z tachografu.
//
// Recovery:
//   trzymaj PUSH enkodera podczas RESET -> powrot do poprzedniej
//   partycji OTA.
// ============================================================

static const char* TB_FW_VERSION =
  "2026.10.22.9-HANDSHAKE";

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
bool autoHandshakeSent = false;

bool fifoCanWrite = false;
bool fifoCanWriteNR = false;
bool fifoCanIndicate = false;

bool lastTxOk = false;
bool triedNoResponse = false;
bool lastModeNoResponse = false;

uint32_t transportReadyMs = 0;
uint32_t requestMs = 0;

static const uint8_t HS_IDLE = 0;
static const uint8_t HS_WAIT_STARTCOMM = 1;
static const uint8_t HS_WAIT_SESSION = 2;
static const uint8_t HS_WAIT_TIME = 3;

uint8_t hsState = HS_IDLE;

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

String propLine()
{
  return String("W:") + (fifoCanWrite ? "Y" : "N") +
         " NR:" + (fifoCanWriteNR ? "Y" : "N") +
         " I:" + (fifoCanIndicate ? "Y" : "N");
}

String rxLine()
{
  return String("Crd:") + totalCreditsRx +
         " FIFO:" + totalFifoRx;
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
// SCAN + CONNECT
// ============================================================

bool findTacho()
{
  showMessage(
    "HANDSHAKE TEST",
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
      "HANDSHAKE TEST",
      "SE5000 FOUND",
      tachoName,
      String("RSSI ") + dev->getRSSI()
    );

    delay(400);
    return true;
  }

  showMessage(
    "HANDSHAKE ERROR",
    "SE5000",
    "NIE ZNALEZIONY",
    "BACK=retry"
  );

  return false;
}

bool connectTacho()
{
  showMessage(
    "HANDSHAKE TEST",
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
      "HANDSHAKE ERROR",
      "CONNECT FAIL",
      "BACK=retry"
    );

    return false;
  }

  showMessage(
    "HANDSHAKE TEST",
    "3/5 SECURITY",
    "BLE link: OK"
  );

  bool secure =
    client->secureConnection();

  if (!secure)
  {
    showMessage(
      "HANDSHAKE ERROR",
      "SECURITY FAIL",
      "BACK=retry"
    );

    return false;
  }

  showMessage(
    "HANDSHAKE TEST",
    "SECURITY OK",
    NimBLEDevice::isBonded(
      client->getPeerAddress()
    ) ? "Bond:Y" : "Bond:N"
  );

  delay(350);
  return true;
}

// ============================================================
// TRANSPORT
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
    "HANDSHAKE TEST",
    "4/5 SERVICE",
    "Diagnostics..."
  );

  diagService =
    client->getService(UUID_DIAG_SERVICE);

  if (diagService == nullptr)
  {
    showMessage(
      "HANDSHAKE ERROR",
      "NO DIAG SERVICE",
      "BACK=retry"
    );
    return false;
  }

  diagFifo =
    diagService->getCharacteristic(UUID_DIAG_FIFO);

  diagCredits =
    diagService->getCharacteristic(UUID_DIAG_CREDITS);

  if (diagFifo == nullptr ||
      diagCredits == nullptr)
  {
    showMessage(
      "HANDSHAKE ERROR",
      "CHAR MISSING",
      diagFifo ? "FIFO:Y" : "FIFO:N",
      diagCredits ? "CREDIT:Y" : "CREDIT:N"
    );
    return false;
  }

  fifoCanWrite = diagFifo->canWrite();
  fifoCanWriteNR = diagFifo->canWriteNoResponse();
  fifoCanIndicate = diagFifo->canIndicate();

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
      "HANDSHAKE ERROR",
      "SUBSCRIBE FAIL",
      fifoSub ? "FIFO:Y" : "FIFO:N",
      creditSub ? "CREDIT:Y" : "CREDIT:N"
    );
    return false;
  }

  delay(120);

  if (!giveCreditsToTacho(32))
  {
    showMessage(
      "HANDSHAKE ERROR",
      "TX CREDIT FAIL",
      "BACK=retry"
    );
    return false;
  }

  transportReady = true;
  transportReadyMs = millis();

  showMessage(
    "HANDSHAKE READY",
    propLine(),
    "TX grant:32",
    "Czekam Crd RX..."
  );

  return true;
}

// ============================================================
// TX FRAMES
// ============================================================

bool sendFrame(
  const uint8_t* data,
  size_t len,
  bool withResponse,
  uint8_t nextState,
  const char* label
)
{
  if (client == nullptr ||
      !client->isConnected() ||
      diagFifo == nullptr ||
      creditsForTx == 0)
  {
    lastTxOk = false;
    return false;
  }

  Serial.print("TX ");
  Serial.print(label);
  Serial.print(withResponse ? " WR: " : " WNR: ");

  for (size_t i = 0; i < len; ++i)
  {
    Serial.print(hexByte(data[i]));
    Serial.print(" ");
  }
  Serial.println();

  bool ok =
    diagFifo->writeValue(
      data,
      len,
      withResponse
    );

  lastTxOk = ok;
  lastModeNoResponse = !withResponse;

  if (!ok)
    return false;

  creditsForTx--;

  hsState = nextState;
  requestMs = millis();

  showMessage(
    "TX DIAG",
    String(label) + (withResponse ? " WR" : " WNR"),
    String("TX:") + (ok ? "OK" : "FAIL"),
    propLine(),
    rxLine()
  );

  return true;
}

bool sendStartCommunication(bool withResponse)
{
  uint8_t msg[5] =
  {
    0x81,
    VU_ADDRESS,
    TESTER_ADDRESS,
    0x81,
    0x00
  };

  msg[4] = kwpChecksum(msg, 4);

  return sendFrame(
    msg,
    sizeof(msg),
    withResponse,
    HS_WAIT_STARTCOMM,
    "StartComm"
  );
}

bool sendStartDiagnosticSession()
{
  uint8_t msg[7] =
  {
    0x80,
    VU_ADDRESS,
    TESTER_ADDRESS,
    0x02,
    0x10,
    0x81,
    0x00
  };

  msg[6] = kwpChecksum(msg, 6);

  return sendFrame(
    msg,
    sizeof(msg),
    true,
    HS_WAIT_SESSION,
    "StdSession"
  );
}

bool sendTimeF90B()
{
  uint8_t msg[8] =
  {
    0x80,
    VU_ADDRESS,
    TESTER_ADDRESS,
    0x03,
    0x22,
    0xF9,
    0x0B,
    0x00
  };

  msg[7] = kwpChecksum(msg, 7);

  return sendFrame(
    msg,
    sizeof(msg),
    true,
    HS_WAIT_TIME,
    "F90B Time"
  );
}

// ============================================================
// RX PROCESS
// ============================================================

bool isPositiveStartComm(const uint8_t* d, size_t len)
{
  return len >= 8 &&
         d[0] == 0x80 &&
         d[1] == TESTER_ADDRESS &&
         d[2] == VU_ADDRESS &&
         d[4] == 0xC1;
}

bool isPositiveSession(const uint8_t* d, size_t len)
{
  return len >= 7 &&
         d[0] == 0x80 &&
         d[1] == TESTER_ADDRESS &&
         d[2] == VU_ADDRESS &&
         d[4] == 0x50 &&
         d[5] == 0x81;
}

bool isPositiveTime(const uint8_t* d, size_t len)
{
  return len >= 8 &&
         d[0] == 0x80 &&
         d[1] == TESTER_ADDRESS &&
         d[2] == VU_ADDRESS &&
         d[4] == 0x62 &&
         d[5] == 0xF9 &&
         d[6] == 0x0B;
}

void showRawRx(const uint8_t* data, size_t len, const char* title)
{
  showMessage(
    title,
    String("Len:") + len,
    rxLine(),
    rawLine(data, len, 0, 6),
    rawLine(data, len, 6, 6)
  );
}

void handleFifo(const uint8_t* data, size_t len)
{
  totalFifoRx++;

  // Tachograf zuzywa jeden przyznany przez nas credit
  // na pakiet wyslany do ESP. Oddajemy jeden.
  giveCreditsToTacho(1);

  Serial.print("RX FIFO: ");
  for (size_t i = 0; i < len; ++i)
  {
    Serial.print(hexByte(data[i]));
    Serial.print(" ");
  }
  Serial.println();

  if (hsState == HS_WAIT_STARTCOMM &&
      isPositiveStartComm(data, len))
  {
    showRawRx(
      data,
      len,
      "STARTCOMM RX OK"
    );

    hsState = HS_IDLE;

    delay(90);

    if (!sendStartDiagnosticSession())
    {
      showMessage(
        "HANDSHAKE ERROR",
        "Session TX FAIL",
        propLine(),
        rxLine()
      );
    }

    return;
  }

  if (hsState == HS_WAIT_SESSION &&
      isPositiveSession(data, len))
  {
    showRawRx(
      data,
      len,
      "SESSION RX OK"
    );

    hsState = HS_IDLE;

    delay(90);

    if (!sendTimeF90B())
    {
      showMessage(
        "HANDSHAKE ERROR",
        "F90B TX FAIL",
        propLine(),
        rxLine()
      );
    }

    return;
  }

  if (hsState == HS_WAIT_TIME &&
      isPositiveTime(data, len))
  {
    hsState = HS_IDLE;

    showMessage(
      "DIAG PATH OK",
      "F90B TIME RX",
      rxLine(),
      rawLine(data, len, 0, 6),
      rawLine(data, len, 6, 6)
    );

    return;
  }

  hsState = HS_IDLE;

  if (len >= 7 &&
      data[0] == 0x80 &&
      data[4] == 0x7F)
  {
    showMessage(
      "NEGATIVE RX",
      String("SID:") + hexByte(data[5]),
      String("NRC:") + hexByte(data[6]),
      rxLine(),
      rawLine(data, len, 0, 6)
    );
    return;
  }

  showRawRx(
    data,
    len,
    "UNEXPECTED FIFO RX"
  );
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
      "HANDSHAKE ERROR",
      "RX CREDIT=FF",
      "Tacho odrzucil"
    );
  }

  if (haveCredit)
  {
    totalCreditsRx += credit;
    creditsForTx += credit;

    showMessage(
      "CREDIT RX OK",
      String("Now:") + credit,
      String("TX credits:") + creditsForTx,
      propLine(),
      rxLine()
    );

    Serial.print("RX CREDIT=");
    Serial.println(credit);
  }

  if (haveFifo)
    handleFifo(local, len);
}

// ============================================================
// START / RESTART
// ============================================================

void cleanupClient()
{
  transportReady = false;
  autoHandshakeSent = false;
  triedNoResponse = false;
  hsState = HS_IDLE;

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
    "HANDSHAKE PROBE",
    "StartComm FIRST",
    "Session -> F90B",
    "FW .9"
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
      "HANDSHAKE ERROR",
      "BLE DISCONNECT",
      String("Reason:") + disconnectReason,
      "BACK=retry"
    );
  }

  // Appendix 8: najpierw StartCommunication.
  if (transportReady &&
      !autoHandshakeSent &&
      creditsForTx > 0 &&
      millis() - transportReadyMs > 700)
  {
    autoHandshakeSent = true;
    triedNoResponse = false;

    if (!sendStartCommunication(true))
    {
      showMessage(
        "HANDSHAKE ERROR",
        "StartComm TX FAIL",
        propLine(),
        rxLine()
      );
    }
  }

  if (hsState != HS_IDLE &&
      millis() - requestMs > 1800)
  {
    uint8_t timedOut = hsState;

    // Jednorazowo sprawdzamy drugi rodzaj zapisu BLE,
    // ale tylko dla StartCommunication i tylko jesli FIFO
    // faktycznie reklamuje Write Without Response.
    if (timedOut == HS_WAIT_STARTCOMM &&
        !triedNoResponse &&
        fifoCanWriteNR &&
        creditsForTx > 0)
    {
      triedNoResponse = true;

      if (sendStartCommunication(false))
      {
        showMessage(
          "STARTCOMM RETRY",
          "Tryb WNR",
          String("TX:") + (lastTxOk ? "OK" : "FAIL"),
          propLine(),
          rxLine()
        );
        delay(2);
        return;
      }
    }

    hsState = HS_IDLE;

    String what = "UNKNOWN";
    if (timedOut == HS_WAIT_STARTCOMM) what = "StartComm brak FIFO";
    if (timedOut == HS_WAIT_SESSION) what = "Session brak FIFO";
    if (timedOut == HS_WAIT_TIME) what = "F90B brak FIFO";

    showMessage(
      "HANDSHAKE TIMEOUT",
      what,
      String("TX:") + (lastTxOk ? "OK" : "FAIL") +
        (lastModeNoResponse ? " WNR" : " WR"),
      propLine(),
      rxLine()
    );
  }

  // CONFIRM -> StartCommunication od nowa przez Write Request.
  if (buttonPressed(PIN_CONFIRM))
  {
    triedNoResponse = false;

    if (creditsForTx > 0)
      sendStartCommunication(true);
    else
      showMessage(
        "HANDSHAKE",
        "Brak TX credit",
        propLine(),
        rxLine()
      );
  }

  // PUSH -> reczny StartCommunication przez Write Without Response,
  // jesli charakterystyka to obsluguje.
  if (buttonPressed(PIN_ENC_PUSH))
  {
    if (fifoCanWriteNR &&
        creditsForTx > 0)
    {
      triedNoResponse = true;
      sendStartCommunication(false);
    }
    else
    {
      showMessage(
        "HANDSHAKE",
        "WNR niedostepne",
        propLine(),
        rxLine()
      );
    }
  }

  // BACK -> pelne ponowne polaczenie.
  if (buttonPressed(PIN_BACK))
    startProbe();

  delay(2);
}
