#include <WiFi.h>
#include <WebServer.h>
#define EXTERNAL_SWITCH_PIN 39
#include <AutoConnect.h>
#include <EEPROM.h>
#include <M5StickC.h>
#include <SkaarhojPgmspace.h>
#include <ATEMbase.h>
#include <ATEMstd.h>

AutoConnect Portal;
AutoConnectConfig Config;
AutoConnectAux    auxIPConfig;
AutoConnectAux    auxRestart;

ATEMstd AtemSwitcher;

#define AUTOUPDATE_ORIENTATION 0

#define GRAY  0x7BEF
#define GREEN 0x0400
#define RED   0xB0C0 // 0xF800

// Pin assignment for an external configuration switch
uint8_t ConfigPin = EXTERNAL_SWITCH_PIN;
uint8_t ActiveLevel = LOW;

uint32_t chipId = 0;

int orientation = 0;
int orientationPrevious = 0;
int orientationMillisPrevious = millis();
int buttonBMillis = 0;

int cameraNumber = 1;

int previewTallyPrevious = 1;
int programTallyPrevious = 1;
int cameraNumberPrevious = cameraNumber;


static const char AUX_CONFIGIP[] PROGMEM = R"(
{
  "title": "ATEM Switcher",
  "uri": "/configip",
  "menu": true,
  "element": [
    {
      "name": "caption",
      "type": "ACText",
      "value": "<b>Setup IP of the ATEM Switcher </b>",
      "style": "color:steelblue"
    },
    {
      "name": "mac",
      "type": "ACText",
      "format": "MAC: %s",
      "posterior": "br"
    },
    {
      "name": "staip",
      "type": "ACInput",
      "label": "ATEM Switcher IP",
      "pattern": "^(?:(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\\.){3}(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$",
      "global": true
    },
    {
      "name": "ok",
      "type": "ACSubmit",
      "value": "OK",
      "uri": "/restart"
    },
    {
      "name": "cancel",
      "type": "ACSubmit",
      "value": "Cancel",
      "uri": "/_ac"
    }
  ]
}
)";

static const char AUX_RESTART[] PROGMEM = R"(
{
  "title": "ATEM Switcher",
  "uri": "/restart",
  "menu": false,
  "element": [
    {
      "name": "caption",
      "type": "ACText",
      "value": "Settings",
      "style": "font-family:Arial;font-weight:bold;text-align:center;margin-bottom:10px;color:steelblue"
    },
    {
      "name": "staip",
      "type": "ACText",
      "format": "ATEM Switcher IP: %s",
      "posterior": "br",
      "global": true
    },
    {
      "name": "result",
      "type": "ACText",
      "posterior": "par"
    }
  ]
}
)";

// EEPROM saving structure
typedef union {
  struct {
    uint32_t  ip;
//    uint32_t  gateway;
//    uint32_t  netmask;
//    uint32_t  dns1;
  } ipconfig;
  uint8_t  ipraw[sizeof(uint32_t) * 1];
} IPCONFIG;

// Load IP configuration from EEPROM
void loadConfig(IPCONFIG* ipconfig) {
  EEPROM.begin(sizeof(IPCONFIG));
  int dp = 0;
  for (uint8_t i = 0; i < 1; i++) {
    for (uint8_t c = 0; c < sizeof(uint32_t); c++)
      ipconfig->ipraw[c + i * sizeof(uint32_t)] = EEPROM.read(dp++);
  }
  EEPROM.end();

  // Unset value screening
  if (ipconfig->ipconfig.ip == 0xffffffffL)
    ipconfig->ipconfig.ip = 0U;

  Serial.println("IP configuration loaded");
  Serial.printf("ATEM Switcher IP :0x%08lx\n", ipconfig->ipconfig.ip);

}

// Save current IP configuration to EEPROM
void saveConfig(const IPCONFIG* ipconfig) {
  // EEPROM.begin will truncate the area to the size given by the argument.
  // The part overflowing from the specified size is filled with 0xff,
  // so if the argument value is too small, the credentials may be lost.
  EEPROM.begin(128);

  int dp = 0;
  for (uint8_t i = 0; i < 1; i++) {
    for (uint8_t d = 0; d < sizeof(uint32_t); d++)
      EEPROM.write(dp++, ipconfig->ipraw[d + i * sizeof(uint32_t)]);
  }
  EEPROM.end();
  delay(100);
}

// Custom web page handler to set current configuration to the page
String getConfig(AutoConnectAux& aux, PageArgument& args) {
  IPCONFIG  ipconfig;
  loadConfig(&ipconfig);

  // Fetch MAC address
  String  macAddress;
  uint8_t mac[6];
  WiFi.macAddress(mac);
  for (uint8_t i = 0; i < 6; i++) {
    char buf[3];
    sprintf(buf, "%02X", mac[i]);
    macAddress += buf;
    if (i < 5)
      macAddress += ':';
  }
  aux["mac"].value = macAddress;

  // Fetch each IP address configuration from EEPROM
  IPAddress staip = IPAddress(ipconfig.ipconfig.ip);

  // Echo back the IP settings
  aux["staip"].value = staip.toString();

  return String();
}

// Convert IP address from AutoConnectInput string value
void getIPAddress(String ipString, uint32_t* ip) {
  IPAddress ipAddress;

  if (ipString.length())
    ipAddress.fromString(ipString);
  *ip = (uint32_t)ipAddress;
}

// Custom web page handler to save the configuration to AutoConnectConfig
String setConfig(AutoConnectAux& aux, PageArgument& args) {
  IPCONFIG  ipconfig;

  // Retrieve each IP address from AutoConnectInput field
  getIPAddress(aux["staip"].value, &ipconfig.ipconfig.ip);

  // Make a result message
  if (auxIPConfig.isValid()) {
    saveConfig(&ipconfig);
    aux["result"].value = "Reset by AutoConnect menu will restart with the above.";
  }
  else
    aux["result"].value = "Invalid IP address specified.";
  return String();
}

// Sense the external switch to enter the configuraton mode
bool senseSW(const uint8_t pin, const uint8_t activeLevel) {
  bool  sw = digitalRead(pin) == activeLevel;
  if (sw) {
    // Cut-off the chattering noise
    unsigned long tm = millis();
    while (digitalRead(pin) == activeLevel) {
      if (millis() - tm > 1000)
        break;
      delay(1);
    }
  }
  return sw;
}

void setup() {
  delay(1000);
  Serial.begin(115200);
  Serial.println();
  Serial.println("\nStarting.....");
  Serial.printf("ESP32 Chip model = %s Rev %d\n", ESP.getChipModel(), ESP.getChipRevision());
  Serial.printf("This chip has %d cores\n", ESP.getChipCores());

  // Shift the credentials storage to reserve saving IPCONFIG
  Config.boundaryOffset = sizeof(IPCONFIG);

  // Load current IP configuration
  IPCONFIG  ipconfig;
  loadConfig(&ipconfig);

  Serial.println("IP configuration enable");
  auxIPConfig.load(AUX_CONFIGIP);
  auxIPConfig.on(getConfig);
  auxRestart.load(AUX_RESTART);
  auxRestart.on(setConfig);
  Portal.join({ auxIPConfig, auxRestart });



  Config.apid = "AtemTally-" + String((uint32_t)(ESP.getEfuseMac() >> 32), HEX);
  Config.psk = "af2617735";
  Config.homeUri= "/_ac";
  Config.beginTimeout = 15000;
  Portal.config(Config);
  if (Portal.begin()) {
    Serial.println("WiFi SSID : " + WiFi.SSID());
    Serial.println("WiFi connected: " + WiFi.localIP().toString());
  } 

  M5.begin();
  M5.MPU6886.Init();
  M5.Lcd.setRotation(orientation);

  IPAddress swIP = IPAddress(ipconfig.ipconfig.ip);
  
  AtemSwitcher.begin(swIP);
  AtemSwitcher.serialOutput(0x80);
  AtemSwitcher.connect();
}

void loop() {
  // User sketch process is here.
 M5.update();

  if (AUTOUPDATE_ORIENTATION) {
    if (orientationMillisPrevious + 500 < millis()) {
      setOrientation();
      orientationMillisPrevious = millis();
    }
  }

  if (M5.BtnA.wasPressed()) {
    setOrientation();
    buttonBMillis = millis();
  }

  if (M5.BtnA.isPressed() && buttonBMillis != 0 && buttonBMillis < millis() - 500) {
    Serial.println("Changing camera number");
    cameraNumber = (cameraNumber % 4) + 1;
    Serial.printf("New camera number: %d\n", cameraNumber);

    buttonBMillis = 0;
  }

AtemSwitcher.runLoop();

  int programTally = AtemSwitcher.getProgramTally(cameraNumber);
  int previewTally = AtemSwitcher.getPreviewTally(cameraNumber);

  if ((orientation != orientationPrevious) || (cameraNumber != cameraNumberPrevious) || (programTallyPrevious != programTally) || (previewTallyPrevious != previewTally)) { // changed?
    if (programTally && !previewTally) { // only program
      drawLabel(RED, BLACK, LOW);
     Serial.println("only program");
    } else if (programTally && previewTally) { // program AND preview
      drawLabel(RED, GREEN, LOW);
      Serial.println("program AND preview");
    } else if (previewTally && !programTally) { // only preview
      drawLabel(GREEN, BLACK, HIGH);
      Serial.println("only preview");
    } else if (!previewTally || !programTally) { // neither
      drawLabel(BLACK, GRAY, HIGH);
      Serial.println("neither");
    }
  }

  programTallyPrevious = programTally;
  previewTallyPrevious = previewTally;
  cameraNumberPrevious = cameraNumber;
  orientationPrevious  = orientation;

  Portal.handleClient();
}

void drawLabel(unsigned long int screenColor, unsigned long int labelColor, bool ledValue) {
  // digitalWrite(LED_PIN, ledValue);
  M5.Lcd.fillScreen(screenColor);
  M5.Lcd.setTextColor(labelColor, screenColor);
  drawStringInCenter(String(cameraNumber), 8);
}

void drawStringInCenter(String input, int font) {
  int datumPrevious = M5.Lcd.getTextDatum();
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.drawString(input, M5.Lcd.width() / 2, M5.Lcd.height() / 2, font);
  M5.Lcd.setTextDatum(datumPrevious);
}


void setOrientation() {
  float accX = 0, accY = 0, accZ = 0;
  M5.MPU6886.getAccelData(&accX, &accY, &accZ);
  //Serial.printf("%.2f   %.2f   %.2f \n",accX * 1000, accY * 1000, accZ * 1000);

  if (accZ < .9) {
    if (accX > .6) {
      orientation = 1;
    } else if (accX < .4 && accX > -.5) {
      if (accY > 0) {
        orientation = 0;
      } else {
        orientation = 2;
      }
    } else {
      orientation = 3;
    }
  }

  if (orientation != orientationPrevious) {
    Serial.printf("Orientation changed to %d\n", orientation);
    M5.Lcd.setRotation(orientation);
  }
}
