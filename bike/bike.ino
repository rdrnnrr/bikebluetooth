#include <BleKeyboard.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <NimBLEDevice.h>
#include <NimBLEUtils.h>
#include <NimBLEConnInfo.h>
#include <nimble/nimble/host/include/host/ble_hs_adv.h>
#include <ctype.h>
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

// ===== DISPLAY BUSSES =====
TwoWire WireTop = TwoWire(0);
TwoWire WireBot = TwoWire(1);

Adafruit_SSD1306 oledTop(128, 32, &WireTop, -1);
Adafruit_SSD1306 oledBot(128, 32, &WireBot, -1);

// ===== BLE OBJECTS =====
BleKeyboard bleKeyboard("JuiceBox Remote", "JuiceBox", 100);

static NimBLEClient* pANCSClient = nullptr;
static NimBLERemoteCharacteristic* pNotificationSource = nullptr;
static NimBLERemoteCharacteristic* pControlPoint = nullptr;
static NimBLERemoteCharacteristic* pDataSource = nullptr;
static bool scanningForANCS = false;
static bool ancsReady = false;

// ===== ANCS CONSTANTS =====
static const NimBLEUUID ANCS_SERVICE_UUID("7905F431-B5CE-4E99-A40F-4B1E122D00D0");
static const NimBLEUUID ANCS_NOTIFICATION_SOURCE_UUID("9FBF120D-6301-42D9-8C58-25E699A21DBD");
static const NimBLEUUID ANCS_CONTROL_POINT_UUID("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9");
static const NimBLEUUID ANCS_DATA_SOURCE_UUID("22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB");

enum AncsAttributeId : uint8_t {
  ANCS_ATTR_ID_APP_IDENTIFIER = 0,
  ANCS_ATTR_ID_TITLE = 1,
  ANCS_ATTR_ID_SUBTITLE = 2,
  ANCS_ATTR_ID_MESSAGE = 3
};

enum AncsEventId : uint8_t {
  ANCS_EVENT_ADDED = 0,
  ANCS_EVENT_MODIFIED = 1,
  ANCS_EVENT_REMOVED = 2
};

// ===== JOYSTICK =====
const float EMA_ALPHA = 0.18f;
const int DEADZONE = 350;
const int TRIGGER_THRESH = 1200;
const uint16_t VOL_REPEAT_MS = 120;
const uint16_t ACTION_COOLDOWN = 220;
float emaX = 0, emaY = 0;
int midX = 2048, midY = 2048;
uint32_t tUp = 0, tDn = 0, tL = 0, tR = 0, tBtn = 0;

// ===== DISPLAY STATE =====
String artist = "";
String album = "";
String song = "";
uint32_t lastScroll = 0;
int scrollOffset = 0;
uint32_t lastConnectAttempt = 0;
bool lastKeyboardConnected = false;

// ===== ANCS NOTIFICATION STATE =====
struct NotificationContext {
  uint32_t uid = 0;
  uint8_t categoryId = 0xFF;
  bool inUse = false;
  bool requesting = false;
  String appId;
  String title;
  String subtitle;
  String message;

  void reset() {
    uid = 0;
    categoryId = 0xFF;
    inUse = false;
    requesting = false;
    appId = "";
    title = "";
    subtitle = "";
    message = "";
  }
};

static NotificationContext notificationSlots[4];
static uint8_t nextContextSlot = 0;
static uint32_t currentDisplayUid = 0;
static NimBLEAddress pendingAncsAddress;
static NimBLEAdvertisedDevice* pendingAncsDevice = nullptr;
static bool ancsConnectPending = false;
static bool displayResetPending = false;
static uint8_t ancsConnectRetries = 0;
static NimBLEAddress keyboardPeerAddress;
static bool keyboardPeerValid = false;

// ===== FORWARD DECLARATIONS =====
void startANCSScan();
bool connectToANCS(NimBLEAdvertisedDevice* device);
bool discoverANCS();
void requestNotificationAttributes(NotificationContext& ctx);
void updateDisplayFromContext(const NotificationContext& ctx);
void clearDisplayState();
NotificationContext* findContext(uint32_t uid);
NotificationContext* getOrAllocContext(uint32_t uid);
void releaseContext(NotificationContext& ctx);
String friendlyAppName(const String& appId);
String cleanDisplayText(const String& input);
void showBoot();
void showReady();
void drawTopInfo();
void drawBottomScroll();
void smoothRead();
void sendMedia(const uint8_t* key);

// ===== CALLBACKS =====
class ANCSClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* /*client*/) override {
    Serial.println("ANCS connected");
  }

  void onConnectFail(NimBLEClient* /*client*/, int reason) override {
    Serial.print("ANCS connect failed callback. reason=");
    Serial.print(reason);
    Serial.print(" (" );
    Serial.print(NimBLEUtils::returnCodeToString(reason));
    Serial.println(")");
  }

  void onDisconnect(NimBLEClient* /*client*/, int reason) override {
    Serial.printf("ANCS disconnected (reason %d)\n", reason);
    ancsReady = false;
    pNotificationSource = nullptr;
    pControlPoint = nullptr;
    pDataSource = nullptr;
    currentDisplayUid = 0;
    for (auto& ctx : notificationSlots) {
      ctx.reset();
    }
    pendingAncsAddress = NimBLEAddress();
    if (pendingAncsDevice) {
      delete pendingAncsDevice;
      pendingAncsDevice = nullptr;
    }
    ancsConnectPending = false;
    displayResetPending = true;
    scanningForANCS = false;
    ancsConnectRetries = 0;
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    if (!connInfo.isEncrypted()) {
      Serial.println("ANCS auth failed: encryption unavailable");
      if (pANCSClient) {
        pANCSClient->disconnect();
      }
    } else {
      Serial.println("ANCS link encrypted");
    }
  }
};

static ANCSClientCallbacks ancsClientCallbacks;

class ANCSScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    Serial.print("Advertised device: ");
    Serial.println(advertisedDevice->toString().c_str());
    uint8_t advType = advertisedDevice->getAdvType();
    uint8_t addrType = advertisedDevice->getAddressType();
    Serial.print("  addr type: ");
    Serial.print(addrType);
    Serial.print(", adv type: ");
    Serial.print(advType);
    Serial.print(", RSSI: ");
    Serial.println(advertisedDevice->getRSSI());

    if (advertisedDevice->getName() == "JuiceBox Remote") {
      return;
    }

    // Only consider connectable advertisements.
    if (!(advType == BLE_HCI_ADV_TYPE_ADV_IND || advType == BLE_HCI_ADV_TYPE_ADV_DIRECT_IND_HD)) {
      return;
    }

    bool isCandidate = advertisedDevice->isAdvertisingService(ANCS_SERVICE_UUID);

    if (!isCandidate) {
      std::string mfg = advertisedDevice->getManufacturerData();
      if (mfg.length() >= 2 && static_cast<uint8_t>(mfg[0]) == 0x4C && static_cast<uint8_t>(mfg[1]) == 0x00) {
        isCandidate = true;
      }
    }

    if (!isCandidate) {
      return;
    }

    NimBLEAddress foundAddress = advertisedDevice->getAddress();
    if (!pendingAncsAddress.isNull() && pendingAncsAddress == foundAddress) {
      return;
    }

    if (ancsConnectPending || (pANCSClient && pANCSClient->isConnected())) {
      return;
    }

    Serial.print("Found ANCS candidate: ");
    Serial.println(foundAddress.toString().c_str());

    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan && scan->isScanning()) {
      scan->stop();
    }

    scanningForANCS = false;
    if (pendingAncsDevice) {
      delete pendingAncsDevice;
      pendingAncsDevice = nullptr;
    }
    pendingAncsDevice = new NimBLEAdvertisedDevice(*advertisedDevice);
    pendingAncsAddress = foundAddress;
    ancsConnectPending = true;
    ancsConnectRetries = 0;
  }

  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    Serial.print("ANCS scan ended. reason = ");
    Serial.print(reason);
    Serial.print(", devices = ");
    Serial.println(results.getCount());
    scanningForANCS = false;
  }
};

static ANCSScanCallbacks ancsScanCallbacks;

// ===== ANCS NOTIFY HANDLERS =====
void notificationSourceHandler(NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool);
void dataSourceHandler(NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool);

// ===== UTILITIES =====
uint32_t readUint32(const uint8_t* data) {
  return data[0] |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

// ===== ANCS SETUP =====
void setupANCS() {
  if (!pANCSClient) {
    pANCSClient = NimBLEDevice::createClient();
    pANCSClient->setClientCallbacks(&ancsClientCallbacks, false);
    pANCSClient->setConnectionParams(12, 24, 0, 60);
    pANCSClient->setConnectTimeout(6);
  }

  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setMTU(247);

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&ancsScanCallbacks, true);
  scan->setInterval(80);
  scan->setWindow(40);
  scan->setActiveScan(true);

  startANCSScan();
}

void startANCSScan() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (!scan || scanningForANCS || (pANCSClient && pANCSClient->isConnected())) {
    return;
  }

  Serial.println("Scanning for ANCS...");
  scan->clearResults();
  bool started = scan->start(0, false);
  scanningForANCS = started;
  Serial.println(started ? "ANCS scan active" : "Failed to start ANCS scan");
}

bool connectToANCS(NimBLEAdvertisedDevice* device) {
  if (!pANCSClient || !device) {
    return false;
  }

  NimBLEAddress address = device->getAddress();
  Serial.print("Connecting to ANCS peripheral: ");
  Serial.println(address.toString().c_str());
  Serial.print("  address type: ");
  Serial.print(device->getAddressType());
  Serial.print(", adv type: ");
  Serial.print(device->getAdvType());
  Serial.print(", RSSI: ");
  Serial.println(device->getRSSI());

  if (!pANCSClient->isConnected()) {
    if (!pANCSClient->connect(device, false)) {
      int err = pANCSClient->getLastError();
      Serial.print("ANCS connect failed (connect). err=");
      Serial.print(err);
      Serial.print(" (" );
      Serial.print(NimBLEUtils::returnCodeToString(err));
      Serial.println(")");
      return false;
    }
  }

  if (!discoverANCS()) {
    Serial.println("ANCS discover failed");
    pANCSClient->disconnect();
    Serial.println("Disconnected after failed discovery");
    return false;
  }

  Serial.println("ANCS discover succeeded");
  return true;
}

bool discoverANCS() {
  if (!pANCSClient || !pANCSClient->isConnected()) {
    return false;
  }

  NimBLERemoteService* service = pANCSClient->getService(ANCS_SERVICE_UUID);
  if (!service) {
    Serial.println("ANCS service not found");
    return false;
  }

  pNotificationSource = service->getCharacteristic(ANCS_NOTIFICATION_SOURCE_UUID);
  pControlPoint = service->getCharacteristic(ANCS_CONTROL_POINT_UUID);
  pDataSource = service->getCharacteristic(ANCS_DATA_SOURCE_UUID);

  if (!pNotificationSource || !pControlPoint || !pDataSource) {
    Serial.println("ANCS characteristic missing");
    return false;
  }

  if (!pNotificationSource->subscribe(true, notificationSourceHandler)) {
    Serial.println("Failed to subscribe to Notification Source");
    return false;
  }

  if (!pDataSource->subscribe(true, dataSourceHandler)) {
    Serial.println("Failed to subscribe to Data Source");
    return false;
  }

  ancsReady = true;
  Serial.println("ANCS ready");
  return true;
}

// ===== NOTIFICATION HELPERS =====
NotificationContext* findContext(uint32_t uid) {
  for (auto& ctx : notificationSlots) {
    if (ctx.inUse && ctx.uid == uid) {
      return &ctx;
    }
  }
  return nullptr;
}

NotificationContext* getOrAllocContext(uint32_t uid) {
  if (NotificationContext* existing = findContext(uid)) {
    return existing;
  }

  for (auto& ctx : notificationSlots) {
    if (!ctx.inUse) {
      ctx.reset();
      ctx.inUse = true;
      ctx.uid = uid;
      return &ctx;
    }
  }

  NotificationContext& chosen = notificationSlots[nextContextSlot];
  nextContextSlot = (nextContextSlot + 1) % (sizeof(notificationSlots) / sizeof(notificationSlots[0]));
  chosen.reset();
  chosen.inUse = true;
  chosen.uid = uid;
  return &chosen;
}

void releaseContext(NotificationContext& ctx) {
  ctx.reset();
}

void requestNotificationAttributes(NotificationContext& ctx) {
  if (!pControlPoint || !ancsReady) {
    return;
  }

  uint8_t payload[16];
  size_t idx = 0;
  payload[idx++] = 0x00;  // Get Notification Attributes

  uint32_t uid = ctx.uid;
  payload[idx++] = static_cast<uint8_t>(uid & 0xFF);
  payload[idx++] = static_cast<uint8_t>((uid >> 8) & 0xFF);
  payload[idx++] = static_cast<uint8_t>((uid >> 16) & 0xFF);
  payload[idx++] = static_cast<uint8_t>((uid >> 24) & 0xFF);

  payload[idx++] = ANCS_ATTR_ID_APP_IDENTIFIER;

  payload[idx++] = ANCS_ATTR_ID_TITLE;
  payload[idx++] = 64;
  payload[idx++] = 0;

  payload[idx++] = ANCS_ATTR_ID_SUBTITLE;
  payload[idx++] = 64;
  payload[idx++] = 0;

  payload[idx++] = ANCS_ATTR_ID_MESSAGE;
  payload[idx++] = 128;
  payload[idx++] = 0;

  if (!pControlPoint->writeValue(payload, idx, true)) {
    Serial.println("Failed to write ANCS Control Point");
    return;
  }

  ctx.requesting = true;
}

void notificationSourceHandler(NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
  if (length < 8) {
    return;
  }

  uint8_t eventId = data[0];
  uint8_t categoryId = data[2];
  uint32_t uid = readUint32(&data[4]);

  if (eventId == ANCS_EVENT_REMOVED) {
    if (NotificationContext* ctx = findContext(uid)) {
      if (currentDisplayUid == uid) {
        clearDisplayState();
      }
      releaseContext(*ctx);
    }
    return;
  }

  NotificationContext* ctx = getOrAllocContext(uid);
  if (!ctx) {
    return;
  }

  ctx->categoryId = categoryId;
  if (eventId == ANCS_EVENT_ADDED || eventId == ANCS_EVENT_MODIFIED) {
    requestNotificationAttributes(*ctx);
  }
}

void dataSourceHandler(NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
  if (length < 5) {
    return;
  }

  size_t offset = 0;
  uint8_t commandId = data[offset++];
  if (commandId != 0x00) {
    return;
  }

  if (offset + 4 > length) {
    return;
  }

  uint32_t uid = readUint32(&data[offset]);
  offset += 4;

  NotificationContext* ctx = findContext(uid);
  if (!ctx) {
    return;
  }

  while (offset + 3 <= length) {
    uint8_t attrId = data[offset++];
    uint16_t attrLen = data[offset++];
    attrLen |= (static_cast<uint16_t>(data[offset++]) << 8);

    if (offset + attrLen > length) {
      break;
    }

    String value;
    if (attrLen > 0) {
      value.reserve(attrLen);
      for (uint16_t i = 0; i < attrLen; ++i) {
        value += static_cast<char>(data[offset + i]);
      }
      value = cleanDisplayText(value);
    }
    offset += attrLen;

    switch (attrId) {
      case ANCS_ATTR_ID_APP_IDENTIFIER:
        ctx->appId = value;
        break;
      case ANCS_ATTR_ID_TITLE:
        ctx->title = value;
        break;
      case ANCS_ATTR_ID_SUBTITLE:
        ctx->subtitle = value;
        break;
      case ANCS_ATTR_ID_MESSAGE:
        ctx->message = value;
        break;
      default:
        break;
    }
  }

  ctx->requesting = false;
  updateDisplayFromContext(*ctx);
}

// ===== DISPLAY HELPERS =====
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
  int16_t x1, y1;
  uint16_t w, h;
  oledBot.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  int x = (128 - w) / 2;
  int y = (32 - h) / 2;
  oledBot.setCursor(x, y);
  oledBot.println(txt);
  oledBot.display();
}

void drawBottomScroll() {
  oledBot.clearDisplay();
  oledBot.setTextColor(SSD1306_WHITE);
  oledBot.setTextSize(2);

  int16_t x1, y1;
  uint16_t w, h;
  oledBot.getTextBounds(song.c_str(), 0, 0, &x1, &y1, &w, &h);

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
    int x = (128 - w) / 2;
    oledBot.setCursor(x, 8);
    oledBot.print(song);
    oledBot.display();
  }
}

void showBoot() {
  artist = "";
  album = "";
  song = "";
  drawTopInfo();
  drawBottomStatic("Booting");
}

void showReady() {
  artist = "";
  album = "";
  song = "";
  drawTopInfo();
  drawBottomStatic("Ready");
}

void clearDisplayState() {
  artist = "";
  album = "";
  song = "";
  scrollOffset = 0;
  lastScroll = millis();
  drawTopInfo();
  drawBottomStatic("Ready");
  currentDisplayUid = 0;
}

void updateDisplayFromContext(const NotificationContext& ctx) {
  String appName = friendlyAppName(ctx.appId);
  String subtitle = ctx.subtitle.length() ? ctx.subtitle : ctx.title;
  String primary = ctx.message.length() ? ctx.message : (ctx.title.length() ? ctx.title : ctx.subtitle);

  artist = appName.length() ? appName : "Notification";
  album = subtitle.length() ? subtitle : "";
  song = primary.length() ? primary : artist;

  scrollOffset = 0;
  lastScroll = millis();
  drawTopInfo();
  drawBottomScroll();
  currentDisplayUid = ctx.uid;
}

// ===== INPUT HELPERS =====
inline bool inDZ(float v) { return fabs(v) < DEADZONE; }
inline bool beyond(float v) { return fabs(v) >= TRIGGER_THRESH; }

void calibrateCenter() {
  long sx = 0, sy = 0;
  for (int i = 0; i < 24; i++) {
    sx += analogRead(JOY_X_PIN);
    sy += analogRead(JOY_Y_PIN);
    delay(4);
  }
  midX = sx / 24;
  midY = sy / 24;
  emaX = emaY = 0;
}

void smoothRead() {
  int rx = analogRead(JOY_X_PIN);
  int ry = analogRead(JOY_Y_PIN);
  emaX = (1.0f - EMA_ALPHA) * emaX + EMA_ALPHA * (rx - midX);
  emaY = (1.0f - EMA_ALPHA) * emaY + EMA_ALPHA * (ry - midY);
}

void sendMedia(const uint8_t* key) {
  if (bleKeyboard.isConnected()) {
    bleKeyboard.write(key);
  }
}

String friendlyAppName(const String& appId) {
  if (appId.length() == 0) return "";

  if (appId.indexOf("whatsapp") >= 0 || appId.indexOf("WhatsApp") >= 0) return "WhatsApp";
  if (appId.indexOf("mobilephone") >= 0) return "Phone";
  if (appId.indexOf("facetime") >= 0) return "FaceTime";
  if (appId.indexOf("youtubemusic") >= 0) return "YouTube Music";
  if (appId.indexOf("apple.Music") >= 0) return "Apple Music";
  if (appId.indexOf("messages") >= 0) return "Messages";

  int lastDot = appId.lastIndexOf('.');
  if (lastDot >= 0 && lastDot < static_cast<int>(appId.length() - 1)) {
    String tail = appId.substring(lastDot + 1);
    if (tail.length() > 0) {
      tail[0] = toupper(tail[0]);
      for (size_t i = 1; i < tail.length(); ++i) {
        if (tail[i] == '-') {
          tail[i] = ' ';
        }
      }
      return tail;
    }
  }

  return appId;
}

String cleanDisplayText(const String& input) {
  String cleaned;
  cleaned.reserve(input.length());

  for (uint16_t i = 0; i < input.length();) {
    uint8_t byte = static_cast<uint8_t>(input.charAt(i));

    if (byte == '\r' || byte == '\n') {
      ++i;
      continue;
    }

    if (byte == '\t') {
      cleaned += ' ';
      ++i;
      continue;
    }

    if (byte >= 32 && byte <= 126) {
      cleaned += static_cast<char>(byte);
      ++i;
      continue;
    }

    if (byte >= 0x80) {
      cleaned += '?';
      ++i;
      while (i < input.length()) {
        uint8_t next = static_cast<uint8_t>(input.charAt(i));
        if ((next & 0xC0) == 0x80) {
          ++i;
        } else {
          break;
        }
      }
      continue;
    }

    ++i;
  }

  cleaned.trim();
  return cleaned;
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  pinMode(JOY_SW_PIN, INPUT_PULLUP);
  analogReadResolution(12);
  WireTop.begin(TOP_SDA, TOP_SCL);
  WireBot.begin(BOT_SDA, BOT_SCL);

  oledTop.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  oledBot.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);

  showBoot();

  bool clearBonds = false;
  if (digitalRead(JOY_SW_PIN) == LOW) {
    uint32_t pressedAt = millis();
    while (digitalRead(JOY_SW_PIN) == LOW) {
      if (millis() - pressedAt > 2000) {
        clearBonds = true;
        break;
      }
      delay(20);
    }
    if (clearBonds) {
      Serial.println("Joystick held — clearing stored BLE bonds");
      drawBottomStatic("Clr Bonds");
    }
  }

  NimBLEDevice::init("JuiceBox Remote");
  NimBLEDevice::setSecurityAuth(false, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);

  if (clearBonds) {
    if (NimBLEDevice::deleteAllBonds()) {
      Serial.println("All NimBLE bonds cleared");
    } else {
      Serial.println("Failed to clear NimBLE bonds");
    }
    delay(400);
  }

  bleKeyboard.begin();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (adv) {
    adv->setName("JuiceBox Remote");
    adv->setAppearance(0x0080);
    adv->setPreferredParams(0x06, 0x12);
    adv->addServiceUUID(NimBLEUUID((uint16_t)0x1812));
    adv->enableScanResponse(true);
    adv->start();
  }

  setupANCS();

  calibrateCenter();
  showReady();
  lastConnectAttempt = millis();
}

// ===== LOOP =====
void loop() {
  uint32_t now = millis();

  if (displayResetPending) {
    clearDisplayState();
    displayResetPending = false;
  }

  NimBLEScan* scan = NimBLEDevice::getScan();
  bool ancsConnected = (pANCSClient && pANCSClient->isConnected());
  if (!ancsConnected && !ancsConnectPending && !scanningForANCS) {
    startANCSScan();
  }

  bool kbConnected = bleKeyboard.isConnected();

  if (ancsConnectPending && !ancsConnected) {
    if (pendingAncsDevice) {
      if (connectToANCS(pendingAncsDevice)) {
        ancsConnected = (pANCSClient && pANCSClient->isConnected());
        ancsConnectPending = false;
        pendingAncsAddress = NimBLEAddress();
        delete pendingAncsDevice;
        pendingAncsDevice = nullptr;
        ancsConnectRetries = 0;
      } else {
        ancsConnectRetries++;
        if (ancsConnectRetries >= 3) {
          ancsConnectPending = false;
          pendingAncsAddress = NimBLEAddress();
          delete pendingAncsDevice;
          pendingAncsDevice = nullptr;
          if (scan) {
            scan->clearResults();
          }
        } else {
          delay(100);
        }
      }
    } else {
      ancsConnectPending = false;
    }
  }

  bool anyConnection = kbConnected || ancsConnected;
  if (!anyConnection && (now - lastConnectAttempt > 30000)) {
    Serial.println("No BLE connection after 30s — restarting...");
    drawBottomStatic("Restarting");
    delay(1500);
    ESP.restart();
  }

  if (anyConnection) {
    lastConnectAttempt = now;
  }

  if (kbConnected != lastKeyboardConnected) {
    Serial.print("Keyboard ");
    Serial.println(kbConnected ? "connected" : "disconnected");
    lastKeyboardConnected = kbConnected;

    if (kbConnected) {
      NimBLEServer* server = NimBLEDevice::getServer();
      if (server) {
        NimBLEConnInfo info = server->getPeerInfo(0);
        NimBLEAddress addr = info.getAddress();
        if (!addr.isNull()) {
          keyboardPeerAddress = addr;
          keyboardPeerValid = true;
          Serial.print("Keyboard peer address captured: ");
          Serial.print(addr.toString().c_str());
          Serial.print(" type=");
          Serial.println(addr.getType());
        }
      }
    } else {
      keyboardPeerValid = false;
      keyboardPeerAddress = NimBLEAddress();
    }
  }

  if (song.length() > 0 && anyConnection) {
    drawBottomScroll();
  }

  smoothRead();
  bool btn = (digitalRead(JOY_SW_PIN) == LOW);
  if (btn && (now - tBtn > 280)) {
    tBtn = now;
    sendMedia(KEY_MEDIA_PLAY_PAUSE);
    drawBottomStatic("Play");
  }

  if (!inDZ(emaX) && beyond(emaX)) {
    if (emaX > 0 && now - tUp > VOL_REPEAT_MS) {
      sendMedia(KEY_MEDIA_VOLUME_UP);
      drawBottomStatic("Vol +");
      tUp = now;
    } else if (emaX < 0 && now - tDn > VOL_REPEAT_MS) {
      sendMedia(KEY_MEDIA_VOLUME_DOWN);
      drawBottomStatic("Vol -");
      tDn = now;
    }
  }

  if (!inDZ(emaY) && beyond(emaY)) {
    if (emaY > 0 && now - tR > ACTION_COOLDOWN) {
      sendMedia(KEY_MEDIA_NEXT_TRACK);
      drawBottomStatic("Next >");
      tR = now;
    } else if (emaY < 0 && now - tL > ACTION_COOLDOWN) {
      sendMedia(KEY_MEDIA_PREVIOUS_TRACK);
      drawBottomStatic("< Prev");
      tL = now;
    }
  }

  delay(6);
}
