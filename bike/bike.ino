#include <BleKeyboard.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <NimBLEDevice.h>

// ===== PINS =====
#define JOY_X_PIN 34
#define JOY_Y_PIN 35
#define JOY_SW_PIN 32
#define OLED_ADDR 0x3C
#define TOP_SDA 21
#define TOP_SCL 22
#define BOT_SDA 25
#define BOT_SCL 26

TwoWire WireTop = TwoWire(0);
TwoWire WireBot = TwoWire(1);

Adafruit_SSD1306 oledTop(128, 32, &WireTop, -1);
Adafruit_SSD1306 oledBot(128, 32, &WireBot, -1);

// ===== BLE OBJECTS =====
BleKeyboard bleKeyboard("JuiceBox Remote", "JuiceBox", 100);
static const char* UART_SVC_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* UART_RX_UUID  = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* UART_TX_UUID  = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

NimBLEService* uartSvc = nullptr;
NimBLECharacteristic* uartRX = nullptr;
NimBLECharacteristic* uartTX = nullptr;
String uartBuffer = "";

// ===== JOYSTICK =====
const float EMA_ALPHA = 0.18f;
const int DEADZONE = 350;
const int TRIGGER_THRESH = 1200;
const uint16_t VOL_REPEAT_MS = 120;
const uint16_t ACTION_COOLDOWN = 220;
float emaX = 0, emaY = 0;
int midX = 2048, midY = 2048;
uint32_t tUp=0, tDn=0, tL=0, tR=0, tBtn=0;

// ===== STATE =====
bool lastConnected = false;
String artist = "";
String album = "";
String song = "";
uint32_t lastConnectAttempt = 0;
uint32_t lastScroll = 0;
int scrollOffset = 0;
bool songRequestPending = false;
bool uartSubscribed = false;

// Forward declarations
void sendUartNotification(const String& message);
void handleUartMessage(const String& message);
String sanitizeForDisplay(const String& input);
bool appendMappedCodepoint(String& out, uint32_t codepoint);

// ===== HELPERS =====
inline bool inDZ(float v){ return fabs(v) < DEADZONE; }
inline bool beyond(float v){ return fabs(v) >= TRIGGER_THRESH; }

void calibrateCenter(){
  long sx=0, sy=0;
  for(int i=0;i<24;i++){ sx+=analogRead(JOY_X_PIN); sy+=analogRead(JOY_Y_PIN); delay(4); }
  midX=sx/24; midY=sy/24; emaX=emaY=0;
}

void smoothRead(){
  int rx=analogRead(JOY_X_PIN);
  int ry=analogRead(JOY_Y_PIN);
  emaX=(1.0f-EMA_ALPHA)*emaX + EMA_ALPHA*(rx-midX);
  emaY=(1.0f-EMA_ALPHA)*emaY + EMA_ALPHA*(ry-midY);
}

void sendMedia(const uint8_t* key){ if(bleKeyboard.isConnected()) bleKeyboard.write(key); }

// ===== DISPLAY =====
void drawTopInfo() {
  oledTop.clearDisplay();
  oledTop.setTextColor(SSD1306_WHITE);
  oledTop.setTextSize(1);
  oledTop.setCursor(0, 0);
  oledTop.println(artist.length() ? artist : "Unknown Artist");
  oledTop.setCursor(0, 16);
  oledTop.println(album.length() ? album : "Unknown Album");
  oledTop.display();
}

void drawBottomStatic(const char* txt) {
  oledBot.clearDisplay();
  oledBot.setTextColor(SSD1306_WHITE);
  oledBot.setTextSize(2);
  int16_t x1,y1; uint16_t w,h;
  oledBot.getTextBounds(txt,0,0,&x1,&y1,&w,&h);
  int x=(128-w)/2; int y=(32-h)/2;
  oledBot.setCursor(x,y);
  oledBot.println(txt);
  oledBot.display();
}

void drawBottomScroll() {
  oledBot.clearDisplay();
  oledBot.setTextColor(SSD1306_WHITE);
  oledBot.setTextSize(2);

  int16_t x1,y1; uint16_t w,h;
  oledBot.getTextBounds(song.c_str(),0,0,&x1,&y1,&w,&h);

  // Scroll if longer than screen width
  if (w > 128) {
    oledBot.setCursor(-scrollOffset, 8);
    oledBot.print(song);
    oledBot.display();

    if (millis() - lastScroll > 150) {
      scrollOffset++;
      if (scrollOffset > w + 16) scrollOffset = 0;
      lastScroll = millis();
    }
  } else {
    int x = (128 - w)/2;
    oledBot.setCursor(x, 8);
    oledBot.print(song);
    oledBot.display();
  }
}

void showBoot(){ oledTop.clearDisplay(); oledBot.clearDisplay(); drawBottomStatic("Booting"); }
void showReady(){ drawTopInfo(); drawBottomStatic("Ready"); }

// ===== UART CALLBACK =====
class TXCB : public NimBLECharacteristicCallbacks {
public:
  void onSubscribe(NimBLECharacteristic* /*c*/, NimBLEConnInfo& /*ci*/, uint16_t subValue) override {
    uartSubscribed = (subValue != 0);
    if(uartSubscribed) {
      songRequestPending = true;
      if(song.length() == 0) {
        // ensure we clear any stale scroll state before the app pushes data
        scrollOffset = 0;
      }
    } else {
      songRequestPending = false;
      uartBuffer = "";
      NimBLEDevice::startAdvertising();
    }
  }
};

String sanitizeForDisplay(const String& input) {
  String sanitized;
  sanitized.reserve(input.length());

  for (uint16_t i = 0; i < input.length();) {
    uint8_t b = static_cast<uint8_t>(input.charAt(i));

    if (b < 0x80) {
      if (b >= 32 && b <= 126) {
        sanitized += static_cast<char>(b);
      } else if (b == '\t') {
        sanitized += ' ';
      }
      i++;
      continue;
    }

    uint32_t codepoint = 0;
    uint8_t needed = 0;

    if ((b & 0xE0) == 0xC0 && (i + 1) < input.length()) {
      codepoint = ((uint32_t)(b & 0x1F) << 6) |
                  (static_cast<uint32_t>(static_cast<uint8_t>(input.charAt(i + 1))) & 0x3F);
      needed = 1;
    } else if ((b & 0xF0) == 0xE0 && (i + 2) < input.length()) {
      codepoint = ((uint32_t)(b & 0x0F) << 12) |
                  ((static_cast<uint32_t>(static_cast<uint8_t>(input.charAt(i + 1))) & 0x3F) << 6) |
                  (static_cast<uint32_t>(static_cast<uint8_t>(input.charAt(i + 2))) & 0x3F);
      needed = 2;
    } else if ((b & 0xF8) == 0xF0 && (i + 3) < input.length()) {
      codepoint = ((uint32_t)(b & 0x07) << 18) |
                  ((static_cast<uint32_t>(static_cast<uint8_t>(input.charAt(i + 1))) & 0x3F) << 12) |
                  ((static_cast<uint32_t>(static_cast<uint8_t>(input.charAt(i + 2))) & 0x3F) << 6) |
                  (static_cast<uint32_t>(static_cast<uint8_t>(input.charAt(i + 3))) & 0x3F);
      needed = 3;
    } else {
      i++;
      continue;
    }

    i += needed + 1;
    if (!appendMappedCodepoint(sanitized, codepoint)) {
      sanitized += '?';
    }
  }

  sanitized.trim();
  return sanitized;
}

bool appendMappedCodepoint(String& out, uint32_t cp) {
  switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: case 0x2035:
      out += '\'';
      return true;
    case 0x201C: case 0x201D: case 0x201E:
      out += '"';
      return true;
    case 0x2013: case 0x2014: case 0x2212:
      out += '-';
      return true;
    case 0x2022:
      out += '*';
      return true;
    case 0x2026:
      out += "...";
      return true;
    case 0x2122:
      out += "TM";
      return true;
  }

  if ((cp >= 0x00C0 && cp <= 0x00C5) || cp == 0x0100 || cp == 0x0102 || cp == 0x0104) { out += 'A'; return true; }
  if ((cp >= 0x00E0 && cp <= 0x00E5) || cp == 0x0101 || cp == 0x0103 || cp == 0x0105) { out += 'a'; return true; }
  if (cp == 0x00C6) { out += "AE"; return true; }
  if (cp == 0x00E6) { out += "ae"; return true; }
  if (cp == 0x00C7 || cp == 0x0106 || cp == 0x0108 || cp == 0x010A || cp == 0x010C) { out += 'C'; return true; }
  if (cp == 0x00E7 || cp == 0x0107 || cp == 0x0109 || cp == 0x010B || cp == 0x010D) { out += 'c'; return true; }
  if ((cp >= 0x00C8 && cp <= 0x00CB) || cp == 0x0112 || cp == 0x0114 || cp == 0x0116 || cp == 0x0118 || cp == 0x011A) { out += 'E'; return true; }
  if ((cp >= 0x00E8 && cp <= 0x00EB) || cp == 0x0113 || cp == 0x0115 || cp == 0x0117 || cp == 0x0119 || cp == 0x011B) { out += 'e'; return true; }
  if ((cp >= 0x00CC && cp <= 0x00CF) || cp == 0x0128 || cp == 0x012A || cp == 0x012C || cp == 0x012E || cp == 0x0130) { out += 'I'; return true; }
  if ((cp >= 0x00EC && cp <= 0x00EF) || cp == 0x0129 || cp == 0x012B || cp == 0x012D || cp == 0x012F || cp == 0x0131) { out += 'i'; return true; }
  if (cp == 0x00D0 || cp == 0x010E) { out += 'D'; return true; }
  if (cp == 0x00F0 || cp == 0x010F) { out += 'd'; return true; }
  if (cp == 0x00D1 || cp == 0x0143 || cp == 0x0147) { out += 'N'; return true; }
  if (cp == 0x00F1 || cp == 0x0144 || cp == 0x0148) { out += 'n'; return true; }
  if ((cp >= 0x00D2 && cp <= 0x00D6) || cp == 0x014C || cp == 0x0150 || cp == 0x00D8) { out += 'O'; return true; }
  if ((cp >= 0x00F2 && cp <= 0x00F6) || cp == 0x014D || cp == 0x0151 || cp == 0x00F8) { out += 'o'; return true; }
  if ((cp >= 0x00D9 && cp <= 0x00DC) || cp == 0x0168 || cp == 0x016A || cp == 0x016C || cp == 0x016E || cp == 0x0170 || cp == 0x0172) { out += 'U'; return true; }
  if ((cp >= 0x00F9 && cp <= 0x00FC) || cp == 0x0169 || cp == 0x016B || cp == 0x016D || cp == 0x016F || cp == 0x0171 || cp == 0x0173) { out += 'u'; return true; }
  if (cp == 0x00DD || cp == 0x0178) { out += 'Y'; return true; }
  if (cp == 0x00FD || cp == 0x00FF) { out += 'y'; return true; }
  if (cp == 0x00DE) { out += 'T'; return true; }
  if (cp == 0x00FE) { out += 't'; return true; }
  if (cp == 0x00DF) { out += 's'; out += 's'; return true; }
  if (cp == 0x0152) { out += "OE"; return true; }
  if (cp == 0x0153) { out += "oe"; return true; }
  if (cp == 0x00B0) { out += 'o'; return true; }

  return false;
}
void handleUartMessage(const String& s) {
  if(!s.length()) return;

  int p1 = s.indexOf('|');
  if(p1 < 0) return;
  String cmd = s.substring(0, p1);

  if(cmd == "SONG") {
    int p2 = s.indexOf('|', p1+1);
    int p3 = s.indexOf('|', p2+1);
    if(p2 < 0 || p3 < 0) return;

    artist = sanitizeForDisplay(s.substring(p1+1, p2));
    album  = sanitizeForDisplay(s.substring(p2+1, p3));
    song   = sanitizeForDisplay(s.substring(p3+1));
    drawTopInfo();
    scrollOffset = 0;
    sendUartNotification("ACK");
  }
}

class RXCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& /*ci*/) override {
    std::string v = c->getValue();
    if(v.empty()) return;

    uartBuffer += String(v.c_str());
    if(uartBuffer.length() > 256) {
      uartBuffer = uartBuffer.substring(uartBuffer.length() - 256);
    }

    int newlineIndex = uartBuffer.indexOf('\n');
    while(newlineIndex >= 0) {
      String message = uartBuffer.substring(0, newlineIndex);
      uartBuffer.remove(0, newlineIndex + 1);
      message.trim();
      handleUartMessage(message);
      newlineIndex = uartBuffer.indexOf('\n');
    }
  }
};

// ===== UART SERVICE =====
void setupUartService() {
  NimBLEServer* srv = NimBLEDevice::getServer();
  if(!srv) {
    srv = NimBLEDevice::createServer();
  }
  uartSvc = srv->createService(UART_SVC_UUID);
  uartRX = uartSvc->createCharacteristic(UART_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  uartTX = uartSvc->createCharacteristic(UART_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  uartTX->setCallbacks(new TXCB());
  uartRX->setCallbacks(new RXCB());
  uartSvc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if(adv) {
    adv->addServiceUUID(UART_SVC_UUID);
    if(!adv->isAdvertising()) {
      adv->start();
    }
  }
}

void sendUartNotification(const String& message) {
  if(!uartTX || !uartSubscribed) return;

  std::string payload(message.c_str());
  uartTX->setValue(payload);
  uartTX->notify();
}

// ===== SETUP =====
void setup(){
  Serial.begin(115200);
  pinMode(JOY_SW_PIN, INPUT_PULLUP);
  analogReadResolution(12);
  WireTop.begin(TOP_SDA, TOP_SCL);
  WireBot.begin(BOT_SDA, BOT_SCL);

  oledTop.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  oledBot.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);

  NimBLEDevice::init("JuiceBox Remote");
  bleKeyboard.begin();
  setupUartService();

  calibrateCenter();
  showBoot();

  lastConnectAttempt = millis();
}

// ===== LOOP =====
void loop(){
  uint32_t now = millis();

  bool kbConnected = bleKeyboard.isConnected();
  if(kbConnected != lastConnected) {
    if(kbConnected) {
      songRequestPending = true;
    } else {
      songRequestPending = false;
      NimBLEDevice::startAdvertising();
    }
    lastConnected = kbConnected;
  }

  // Restart if no connection after 30s
  bool anyConnection = kbConnected || uartSubscribed;

  if (!anyConnection && (now - lastConnectAttempt > 30000)) {
    Serial.println("No connection after 30s — restarting...");
    drawBottomStatic("Restarting");
    delay(1500);
    ESP.restart();
  }

  if (anyConnection) lastConnectAttempt = now;

  if(songRequestPending && uartTX && uartSubscribed) {
    sendUartNotification("REQ|SONG");
    songRequestPending = false;
  }

  if(kbConnected && song.length() > 0) {
    drawBottomScroll();  // Continuous scroll update
  }

  smoothRead();
  bool btn = (digitalRead(JOY_SW_PIN) == LOW);
  if(btn && (now - tBtn > 280)){
    tBtn = now; sendMedia(KEY_MEDIA_PLAY_PAUSE);
    drawBottomStatic("Play");
  }

  if(!inDZ(emaX) && beyond(emaX)){
    if(emaX>0 && now-tR>ACTION_COOLDOWN){
      sendMedia(KEY_MEDIA_NEXT_TRACK);
      drawBottomStatic("Next ▶"); tR=now;
    } else if(emaX<0 && now-tL>ACTION_COOLDOWN){
      sendMedia(KEY_MEDIA_PREVIOUS_TRACK);
      drawBottomStatic("◀ Prev"); tL=now;
    }
  }

  if(!inDZ(emaY) && beyond(emaY)){
    if(emaY<0 && now-tUp>VOL_REPEAT_MS){
      sendMedia(KEY_MEDIA_VOLUME_UP);
      drawBottomStatic("Vol +"); tUp=now;
    } else if(emaY>0 && now-tDn>VOL_REPEAT_MS){
      sendMedia(KEY_MEDIA_VOLUME_DOWN);
      drawBottomStatic("Vol -"); tDn=now;
    }
  }

  delay(6);
}
