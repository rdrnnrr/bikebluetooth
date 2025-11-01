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
static const NimBLEUUID ANCS_SERVICE_UUID("7905F431-B5CE-4E99-A40F-4B1E122D00D0");
static const NimBLEUUID ANCS_NOTIFICATION_SOURCE_UUID("9FBF120D-6301-42D9-8C58-25E699A21DBD");
static const NimBLEUUID ANCS_CONTROL_POINT_UUID("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9");
static const NimBLEUUID ANCS_DATA_SOURCE_UUID("22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB");
static const NimBLEUUID AMS_SERVICE_UUID("89D3502B-0F36-433A-8EF4-C502AD55F8DC");
static const NimBLEUUID AMS_REMOTE_COMMAND_UUID("9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2");
static const NimBLEUUID AMS_ENTITY_UPDATE_UUID("2F7CABCE-808D-411F-9A0C-BB92BA96C102");
static const NimBLEUUID AMS_ENTITY_ATTRIBUTE_UUID("C6B2F38C-23AB-46D8-A6AB-A3A870B5A1D6");

bool iosConnected = false;
bool amsReady = false;
bool ancsReady = false;

NimBLEClient* pClient = nullptr;
NimBLEAddress* pServerAddress = nullptr;

// ===== JOYSTICK & STATE (Unchanged) =====
const float EMA_ALPHA = 0.18f; const int DEADZONE = 350; const int TRIGGER_THRESH = 1200;
const uint16_t VOL_REPEAT_MS = 120; const uint16_t ACTION_COOLDOWN = 220; const uint32_t BOND_CLEAR_HOLD_MS = 3000;
float emaX = 0, emaY = 0; int midX = 2048, midY = 2048;
uint32_t tUp=0, tDn=0, tL=0, tR=0, tBtn=0; bool btnWasDown = false; uint32_t btnDownAt = 0;
String artist = ""; String album = ""; String song = "";
String notificationApp = ""; String notificationTitle = ""; String notificationMessage = "";
uint32_t notificationExpiry = 0; uint32_t statusExpiry = 0; uint32_t lastScroll = 0;
int scrollOffset = 0; bool notificationActive = false; bool statusActive = false;
uint32_t currentNotificationUid = 0; uint8_t currentNotificationCategory = 0;
enum AmsRemoteCommand : uint8_t { AMS_CMD_PLAY = 0x00, AMS_CMD_PAUSE = 0x01, AMS_CMD_TOGGLE_PLAY_PAUSE = 0x02, AMS_CMD_NEXT_TRACK = 0x03, AMS_CMD_PREVIOUS_TRACK = 0x04, AMS_CMD_VOLUME_UP = 0x05, AMS_CMD_VOLUME_DOWN = 0x06 };

// ===== UI Functions (Unchanged) =====
void showNotification(const String& title, const String& message, const String& appName, uint32_t durationMs) { notificationTitle = title; notificationMessage = message; notificationApp = appName; notificationActive = true; notificationExpiry = durationMs ? millis() + durationMs : 0; oledTop.clearDisplay(); oledTop.setTextColor(SSD1306_WHITE); oledTop.setTextSize(1); oledTop.setCursor(0, 0); oledTop.println(notificationApp.length() ? notificationApp : "Notification"); String secondLine = notificationTitle.length() ? notificationTitle : ""; if(secondLine.length() > 20) secondLine = secondLine.substring(0, 20); oledTop.setCursor(0, 16); oledTop.println(secondLine); oledTop.display(); String line1, line2; if(notificationMessage.length() <= 21) { line1 = notificationMessage; line2 = ""; } else { line1 = notificationMessage.substring(0, 21); uint16_t endIndex = notificationMessage.length() > 42 ? 42 : notificationMessage.length(); line2 = notificationMessage.substring(21, endIndex); } drawBottomMessage(line1, line2); statusActive = false; }
void clearNotification() { notificationActive = false; notificationExpiry = 0; notificationApp = ""; notificationTitle = ""; notificationMessage = ""; drawTopInfo(); if(song.length()) drawBottomScroll(); else if(!statusActive) drawBottomStatic("Ready"); }
void showStatus(const String& message, uint32_t durationMs) { if(notificationActive) return; drawBottomStatic(message.c_str()); statusActive = true; statusExpiry = durationMs ? millis() + durationMs : 0; }
void drawBottomMessage(const String& line1, const String& line2) { oledBot.clearDisplay(); oledBot.setTextColor(SSD1306_WHITE); oledBot.setTextSize(1); oledBot.setCursor(0, 0); oledBot.println(line1); oledBot.setCursor(0, 16); oledBot.println(line2); oledBot.display(); }
void calibrateCenter(){ long sx=0, sy=0; for(int i=0;i<24;i++){ sx+=analogRead(JOY_X_PIN); sy+=analogRead(JOY_Y_PIN); delay(4); } midX=sx/24; midY=sy/24; emaX=emaY=0; }
void smoothRead(){ int rx=analogRead(JOY_X_PIN); int ry=analogRead(JOY_Y_PIN); emaX=(1.0f-EMA_ALPHA)*emaX + EMA_ALPHA*(rx-midX); emaY=(1.0f-EMA_ALPHA)*emaY + EMA_ALPHA*(ry-midY); }
void drawTopInfo() { if(notificationActive) return; oledTop.clearDisplay(); oledTop.setTextColor(SSD1306_WHITE); oledTop.setTextSize(1); oledTop.setCursor(0, 0); oledTop.println(artist.length() ? artist : "Unknown Artist"); oledTop.setCursor(0, 16); oledTop.println(album.length() ? album : "Unknown Album"); oledTop.display(); }
void drawBottomStatic(const char* txt) { oledBot.clearDisplay(); oledBot.setTextColor(SSD1306_WHITE); oledBot.setTextSize(2); int16_t x1,y1; uint16_t w,h; oledBot.getTextBounds(txt,0,0,&x1,&y1,&w,&h); int x=(128-w)/2; int y=(32-h)/2; oledBot.setCursor(x,y); oledBot.println(txt); oledBot.display(); }
void drawBottomScroll() { if(notificationActive || statusActive) return; oledBot.clearDisplay(); oledBot.setTextColor(SSD1306_WHITE); oledBot.setTextSize(2); int16_t x1,y1; uint16_t w,h; oledBot.getTextBounds(song.c_str(),0,0,&x1,&y1,&w,&h); if (w > 128) { oledBot.setCursor(-scrollOffset, 8); oledBot.print(song); oledBot.display(); if (millis() - lastScroll > 150) { scrollOffset++; if (scrollOffset > w + 16) scrollOffset = 0; lastScroll = millis(); } } else { int x = (128 - w)/2; oledBot.setCursor(x, 8); oledBot.print(song); oledBot.display(); } }
String cleanDisplayText(const String& input) { String cleaned; cleaned.reserve(input.length()); for (uint16_t i = 0; i < input.length();) { uint8_t byte = static_cast<uint8_t>(input.charAt(i)); if (byte == '\r' || byte == '\n') { i++; continue; } if (byte == '\t') { cleaned += ' '; i++; continue; } if (byte >= 32 && byte <= 126) { cleaned += static_cast<char>(byte); i++; continue; } if (byte >= 0x80) { cleaned += '?'; i++; while (i < input.length()) { uint8_t next = static_cast<uint8_t>(input.charAt(i)); if ((next & 0xC0) == 0x80) { i++; } else { break; } } continue; } i++; } cleaned.trim(); return cleaned; }

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) {
        Serial.println("iPhone connected. Creating client and requesting security...");
        iosConnected = true;

        // The server now knows the address of the client (iPhone)
        // We can create a client instance to connect back to it for ANCS/AMS
        pServerAddress = new NimBLEAddress(desc->peer_ota_addr);

        // Request security on the server-side connection
        pServer->getAdvertising()->stop();
        NimBLEDevice::startSecurity(desc->conn_handle);
    }
    void onDisconnect(NimBLEServer* pServer) {
        Serial.println("iPhone disconnected");
        iosConnected = false; ancsReady = false; amsReady = false;
        if(pServerAddress) {
            delete pServerAddress;
            pServerAddress = nullptr;
        }
        // Restart advertising
        pServer->getAdvertising()->start();
    }

    void onAuthenticationComplete(ble_gap_conn_desc* desc) {
        Serial.println("Link encrypted. Proceeding with service discovery.");
        // Now that the server link is encrypted, we can connect back as a client
        if (pServerAddress != nullptr) {
            // Use the same client object if it exists
            if (!pClient) {
                pClient = NimBLEDevice::createClient();
            }
            // The connect call is blocking and will handle security and service discovery
            if (pClient->connect(*pServerAddress)) {
                Serial.println("Connected back to iPhone as client.");
                // Discover services and characteristics
                // You can now get services and characteristics from pClient
            } else {
                Serial.println("Failed to connect back to iPhone as client.");
            }
        }
    }
};

void requestAncsAttributes(uint32_t uid) { /* ... */ }
void handleAncsData(const uint8_t* data, size_t length) { /* ... */ }
void sendAmsCommand(uint8_t commandId) { /* ... */ }

void showBoot() {
    oledTop.clearDisplay(); oledTop.setTextColor(SSD1306_WHITE); oledTop.setTextSize(2);
    oledTop.setCursor(20, 8); oledTop.print("BIKE OS"); oledTop.display();
    oledBot.clearDisplay(); oledBot.setTextColor(SSD1306_WHITE); oledBot.setTextSize(1);
    oledBot.setCursor(30, 8); oledBot.print("Booting..."); oledBot.display();
    delay(1500);
}

// --- SETUP ---
void setup() {
    Serial.begin(115200);
    WiFi.mode(WIFI_OFF);
    pinMode(JOY_SW_PIN, INPUT_PULLUP);
    analogReadResolution(12);
    WireTop.begin(TOP_SDA, TOP_SCL); WireBot.begin(BOT_SDA, BOT_SCL);
    oledTop.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR); oledBot.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
    showBoot();

    Serial.println("Initializing NimBLE...");
    NimBLEDevice::init("Remote");
    NimBLEDevice::setMTU(185);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    uint8_t keyMask = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    NimBLEDevice::setSecurityInitKey(keyMask); NimBLEDevice::setSecurityRespKey(keyMask);

    NimBLEServer* pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    calibrateCenter();
    showStatus("Advertising", 0);

    Serial.println("Starting BLE advertising...");
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->setName("Remote");
    pAdvertising->addServiceUUID(ANCS_SERVICE_UUID);
    pAdvertising->addServiceUUID(AMS_SERVICE_UUID);
    pAdvertising->start();
}

// ===== LOOP (Unchanged) =====
bool inDZ(float val) { return fabsf(val) < DEADZONE; }
bool beyond(float val) { return fabsf(val) > TRIGGER_THRESH; }

void loop(){
  uint32_t now = millis();
  if(notificationActive && notificationExpiry && now > notificationExpiry) clearNotification();
  if(statusActive && statusExpiry && now > statusExpiry) { statusActive = false; if(song.length() > 0 && !notificationActive) drawBottomScroll(); else if(!notificationActive) drawBottomStatic("Ready"); }
  if(song.length() > 0 && !notificationActive && !statusActive) drawBottomScroll();
  smoothRead();
  bool btnDown = (digitalRead(JOY_SW_PIN) == LOW);
  if(btnDown && !btnWasDown) btnDownAt = now;
  if(btnDown && (now - btnDownAt > BOND_CLEAR_HOLD_MS)) { Serial.println("Long press: Clearing bonds and restarting."); NimBLEDevice::deleteAllBonds(); showStatus("Bonds cleared", 1200); delay(1200); ESP.restart(); }
  if(btnDown && (now - tBtn > 280)) { if(btnWasDown == false) { Serial.println("Button pressed"); tBtn = now; sendAmsCommand(AMS_CMD_TOGGLE_PLAY_PAUSE); showStatus("Play/Pause", 800); } } 
  btnWasDown = btnDown;
  if(!inDZ(emaY) && beyond(emaY)){ if(emaY<0 && now-tUp>VOL_REPEAT_MS){ sendAmsCommand(AMS_CMD_VOLUME_UP); showStatus("Vol +", 600); tUp=now; } else if(emaY>0 && now-tDn>VOL_REPEAT_MS){ sendAmsCommand(AMS_CMD_VOLUME_DOWN); showStatus("Vol -", 600); tDn=now; } }
  if(!inDZ(emaX) && beyond(emaX)){ if(emaX<0 && now-tR>ACTION_COOLDOWN){ sendAmsCommand(AMS_CMD_NEXT_TRACK); showStatus("Next >", 800); tR=now; } else if(emaX>0 && now-tL>ACTION_COOLDOWN){ sendAmsCommand(AMS_CMD_PREVIOUS_TRACK); showStatus("< Prev", 800); tL=now; } }
  delay(6);
}