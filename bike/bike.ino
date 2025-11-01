#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <NimBLEDevice.h>
#include <math.h>
#include <string>
#include <limits.h>
#include <cstring>
#include <WiFi.h>
#include <esp_log.h>
#include <esp_gap_ble_api.h>

#if defined(CONFIG_NIMBLE_CPP_IDF)
extern "C" {
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "nimble/hci_common.h"
}
#else
extern "C" {
#include "nimble/nimble/host/include/host/ble_gap.h"
#include "nimble/nimble/host/include/host/ble_hs.h"
#include "nimble/nimble/include/nimble/hci_common.h"
}
#endif

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

// --- AMS official UUIDs (per Apple docs) ---
static const NimBLEUUID AMS_SERVICE_UUID("89D3502B-0F36-433A-8EF4-C502AD55F8DC");
static const NimBLEUUID AMS_REMOTE_COMMAND_UUID("9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2");
static const NimBLEUUID AMS_ENTITY_UPDATE_UUID("2F7CABCE-808D-411F-9A0C-BB92BA96C102");
static const NimBLEUUID AMS_ENTITY_ATTRIBUTE_UUID("C6B2F38C-23AB-46D8-A6AB-A3A870B5A1D6");
// Reference: https://developer.apple.com/library/archive/documentation/CoreBluetooth/Reference/AppleMediaService_Reference/Specification/Specification.html


NimBLERemoteCharacteristic* ancsControlPoint = nullptr;
NimBLERemoteCharacteristic* ancsDataSource = nullptr;

NimBLERemoteCharacteristic* amsRemoteCommand = nullptr;


bool iosConnected = false;
bool amsReady = false;
bool ancsReady = false;
bool isConnecting = false;



// Optional hint for the advertised iOS device name to prioritize/accept.
const char* IOS_NAME_HINT = "iPhone";

// ===== JOYSTICK =====
const float EMA_ALPHA = 0.18f;
const int DEADZONE = 350;
const int TRIGGER_THRESH = 1200;
const uint16_t VOL_REPEAT_MS = 120;
const uint16_t ACTION_COOLDOWN = 220;
// Long-press time (ms) to wipe bonds from the joystick button.
const uint32_t BOND_CLEAR_HOLD_MS = 3000;
float emaX = 0, emaY = 0;
int midX = 2048, midY = 2048;
uint32_t tUp=0, tDn=0, tL=0, tR=0, tBtn=0;
bool btnWasDown = false;
uint32_t btnDownAt = 0;

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
void showStatus(const String& message, uint32_t durationMs);
void showNotification(const String& title, const String& message, const String& appName, uint32_t durationMs);
void clearNotification();
void startAdvertising();
String cleanDisplayText(const String& input);
void drawBottomMessage(const String& line1, const String& line2);
void drawTopInfo();
void drawBottomStatic(const char* txt);
void drawBottomScroll();

void handleAncsData(const uint8_t* data, size_t length);
void requestAncsAttributes(uint32_t uid);





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
  statusExpiry = durationMs ? millis() + durationMs : 0;
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

// ===== HELPERS ====== 
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

// ===== BOND MANAGEMENT =====


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



void drawBottomScroll() {
  if(notificationActive || statusActive) return;
  oledBot.clearDisplay();
  oledBot.setTextColor(SSD1306_WHITE);
  oledBot.setTextSize(2);

  int16_t x1,y1; uint16_t w,h;
  oledBot.getTextBounds(song.c_str(),0,0,&x1,&y1,&w,&h);

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




void connectToServer(NimBLEAdvertisedDevice* advertisedDevice);
static void scanEndedCB(NimBLEScanResults results);

class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* pClient) {
        Serial.println("Connected to iPhone");
        iosConnected = true;
    }

    void onDisconnect(NimBLEClient* pClient) {
        Serial.println("Disconnected from iPhone");
        iosConnected = false;
        NimBLEDevice::getScan()->start(0, scanEndedCB);
    }
};

class AdvertisedDeviceCallbacks: public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) {
        Serial.print("Advertised Device: ");
        Serial.println(advertisedDevice->toString().c_str());
        if (advertisedDevice->haveServiceUUID()) {
            Serial.print("Service UUIDs: ");
            for (int i = 0; i < advertisedDevice->getServiceUUIDCount(); i++) {
                Serial.print(advertisedDevice->getServiceUUID(i).toString().c_str());
                Serial.print(" ");
            }
            Serial.println();
        }

        if (advertisedDevice->isAdvertisingService(ANCS_SERVICE_UUID)) {
            Serial.println("Found an iPhone with ANCS");
            NimBLEDevice::getScan()->stop();
            connectToServer(advertisedDevice);
        }
    }
};

static void scanEndedCB(NimBLEScanResults results) {
    Serial.println("Scan ended; no ANCS device found.");
}

void connectToServer(NimBLEAdvertisedDevice* advertisedDevice) {
    NimBLEClient* pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(new ClientCallbacks());

    if (pClient->connect(advertisedDevice)) {
        NimBLERemoteService* pANCS = pClient->getService(ANCS_SERVICE_UUID);
        if (pANCS) {
            ancsControlPoint = pANCS->getCharacteristic(ANCS_CONTROL_POINT_UUID);
            ancsDataSource = pANCS->getCharacteristic(ANCS_DATA_SOURCE_UUID);
            NimBLERemoteCharacteristic* ancsNotificationSource = pANCS->getCharacteristic(ANCS_NOTIFICATION_SOURCE_UUID);

            if (ancsNotificationSource && ancsNotificationSource->canNotify()) {
                ancsNotificationSource->subscribe(true, [](NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify){
                    if (length >= 8) {
                        uint32_t uid = (pData[7] << 24) | (pData[6] << 16) | (pData[5] << 8) | pData[4];
                        requestAncsAttributes(uid);
                    }
                });
            }
            if (ancsDataSource && ancsDataSource->canNotify()) {
                ancsDataSource->subscribe(true, [](NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify){
                    handleAncsData(pData, length);
                });
            }
        }

        NimBLERemoteService* pAMS = pClient->getService(AMS_SERVICE_UUID);
        if (pAMS) {
            amsRemoteCommand = pAMS->getCharacteristic(AMS_REMOTE_COMMAND_UUID);
            NimBLERemoteCharacteristic* amsEntityUpdate = pAMS->getCharacteristic(AMS_ENTITY_UPDATE_UUID);
            if (amsEntityUpdate && amsEntityUpdate->canNotify()) {
                amsEntityUpdate->subscribe(true, [](NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify){
                    // Handle AMS entity update
                });
            }
        }
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
  payload[idx++] = 64; payload[idx++] = 0;
  payload[idx++] = 0x02; // Subtitle
  payload[idx++] = 64; payload[idx++] = 0;
  payload[idx++] = 0x03; // Message
  payload[idx++] = 128; payload[idx++] = 0;

  ancsControlPoint->writeValue(payload, idx, true);
}

void handleAncsData(const uint8_t* data, size_t length) {
  if(length < 5) return;
  if(data[0] != 0x00) return; // Expect Get Notification Attributes response

  uint32_t uid = data[1] | (data[2] << 8) | (data[3] << 16) | (data[4] << 24);
  if(uid != currentNotificationUid) currentNotificationUid = uid;

  size_t index = 5;
  String appId, title, subtitle, message;

  while(index + 2 < length) {
    uint8_t attrId = data[index++];
    if(index + 2 > length) break;
    uint16_t attrLen = data[index] | (data[index + 1] << 8);
    index += 2;

    String value = "";
    if(attrLen && index + attrLen <= length) {
      value.reserve(attrLen);
      for(uint16_t i = 0; i < attrLen; ++i) value += static_cast<char>(data[index + i]);
      index += attrLen;
    } else if(attrLen) break;

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
  if(dot >= 0 && dot + 1 < appDisplay.length()) appDisplay = appDisplay.substring(dot + 1);
  appDisplay.replace('_', ' ');
  appDisplay.replace('-', ' ');

  uint32_t duration = (currentNotificationCategory == 0x01 || currentNotificationCategory == 0x02) ? 0 : 12000;
  showNotification(displayTitle, displayMessage, appDisplay, duration);
}

void showBoot(){ oledTop.clearDisplay(); oledBot.clearDisplay(); drawBottomStatic("Booting"); }
void showReady(){ drawTopInfo(); drawBottomStatic("Ready"); }





String cleanDisplayText(const String& input) {
  String cleaned;
  cleaned.reserve(input.length());

  for (uint16_t i = 0; i < input.length();) {
    uint8_t byte = static_cast<uint8_t>(input.charAt(i));

    if (byte == '\r' || byte == '\n') { i++; continue; }
    if (byte == '\t') { cleaned += ' '; i++; continue; }

    if (byte >= 32 && byte <= 126) { cleaned += static_cast<char>(byte); i++; continue; }

    if (byte >= 0x80) {
      cleaned += '?'; i++;
      while (i < input.length()) {
        uint8_t next = static_cast<uint8_t>(input.charAt(i));
        if ((next & 0xC0) == 0x80) { i++; } else { break; }
      }
      continue;
    }

    i++;
  }

  cleaned.trim();
  return cleaned;
}

static const char* gapEventTypeName(uint8_t type) {
  switch(type) {
    case BLE_GAP_EVENT_CONNECT: return "CONNECT";
    case BLE_GAP_EVENT_DISCONNECT: return "DISCONNECT";
    case BLE_GAP_EVENT_CONN_UPDATE: return "CONN_UPDATE";
    case BLE_GAP_EVENT_CONN_UPDATE_REQ: return "CONN_UPDATE_REQ";
    case BLE_GAP_EVENT_L2CAP_UPDATE_REQ: return "L2CAP_UPDATE_REQ";
    case BLE_GAP_EVENT_TERM_FAILURE: return "TERM_FAILURE";
    case BLE_GAP_EVENT_DISC: return "DISC";
    case BLE_GAP_EVENT_DISC_COMPLETE: return "DISC_COMPLETE";
    case BLE_GAP_EVENT_ADV_COMPLETE: return "ADV_COMPLETE";
    case BLE_GAP_EVENT_ENC_CHANGE: return "ENC_CHANGE";
    case BLE_GAP_EVENT_PASSKEY_ACTION: return "PASSKEY_ACTION";
    case BLE_GAP_EVENT_MTU: return "MTU";
    case BLE_GAP_EVENT_IDENTITY_RESOLVED: return "IDENTITY_RESOLVED";
    case BLE_GAP_EVENT_REPEAT_PAIRING: return "REPEAT_PAIRING";
    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE: return "PHY_UPDATE";
    default: return "UNKNOWN";
  }
}

int gapEventLogger(struct ble_gap_event* event, void*) {
  if(!event) return 0;

  Serial.printf("GAP event: %u (%s)\n", event->type, gapEventTypeName(event->type));

  switch(event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if(event->connect.status != 0) {
        const char* statusStr = NimBLEUtils::returnCodeToString(event->connect.status);
        Serial.printf("GAP connect failed: status=%d (%s)\n",
                      event->connect.status,
                      statusStr);
      } else {
        ble_gap_conn_desc desc;
        int rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
        if(rc == 0) {
          NimBLEAddress peer(desc.peer_ota_addr);
          Serial.printf("GAP connected: handle=%u peer=%s addrType=%u\n",
                        event->connect.conn_handle,
                        peer.toString().c_str(),
                        desc.peer_ota_addr.type);
        } else {
          Serial.printf("GAP connected: handle=%u (descriptor lookup failed rc=%d)\n",
                        event->connect.conn_handle,
                        rc);
        }
      }
      break;

    case BLE_GAP_EVENT_DISCONNECT:
      Serial.printf("GAP disconnected: handle=%u reason=%d (%s)\n",
                    event->disconnect.conn.conn_handle,
                    event->disconnect.reason,
                    NimBLEUtils::returnCodeToString(event->disconnect.reason));
      break;

    case BLE_GAP_EVENT_CONN_UPDATE:
      Serial.printf("GAP conn update: handle=%u status=%d (%s)\n",
                    event->conn_update.conn_handle,
                    event->conn_update.status,
                    NimBLEUtils::returnCodeToString(event->conn_update.status));
      break;

    case BLE_GAP_EVENT_ENC_CHANGE:
      Serial.printf("GAP encryption change: handle=%u status=%d (%s)\n",
                    event->enc_change.conn_handle,
                    event->enc_change.status,
                    NimBLEUtils::returnCodeToString(event->enc_change.status));
      break;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
      Serial.printf("GAP passkey action: handle=%u action=%d\n",
                    event->passkey.conn_handle,
                    event->passkey.params.action);
      break;

    case BLE_GAP_EVENT_MTU:
      Serial.printf("GAP MTU updated: handle=%u channel=%u mtu=%u\n",
                    event->mtu.conn_handle,
                    event->mtu.channel_id,
                    event->mtu.value);
      break;

    case BLE_GAP_EVENT_DISC_COMPLETE:
      Serial.printf("GAP discovery complete: reason=%d (%s)\n",
                    event->disc_complete.reason,
                    NimBLEUtils::returnCodeToString(event->disc_complete.reason));
      break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
      Serial.printf("GAP adv complete: reason=%d (%s)\n",
                    event->adv_complete.reason,
                    NimBLEUtils::returnCodeToString(event->adv_complete.reason));
      break;

    default:
      break;
  }

  return 0;
}







// ===== SETUP =====
void setup(){
  Serial.begin(115200);
  esp_log_level_set("ble_hs", ESP_LOG_DEBUG);
  esp_log_level_set("ble_gap", ESP_LOG_DEBUG);
  esp_log_level_set("ble_hs_hci", ESP_LOG_DEBUG);

  // Turn off Wi-Fi cleanly on ESP32 (no ESP8266 forceSleepBegin here)
  WiFi.mode(WIFI_OFF);

#if defined(CONFIG_BT_NIMBLE_ROLE_CENTRAL)
  esp_err_t privacyRc = esp_ble_gap_config_local_privacy(true);
  Serial.printf("Local privacy config: rc=%d\n", privacyRc);
#endif

  pinMode(JOY_SW_PIN, INPUT_PULLUP);
  analogReadResolution(12);
  WireTop.begin(TOP_SDA, TOP_SCL);
  WireBot.begin(BOT_SDA, BOT_SCL);

  oledTop.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  oledBot.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);

  NimBLEDevice::init("Remote");
  NimBLEDevice::setMTU(185);

  // Keep bonds so iOS trusts us after first pairing.
  // clearBonds(); // leave commented

  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  if(privacyRc == ESP_OK) {
#if defined(BLE_OWN_ADDR_RPA_RANDOM_DEFAULT)
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RPA_RANDOM_DEFAULT);
#elif defined(BLE_OWN_ADDR_RPA_PUBLIC_DEFAULT)
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RPA_PUBLIC_DEFAULT);
#elif defined(BLE_OWN_ADDR_RANDOM)
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
#endif
  } else {
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);
  }
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  uint8_t keyMask = securityKeyMask();
  NimBLEDevice::setSecurityInitKey(keyMask);
  NimBLEDevice::setSecurityRespKey(keyMask);
  NimBLEDevice::setCustomGapHandler(gapEventLogger);
  Serial.println("GAP event logger registered");

  showBoot();
  calibrateCenter();
  showStatus("Scanning", 0);

  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallbacks());
  pScan->setActiveScan(true);
  pScan->start(0, scanEndedCB);
}

// ===== LOOP =====
void sendAmsCommand(uint8_t commandId) {
  if(!amsReady || !amsRemoteCommand) return;
  amsRemoteCommand->writeValue(&commandId, 1, false);
}

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

  if (iosConnected) lastConnectAttempt = now;

  if(song.length() > 0 && !notificationActive && !statusActive) {
    drawBottomScroll();
  }

  smoothRead();

  bool btnDown = (digitalRead(JOY_SW_PIN) == LOW);
  if(btnDown && !btnWasDown) {
    btnDownAt = now;
  }

  if(btnDown && (now - btnDownAt > BOND_CLEAR_HOLD_MS)) {
    // Long press: forget bonds and restart discovery.
    Serial.println("Long press detected.");
    while(digitalRead(JOY_SW_PIN) == LOW) delay(20);
    btnWasDown = false;
    btnDownAt = millis();
    tBtn = now;
    delay(6);
    return;
  }

  btnWasDown = btnDown;

  if(btnDown && (now - tBtn > 280)){
    Serial.println("Button pressed");
    tBtn = now;
    sendAmsCommand(AMS_CMD_TOGGLE_PLAY_PAUSE);
    showStatus("Play/Pause", 800);
  }

  if(!inDZ(emaY) && beyond(emaY)){
    if(emaY<0 && now-tUp>VOL_REPEAT_MS){
      Serial.println("Joystick right");
      sendAmsCommand(AMS_CMD_VOLUME_UP);
      showStatus("Vol +", 600);
      tUp=now;
    } else if(emaY>0 && now-tDn>VOL_REPEAT_MS){
      Serial.println("Joystick left");
      sendAmsCommand(AMS_CMD_VOLUME_DOWN);
      showStatus("Vol -", 600);
      tDn=now;
    }
  }

  if(!inDZ(emaX) && beyond(emaX)){
    if(emaX<0 && now-tR>ACTION_COOLDOWN){
      Serial.println("Joystick down");
      sendAmsCommand(AMS_CMD_NEXT_TRACK);
      showStatus("Next >", 800);
      tR=now;
    } else if(emaX>0 && now-tL>ACTION_COOLDOWN){
      Serial.println("Joystick up");
      sendAmsCommand(AMS_CMD_PREVIOUS_TRACK);
      showStatus("< Prev", 800);
      tL=now;
    }
  }

  delay(6);
}
