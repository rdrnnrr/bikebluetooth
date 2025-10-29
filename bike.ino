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
uint32_t lastBlink = 0;
bool blinkOn = false;
String bottomText = "Ready";
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
class RXCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& /*ci*/) override {
    std::string v = c->getValue();
    if(v.empty()) return;
    String s = String(v.c_str());

    // Expected format: SONG|Artist|Album|Title
    int p1 = s.indexOf('|');
    if(p1 < 0) return;
    String cmd = s.substring(0, p1);

    if(cmd == "SONG") {
      int p2 = s.indexOf('|', p1+1);
      int p3 = s.indexOf('|', p2+1);
      if(p2 < 0 || p3 < 0) return;

      artist = s.substring(p1+1, p2);
      album  = s.substring(p2+1, p3);
      song   = s.substring(p3+1);
      drawTopInfo();
      scrollOffset = 0;
      sendUartNotification("ACK");
    }
  }
};

class ServerCB : public NimBLEServerCallbacks {
public:
  void handleConnect() {
    songRequestPending = true;
  }

  void onConnect(NimBLEServer* /*srv*/, NimBLEConnInfo& /*connInfo*/) override {
    handleConnect();
  }

  void onDisconnect(NimBLEServer* /*srv*/, NimBLEConnInfo& /*connInfo*/, int /*reason*/) override {
    songRequestPending = false;
    uartSubscribed = false;
  }
};

class TXCB : public NimBLECharacteristicCallbacks {
public:
  void onSubscribe(NimBLECharacteristic* /*c*/, NimBLEConnInfo& /*ci*/, uint16_t subValue) override {
    uartSubscribed = (subValue != 0);
  }
};

// ===== UART SERVICE =====
void setupUartService() {
  NimBLEServer* srv = NimBLEDevice::createServer();
  srv->setCallbacks(new ServerCB());
  uartSvc = srv->createService(UART_SVC_UUID);
  uartRX = uartSvc->createCharacteristic(UART_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  uartTX = uartSvc->createCharacteristic(UART_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  uartTX->setCallbacks(new TXCB());
  uartRX->setCallbacks(new RXCB());
  uartSvc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(UART_SVC_UUID);
  adv->setScanResponseData(NimBLEAdvertisementData());
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

  // Restart if no connection after 30s
  if (!bleKeyboard.isConnected() && (now - lastConnectAttempt > 30000)) {
    Serial.println("No connection after 30s — restarting...");
    drawBottomStatic("Restarting");
    delay(1500);
    ESP.restart();
  }

  if (bleKeyboard.isConnected()) lastConnectAttempt = now;

  if(songRequestPending && uartTX && uartSubscribed) {
    sendUartNotification("REQ|SONG");
    songRequestPending = false;
  }

  if(bleKeyboard.isConnected() && song.length() > 0) {
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