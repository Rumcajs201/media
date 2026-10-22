#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <NimBLEDevice.h>
#include <esp_ota_ops.h>

// Parsed by GitHub Actions only; intentionally not used at runtime.
static const char* TB_FW_VERSION =
  "2026.10.22.7-CLEARBOND";

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
  ST_ERROR
};

State state = ST_BOOT;


// ============================================================
// TACHOGRAF
// ============================================================

String tachoName = "";
String tachoAddress = "";

uint8_t tachoAddressType = 0;

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


// ============================================================
// BLE RX FIFO
// ============================================================

portMUX_TYPE rxMux =
  portMUX_INITIALIZER_UNLOCKED;

volatile bool rxPending = false;

uint8_t rxBuffer[128];
size_t rxLength = 0;


// Kredyty, ktore trzeba oddac tachografowi
volatile uint8_t creditsToReturn = 0;


// ============================================================
// CREDIT CALLBACK
// ============================================================

portMUX_TYPE creditMux =
  portMUX_INITIALIZER_UNLOCKED;

volatile bool creditEvent = false;
volatile uint8_t creditReceived = 0;

volatile bool creditRejected = false;


// ============================================================
// BLE FLAGS
// ============================================================

volatile bool disconnected = false;

volatile bool passkeyShown = false;
volatile uint32_t pairingPasskey = 0;

volatile bool passkeyEntryRequested = false;


// ============================================================
// BUTTON DEBOUNCE
// ============================================================

const uint32_t DEBOUNCE_MS = 40;

bool lastConfirm = HIGH;
bool lastBack = HIGH;

uint32_t confirmChangeMs = 0;
uint32_t backChangeMs = 0;


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
  oled.print(odometerKm, 3);

  oled.setFont(u8g2_font_6x10_tf);

  oled.setCursor(0, 58);
  oled.print("km");

  oled.setCursor(30, 58);
  oled.print("Crd:");
  oled.print(tachoCredits);

  oled.sendBuffer();
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

  printHex("TX FIFO: ", data, length);

  bool ok =
    diagFifo->writeValue(
      data,
      length,
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

  state = ST_WAIT_ODOMETER;

  requestStartMs = millis();

  showMessage(
    "TrackBox G2 ODO",
    "Czytam F912...",
    "czekaj"
  );

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

void fifoCallback(
  NimBLERemoteCharacteristic* chr,
  uint8_t* data,
  size_t length,
  bool isNotify
)
{
  if (data == nullptr || length == 0)
    return;

  if (length > sizeof(rxBuffer))
    length = sizeof(rxBuffer);

  portENTER_CRITICAL(&rxMux);

  memcpy(
    rxBuffer,
    data,
    length
  );

  rxLength = length;

  rxPending = true;

  // Tachograf zuzywa jeden przyznany
  // przez nas credit na ten FIFO packet.
  if (creditsToReturn < 250)
    creditsToReturn++;

  portEXIT_CRITICAL(&rxMux);
}


// ============================================================
// BLE CREDIT CALLBACK
// ============================================================

void creditCallback(
  NimBLERemoteCharacteristic* chr,
  uint8_t* data,
  size_t length,
  bool isNotify
)
{
  if (data == nullptr || length < 1)
    return;

  portENTER_CRITICAL(&creditMux);

  if (data[0] == 0xFF)
  {
    creditRejected = true;
  }
  else
  {
    creditReceived = data[0];
    creditEvent = true;
  }

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

  showMessage(
    "TrackBox G2",
    "Szukam SE5000...",
    "BLE scan"
  );

  Serial.println();
  Serial.println(
    "=== SCAN SE5000 ==="
  );

  NimBLEScan* scan =
    NimBLEDevice::getScan();

  scan->stop();

  scan->clearResults();

  scan->setActiveScan(true);

  scan->setInterval(100);

  scan->setWindow(90);

  scan->setMaxResults(40);

  NimBLEScanResults results =
    scan->getResults(
      8000,
      false
    );

  Serial.printf(
    "Found BLE devices: %d\n",
    results.getCount()
  );

  for (int i = 0;
       i < results.getCount();
       i++)
  {
    const NimBLEAdvertisedDevice* dev =
      results.getDevice(i);

    if (dev == nullptr)
      continue;

    String name = "";

    if (dev->haveName())
      name =
        String(
          dev->getName().c_str()
        );

    Serial.printf(
      "[%d] %s  %s  RSSI=%d\n",
      i,
      name.length()
        ? name.c_str()
        : "(no name)",
      dev->getAddress()
        .toString()
        .c_str(),
      dev->getRSSI()
    );

    if (name.startsWith(TACHO_PREFIX))
    {
      tachoName = name;

      tachoAddress =
        String(
          dev->getAddress()
            .toString()
            .c_str()
        );

      tachoAddressType =
        dev->getAddressType();

      Serial.println();
      Serial.println(
        "*** SE5000 FOUND ***"
      );

      Serial.print("Name: ");
      Serial.println(tachoName);

      Serial.print("Address: ");
      Serial.println(tachoAddress);

      return true;
    }
  }

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

    showOdometer();

    return;
  }


  // --------------------------------------------------------
  // KWP FRAME
  // --------------------------------------------------------

  if (length < 6)
  {
    Serial.println(
      "RX too short"
    );

    return;
  }


  if (!checkKwpChecksum(data, length))
  {
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
    Serial.printf(
      "Unknown FMT: %02X\n",
      data[0]
    );

    return;
  }


  uint8_t sid = data[4];


  // ========================================================
  // POSITIVE F912
  // ========================================================

  if (sid == SID_READ_DATA_POS &&
      length >= 12 &&
      data[5] == 0xF9 &&
      data[6] == 0x12)
  {
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

      state = ST_ERROR;

      showMessage(
        "TrackBox G2 ODO",
        "F912 indicator",
        String("0x") +
        String(data[7], HEX)
      );

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

    showOdometer();

    return;
  }


  // ========================================================
  // START COMMUNICATION POSITIVE
  // ========================================================

  if (sid == 0xC1)
  {
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


    state = ST_ERROR;

    char temp[24];

    snprintf(
      temp,
      sizeof(temp),
      "SID %02X NRC %02X",
      rejectedService,
      nrc
    );


    showMessage(
      "TrackBox G2 ERROR",
      "Negative response",
      String(temp),
      "CONFIRM=retry"
    );

    return;
  }


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
  if (!rxPending)
    return;


  uint8_t local[128];

  size_t len = 0;


  portENTER_CRITICAL(&rxMux);

  len = rxLength;

  memcpy(
    local,
    rxBuffer,
    len
  );

  rxPending = false;

  portEXIT_CRITICAL(&rxMux);


  handleKwpFrame(
    local,
    len
  );
}


// ============================================================
// PROCESS CREDIT
// ============================================================

void processCredits()
{
  bool haveEvent = false;

  uint8_t count = 0;


  portENTER_CRITICAL(&creditMux);

  if (creditEvent)
  {
    haveEvent = true;

    count =
      creditReceived;

    creditEvent = false;
  }

  portEXIT_CRITICAL(&creditMux);


  if (creditRejected)
  {
    creditRejected = false;

    Serial.println(
      "TACHO REJECTED CREDIT CONNECTION"
    );

    state = ST_ERROR;

    showMessage(
      "TrackBox G2 ERROR",
      "Credit = FF",
      "Tacho rejected"
    );

    return;
  }


  if (haveEvent)
  {
    tachoCredits += count;


    Serial.print(
      "RX Credits from tacho: "
    );

    Serial.println(count);


    Serial.print(
      "Total tacho credits: "
    );

    Serial.println(tachoCredits);


    if (transportConfigured &&
        tachoCredits > 0 &&
        !firstReadSent)
    {
      state = ST_DIAG_READY;

      diagReadyMs =
        millis();

      showDiagReady();
    }
  }
}


// ============================================================
// RETURN CREDIT
// ============================================================

void processCreditReturn()
{
  uint8_t count = 0;


  portENTER_CRITICAL(&rxMux);

  if (creditsToReturn > 0)
  {
    count =
      creditsToReturn;

    creditsToReturn = 0;
  }

  portEXIT_CRITICAL(&rxMux);


  if (count > 0 &&
      client != nullptr &&
      client->isConnected())
  {
    giveCreditsToTacho(count);
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
  tachoCredits = 0;

  transportConfigured = false;

  firstReadSent = false;

  fallbackStartCommUsed = false;

  retryOdometerPending = false;


  if (!findTachograph())
  {
    state = ST_ERROR;

    showMessage(
      "TrackBox G2",
      "SE5000",
      "NIE ZNALEZIONY",
      "BACK=retry"
    );

    return false;
  }


  if (!connectTachograph())
  {
    state = ST_ERROR;

    showMessage(
      "TrackBox G2",
      "BLE / Security",
      "FAILED",
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
      client->disconnect();

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


  // CONTROLLED TEST: clear the ESP32-side NimBLE bond store.
  // Removing TrackBox only from the tachograph does not remove
  // the keys persisted by NimBLE in ESP32 NVS.
  int bondsBefore =
    NimBLEDevice::getNumBonds();

  bool bondsCleared =
    NimBLEDevice::deleteAllBonds();

  showMessage(
    "TrackBox BOND TEST",
    String("ESP bonds: ") + bondsBefore,
    bondsCleared ? "ESP bond: CLEAR" : "ESP bond: ERR",
    "Usun TB w tacho",
    "CONFIRM=start"
  );

  // Wait for a deliberate press after the user removes the
  // TrackBox entry from the tachograph's paired-device list.
  while (digitalRead(PIN_CONFIRM) == LOW)
    delay(10);

  while (digitalRead(PIN_CONFIRM) == HIGH)
    delay(10);

  delay(60);

  while (digitalRead(PIN_CONFIRM) == LOW)
    delay(10);

  delay(200);


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

  processCredits();

  processRx();

  processCreditReturn();


  // ========================================================
  // AUTOMATYCZNY PIERWSZY F912
  // ========================================================

  if (state == ST_DIAG_READY &&
      !firstReadSent &&
      tachoCredits > 0 &&
      millis() - diagReadyMs > 700)
  {
    firstReadSent = true;

    sendOdometerRequest();
  }


  // ========================================================
  // RETRY F912 PO START COMMUNICATION
  // ========================================================

  if (retryOdometerPending &&
      millis() >= retryOdometerAt)
  {
    retryOdometerPending = false;

    sendOdometerRequest();
  }


  // ========================================================
  // TIMEOUT F912
  // ========================================================

  if (state == ST_WAIT_ODOMETER &&
      millis() - requestStartMs > 1500)
  {
    if (!fallbackStartCommUsed)
    {
      fallbackStartCommUsed = true;

      Serial.println(
        "F912 timeout -> trying StartCommunication"
      );

      sendStartCommunication();
    }
    else
    {
      state = ST_ERROR;

      showMessage(
        "TrackBox G2 ERROR",
        "F912 timeout",
        "brak odpowiedzi",
        "CONFIRM=retry"
      );

      Serial.println(
        "F912: NO RESPONSE"
      );
    }
  }


  // ========================================================
  // TIMEOUT START COMM
  // ========================================================

  if (state == ST_WAIT_STARTCOMM &&
      millis() - requestStartMs > 1500)
  {
    state = ST_ERROR;

    showMessage(
      "TrackBox G2 ERROR",
      "StartComm timeout",
      "brak odpowiedzi",
      "CONFIRM=retry"
    );

    Serial.println(
      "StartCommunication: NO RESPONSE"
    );
  }


  // ========================================================
  // DISCONNECT
  // ========================================================

  if (disconnected)
  {
    disconnected = false;

    state = ST_ERROR;

    showMessage(
      "TrackBox G2",
      "BLE rozlaczone",
      "BACK=connect"
    );
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

  if (pressed(
        PIN_CONFIRM,
        lastConfirm,
        confirmChangeMs))
  {
    if ((state == ST_DIAG_READY ||
         state == ST_ODOMETER_OK ||
         state == ST_ERROR) &&
        client != nullptr &&
        client->isConnected() &&
        tachoCredits > 0)
    {
      fallbackStartCommUsed = false;

      firstReadSent = true;

      sendOdometerRequest();
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