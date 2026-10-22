#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <NimBLEDevice.h>
#include <esp_ota_ops.h>

// ============================================================
// TrackBox Gen2 - DUAL PORT PROBE
// 2026-10-22
//
// Niezalezny test dwoch oficjalnych portow ITS:
//   1) DOWNLOAD service (Appendix 7)
//   2) DIAGNOSTICS service (Appendix 8)
//
// Dla kazdego portu:
//   - wlaczamy FIFO indications + Credits indications
//   - dajemy VU 32 credits
//   - czekamy na credits od VU
//   - jesli VU zaakceptowalo polaczenie, wysylamy
//     StartCommunication: 81 EE F0 81 E0
//   - liczymy odpowiedzi FIFO niezaleznie dla obu portow
//
// To rozdziela:
//   BLE / transport serial-port vs.
//   parser DOWNLOAD vs parser DIAGNOSTICS.
//
// Recovery:
//   hold encoder PUSH during RESET -> previous OTA partition.
// ============================================================

static const char* TB_FW_VERSION =
  "2026.10.22.11-DUALPORT";

// Arduino sketch preprocessor may generate function prototypes before
// the full PortState definition. Forward-declare it explicitly.
struct PortState;

#define PIN_CONFIRM   38
#define PIN_SDA       39
#define PIN_SCL       40
#define PIN_ENC_PUSH  41
#define PIN_ENC_A     42
#define PIN_ENC_B      2
#define PIN_BACK       1

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
// BLE UUIDs
// ============================================================

static const char* LOCAL_NAME =
  "TrackBox-G2-Rumcajs";

static const char* TACHO_PREFIX =
  "SE5000-";

// Download serial port - official ITS transport spec
static const char* UUID_DL_SERVICE =
  "eef90782-55dd-4388-b80b-695aba7a69b5";

static const char* UUID_DL_FIFO =
  "29d3a479-1592-47df-80a4-afa742d369bb";

static const char* UUID_DL_CREDITS =
  "db9c4128-bff3-41fe-a306-fb6f9a8aeb2d";

// Diagnostics serial port - official ITS transport spec
static const char* UUID_DG_SERVICE =
  "fa213def-aef4-475c-bcea-0a8d69073efc";

static const char* UUID_DG_FIFO =
  "e413960c-75ba-4ca9-8a67-99bc052a1b13";

static const char* UUID_DG_CREDITS =
  "e168d1a6-304f-42b4-ab96-4cd1d4efebd9";

NimBLEClient* client = nullptr;

String tachoName;
String tachoAddress;
uint8_t tachoAddressType = 0;

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// ============================================================
// Per-port state
// ============================================================

struct PortState
{
  NimBLERemoteService* service = nullptr;
  NimBLERemoteCharacteristic* fifo = nullptr;
  NimBLERemoteCharacteristic* credits = nullptr;

  volatile bool creditEvent = false;
  volatile uint8_t creditValue = 0;
  volatile bool rejected = false;

  volatile bool fifoEvent = false;
  volatile size_t fifoLen = 0;
  uint8_t fifoBuf[96];

  uint16_t creditsRx = 0;
  uint16_t fifoRx = 0;
  uint16_t txCredits = 0;

  bool setupOk = false;
  bool startSent = false;
  bool startTxOk = false;
};

PortState dl;
PortState dg;

volatile bool disconnected = false;
volatile int disconnectReason = 0;

uint32_t testStartMs = 0;
bool finalShown = false;

// ============================================================
// Helpers
// ============================================================

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

String portLine(const char* name, const PortState& p)
{
  return String(name) +
         " C:" + p.creditsRx +
         " F:" + p.fifoRx +
         (p.rejected ? " REJ" : "");
}

bool buttonPressed(uint8_t pin)
{
  static uint32_t lastConfirm = 0;
  static uint32_t lastBack = 0;

  uint32_t* stamp = nullptr;
  if (pin == PIN_CONFIRM) stamp = &lastConfirm;
  if (pin == PIN_BACK) stamp = &lastBack;
  if (stamp == nullptr) return false;

  if (digitalRead(pin) == LOW &&
      millis() - *stamp > 350)
  {
    *stamp = millis();
    while (digitalRead(pin) == LOW) delay(5);
    return true;
  }

  return false;
}

// ============================================================
// Callbacks
// ============================================================

void dlFifoCb(
  NimBLERemoteCharacteristic* chr,
  uint8_t* data,
  size_t length,
  bool isNotify
)
{
  if (!data || length == 0) return;
  if (length > sizeof(dl.fifoBuf)) length = sizeof(dl.fifoBuf);

  portENTER_CRITICAL(&mux);
  memcpy(dl.fifoBuf, data, length);
  dl.fifoLen = length;
  dl.fifoEvent = true;
  portEXIT_CRITICAL(&mux);
}

void dgFifoCb(
  NimBLERemoteCharacteristic* chr,
  uint8_t* data,
  size_t length,
  bool isNotify
)
{
  if (!data || length == 0) return;
  if (length > sizeof(dg.fifoBuf)) length = sizeof(dg.fifoBuf);

  portENTER_CRITICAL(&mux);
  memcpy(dg.fifoBuf, data, length);
  dg.fifoLen = length;
  dg.fifoEvent = true;
  portEXIT_CRITICAL(&mux);
}

void dlCreditCb(
  NimBLERemoteCharacteristic* chr,
  uint8_t* data,
  size_t length,
  bool isNotify
)
{
  if (!data || length < 1) return;

  portENTER_CRITICAL(&mux);

  if (data[0] == 0xFF)
    dl.rejected = true;
  else
  {
    dl.creditValue = data[0];
    dl.creditEvent = true;
  }

  portEXIT_CRITICAL(&mux);
}

void dgCreditCb(
  NimBLERemoteCharacteristic* chr,
  uint8_t* data,
  size_t length,
  bool isNotify
)
{
  if (!data || length < 1) return;

  portENTER_CRITICAL(&mux);

  if (data[0] == 0xFF)
    dg.rejected = true;
  else
  {
    dg.creditValue = data[0];
    dg.creditEvent = true;
  }

  portEXIT_CRITICAL(&mux);
}

class ClientCallbacks : public NimBLEClientCallbacks
{
  void onDisconnect(NimBLEClient* c, int reason) override
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
};

ClientCallbacks callbacks;

// ============================================================
// BLE setup
// ============================================================

bool findTacho()
{
  showMessage(
    "DUALPORT TEST",
    "SCAN SE5000..."
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

    if (!dev || !dev->haveName()) continue;

    String name(dev->getName().c_str());

    if (!name.startsWith(TACHO_PREFIX)) continue;

    tachoName = name;
    tachoAddress =
      String(dev->getAddress().toString().c_str());
    tachoAddressType = dev->getAddressType();

    showMessage(
      "DUALPORT TEST",
      "SE5000 FOUND",
      tachoName
    );

    delay(300);
    return true;
  }

  showMessage(
    "DUALPORT ERROR",
    "SE5000 not found",
    "BACK=retry"
  );

  return false;
}

bool connectTacho()
{
  client = NimBLEDevice::createClient();
  if (!client) return false;

  client->setClientCallbacks(&callbacks, false);
  client->setConnectionParams(12, 24, 0, 200);
  client->setConnectTimeout(10000);

  NimBLEAddress addr(
    tachoAddress.c_str(),
    tachoAddressType
  );

  if (!client->connect(addr))
  {
    showMessage(
      "DUALPORT ERROR",
      "CONNECT FAIL"
    );
    return false;
  }

  showMessage(
    "DUALPORT TEST",
    "BLE connected",
    "Security..."
  );

  if (!client->secureConnection())
  {
    showMessage(
      "DUALPORT ERROR",
      "SECURITY FAIL"
    );
    return false;
  }

  return true;
}

bool grantCredits(PortState& p, uint8_t count)
{
  if (!p.credits) return false;
  return p.credits->writeValue(&count, 1, true);
}

bool setupPort(
  PortState& p,
  const char* serviceUuid,
  const char* fifoUuid,
  const char* creditsUuid,
  NimBLERemoteCharacteristic::notify_callback fifoCb,
  NimBLERemoteCharacteristic::notify_callback creditCb,
  const char* label
)
{
  p.service = client->getService(serviceUuid);

  if (!p.service)
  {
    Serial.print(label);
    Serial.println(" service missing");
    return false;
  }

  p.fifo = p.service->getCharacteristic(fifoUuid);
  p.credits = p.service->getCharacteristic(creditsUuid);

  if (!p.fifo || !p.credits)
  {
    Serial.print(label);
    Serial.println(" chars missing");
    return false;
  }

  bool s1 = p.fifo->subscribe(false, fifoCb, true);
  bool s2 = p.credits->subscribe(false, creditCb, true);

  if (!s1 || !s2)
  {
    Serial.print(label);
    Serial.println(" subscribe failed");
    return false;
  }

  delay(100);

  if (!grantCredits(p, 32))
  {
    Serial.print(label);
    Serial.println(" grant credits failed");
    return false;
  }

  p.setupOk = true;
  return true;
}

bool sendStartComm(PortState& p, const char* label)
{
  if (!p.setupOk ||
      !p.fifo ||
      p.txCredits == 0)
    return false;

  const uint8_t msg[5] =
  {
    0x81, 0xEE, 0xF0, 0x81, 0xE0
  };

  bool ok =
    p.fifo->writeValue(
      msg,
      sizeof(msg),
      true
    );

  p.startSent = true;
  p.startTxOk = ok;

  if (ok) p.txCredits--;

  Serial.print("TX ");
  Serial.print(label);
  Serial.print(" StartComm -> ");
  Serial.println(ok ? "OK" : "FAIL");

  return ok;
}

void resetStates()
{
  dl.service = nullptr;
  dl.fifo = nullptr;
  dl.credits = nullptr;
  dl.creditEvent = false;
  dl.creditValue = 0;
  dl.rejected = false;
  dl.fifoEvent = false;
  dl.fifoLen = 0;
  dl.creditsRx = 0;
  dl.fifoRx = 0;
  dl.txCredits = 0;
  dl.setupOk = false;
  dl.startSent = false;
  dl.startTxOk = false;

  dg.service = nullptr;
  dg.fifo = nullptr;
  dg.credits = nullptr;
  dg.creditEvent = false;
  dg.creditValue = 0;
  dg.rejected = false;
  dg.fifoEvent = false;
  dg.fifoLen = 0;
  dg.creditsRx = 0;
  dg.fifoRx = 0;
  dg.txCredits = 0;
  dg.setupOk = false;
  dg.startSent = false;
  dg.startTxOk = false;

  finalShown = false;
}

void cleanupClient()
{
  resetStates();

  if (client)
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

  if (!findTacho()) return;
  if (!connectTacho()) return;

  showMessage(
    "DUALPORT TEST",
    "Setup DOWNLOAD..."
  );

  bool dlOk =
    setupPort(
      dl,
      UUID_DL_SERVICE,
      UUID_DL_FIFO,
      UUID_DL_CREDITS,
      dlFifoCb,
      dlCreditCb,
      "DL"
    );

  showMessage(
    "DUALPORT TEST",
    String("DOWNLOAD:") + (dlOk ? "OK" : "FAIL"),
    "Setup DIAG..."
  );

  bool dgOk =
    setupPort(
      dg,
      UUID_DG_SERVICE,
      UUID_DG_FIFO,
      UUID_DG_CREDITS,
      dgFifoCb,
      dgCreditCb,
      "DG"
    );

  showMessage(
    "DUALPORT READY",
    String("DL:") + (dlOk ? "OK" : "FAIL"),
    String("DG:") + (dgOk ? "OK" : "FAIL"),
    "Waiting credits..."
  );

  testStartMs = millis();
}

// ============================================================
// Event processing
// ============================================================

void processPortEvents(
  PortState& p,
  const char* label
)
{
  bool haveCredit = false;
  uint8_t credit = 0;
  bool haveFifo = false;
  size_t len = 0;
  uint8_t local[96];

  portENTER_CRITICAL(&mux);

  if (p.creditEvent)
  {
    haveCredit = true;
    credit = p.creditValue;
    p.creditEvent = false;
  }

  if (p.fifoEvent)
  {
    haveFifo = true;
    len = p.fifoLen;
    memcpy(local, p.fifoBuf, len);
    p.fifoEvent = false;
  }

  portEXIT_CRITICAL(&mux);

  if (haveCredit)
  {
    p.creditsRx += credit;
    p.txCredits += credit;

    Serial.print(label);
    Serial.print(" CREDIT RX=");
    Serial.println(credit);

    if (!p.startSent)
    {
      delay(100);
      sendStartComm(p, label);
    }
  }

  if (haveFifo)
  {
    p.fifoRx++;
    grantCredits(p, 1);

    Serial.print(label);
    Serial.print(" FIFO RX: ");

    for (size_t i = 0; i < len; ++i)
    {
      Serial.print(hexByte(local[i]));
      Serial.print(" ");
    }

    Serial.println();

    showMessage(
      "FIFO RESPONSE!",
      String(label) + " port",
      String("Len:") + len,
      rawLine(local, len, 0, 6),
      rawLine(local, len, 6, 6)
    );
  }
}

void showFinal()
{
  finalShown = true;

  showMessage(
    "DUALPORT RESULT",
    portLine("DL", dl),
    portLine("DG", dg),
    String("DLtx:") + (dl.startTxOk ? "OK" : "NO") +
      " DGtx:" + (dg.startTxOk ? "OK" : "NO"),
    "CONF=again"
  );
}

// ============================================================
// Setup / loop
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

  if (digitalRead(PIN_ENC_PUSH) == LOW)
  {
    showMessage(
      "TrackBox RECOVERY",
      "Powrot do OTA..."
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

    while (true) delay(100);
  }

  showMessage(
    "DUALPORT PROBE",
    "DOWNLOAD vs DIAG",
    "StartComm both",
    "FW .11"
  );

  delay(1000);

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

void loop()
{
  processPortEvents(dl, "DL");
  processPortEvents(dg, "DG");

  if (disconnected)
  {
    disconnected = false;

    showMessage(
      "DUALPORT ERROR",
      "BLE DISCONNECT",
      String("Reason:") + disconnectReason,
      "BACK=retry"
    );
  }

  if (!finalShown &&
      millis() - testStartMs > 4500)
  {
    showFinal();
  }

  if (buttonPressed(PIN_CONFIRM))
  {
    startProbe();
  }

  if (buttonPressed(PIN_BACK))
  {
    startProbe();
  }

  delay(2);
}
