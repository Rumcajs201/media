#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// ============================================================
// TrackBox Gen2 - Stoneridge SE5000 diagnostic tester
// AUTO OTA CLOUD: GitHub build + automatyczna aktualizacja przez Wi-Fi
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
// OTA PRZEZ ROUTER - TRYB SERWISOWY
//
// Uruchomienie:
// podczas wlaczania trzymaj BACK + CONFIRM przez ok. 2 s.
//
// Normalna praca:
// Wi-Fi NIE jest uruchamiane, wiec BLE tachografu pozostaje
// nietkniete.
//
// Tryb OTA:
// - najpierw probuje ostatniej zapisanej sieci Wi-Fi,
// - gdy jej nie ma albo polaczenie sie nie uda, skanuje sieci,
// - enkoder wybiera siec i znaki hasla,
// - klik enkodera dodaje znak,
// - BACK kasuje znak / wraca,
// - CONFIRM lub klik enkodera wybiera siec,
// - po udanym polaczeniu SSID i haslo sa zapisywane w NVS.
// ============================================================

static const char* OTA_HOSTNAME =
  "trackbox-g2";

static const char* TB_FW_VERSION =
  "2026.10.21.1";

static const char* OTA_VERSION_URL =
  "https://raw.githubusercontent.com/Rumcajs201/media/main/trackbox-g2/firmware/version.txt";

static const char* OTA_LATEST_BIN_URL =
  "https://raw.githubusercontent.com/Rumcajs201/media/main/trackbox-g2/firmware/latest.bin";

static const uint8_t OTA_MAX_NETWORKS = 24;
static const uint8_t OTA_MAX_PASSWORD = 64;

struct OtaNetwork
{
  String ssid;
  int32_t rssi;
  uint8_t auth;
};

OtaNetwork otaNetworks[OTA_MAX_NETWORKS];
uint8_t otaNetworkCount = 0;

WebServer otaServer(80);
Preferences otaPrefs;

bool otaUploadOk = false;

String otaSavedSsid = "";
String otaSavedPass = "";

// Kolejnosc celowo ulatwia typowe hasla: male litery, duze,
// cyfry i najczestsze znaki specjalne. Pierwszy znak to spacja.
static const char OTA_CHARSET[] =
  " abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-!@#$%^&*().,?+=:/";

static const size_t OTA_CHARSET_LEN =
  sizeof(OTA_CHARSET) - 1;


// ============================================================
// OTA - STRONA WWW
// ============================================================

static const char OTA_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pl">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>TrackBox G2 OTA</title>
  <style>
    body {
      font-family: -apple-system, BlinkMacSystemFont, Arial, sans-serif;
      max-width: 520px;
      margin: 32px auto;
      padding: 0 18px;
      line-height: 1.45;
    }
    .box {
      border: 1px solid #bbb;
      border-radius: 12px;
      padding: 18px;
    }
    input[type=file] {
      width: 100%;
      margin: 12px 0 18px 0;
    }
    input[type=submit] {
      font-size: 18px;
      padding: 10px 18px;
    }
    code {
      background: #eee;
      padding: 2px 5px;
      border-radius: 4px;
    }
  </style>
</head>
<body>
  <h2>TrackBox Gen2 - OTA</h2>
  <div class="box">
    <p>Firmware: <code>%VERSION%</code></p>
    <form method="POST" action="/update-online">
      <input type="submit" value="Wgraj najnowszy z GitHub">
    </form>
    <hr>
    <p>Awaryjnie mozna tez wybrac lokalny plik <code>.bin</code>.</p>
    <form method="POST" action="/update" enctype="multipart/form-data">
      <input type="file" name="update" accept=".bin,application/octet-stream" required>
      <input type="submit" value="Wgraj plik .bin">
    </form>
    <p>Po poprawnym wgraniu TrackBox uruchomi sie ponownie.</p>
  </div>
</body>
</html>
)rawliteral";


// ============================================================
// OTA - POMOCNICZE PRZYCISKI / ENKODER
// ============================================================

bool otaButtonPressed(
  uint8_t pin,
  bool& lastState,
  uint32_t& lastChange
)
{
  bool nowState = digitalRead(pin);
  uint32_t now = millis();

  if (nowState != lastState &&
      now - lastChange >= 35)
  {
    lastState = nowState;
    lastChange = now;

    if (nowState == LOW)
      return true;
  }

  return false;
}


// Po wejsciu do OTA BACK i CONFIRM byly trzymane przez ok. 2 s.
// Czekamy na ich pelne puszczenie, zeby pierwszy ekran OTA
// nie odziedziczyl stanu przycisku z kombinacji startowej.
void otaWaitButtonsReleased()
{
  while (digitalRead(PIN_BACK) == LOW ||
         digitalRead(PIN_CONFIRM) == LOW ||
         digitalRead(PIN_ENC_PUSH) == LOW)
  {
    delay(5);
  }

  delay(60);
}


int8_t otaReadEncoder()
{
  static bool initialized = false;
  static uint8_t previous = 0;
  static int8_t accumulator = 0;

  uint8_t current =
      (digitalRead(PIN_ENC_A) << 1)
    |  digitalRead(PIN_ENC_B);

  if (!initialized)
  {
    previous = current;
    initialized = true;
    return 0;
  }

  if (current == previous)
    return 0;

  static const int8_t table[16] =
  {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
  };

  uint8_t index =
      (previous << 2)
    | current;

  accumulator += table[index];
  previous = current;

  if (accumulator >= 4)
  {
    accumulator = 0;
    return 1;
  }

  if (accumulator <= -4)
  {
    accumulator = 0;
    return -1;
  }

  return 0;
}


String otaShorten(
  const String& text,
  uint8_t maxLen
)
{
  if (text.length() <= maxLen)
    return text;

  if (maxLen < 4)
    return text.substring(0, maxLen);

  return text.substring(0, maxLen - 3) + "...";
}


String otaPasswordTail(
  const String& password,
  uint8_t maxLen
)
{
  if (password.length() <= maxLen)
    return password;

  return "..." +
    password.substring(
      password.length() - (maxLen - 3)
    );
}


// ============================================================
// OTA - PREFERENCES / ZAPIS Wi-Fi
// ============================================================

void otaLoadSavedWifi()
{
  otaPrefs.begin(
    "tb_ota",
    true
  );

  otaSavedSsid =
    otaPrefs.getString(
      "ssid",
      ""
    );

  otaSavedPass =
    otaPrefs.getString(
      "pass",
      ""
    );

  otaPrefs.end();
}


void otaSaveWifi(
  const String& ssid,
  const String& password
)
{
  otaPrefs.begin(
    "tb_ota",
    false
  );

  otaPrefs.putString(
    "ssid",
    ssid
  );

  otaPrefs.putString(
    "pass",
    password
  );

  otaPrefs.end();

  otaSavedSsid = ssid;
  otaSavedPass = password;
}


// ============================================================
// OTA - SKANOWANIE SIECI
// ============================================================

void otaSortNetworks()
{
  for (uint8_t i = 0;
       i < otaNetworkCount;
       i++)
  {
    for (uint8_t j = i + 1;
         j < otaNetworkCount;
         j++)
    {
      if (otaNetworks[j].rssi >
          otaNetworks[i].rssi)
      {
        OtaNetwork tmp =
          otaNetworks[i];

        otaNetworks[i] =
          otaNetworks[j];

        otaNetworks[j] = tmp;
      }
    }
  }
}


int otaFindNetworkBySsid(
  const String& ssid
)
{
  for (uint8_t i = 0;
       i < otaNetworkCount;
       i++)
  {
    if (otaNetworks[i].ssid == ssid)
      return i;
  }

  return -1;
}


bool otaScanNetworks()
{
  showMessage(
    "TrackBox G2 OTA",
    "Skan WiFi...",
    "2.4 GHz"
  );

  Serial.println();
  Serial.println(
    "=== OTA WIFI SCAN ==="
  );

  WiFi.scanDelete();

  int16_t found =
    WiFi.scanNetworks(
      false,
      true
    );

  if (found <= 0)
  {
    Serial.println(
      "No WiFi networks found"
    );

    otaNetworkCount = 0;
    return false;
  }

  otaNetworkCount = 0;

  for (int16_t i = 0;
       i < found;
       i++)
  {
    String ssid = WiFi.SSID(i);

    if (ssid.length() == 0)
      continue;

    int32_t rssi =
      WiFi.RSSI(i);

    uint8_t auth =
      (uint8_t)WiFi.encryptionType(i);

    int existing =
      otaFindNetworkBySsid(ssid);

    if (existing >= 0)
    {
      if (rssi >
          otaNetworks[existing].rssi)
      {
        otaNetworks[existing].rssi = rssi;
        otaNetworks[existing].auth = auth;
      }

      continue;
    }

    if (otaNetworkCount >=
        OTA_MAX_NETWORKS)
    {
      continue;
    }

    otaNetworks[otaNetworkCount].ssid =
      ssid;

    otaNetworks[otaNetworkCount].rssi =
      rssi;

    otaNetworks[otaNetworkCount].auth =
      auth;

    otaNetworkCount++;
  }

  WiFi.scanDelete();

  otaSortNetworks();

  Serial.printf(
    "Unique networks: %u\n",
    otaNetworkCount
  );

  for (uint8_t i = 0;
       i < otaNetworkCount;
       i++)
  {
    Serial.printf(
      "[%u] %s RSSI=%ld auth=%u\n",
      i,
      otaNetworks[i].ssid.c_str(),
      (long)otaNetworks[i].rssi,
      otaNetworks[i].auth
    );
  }

  return otaNetworkCount > 0;
}


int otaSelectNetwork()
{
  if (otaNetworkCount == 0)
    return -1;

  int selected = 0;

  if (otaSavedSsid.length())
  {
    int savedIndex =
      otaFindNetworkBySsid(
        otaSavedSsid
      );

    if (savedIndex >= 0)
      selected = savedIndex;
  }

  bool lastConfirmLocal = HIGH;
  bool lastBackLocal = HIGH;
  bool lastPushLocal = HIGH;

  uint32_t confirmChanged = 0;
  uint32_t backChanged = 0;
  uint32_t pushChanged = 0;

  while (true)
  {
    drawHeader(
      "OTA WiFi - wybor"
    );

    oled.setCursor(0, 25);
    oled.print(
      otaShorten(
        otaNetworks[selected].ssid,
        20
      )
    );

    oled.setCursor(0, 37);
    oled.print("RSSI ");
    oled.print(
      otaNetworks[selected].rssi
    );
    oled.print(" dBm ");

    if (otaNetworks[selected].auth ==
        WIFI_AUTH_OPEN)
      oled.print("OPEN");
    else
      oled.print("LOCK");

    oled.setCursor(0, 49);
    oled.print(selected + 1);
    oled.print("/");
    oled.print(otaNetworkCount);
    oled.print(" ENC=wybor");

    oled.setCursor(0, 61);
    oled.print("CON/PUSH=wyb BAK");

    oled.sendBuffer();

    int8_t step =
      otaReadEncoder();

    if (step > 0)
    {
      selected++;

      if (selected >=
          otaNetworkCount)
      {
        selected = 0;
      }
    }
    else if (step < 0)
    {
      selected--;

      if (selected < 0)
      {
        selected =
          otaNetworkCount - 1;
      }
    }

    if (otaButtonPressed(
          PIN_CONFIRM,
          lastConfirmLocal,
          confirmChanged) ||
        otaButtonPressed(
          PIN_ENC_PUSH,
          lastPushLocal,
          pushChanged))
    {
      return selected;
    }

    if (otaButtonPressed(
          PIN_BACK,
          lastBackLocal,
          backChanged))
    {
      return -2;
    }

    delay(2);
  }
}


// ============================================================
// OTA - EDYTOR HASLA
// ============================================================

bool otaEditPassword(
  const String& ssid,
  String& password
)
{
  int charIndex = 1; // 'a'

  bool lastConfirmLocal = HIGH;
  bool lastBackLocal = HIGH;
  bool lastPushLocal = HIGH;

  uint32_t confirmChanged = 0;
  uint32_t backChanged = 0;
  uint32_t pushChanged = 0;

  while (true)
  {
    drawHeader(
      "OTA WiFi - haslo"
    );

    oled.setCursor(0, 24);
    oled.print(
      otaShorten(
        ssid,
        20
      )
    );

    oled.setCursor(0, 36);
    oled.print("PASS:");
    oled.print(
      otaPasswordTail(
        password,
        15
      )
    );

    oled.setCursor(0, 48);
    oled.print("Znak [");

    char currentChar =
      OTA_CHARSET[charIndex];

    if (currentChar == ' ')
      oled.print("SP");
    else
      oled.print(currentChar);

    oled.print("] len:");
    oled.print(password.length());

    oled.setCursor(0, 61);
    oled.print("PUSH:+ BAK:- CON=go");

    oled.sendBuffer();

    int8_t step =
      otaReadEncoder();

    if (step > 0)
    {
      charIndex++;

      if (charIndex >=
          (int)OTA_CHARSET_LEN)
      {
        charIndex = 0;
      }
    }
    else if (step < 0)
    {
      charIndex--;

      if (charIndex < 0)
      {
        charIndex =
          OTA_CHARSET_LEN - 1;
      }
    }

    if (otaButtonPressed(
          PIN_ENC_PUSH,
          lastPushLocal,
          pushChanged))
    {
      if (password.length() <
          OTA_MAX_PASSWORD)
      {
        password +=
          OTA_CHARSET[charIndex];
      }
    }

    if (otaButtonPressed(
          PIN_BACK,
          lastBackLocal,
          backChanged))
    {
      if (password.length() > 0)
      {
        password.remove(
          password.length() - 1
        );
      }
      else
      {
        return false;
      }
    }

    if (otaButtonPressed(
          PIN_CONFIRM,
          lastConfirmLocal,
          confirmChanged))
    {
      return true;
    }

    delay(2);
  }
}


// ============================================================
// OTA - POLACZENIE Z ROUTEREM
// ============================================================

bool otaConnectWifi(
  const String& ssid,
  const String& password,
  uint32_t timeoutMs = 12000
)
{
  Serial.println();
  Serial.println(
    "=== OTA WIFI CONNECT ==="
  );

  Serial.print("SSID: ");
  Serial.println(ssid);

  // Celowo NIE wypisujemy hasla do Serial Monitora.

  WiFi.disconnect(
    false,
    false
  );

  delay(100);

  if (password.length() == 0)
  {
    WiFi.begin(
      ssid.c_str()
    );
  }
  else
  {
    WiFi.begin(
      ssid.c_str(),
      password.c_str()
    );
  }

  uint32_t started =
    millis();

  bool lastBackLocal = HIGH;
  uint32_t backChanged = 0;

  uint8_t dots = 0;

  while (millis() - started <
         timeoutMs)
  {
    if (WiFi.status() ==
        WL_CONNECTED)
    {
      Serial.println(
        "WiFi CONNECTED"
      );

      Serial.print("IP: ");
      Serial.println(
        WiFi.localIP()
      );

      return true;
    }

    showMessage(
      "TrackBox G2 OTA",
      otaShorten(
        ssid,
        20
      ),
      "Laczenie" +
        String(dots == 0 ? "." :
               dots == 1 ? ".." : "..."),
      "BACK=anuluj"
    );

    dots++;
    if (dots > 2)
      dots = 0;

    uint32_t waitStart = millis();

    while (millis() - waitStart < 300)
    {
      if (WiFi.status() ==
          WL_CONNECTED)
      {
        return true;
      }

      if (otaButtonPressed(
            PIN_BACK,
            lastBackLocal,
            backChanged))
      {
        WiFi.disconnect(
          false,
          false
        );

        return false;
      }

      delay(2);
    }
  }

  Serial.print(
    "WiFi connection failed, status="
  );
  Serial.println(
    (int)WiFi.status()
  );

  WiFi.disconnect(
    false,
    false
  );

  return false;
}


// ============================================================
// OTA - AKTUALIZACJA Z CHMURY / GITHUB
// ============================================================

bool otaFetchText(
  const String& url,
  String& out
)
{
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(15000);

  if (!http.begin(client, url))
  {
    Serial.println("HTTP begin failed");
    return false;
  }

  int code = http.GET();

  if (code != HTTP_CODE_OK)
  {
    Serial.printf("HTTP GET failed: %d\n", code);
    http.end();
    return false;
  }

  out = http.getString();
  http.end();
  out.trim();

  return out.length() > 0;
}


bool otaInstallFromGithub(
  const String& version
)
{
  String url =
    String(OTA_LATEST_BIN_URL) +
    "?v=" + version;

  showMessage(
    "TrackBox G2 OTA",
    "Nowy firmware",
    String("FW ") + version,
    "Pobieram...",
    "Nie wylaczaj!"
  );

  Serial.println();
  Serial.println("=== OTA GITHUB DOWNLOAD ===");
  Serial.print("Version: ");
  Serial.println(version);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(30000);

  if (!http.begin(client, url))
  {
    Serial.println("HTTP begin failed");
    showMessage(
      "TrackBox G2 OTA",
      "Blad polaczenia",
      "GitHub niedostepny"
    );
    return false;
  }

  int code = http.GET();

  if (code != HTTP_CODE_OK)
  {
    Serial.printf("Firmware GET failed: %d\n", code);
    http.end();

    showMessage(
      "TrackBox G2 OTA",
      "Blad pobierania",
      String("HTTP ") + code
    );

    return false;
  }

  int contentLength = http.getSize();

  if (!Update.begin(
        contentLength > 0 ?
          (size_t)contentLength :
          UPDATE_SIZE_UNKNOWN))
  {
    Serial.println("Update.begin failed");
    Update.printError(Serial);
    http.end();
    return false;
  }

  WiFiClient* stream =
    http.getStreamPtr();

  size_t written =
    Update.writeStream(
      *stream
    );

  Serial.printf(
    "GitHub OTA written: %u bytes\n",
    (unsigned)written
  );

  bool ok =
    Update.end(true);

  http.end();

  if (!ok)
  {
    Serial.println("GitHub OTA Update.end failed");
    Update.printError(Serial);

    showMessage(
      "TrackBox G2 OTA",
      "BLAD UPDATE",
      "Firmware zostaje",
      "Sprawdz Serial"
    );

    return false;
  }

  showMessage(
    "TrackBox G2 OTA",
    "UPDATE OK",
    String("FW ") + version,
    "Restart..."
  );

  delay(1200);
  ESP.restart();

  return true;
}


bool otaAutoUpdateIfAvailable(
  bool forceUpdate = false
)
{
  String latestVersion;

  String versionUrl =
    String(OTA_VERSION_URL) +
    "?cb=" + String(millis());

  showMessage(
    "TrackBox G2 OTA",
    "Sprawdzam update",
    String("Mam: ") +
      TB_FW_VERSION,
    "GitHub..."
  );

  if (!otaFetchText(
        versionUrl,
        latestVersion))
  {
    Serial.println(
      "Could not read cloud version"
    );

    return false;
  }

  Serial.print("Current FW: ");
  Serial.println(TB_FW_VERSION);

  Serial.print("Cloud FW: ");
  Serial.println(latestVersion);

  if (!forceUpdate &&
      latestVersion ==
        String(TB_FW_VERSION))
  {
    showMessage(
      "TrackBox G2 OTA",
      "Firmware aktualny",
      String("FW ") +
        TB_FW_VERSION,
      "Start serwera OTA"
    );

    delay(700);
    return false;
  }

  return otaInstallFromGithub(
    latestVersion
  );
}


// ============================================================
// OTA - OBSLUGA WGRYWANIA
// ============================================================

void handleOtaUpload()
{
  HTTPUpload& upload =
    otaServer.upload();

  if (upload.status ==
      UPLOAD_FILE_START)
  {
    otaUploadOk = false;

    Serial.println();
    Serial.println(
      "=== OTA UPLOAD START ==="
    );

    Serial.print("File: ");
    Serial.println(
      upload.filename
    );

    showMessage(
      "TrackBox G2 OTA",
      "Wgrywanie...",
      upload.filename.substring(
        0,
        20
      ),
      "Nie wylaczaj!"
    );

    if (!Update.begin(
          UPDATE_SIZE_UNKNOWN))
    {
      Serial.println(
        "Update.begin FAILED"
      );

      Update.printError(
        Serial
      );
    }
  }

  else if (upload.status ==
           UPLOAD_FILE_WRITE)
  {
    if (Update.write(
          upload.buf,
          upload.currentSize)
        != upload.currentSize)
    {
      Serial.println(
        "Update.write FAILED"
      );

      Update.printError(
        Serial
      );
    }
  }

  else if (upload.status ==
           UPLOAD_FILE_END)
  {
    if (Update.end(true))
    {
      otaUploadOk = true;

      Serial.printf(
        "OTA OK: %u bytes\n",
        (unsigned)upload.totalSize
      );

      showMessage(
        "TrackBox G2 OTA",
        "WGRANO OK",
        String(upload.totalSize) +
          " bytes",
        "Restart..."
      );
    }
    else
    {
      otaUploadOk = false;

      Serial.println(
        "Update.end FAILED"
      );

      Update.printError(
        Serial
      );

      showMessage(
        "TrackBox G2 OTA",
        "BLAD OTA",
        "Sprawdz Serial"
      );
    }
  }

  else if (upload.status ==
           UPLOAD_FILE_ABORTED)
  {
    Update.abort();

    otaUploadOk = false;

    Serial.println(
      "OTA ABORTED"
    );

    showMessage(
      "TrackBox G2 OTA",
      "PRZERWANO",
      "Sprobuj ponownie"
    );
  }
}


// ============================================================
// OTA - SERWER WWW PO POLACZENIU Z ROUTEREM
// ============================================================

void otaStartWebServer(
  const String& ssid
)
{
  IPAddress ip =
    WiFi.localIP();

  Serial.println();
  Serial.println(
    "=== OTA WEB SERVER ==="
  );

  Serial.print("WiFi: ");
  Serial.println(ssid);

  Serial.print("OTA URL: http://");
  Serial.println(ip);

  bool mdnsOk =
    MDNS.begin(
      OTA_HOSTNAME
    );

  if (mdnsOk)
  {
    MDNS.addService(
      "http",
      "tcp",
      80
    );

    Serial.println(
      "mDNS: http://trackbox-g2.local"
    );
  }
  else
  {
    Serial.println(
      "mDNS start failed - use IP"
    );
  }

  showMessage(
    "TrackBox G2 OTA",
    "OTA READY",
    otaShorten(
      ssid,
      20
    ),
    String("IP: ") +
      ip.toString(),
    "Safari -> IP"
  );

  otaServer.on(
    "/",
    HTTP_GET,
    []()
    {
      String page = OTA_PAGE;
      page.replace(
        "%VERSION%",
        TB_FW_VERSION
      );

      otaServer.send(
        200,
        "text/html; charset=utf-8",
        page
      );
    }
  );

  otaServer.on(
    "/update-online",
    HTTP_POST,
    []()
    {
      otaServer.send(
        200,
        "text/html; charset=utf-8",
        "<!doctype html><html><body>"
        "<h2>TrackBox pobiera najnowszy firmware.</h2>"
        "<p>Nie wylaczaj zasilania.</p>"
        "</body></html>"
      );

      delay(250);

      if (!otaAutoUpdateIfAvailable(true))
      {
        showMessage(
          "TrackBox G2 OTA",
          "Update online",
          "nieudany",
          "Mozesz uzyc BIN"
        );
      }
    }
  );

    otaServer.on(
    "/update",
    HTTP_POST,

    []()
    {
      otaServer.sendHeader(
        "Connection",
        "close"
      );

      if (otaUploadOk &&
          !Update.hasError())
      {
        otaServer.send(
          200,
          "text/html; charset=utf-8",
          "<!doctype html><html><body>"
          "<h2>Wgrano poprawnie.</h2>"
          "<p>TrackBox uruchomi sie ponownie.</p>"
          "</body></html>"
        );

        delay(1200);

        ESP.restart();
      }
      else
      {
        otaServer.send(
          500,
          "text/html; charset=utf-8",
          "<!doctype html><html><body>"
          "<h2>Blad OTA.</h2>"
          "<p>Firmware nie zostal uruchomiony. "
          "Sprobuj ponownie.</p>"
          "</body></html>"
        );
      }
    },

    handleOtaUpload
  );

  otaServer.onNotFound(
    []()
    {
      otaServer.sendHeader(
        "Location",
        "/",
        true
      );

      otaServer.send(
        302,
        "text/plain",
        ""
      );
    }
  );

  otaServer.begin();

  Serial.println(
    "OTA web server READY"
  );

  while (true)
  {
    otaServer.handleClient();

    // Jesli router zniknie, informujemy na OLED,
    // ale serwer pozostaje aktywny, gdy polaczenie wroci.
    if (WiFi.status() !=
        WL_CONNECTED)
    {
      showMessage(
        "TrackBox G2 OTA",
        "WiFi rozlaczone",
        "Restartuj OTA",
        "BACK+OK przy starcie"
      );
    }

    delay(2);
  }
}


// ============================================================
// OTA - TRYB SERWISOWY PRZEZ ROUTER
//
// Funkcja celowo nie wraca.
// W trybie OTA BLE tachografu nie jest inicjalizowane.
// ============================================================

void startOtaMode()
{
  Serial.println();
  Serial.println(
    "======================================"
  );

  Serial.println(
    " TrackBox Gen2 - OTA VIA ROUTER"
  );

  Serial.println(
    "======================================"
  );

  // Uzytkownik wszedl do OTA trzymajac BACK + CONFIRM.
  // Najpierw wymagamy puszczenia przyciskow.
  otaWaitButtonsReleased();

  WiFi.mode(
    WIFI_STA
  );

  WiFi.setSleep(
    false
  );

  WiFi.setAutoReconnect(
    true
  );

  WiFi.setHostname(
    OTA_HOSTNAME
  );

  otaLoadSavedWifi();

  // --------------------------------------------------------
  // 1. Najpierw probujemy ostatniej zapisanej sieci.
  // BACK podczas laczenia = od razu przejdz do skanowania.
  // --------------------------------------------------------

  if (otaSavedSsid.length() > 0)
  {
    showMessage(
      "TrackBox G2 OTA",
      "Zapisana siec:",
      otaShorten(
        otaSavedSsid,
        20
      ),
      "Probuje...",
      "BACK=scan"
    );

    if (otaConnectWifi(
          otaSavedSsid,
          otaSavedPass,
          6500))
    {
      if (!otaAutoUpdateIfAvailable(false))
      {
        otaStartWebServer(
          otaSavedSsid
        );
      }
    }
  }

  // --------------------------------------------------------
  // 2. Jesli zapisanej sieci nie ma / nie dziala -> scan.
  // --------------------------------------------------------

  while (true)
  {
    if (!otaScanNetworks())
    {
      showMessage(
        "TrackBox G2 OTA",
        "Brak sieci 2.4G",
        "CONFIRM=scan",
        "Sprawdz router"
      );

      bool lastConfirmLocal = HIGH;
      bool lastPushLocal = HIGH;
      uint32_t confirmChanged = 0;
      uint32_t pushChanged = 0;

      while (true)
      {
        if (otaButtonPressed(
              PIN_CONFIRM,
              lastConfirmLocal,
              confirmChanged) ||
            otaButtonPressed(
              PIN_ENC_PUSH,
              lastPushLocal,
              pushChanged))
        {
          break;
        }

        delay(2);
      }

      continue;
    }

    int selected =
      otaSelectNetwork();

    if (selected == -2)
    {
      // BACK na ekranie listy = ponowny scan.
      continue;
    }

    if (selected < 0 ||
        selected >=
          otaNetworkCount)
    {
      continue;
    }

    String ssid =
      otaNetworks[selected].ssid;

    String password = "";

    bool isOpen =
      otaNetworks[selected].auth ==
      WIFI_AUTH_OPEN;

    // Jesli wybieramy ostatnia zapisana siec,
    // podstawiamy poprzednie haslo. Mozna je edytowac.
    if (!isOpen &&
        ssid == otaSavedSsid)
    {
      password = otaSavedPass;
    }

    if (!isOpen)
    {
      if (!otaEditPassword(
            ssid,
            password))
      {
        // BACK przy pustym hasle = powrot do wyboru sieci.
        continue;
      }
    }

    if (otaConnectWifi(
          ssid,
          password,
          12000))
    {
      otaSaveWifi(
        ssid,
        password
      );

      if (!otaAutoUpdateIfAvailable(false))
      {
        otaStartWebServer(
          ssid
        );
      }
    }

    showMessage(
      "TrackBox G2 OTA",
      "Brak polaczenia",
      otaShorten(
        ssid,
        20
      ),
      "Haslo / zasieg?",
      "CONFIRM=dalej"
    );

    bool lastConfirmLocal = HIGH;
    bool lastPushLocal = HIGH;
    uint32_t confirmChanged = 0;
    uint32_t pushChanged = 0;

    while (true)
    {
      if (otaButtonPressed(
            PIN_CONFIRM,
            lastConfirmLocal,
            confirmChanged) ||
          otaButtonPressed(
            PIN_ENC_PUSH,
            lastPushLocal,
            pushChanged))
      {
        break;
      }

      delay(2);
    }
  }
}


// ============================================================
// OTA - SPRAWDZENIE KOMBINACJI PRZY STARCIE
// ============================================================

bool otaRequestedAtBoot()
{
  if (digitalRead(PIN_BACK) != LOW ||
      digitalRead(PIN_CONFIRM) != LOW)
  {
    return false;
  }

  showMessage(
    "TrackBox G2 OTA",
    "OTA?",
    "Trzymaj oba",
    "przyciski..."
  );

  uint32_t start =
    millis();

  while (millis() - start <
         1800)
  {
    if (digitalRead(PIN_BACK) != LOW ||
        digitalRead(PIN_CONFIRM) != LOW)
    {
      return false;
    }

    delay(10);
  }

  return true;
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

  diagService =
    client->getService(
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


  // ========================================================
  // OTA SERVICE MODE PRZEZ ROUTER
  //
  // Trzymaj BACK + CONFIRM podczas wlaczania zasilania
  // przez ok. 2 sekundy.
  // Normalny start nie uruchamia Wi-Fi i nie zmienia BLE.
  // Tryb OTA laczy ESP jako klienta z wybranym routerem.
  // ========================================================

  if (otaRequestedAtBoot())
  {
    startOtaMode();
  }


  showMessage(
    "TrackBox G2",
    "AUTO OTA READY",
    String("FW ") +
      TB_FW_VERSION,
    "F912 baseline"
  );

  delay(1500);


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