#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <NimBLEDevice.h>
#include <math.h>

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

// ===== BLE CONSTANTS =====
static uint8_t securityKeyMask() {
  uint8_t mask = 0;
#if defined(BLE_SM_PAIR_KEY_DIST_ENC)
  mask |= BLE_SM_PAIR_KEY_DIST_ENC;
#elif defined(BLE_ENC_KEY)
  mask |= BLE_ENC_KEY;
#endif
#if defined(BLE_SM_PAIR_KEY_DIST_ID)
  mask |= BLE_SM_PAIR_KEY_DIST_ID;
#elif defined(BLE_ID_KEY)
  mask |= BLE_ID_KEY;
#endif
  return mask;
}

static const NimBLEUUID ANCS_SERVICE_UUID("7905F431-B5CE-4E99-A40F-4B1E122D00D0");
static const NimBLEUUID ANCS_NOTIFICATION_SOURCE_UUID("9FBF120D-6301-42D9-8C58-25E699A21DBD");
static const NimBLEUUID ANCS_CONTROL_POINT_UUID("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9");
static const NimBLEUUID ANCS_DATA_SOURCE_UUID("22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB");

static const NimBLEUUID AMS_SERVICE_UUID("89D3502B-0F36-433A-8EF4-C502AD55F8DC");
static const NimBLEUUID AMS_REMOTE_COMMAND_UUID("9B3C81D8-57B1-4510-8A45-A1EFD60B605A");
static const NimBLEUUID AMS_ENTITY_UPDATE_UUID("2F7CABCE-808D-411F-9A0C-BB92BA96C102");
static const NimBLEUUID AMS_ENTITY_ATTRIBUTE_UUID("C6B2F38C-23AB-46D8-A6AB-A3A870B5A1D6");

NimBLEScan* bleScanner = nullptr;
NimBLEClient* iosClient = nullptr;

NimBLERemoteCharacteristic* ancsNotificationSource = nullptr;
NimBLERemoteCharacteristic* ancsControlPoint = nullptr;
NimBLERemoteCharacteristic* ancsDataSource = nullptr;

NimBLERemoteCharacteristic* amsRemoteCommand = nullptr;
NimBLERemoteCharacteristic* amsEntityUpdate = nullptr;
NimBLERemoteCharacteristic* amsEntityAttribute = nullptr;

bool iosConnected = false;
bool ancsReady = false;
bool amsReady = false;

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
String artist = "";
String album = "";
String song = "";

String notificationApp = "";
String notificationTitle = "";
String notificationMessage = "";
uint32_t notificationExpiry = 0;
uint32_t statusExpiry = 0;
uint32_t lastConnectAttempt = 0;
uint32_t lastScroll = 0;
int scrollOffset = 0;
bool notificationActive = false;
bool statusActive = false;
uint32_t currentNotificationUid = 0;
uint8_t currentNotificationCategory = 0;

enum AmsRemoteCommand : uint8_t {
  AMS_CMD_PLAY = 0x00,
  AMS_CMD_PAUSE = 0x01,
  AMS_CMD_TOGGLE_PLAY_PAUSE = 0x02,
  AMS_CMD_NEXT_TRACK = 0x03,
  AMS_CMD_PREVIOUS_TRACK = 0x04,
  AMS_CMD_VOLUME_UP = 0x05,
  AMS_CMD_VOLUME_DOWN = 0x06
};

// Forward declarations
String cleanDisplayText(const String& input);
void startScan();
bool connectToAdvertisedDevice(const NimBLEAdvertisedDevice* device);
bool setupAncs(NimBLEClient* client);
bool setupAms(NimBLEClient* client);
void requestAncsAttributes(uint32_t uid);
void handleAncsNotification(const uint8_t* data, size_t length);
void handleAncsData(const uint8_t* data, size_t length);
void handleAmsEntityUpdate(const uint8_t* data, size_t length);
void subscribeAmsAttributes();
void showNotification(const String& title, const String& message, const String& appName, uint32_t durationMs = 12000);
void clearNotification();
void drawBottomMessage(const String& line1, const String& line2);
void showStatus(const String& message, uint32_t durationMs = 1500);
void sendAmsCommand(uint8_t commandId);

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

// ===== DISPLAY =====
void drawTopInfo() {
  if(notificationActive) return;
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

void drawBottomMessage(const String& line1, const String& line2) {
  oledBot.clearDisplay();
  oledBot.setTextColor(SSD1306_WHITE);
  oledBot.setTextSize(1);
  oledBot.setCursor(0, 0);
  oledBot.println(line1);
  oledBot.setCursor(0, 16);
  oledBot.println(line2);
  oledBot.display();
}

void drawBottomScroll() {
  if(notificationActive || statusActive) return;
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

void showNotification(const String& title, const String& message, const String& appName, uint32_t durationMs) {
  notificationTitle = cleanDisplayText(title);
  notificationMessage = cleanDisplayText(message);
  notificationApp = cleanDisplayText(appName);
  notificationActive = true;
  notificationExpiry = durationMs ? millis() + durationMs : 0;

  oledTop.clearDisplay();
  oledTop.setTextColor(SSD1306_WHITE);
  oledTop.setTextSize(1);
  oledTop.setCursor(0, 0);
  if(notificationApp.length()) {
    oledTop.println(notificationApp);
  } else {
    oledTop.println("Notification");
  }

  String secondLine = notificationTitle.length() ? notificationTitle : "";
  if(secondLine.length() > 20) {
    secondLine = secondLine.substring(0, 20);
  }
  oledTop.setCursor(0, 16);
  oledTop.println(secondLine);
  oledTop.display();

  String line1;
  String line2;
  if(notificationMessage.length() <= 21) {
    line1 = notificationMessage;
    line2 = "";
  } else {
    line1 = notificationMessage.substring(0, 21);
    uint16_t endIndex = notificationMessage.length() > 42 ? 42 : notificationMessage.length();
    line2 = notificationMessage.substring(21, endIndex);
  }
  drawBottomMessage(line1, line2);
  statusActive = false;
}

void clearNotification() {
  notificationActive = false;
  notificationExpiry = 0;
  notificationApp = "";
  notificationTitle = "";
  notificationMessage = "";
  drawTopInfo();
  if(song.length()) {
    drawBottomScroll();
  } else if(!statusActive) {
    drawBottomStatic("Ready");
  }
}

void showStatus(const String& message, uint32_t durationMs) {
  if(notificationActive) return;
  String cleaned = cleanDisplayText(message);
  drawBottomStatic(cleaned.c_str());
  statusActive = true;
  if(durationMs == 0) {
    statusExpiry = 0;
  } else {
    statusExpiry = millis() + durationMs;
  }
}

class ClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient* /*client*/, int /*reason*/) override {
    Serial.println("iOS device disconnected");
    iosConnected = false;
    ancsReady = false;
    amsReady = false;
    ancsNotificationSource = nullptr;
    ancsControlPoint = nullptr;
    ancsDataSource = nullptr;
    amsRemoteCommand = nullptr;
    amsEntityUpdate = nullptr;
    amsEntityAttribute = nullptr;
    song = "";
    artist = "";
    album = "";
    notificationActive = false;
    statusActive = false;
    drawTopInfo();
    drawBottomStatic("Disconnected");
    startScan();
  }
};

class AdvertisedDeviceCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    Serial.print("Found device: ");
    Serial.print(advertisedDevice->getAddress().toString().c_str());
    Serial.print(", advertisement data: ");
    Serial.println(advertisedDevice->toString().c_str());
    if(iosConnected) return;

    std::string strAddress = advertisedDevice->getAddress().toString();
    if(strAddress.rfind("cc:3f:36", 0) == 0) {
      Serial.println("Found potential iOS device: " + String(advertisedDevice->getAddress().toString().c_str()));
      NimBLEDevice::getScan()->stop();
      NimBLEAdvertisedDevice* device = new NimBLEAdvertisedDevice(*advertisedDevice);
      connectToAdvertisedDevice(device);
      delete device;
    }
  }
};

static ClientCallbacks clientCallbacks;
static AdvertisedDeviceCallbacks advertisedDeviceCallbacks;

void scanEnded(NimBLEScanResults results) {
  Serial.println("Scan ended");
}

void startScan() {
  if(!bleScanner) return;
  if(bleScanner->isScanning()) return;
  if(iosClient && iosClient->isConnected()) return;

  Serial.println("Starting BLE scan for iOS devices...");
  bleScanner->setScanCallbacks(&advertisedDeviceCallbacks, false);
  bleScanner->setInterval(45);
  bleScanner->setWindow(30);
  bleScanner->setActiveScan(true);
  bleScanner->start(0, false);
  lastConnectAttempt = millis();
  showStatus("Scanning", 0);
}

bool setupAncs(NimBLEClient* client) {
  ancsNotificationSource = nullptr;
  ancsControlPoint = nullptr;
  ancsDataSource = nullptr;
  ancsReady = false;

  if(!client) return false;

  NimBLERemoteService* service = client->getService(ANCS_SERVICE_UUID);
  if(!service) {
    Serial.println("ANCS service not found");
    return false;
  }

  ancsNotificationSource = service->getCharacteristic(ANCS_NOTIFICATION_SOURCE_UUID);
  ancsControlPoint = service->getCharacteristic(ANCS_CONTROL_POINT_UUID);
  ancsDataSource = service->getCharacteristic(ANCS_DATA_SOURCE_UUID);

  if(!ancsNotificationSource || !ancsControlPoint || !ancsDataSource) {
    Serial.println("ANCS characteristics missing");
    return false;
  }

  if(ancsNotificationSource->canNotify()) {
    if(!ancsNotificationSource->subscribe(true, [](NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
      handleAncsNotification(data, length);
    })) {
      Serial.println("Failed to subscribe to ANCS notification source");
      return false;
    }
  }

  if(ancsDataSource->canNotify()) {
    if(!ancsDataSource->subscribe(true, [](NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
      handleAncsData(data, length);
    })) {
      Serial.println("Failed to subscribe to ANCS data source");
      return false;
    }
  }

  ancsReady = true;
  Serial.println("ANCS ready");
  return true;
}

bool setupAms(NimBLEClient* client) {
  amsRemoteCommand = nullptr;
  amsEntityUpdate = nullptr;
  amsEntityAttribute = nullptr;
  amsReady = false;

  if(!client) return false;

  NimBLERemoteService* service = client->getService(AMS_SERVICE_UUID);
  if(!service) {
    Serial.println("AMS service not found");
    return false;
  }

  amsRemoteCommand = service->getCharacteristic(AMS_REMOTE_COMMAND_UUID);
  amsEntityUpdate = service->getCharacteristic(AMS_ENTITY_UPDATE_UUID);
  amsEntityAttribute = service->getCharacteristic(AMS_ENTITY_ATTRIBUTE_UUID);

  if(!amsRemoteCommand || !amsEntityUpdate || !amsEntityAttribute) {
    Serial.println("AMS characteristics missing");
    return false;
  }

  if(amsEntityUpdate->canNotify()) {
    if(!amsEntityUpdate->subscribe(true, [](NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
      handleAmsEntityUpdate(data, length);
    })) {
      Serial.println("Failed to subscribe to AMS entity update");
      return false;
    }
  }

  amsReady = true;
  Serial.println("AMS ready");
  return true;
}

bool connectToAdvertisedDevice(const NimBLEAdvertisedDevice* device) {
  if(!device) return false;

  Serial.println("Connecting to iOS device at " + String(device->getAddress().toString().c_str()));

  if(!iosClient) {
    iosClient = NimBLEDevice::createClient();
    iosClient->setClientCallbacks(&clientCallbacks, false);
    iosClient->setConnectionParams(12, 24, 0, 60);
    iosClient->setConnectTimeout(5);
  } else {
    iosClient->setClientCallbacks(&clientCallbacks, false);
    if(iosClient->isConnected()) {
      Serial.println("Already connected to iOS device");
      return true;
    }
  }

  if(!iosClient->connect(device)) {
    Serial.println("Connection to iOS device failed");
    startScan();
    return false;
  }

  iosConnected = true;
  Serial.println("Connected to iOS device");
  lastConnectAttempt = millis();

  bool haveAncs = setupAncs(iosClient);
  bool haveAms = setupAms(iosClient);

  if(!haveAncs && !haveAms) {
    Serial.println("Required Apple services not available, disconnecting");
    iosClient->disconnect();
    iosConnected = false;
    startScan();
    return false;
  }

  showStatus("Connected", 1800);
  subscribeAmsAttributes();
  return true;
}

void subscribeAmsAttributes() {
  if(!amsReady || !amsEntityUpdate || !amsEntityAttribute) return;

  // Request notifications for track attributes (title, artist, album)
  uint8_t trackTitleReq[] = {0x02, 0x00, 0x80};
  uint8_t trackArtistReq[] = {0x02, 0x01, 0x80};
  uint8_t trackAlbumReq[] = {0x02, 0x02, 0x80};
  amsEntityUpdate->writeValue(trackTitleReq, sizeof(trackTitleReq), false);
  amsEntityUpdate->writeValue(trackArtistReq, sizeof(trackArtistReq), false);
  amsEntityUpdate->writeValue(trackAlbumReq, sizeof(trackAlbumReq), false);

  // Request current values for the attributes we care about.
  struct {
    uint8_t attribute;
    uint16_t maxLen;
  } attrRequests[] = {
    {0x00, 128},
    {0x01, 128},
    {0x02, 128}
  };

  for(auto& req : attrRequests) {
    uint8_t payload[] = {0x02, req.attribute, static_cast<uint8_t>(req.maxLen & 0xFF), static_cast<uint8_t>((req.maxLen >> 8) & 0xFF)};
    amsEntityAttribute->writeValue(payload, sizeof(payload), true);
  }
}

void requestAncsAttributes(uint32_t uid) {
  if(!ancsReady || !ancsControlPoint) return;

  currentNotificationUid = uid;

  uint8_t payload[20];
  size_t idx = 0;
  payload[idx++] = 0x00; // Get Notification Attributes command
  payload[idx++] = uid & 0xFF;
  payload[idx++] = (uid >> 8) & 0xFF;
  payload[idx++] = (uid >> 16) & 0xFF;
  payload[idx++] = (uid >> 24) & 0xFF;

  payload[idx++] = 0x00; // App Identifier
  payload[idx++] = 0x01; // Title
  payload[idx++] = 64;
  payload[idx++] = 0;
  payload[idx++] = 0x02; // Subtitle
  payload[idx++] = 64;
  payload[idx++] = 0;
  payload[idx++] = 0x03; // Message
  payload[idx++] = 128;
  payload[idx++] = 0;

  ancsControlPoint->writeValue(payload, idx, true);
}

void handleAncsNotification(const uint8_t* data, size_t length) {
  if(length < 8) return;

  uint8_t eventId = data[0];
  uint8_t eventFlags = data[1];
  (void)eventFlags;
  uint8_t categoryId = data[2];
  uint32_t uid = data[4] | (data[5] << 8) | (data[6] << 16) | (data[7] << 24);

  currentNotificationCategory = categoryId;

  if(eventId == 0x00 || eventId == 0x01) { // Added or Modified
    requestAncsAttributes(uid);
  } else if(eventId == 0x02 && notificationActive && uid == currentNotificationUid) {
    clearNotification();
  }
}

void handleAncsData(const uint8_t* data, size_t length) {
  if(length < 5) return;
  if(data[0] != 0x00) return; // Expect Get Notification Attributes response

  uint32_t uid = data[1] | (data[2] << 8) | (data[3] << 16) | (data[4] << 24);
  if(uid != currentNotificationUid) {
    currentNotificationUid = uid;
  }

  size_t index = 5;
  String appId;
  String title;
  String subtitle;
  String message;

  while(index + 2 < length) {
    uint8_t attrId = data[index++];
    if(index + 2 > length) break;
    uint16_t attrLen = data[index] | (data[index + 1] << 8);
    index += 2;

    String value = "";
    if(attrLen && index + attrLen <= length) {
      value.reserve(attrLen);
      for(uint16_t i = 0; i < attrLen; ++i) {
        value += static_cast<char>(data[index + i]);
      }
      index += attrLen;
    } else if(attrLen) {
      break;
    }

    switch(attrId) {
      case 0x00: appId = value; break;
      case 0x01: title = value; break;
      case 0x02: subtitle = value; break;
      case 0x03: message = value; break;
      default: break;
    }
  }

  String displayTitle = title.length() ? title : subtitle;
  if(displayTitle.length() == 0) displayTitle = message;
  if(displayTitle.length() == 0) displayTitle = appId;

  String displayMessage = message.length() ? message : subtitle;
  if(displayMessage.length() == 0) displayMessage = title;
  if(displayMessage.length() == 0) displayMessage = appId;

  String appDisplay = appId;
  int dot = appDisplay.lastIndexOf('.');
  if(dot >= 0 && dot + 1 < appDisplay.length()) {
    appDisplay = appDisplay.substring(dot + 1);
  }
  appDisplay.replace('_', ' ');
  appDisplay.replace('-', ' ');

  uint32_t duration = (currentNotificationCategory == 0x01 || currentNotificationCategory == 0x02) ? 0 : 12000;
  showNotification(displayTitle, displayMessage, appDisplay, duration);
}

void handleAmsEntityUpdate(const uint8_t* data, size_t length) {
  if(length < 5) return;

  uint8_t entityId = data[0];
  uint8_t attributeId = data[1];
  uint16_t valueLen = data[3] | (data[4] << 8);
  if(5 + valueLen > length) return;

  String value = "";
  if(valueLen) {
    value.reserve(valueLen);
    for(uint16_t i = 0; i < valueLen; ++i) {
      value += static_cast<char>(data[5 + i]);
    }
  }

  if(entityId == 0x02) { // Track entity
    if(attributeId == 0x00) {
      song = cleanDisplayText(value);
      scrollOffset = 0;
      lastScroll = millis();
      if(!notificationActive && !statusActive) {
        drawBottomScroll();
      }
    } else if(attributeId == 0x01) {
      artist = cleanDisplayText(value);
      drawTopInfo();
    } else if(attributeId == 0x02) {
      album = cleanDisplayText(value);
      drawTopInfo();
    }
  }
}

void sendAmsCommand(uint8_t commandId) {
  if(!amsReady || !amsRemoteCommand) return;
  amsRemoteCommand->writeValue(&commandId, 1, false);
}

// ===== STRING UTILITIES =====
String cleanDisplayText(const String& input) {
  String cleaned;
  cleaned.reserve(input.length());

  for (uint16_t i = 0; i < input.length();) {
    uint8_t byte = static_cast<uint8_t>(input.charAt(i));

    if (byte == '\r' || byte == '\n') {
      i++;
      continue;
    }

    if (byte == '\t') {
      cleaned += ' ';
      i++;
      continue;
    }

    if (byte >= 32 && byte <= 126) {
      cleaned += static_cast<char>(byte);
      i++;
      continue;
    }

    if (byte >= 0x80) {
      cleaned += '?';
      i++;
      while (i < input.length()) {
        uint8_t next = static_cast<uint8_t>(input.charAt(i));
        if ((next & 0xC0) == 0x80) {
          i++;
        } else {
          break;
        }
      }
      continue;
    }

    i++;
  }

  cleaned.trim();
  return cleaned;
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
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_YESNO);
  uint8_t keyMask = securityKeyMask();
  NimBLEDevice::setSecurityInitKey(keyMask);
  NimBLEDevice::setSecurityRespKey(keyMask);

  bleScanner = NimBLEDevice::getScan();
  bleScanner->setScanCallbacks(&advertisedDeviceCallbacks, false);
  bleScanner->setInterval(45);
  bleScanner->setWindow(30);
  bleScanner->setActiveScan(true);

  showBoot();
  calibrateCenter();
  showStatus("Scanning", 0);
  startScan();
}

// ===== LOOP =====
void loop(){
  uint32_t now = millis();

  if(notificationActive && notificationExpiry && now > notificationExpiry) {
    clearNotification();
  }

  if(statusActive && statusExpiry && now > statusExpiry) {
    statusActive = false;
    if(song.length() > 0 && !notificationActive) {
      drawBottomScroll();
    } else if(!notificationActive) {
      drawBottomStatic("Ready");
    }
  }

  if(!iosConnected) {
    if(bleScanner && !bleScanner->isScanning() && (now - lastConnectAttempt > 5000)) {
      startScan();
    }

    if(now - lastConnectAttempt > 60000) {
      Serial.println("No iOS connection for 60s — restarting...");
      showStatus("Restarting", 0);
      delay(1500);
      ESP.restart();
    }
  } else {
    lastConnectAttempt = now;
  }

  if(song.length() > 0 && !notificationActive && !statusActive) {
    drawBottomScroll();
  }

  smoothRead();
  bool btn = (digitalRead(JOY_SW_PIN) == LOW);
  if(btn && (now - tBtn > 280)){
    Serial.println("Button pressed");
    tBtn = now;
    sendAmsCommand(AMS_CMD_TOGGLE_PLAY_PAUSE);
    showStatus("Play/Pause", 800);
  }

  if(!inDZ(emaX) && beyond(emaX)){
    if(emaX>0 && now-tUp>VOL_REPEAT_MS){
      Serial.println("Joystick right");
      sendAmsCommand(AMS_CMD_VOLUME_UP);
      showStatus("Vol +", 600);
      tUp=now;
    } else if(emaX<0 && now-tDn>VOL_REPEAT_MS){
      Serial.println("Joystick left");
      sendAmsCommand(AMS_CMD_VOLUME_DOWN);
      showStatus("Vol -", 600);
      tDn=now;
    }
  }

  if(!inDZ(emaY) && beyond(emaY)){
    if(emaY>0 && now-tR>ACTION_COOLDOWN){
      Serial.println("Joystick down");
      sendAmsCommand(AMS_CMD_NEXT_TRACK);
      showStatus("Next >", 800);
      tR=now;
    } else if(emaY<0 && now-tL>ACTION_COOLDOWN){
      Serial.println("Joystick up");
      sendAmsCommand(AMS_CMD_PREVIOUS_TRACK);
      showStatus("< Prev", 800);
      tL=now;
    }
  }

  delay(6);
}
