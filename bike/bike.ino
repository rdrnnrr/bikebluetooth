#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_gap_ble_api.h>
#include <nimble/ble.h>

#include <array>
#include <cstring>
#include <math.h>

// ===== DISPLAY CONSTANTS =====
constexpr uint8_t LEFT_SDA = 21;
constexpr uint8_t LEFT_SCL = 22;
constexpr uint8_t RIGHT_SDA = 25;
constexpr uint8_t RIGHT_SCL = 26;
constexpr uint8_t DISPLAY_ADDR = 0x3C;

constexpr uint16_t HALF_WIDTH = 128;
constexpr uint16_t DISPLAY_HEIGHT = 64;
constexpr uint16_t FULL_WIDTH = 256;

TwoWire WireLeft = TwoWire(0);
TwoWire WireRight = TwoWire(1);
Adafruit_SSD1306 oledLeft(HALF_WIDTH, DISPLAY_HEIGHT, &WireLeft, -1);
Adafruit_SSD1306 oledRight(HALF_WIDTH, DISPLAY_HEIGHT, &WireRight, -1);
GFXcanvas1 canvas(FULL_WIDTH, DISPLAY_HEIGHT);

std::array<uint8_t, HALF_WIDTH * DISPLAY_HEIGHT / 8> leftBuffer{};
std::array<uint8_t, HALF_WIDTH * DISPLAY_HEIGHT / 8> rightBuffer{};

// ===== INPUT PINS =====
constexpr uint8_t JOY_X_PIN = 34;
constexpr uint8_t JOY_Y_PIN = 35;
constexpr uint8_t JOY_SW_PIN = 32;

// ===== BLE CONSTANTS =====
static const NimBLEUUID ANCS_SERVICE_UUID("7905F431-B5CE-4E99-A40F-4B1E122D00D0");
static const NimBLEUUID ANCS_NOTIFICATION_SOURCE_UUID("9FBF120D-6301-42D9-8C58-25E699A21DBD");
static const NimBLEUUID ANCS_CONTROL_POINT_UUID("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9");
static const NimBLEUUID ANCS_DATA_SOURCE_UUID("22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB");

static const NimBLEUUID AMS_SERVICE_UUID("89D3502B-0F36-433A-8EF4-C502AD55F8DC");
static const NimBLEUUID AMS_REMOTE_COMMAND_UUID("9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2");
static const NimBLEUUID AMS_ENTITY_UPDATE_UUID("2F7CABCE-808D-411F-9A0C-BB92BA96C102");
static const NimBLEUUID AMS_ENTITY_ATTRIBUTE_UUID("C6B2F38C-23AB-46D8-A6AB-A3A870BBD5D7");

enum class AmsRemoteCommand : uint8_t {
  Play = 0,
  Pause = 1,
  TogglePlayPause = 2,
  NextTrack = 3,
  PreviousTrack = 4,
  VolumeUp = 5,
  VolumeDown = 6
};

enum class AmsEntityId : uint8_t {
  Player = 0,
  Queue = 1,
  Track = 2
};

enum class AmsTrackAttribute : uint8_t {
  Artist = 0,
  Album = 1,
  Title = 2,
  Duration = 3
};

enum class AmsPlayerAttribute : uint8_t {
  Name = 0,
  PlaybackInfo = 1,
  Volume = 2
};

enum class AncsCategory : uint8_t {
  Other = 0,
  IncomingCall = 1,
  MissedCall = 2,
  Voicemail = 3,
  Social = 4,
  Schedule = 5,
  Email = 6,
  News = 7,
  Health = 8,
  Finance = 9,
  Location = 10,
  Entertainment = 11
};

// ===== STATE STRUCTURES =====
struct TrackInfo {
  String title;
  String artist;
  String album;
  String appName;
};

struct NotificationState {
  bool active = false;
  bool isCall = false;
  bool isWhatsApp = false;
  uint32_t uid = 0;
  AncsCategory category = AncsCategory::Other;
  String appIdentifier;
  String title;
  String message;
};

// ===== GLOBAL STATE =====
TrackInfo currentTrack;
NotificationState currentNotification;

bool iosConnected = false;
bool linkEncrypted = false;
bool amsReady = false;
bool ancsReady = false;
bool pendingServiceInit = false;

uint32_t amsCommandMask = 0;

uint32_t notificationExpiry = 0;
bool statusActive = false;
uint32_t statusExpiry = 0;
String statusMessage;

// Joystick state
constexpr float EMA_ALPHA = 0.18f;
constexpr int DEADZONE = 350;
constexpr int TRIGGER_THRESH = 1200;
constexpr uint16_t VOL_REPEAT_MS = 120;
constexpr uint16_t ACTION_COOLDOWN = 220;
constexpr uint32_t BOND_CLEAR_HOLD_MS = 3000;

float emaX = 0;
float emaY = 0;
int midX = 2048;
int midY = 2048;
uint32_t tUp = 0;
uint32_t tDn = 0;
uint32_t tL = 0;
uint32_t tR = 0;
uint32_t tBtn = 0;
bool btnWasDown = false;
uint32_t btnDownAt = 0;

// BLE objects
constexpr uint16_t INVALID_CONN_HANDLE = 0xFFFF;

NimBLEServer* gServer = nullptr;
NimBLEClient* gClient = nullptr;
NimBLEAddress* connectedAddress = nullptr;
bool hasConnectedAddress = false;
uint16_t serverConnHandle = INVALID_CONN_HANDLE;

NimBLERemoteCharacteristic* ancsNotificationSource = nullptr;
NimBLERemoteCharacteristic* ancsControlPoint = nullptr;
NimBLERemoteCharacteristic* ancsDataSource = nullptr;

NimBLERemoteCharacteristic* amsRemoteCommand = nullptr;
NimBLERemoteCharacteristic* amsEntityUpdate = nullptr;
NimBLERemoteCharacteristic* amsEntityAttribute = nullptr;

// ===== HELPER FUNCTIONS =====
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

String ellipsize(const String& text, size_t maxChars) {
  if (text.length() <= maxChars) {
    return text;
  }
  if (maxChars <= 3) {
    return text.substring(0, maxChars);
  }
  return text.substring(0, maxChars - 3) + "...";
}

String friendlyAppName(const String& identifier) {
  if (identifier.equalsIgnoreCase("com.whatsapp")) {
    return "WhatsApp";
  }
  if (identifier.equalsIgnoreCase("com.apple.mobilephone")) {
    return "Phone";
  }
  if (identifier.equalsIgnoreCase("com.google.ios.youtubemusic")) {
    return "YouTube Music";
  }
  if (identifier.equalsIgnoreCase("com.apple.MobileSMS")) {
    return "Messages";
  }
  if (identifier.length() == 0) {
    return "Notification";
  }
  return identifier;
}

void flushDisplays() {
  const uint8_t* src = canvas.getBuffer();
  const size_t rowStride = FULL_WIDTH / 8; // 32 bytes per row
  const size_t halfStride = HALF_WIDTH / 8; // 16 bytes per row
  for (uint16_t y = 0; y < DISPLAY_HEIGHT; ++y) {
    memcpy(&leftBuffer[y * halfStride], src + y * rowStride, halfStride);
    memcpy(&rightBuffer[y * halfStride], src + y * rowStride + halfStride, halfStride);
  }

  oledLeft.clearDisplay();
  oledLeft.drawBitmap(0, 0, leftBuffer.data(), HALF_WIDTH, DISPLAY_HEIGHT, SSD1306_WHITE);
  oledLeft.display();

  oledRight.clearDisplay();
  oledRight.drawBitmap(0, 0, rightBuffer.data(), HALF_WIDTH, DISPLAY_HEIGHT, SSD1306_WHITE);
  oledRight.display();
}

void renderUI() {
  canvas.fillScreen(0);
  canvas.setTextWrap(false);
  canvas.setTextColor(1);

  // Status line
  canvas.setTextSize(1);
  canvas.setCursor(0, 0);
  if (iosConnected) {
    canvas.print("iPhone: ");
    if (!linkEncrypted) {
      canvas.print("Pairing...");
    } else if (ancsReady && amsReady) {
      canvas.print("Ready");
    } else {
      canvas.print("Securing...");
    }
  } else {
    canvas.print("Waiting for iPhone...");
  }

  canvas.setCursor(150, 0);
  canvas.print(amsReady ? "AMS" : "---");
  canvas.setCursor(190, 0);
  canvas.print(ancsReady ? "ANCS" : "----");

  // Left half: track info
  canvas.setCursor(0, 12);
  canvas.setTextSize(2);
  if (currentTrack.title.length() > 0) {
    canvas.print(ellipsize(currentTrack.title, 24));
  } else {
    canvas.print("No Track");
  }

  canvas.setTextSize(1);
  canvas.setCursor(0, 38);
  if (currentTrack.artist.length() > 0) {
    canvas.print(ellipsize(currentTrack.artist, 40));
  } else {
    canvas.print("Artist unknown");
  }

  canvas.setCursor(0, 50);
  if (currentTrack.album.length() > 0) {
    canvas.print(ellipsize(currentTrack.album, 40));
  } else if (currentTrack.appName.length() > 0) {
    canvas.print(ellipsize(currentTrack.appName, 40));
  } else {
    canvas.print("Album unknown");
  }

  // Separator line
  canvas.drawFastVLine(HALF_WIDTH, 0, DISPLAY_HEIGHT, 1);

  // Right half: notifications / status
  canvas.setTextSize(1);
  if (currentNotification.active) {
    canvas.setCursor(HALF_WIDTH + 6, 10);
    if (currentNotification.isCall) {
      canvas.print("Incoming Call");
    } else if (currentNotification.isWhatsApp) {
      canvas.print("WhatsApp");
    } else {
      canvas.print(ellipsize(friendlyAppName(currentNotification.appIdentifier), 20));
    }

    canvas.setCursor(HALF_WIDTH + 6, 24);
    canvas.print(ellipsize(currentNotification.title, 20));

    canvas.setCursor(HALF_WIDTH + 6, 38);
    canvas.print(ellipsize(currentNotification.message, 20));
  } else if (statusActive) {
    canvas.setCursor(HALF_WIDTH + 6, 26);
    canvas.print(ellipsize(statusMessage, 21));
  } else {
    canvas.setCursor(HALF_WIDTH + 6, 16);
    canvas.print("Joystick controls:");
    canvas.setCursor(HALF_WIDTH + 6, 28);
    canvas.print("Press = Play/Pause");
    canvas.setCursor(HALF_WIDTH + 6, 40);
    canvas.print("Left/Right = Track");
    canvas.setCursor(HALF_WIDTH + 6, 52);
    canvas.print("Up/Down = Volume");
  }

  flushDisplays();
}

void showStatus(const String& message, uint32_t durationMs) {
  statusMessage = message;
  statusActive = true;
  statusExpiry = durationMs ? millis() + durationMs : 0;
  renderUI();
}

void clearStatus() {
  statusMessage = "";
  statusActive = false;
  statusExpiry = 0;
  renderUI();
}

void showNotification(const NotificationState& notification, uint32_t durationMs) {
  currentNotification = notification;
  currentNotification.active = true;
  notificationExpiry = durationMs ? millis() + durationMs : 0;
  statusActive = false;
  renderUI();
}

void clearNotification(uint32_t uid = 0) {
  if (uid == 0 || (currentNotification.active && currentNotification.uid == uid)) {
    currentNotification = NotificationState();
    notificationExpiry = 0;
    renderUI();
  }
}

// ===== JOYSTICK HELPERS =====
void calibrateCenter() {
  long sx = 0;
  long sy = 0;
  for (int i = 0; i < 24; ++i) {
    sx += analogRead(JOY_X_PIN);
    sy += analogRead(JOY_Y_PIN);
    delay(4);
  }
  midX = sx / 24;
  midY = sy / 24;
  emaX = 0;
  emaY = 0;
}

void smoothRead() {
  int rx = analogRead(JOY_X_PIN);
  int ry = analogRead(JOY_Y_PIN);
  emaX = (1.0f - EMA_ALPHA) * emaX + EMA_ALPHA * (rx - midX);
  emaY = (1.0f - EMA_ALPHA) * emaY + EMA_ALPHA * (ry - midY);
}

bool inDeadzone(float val) { return fabsf(val) < DEADZONE; }
bool beyondThreshold(float val) { return fabsf(val) > TRIGGER_THRESH; }

// ===== AMS / ANCS HELPERS =====
void requestAncsAttributes(uint32_t uid);
void handleAncsData(const uint8_t* data, size_t length);
void handleAncsNotificationSource(uint8_t* data, size_t length);
void handleAmsEntityUpdate(uint8_t* data, size_t length);
void handleAmsRemoteCommandUpdate(uint8_t* data, size_t length);
void subscribeAmsTrackAttributes();
bool connectClientIfNeeded();
bool setupAncs();
bool setupAms();
void resetClientState();

// ===== BLE CALLBACKS =====
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
    iosConnected = true;
    linkEncrypted = false;
    serverConnHandle = desc->conn_handle;
    if (connectedAddress) {
      delete connectedAddress;
      connectedAddress = nullptr;
    }
    connectedAddress = new NimBLEAddress(desc->peer_ota_addr);
    hasConnectedAddress = true;
    server->getAdvertising()->stop();
    NimBLEDevice::startSecurity(desc->conn_handle);
    showStatus("Pairing...", 1500);
    renderUI();
  }

  void onDisconnect(NimBLEServer* server) override {
    iosConnected = false;
    linkEncrypted = false;
    hasConnectedAddress = false;
    serverConnHandle = INVALID_CONN_HANDLE;
    if (connectedAddress) {
      delete connectedAddress;
      connectedAddress = nullptr;
    }
    resetClientState();
    clearNotification();
    showStatus("Disconnected", 1500);
    server->getAdvertising()->start();
    renderUI();
  }
};

class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient*) override {
    Serial.println("Client connected to iPhone");
  }

  void onDisconnect(NimBLEClient*) override {
    Serial.println("Client disconnected from iPhone");
    amsReady = false;
    ancsReady = false;
    amsCommandMask = 0;
    ancsNotificationSource = nullptr;
    ancsControlPoint = nullptr;
    ancsDataSource = nullptr;
    amsRemoteCommand = nullptr;
    amsEntityUpdate = nullptr;
    amsEntityAttribute = nullptr;
    renderUI();
  }
};

ClientCallbacks gClientCallbacks;

// ===== AMS / ANCS IMPLEMENTATION =====
void handleAncsNotificationSource(uint8_t* data, size_t length) {
  if (length < 8) {
    return;
  }

  uint8_t eventId = data[0];
  uint8_t category = data[2];
  uint32_t uid = data[4] | (data[5] << 8) | (data[6] << 16) | (data[7] << 24);

  if (eventId == 2) { // removed
    clearNotification(uid);
    return;
  }

  NotificationState next;
  next.uid = uid;
  next.category = static_cast<AncsCategory>(category);
  next.isCall = (category == static_cast<uint8_t>(AncsCategory::IncomingCall));
  next.isWhatsApp = false;
  currentNotification = next;
  currentNotification.active = false;
  requestAncsAttributes(uid);
}

void requestAncsAttributes(uint32_t uid) {
  if (!ancsControlPoint) {
    return;
  }

  uint8_t buffer[8];
  buffer[0] = 0x00; // Get Notification Attributes
  buffer[1] = uid & 0xFF;
  buffer[2] = (uid >> 8) & 0xFF;
  buffer[3] = (uid >> 16) & 0xFF;
  buffer[4] = (uid >> 24) & 0xFF;

  // Request App Identifier
  buffer[5] = 0x00;
  ancsControlPoint->writeValue(buffer, 6, true);

  // Request Title (max 80 chars)
  buffer[5] = 0x01;
  buffer[6] = 80;
  buffer[7] = 0;
  ancsControlPoint->writeValue(buffer, 8, true);

  // Request Message (max 120 chars)
  buffer[5] = 0x03;
  buffer[6] = 120;
  buffer[7] = 0;
  ancsControlPoint->writeValue(buffer, 8, true);
}

void handleAncsData(const uint8_t* data, size_t length) {
  if (length < 5) {
    return;
  }

  uint8_t commandId = data[0];
  if (commandId != 0) {
    return;
  }

  uint32_t uid = data[1] | (data[2] << 8) | (data[3] << 16) | (data[4] << 24);
  if (!currentNotification.active && currentNotification.uid != uid) {
    // We are waiting for this notification's payload
    currentNotification.uid = uid;
  }

  size_t index = 5;
  while (index + 2 < length) {
    uint8_t attributeId = data[index++];
    uint16_t valueLen = data[index] | (data[index + 1] << 8);
    index += 2;
    if (index + valueLen > length) {
      valueLen = length - index;
    }
    String value;
    value.reserve(valueLen);
    for (uint16_t i = 0; i < valueLen; ++i) {
      value += static_cast<char>(data[index + i]);
    }
    index += valueLen;
    value = cleanDisplayText(value);

    switch (attributeId) {
      case 0x00:
        currentNotification.appIdentifier = value;
        currentNotification.isWhatsApp = value.equalsIgnoreCase("com.whatsapp");
        break;
      case 0x01:
        currentNotification.title = value;
        break;
      case 0x03:
        currentNotification.message = value;
        break;
      default:
        break;
    }
  }

  if (currentNotification.appIdentifier.length() > 0 || currentNotification.title.length() > 0 || currentNotification.message.length() > 0) {
    NotificationState toDisplay = currentNotification;
    if (toDisplay.isCall) {
      if (toDisplay.title.length() == 0 && toDisplay.message.length() > 0) {
        toDisplay.title = toDisplay.message;
        toDisplay.message = "";
      }
      showNotification(toDisplay, 0);
    } else {
      uint32_t duration = toDisplay.isWhatsApp ? 15000 : 10000;
      showNotification(toDisplay, duration);
    }
  }
}

void handleAmsEntityUpdate(uint8_t* data, size_t length) {
  if (length < 3) {
    return;
  }
  uint8_t entityId = data[0];
  uint8_t attributeId = data[1];
  String value;
  value.reserve(length - 3);
  for (size_t i = 3; i < length; ++i) {
    value += static_cast<char>(data[i]);
  }
  value = cleanDisplayText(value);

  if (entityId == static_cast<uint8_t>(AmsEntityId::Track)) {
    switch (attributeId) {
      case static_cast<uint8_t>(AmsTrackAttribute::Title):
        if (currentTrack.title != value) {
          currentTrack.title = value;
          renderUI();
        }
        break;
      case static_cast<uint8_t>(AmsTrackAttribute::Artist):
        if (currentTrack.artist != value) {
          currentTrack.artist = value;
          renderUI();
        }
        break;
      case static_cast<uint8_t>(AmsTrackAttribute::Album):
        if (currentTrack.album != value) {
          currentTrack.album = value;
          renderUI();
        }
        break;
      default:
        break;
    }
  } else if (entityId == static_cast<uint8_t>(AmsEntityId::Player)) {
    if (attributeId == static_cast<uint8_t>(AmsPlayerAttribute::Name)) {
      if (currentTrack.appName != value) {
        currentTrack.appName = value;
        renderUI();
      }
    } else if (attributeId == static_cast<uint8_t>(AmsPlayerAttribute::PlaybackInfo)) {
      if (value.length() > 0) {
        int comma = value.indexOf(',');
        int state = comma > 0 ? value.substring(0, comma).toInt() : value.toInt();
        switch (state) {
          case 0:
            showStatus("Paused", 1000);
            break;
          case 1:
            showStatus("Playing", 1000);
            break;
          default:
            break;
        }
      }
    }
  }
}

void handleAmsRemoteCommandUpdate(uint8_t* data, size_t length) {
  amsCommandMask = 0;
  for (size_t i = 0; i < length; ++i) {
    if (data[i] < 32) {
      amsCommandMask |= (1UL << data[i]);
    }
  }
}

void subscribeAmsTrackAttributes() {
  if (!amsEntityUpdate) {
    return;
  }
  const uint8_t trackAttrs[] = {
      static_cast<uint8_t>(AmsEntityId::Track),
      static_cast<uint8_t>(AmsTrackAttribute::Artist),
      static_cast<uint8_t>(AmsTrackAttribute::Album),
      static_cast<uint8_t>(AmsTrackAttribute::Title)};
  amsEntityUpdate->writeValue(trackAttrs, sizeof(trackAttrs), true);

  const uint8_t playerAttrs[] = {
      static_cast<uint8_t>(AmsEntityId::Player),
      static_cast<uint8_t>(AmsPlayerAttribute::Name),
      static_cast<uint8_t>(AmsPlayerAttribute::PlaybackInfo)};
  amsEntityUpdate->writeValue(playerAttrs, sizeof(playerAttrs), true);
}

bool setupAncs() {
  NimBLERemoteService* service = gClient->getService(ANCS_SERVICE_UUID);
  if (!service) {
    Serial.println("ANCS service not found");
    return false;
  }
  ancsNotificationSource = service->getCharacteristic(ANCS_NOTIFICATION_SOURCE_UUID);
  ancsControlPoint = service->getCharacteristic(ANCS_CONTROL_POINT_UUID);
  ancsDataSource = service->getCharacteristic(ANCS_DATA_SOURCE_UUID);
  if (!ancsNotificationSource || !ancsControlPoint || !ancsDataSource) {
    Serial.println("ANCS characteristics missing");
    return false;
  }

  if (!ancsNotificationSource->canNotify() ||
      !ancsNotificationSource->subscribe(true, [](NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
        handleAncsNotificationSource(data, len);
      })) {
    Serial.println("Failed to subscribe to ANCS notification source");
    return false;
  }

  if (!ancsDataSource->canNotify() ||
      !ancsDataSource->subscribe(true, [](NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
        handleAncsData(data, len);
      })) {
    Serial.println("Failed to subscribe to ANCS data source");
    return false;
  }

  ancsReady = true;
  renderUI();
  return true;
}

bool setupAms() {
  NimBLERemoteService* service = gClient->getService(AMS_SERVICE_UUID);
  if (!service) {
    Serial.println("AMS service not found");
    return false;
  }
  amsRemoteCommand = service->getCharacteristic(AMS_REMOTE_COMMAND_UUID);
  amsEntityUpdate = service->getCharacteristic(AMS_ENTITY_UPDATE_UUID);
  amsEntityAttribute = service->getCharacteristic(AMS_ENTITY_ATTRIBUTE_UUID);
  if (!amsRemoteCommand || !amsEntityUpdate || !amsEntityAttribute) {
    Serial.println("AMS characteristics missing");
    return false;
  }

  if (!amsEntityUpdate->canNotify() ||
      !amsEntityUpdate->subscribe(true, [](NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
        handleAmsEntityUpdate(data, len);
      })) {
    Serial.println("Failed to subscribe to AMS entity update");
    return false;
  }

  if (amsRemoteCommand->canNotify()) {
    amsRemoteCommand->subscribe(true, [](NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
      handleAmsRemoteCommandUpdate(data, len);
    });
  }

  subscribeAmsTrackAttributes();
  amsReady = true;
  renderUI();
  return true;
}

bool connectClientIfNeeded() {
  if (!hasConnectedAddress || connectedAddress == nullptr) {
    return false;
  }
  if (gClient == nullptr) {
    gClient = NimBLEDevice::createClient();
    gClient->setClientCallbacks(&gClientCallbacks, false);
    gClient->setConnectionParams(24, 40, 0, 100);
    gClient->setConnectTimeout(10);
  }
  if (gClient->isConnected()) {
    return true;
  }
  Serial.println("Connecting back to iPhone as client...");
  if (!gClient->connect(*connectedAddress, false)) {
    Serial.println("Client connection failed");
    return false;
  }
  Serial.println("Client connection successful");
  return true;
}

bool initializeServices() {
  if (!connectClientIfNeeded()) {
    return false;
  }
  bool ok = true;
  ok &= setupAncs();
  ok &= setupAms();
  if (gClient) {
    gClient->updateConnParams(24, 40, 0, 100);
  }
  return ok;
}

void resetClientState() {
  if (gClient) {
    if (gClient->isConnected()) {
      gClient->disconnect();
    }
    // Keep the client object for faster reconnection
  }
  amsReady = false;
  ancsReady = false;
  amsCommandMask = 0;
  ancsNotificationSource = nullptr;
  ancsControlPoint = nullptr;
  ancsDataSource = nullptr;
  amsRemoteCommand = nullptr;
  amsEntityUpdate = nullptr;
  amsEntityAttribute = nullptr;
}

// ===== BLE GAP HANDLER =====
int gapEventHandler(struct ble_gap_event* event, void*) {
  if (!event) {
    return 0;
  }
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      Serial.printf("GAP connect status=%d\n", event->connect.status);
      if (event->connect.status != 0) {
        iosConnected = false;
        renderUI();
      }
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      Serial.printf("GAP disconnect reason=0x%02X\n", event->disconnect.reason);
      iosConnected = false;
      linkEncrypted = false;
      pendingServiceInit = false;
      resetClientState();
      NimBLEDevice::getAdvertising()->start();
      renderUI();
      break;
    case BLE_GAP_EVENT_ENC_CHANGE:
      Serial.printf("Encryption change status=%d\n", event->enc_change.status);
      if (event->enc_change.status == 0) {
        linkEncrypted = true;
        pendingServiceInit = true;
        renderUI();
      } else {
        linkEncrypted = false;
        if (gServer) {
          gServer->disconnect(event->enc_change.conn_handle);
        }
      }
      break;
    case BLE_GAP_EVENT_CONN_UPDATE:
      Serial.printf("Conn update status=%d interval=%u latency=%u timeout=%u\n",
                    event->conn_update.status,
                    event->conn_update.conn_itvl,
                    event->conn_update.conn_latency,
                    event->conn_update.supervision_timeout);
      break;
    case BLE_GAP_EVENT_IDENTITY_RESOLVED:
      Serial.println("Identity resolved");
      break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
      Serial.println("Repeat pairing requested, clearing old bond");
      NimBLEDevice::deleteBond(NimBLEAddress(event->repeat_pairing.peer_addr));
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    default:
      break;
  }
  return 0;
}

// ===== AMS COMMAND =====
bool sendAmsCommand(AmsRemoteCommand command, const char* label) {
  if (!amsRemoteCommand || !amsReady) {
    return false;
  }
  uint8_t cmd = static_cast<uint8_t>(command);
  if (amsCommandMask != 0 && (amsCommandMask & (1UL << cmd)) == 0) {
    Serial.printf("Command %u not allowed\n", cmd);
    return false;
  }
  bool ok = amsRemoteCommand->writeValue(&cmd, 1, true);
  if (ok && label) {
    showStatus(label, 800);
  }
  return ok;
}

// ===== SETUP UI =====
void showBoot() {
  canvas.fillScreen(0);
  canvas.setTextWrap(false);
  canvas.setTextSize(3);
  canvas.setTextColor(1);
  canvas.setCursor(30, 16);
  canvas.print("BIKE OS");
  canvas.setTextSize(1);
  canvas.setCursor(40, 48);
  canvas.print("Booting...");
  flushDisplays();
  delay(1200);
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_OFF);

  pinMode(JOY_SW_PIN, INPUT_PULLUP);
  analogReadResolution(12);

  WireLeft.begin(LEFT_SDA, LEFT_SCL);
  WireRight.begin(RIGHT_SDA, RIGHT_SCL);

  if (!oledLeft.begin(SSD1306_SWITCHCAPVCC, DISPLAY_ADDR)) {
    Serial.println("Left OLED init failed");
    for (;;)
      ;
  }
  if (!oledRight.begin(SSD1306_SWITCHCAPVCC, DISPLAY_ADDR)) {
    Serial.println("Right OLED init failed");
    for (;;)
      ;
  }
  showBoot();

  calibrateCenter();

  Serial.println("Initializing NimBLE");
  NimBLEDevice::init("Bike Remote");
  NimBLEDevice::setMTU(185);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  uint8_t keyMask = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  NimBLEDevice::setSecurityInitKey(keyMask);
  NimBLEDevice::setSecurityRespKey(keyMask);
  NimBLEDevice::setCustomGapHandler(gapEventHandler);

  gServer = NimBLEDevice::createServer();
  gServer->setCallbacks(new ServerCallbacks());

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.setName("Bike Remote");
  advData.addServiceUUID(ANCS_SERVICE_UUID);
  advData.addServiceUUID(AMS_SERVICE_UUID);
  advertising->setAdvertisementData(advData);

  NimBLEAdvertisementData scanData;
  scanData.setName("Bike Remote");
  advertising->setScanResponseData(scanData);
  advertising->start();

  showStatus("Advertising", 2000);
  renderUI();
}

// ===== LOOP =====
void loop() {
  uint32_t now = millis();

  if (currentNotification.active && notificationExpiry && now > notificationExpiry) {
    clearNotification();
  }
  if (statusActive && statusExpiry && now > statusExpiry) {
    clearStatus();
  }

  if (pendingServiceInit && linkEncrypted) {
    if (initializeServices()) {
      pendingServiceInit = false;
      showStatus("Secured", 1000);
    }
  }

  smoothRead();

  bool btnDown = digitalRead(JOY_SW_PIN) == LOW;
  if (btnDown && !btnWasDown) {
    btnDownAt = now;
  }
  if (btnDown && (now - btnDownAt > BOND_CLEAR_HOLD_MS)) {
    Serial.println("Clearing bonds");
    NimBLEDevice::deleteAllBonds();
    showStatus("Bonds cleared", 1500);
    delay(1500);
    ESP.restart();
  }

  if (btnDown && !btnWasDown) {
    tBtn = now;
    sendAmsCommand(AmsRemoteCommand::TogglePlayPause, "Play/Pause");
  }
  btnWasDown = btnDown;

  if (!inDeadzone(emaY) && beyondThreshold(emaY)) {
    if (emaY < 0 && now - tUp > VOL_REPEAT_MS) {
      sendAmsCommand(AmsRemoteCommand::VolumeUp, "Vol +");
      tUp = now;
    } else if (emaY > 0 && now - tDn > VOL_REPEAT_MS) {
      sendAmsCommand(AmsRemoteCommand::VolumeDown, "Vol -");
      tDn = now;
    }
  }

  if (!inDeadzone(emaX) && beyondThreshold(emaX)) {
    if (emaX < 0 && now - tR > ACTION_COOLDOWN) {
      sendAmsCommand(AmsRemoteCommand::NextTrack, "Next");
      tR = now;
    } else if (emaX > 0 && now - tL > ACTION_COOLDOWN) {
      sendAmsCommand(AmsRemoteCommand::PreviousTrack, "Prev");
      tL = now;
    }
  }

  delay(8);
}

