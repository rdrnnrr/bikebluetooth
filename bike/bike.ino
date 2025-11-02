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

#define DISPLAY_WIDTH 256
#define DISPLAY_HEIGHT 64
#define DISPLAY_HALF_WIDTH (DISPLAY_WIDTH / 2)
#define DISPLAY_HALF_HEIGHT DISPLAY_HEIGHT
#define DISPLAY_HALF_BUFFER_SIZE ((DISPLAY_HALF_WIDTH * DISPLAY_HALF_HEIGHT) / 8)

TwoWire WireTop = TwoWire(0);
TwoWire WireBot = TwoWire(1);

Adafruit_SSD1306 oledLeft(DISPLAY_HALF_WIDTH, DISPLAY_HALF_HEIGHT, &WireTop, -1);
Adafruit_SSD1306 oledRight(DISPLAY_HALF_WIDTH, DISPLAY_HALF_HEIGHT, &WireBot, -1);
GFXcanvas1 compositeCanvas(DISPLAY_WIDTH, DISPLAY_HEIGHT);
uint8_t leftBuffer[DISPLAY_HALF_BUFFER_SIZE];
uint8_t rightBuffer[DISPLAY_HALF_BUFFER_SIZE];

// ===== BLE CONSTANTS =====
static const NimBLEUUID ANCS_SERVICE_UUID("7905F431-B5CE-4E99-A40F-4B1E122D00D0");
static const NimBLEUUID ANCS_NOTIFICATION_SOURCE_UUID("9FBF120D-6301-42D9-8C58-25E699A21DBD");
static const NimBLEUUID ANCS_CONTROL_POINT_UUID("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9");
static const NimBLEUUID ANCS_DATA_SOURCE_UUID("22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB");
static const NimBLEUUID AMS_SERVICE_UUID("89D3502B-0F36-433A-8EF4-C502AD55F8DC");
static const NimBLEUUID AMS_REMOTE_COMMAND_UUID("9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2");
static const NimBLEUUID AMS_ENTITY_UPDATE_UUID("2F7CABCE-808D-411F-9A0C-BB92BA96C102");
static const NimBLEUUID AMS_ENTITY_ATTRIBUTE_UUID("C6B2F38C-23AB-46D8-A6AB-A3A870B5A1D6");

// ===== STATE =====
bool iosConnected = false;
bool amsReady = false;
bool ancsReady = false;

NimBLEClient* pClient = nullptr;
NimBLEAddress* pServerAddress = nullptr;

NimBLERemoteCharacteristic* amsRemoteCommand = nullptr;
NimBLERemoteCharacteristic* amsEntityUpdate = nullptr;
NimBLERemoteCharacteristic* amsEntityAttribute = nullptr;
NimBLERemoteCharacteristic* ancsControlPoint = nullptr;
NimBLERemoteCharacteristic* ancsDataSource = nullptr;

const float EMA_ALPHA = 0.18f;
const int DEADZONE = 350;
const int TRIGGER_THRESH = 1200;
const uint16_t VOL_REPEAT_MS = 120;
const uint16_t ACTION_COOLDOWN = 220;
const uint32_t BOND_CLEAR_HOLD_MS = 3000;

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

String artist = "";
String album = "";
String song = "";
String statusMessage = "";
String notificationApp = "";
String notificationTitle = "";
String notificationMessage = "";

uint32_t notificationExpiry = 0;
uint32_t statusExpiry = 0;
uint32_t lastScroll = 0;
int scrollOffset = 0;
int songPixelWidth = 0;
bool notificationActive = false;
bool statusActive = false;
uint32_t currentNotificationUid = 0;
uint8_t currentNotificationCategory = 0;

bool displayDirty = true;

enum AmsRemoteCommand : uint8_t {
    AMS_CMD_PLAY = 0x00,
    AMS_CMD_PAUSE = 0x01,
    AMS_CMD_TOGGLE_PLAY_PAUSE = 0x02,
    AMS_CMD_NEXT_TRACK = 0x03,
    AMS_CMD_PREVIOUS_TRACK = 0x04,
    AMS_CMD_VOLUME_UP = 0x05,
    AMS_CMD_VOLUME_DOWN = 0x06
};

enum AncsEventId : uint8_t {
    ANCS_EVENT_ADDED = 0,
    ANCS_EVENT_MODIFIED = 1,
    ANCS_EVENT_REMOVED = 2
};

enum AncsCategoryId : uint8_t {
    ANCS_CATEGORY_OTHER = 0,
    ANCS_CATEGORY_INCOMING_CALL = 1,
    ANCS_CATEGORY_MISSED_CALL = 2,
    ANCS_CATEGORY_VOICEMAIL = 3,
    ANCS_CATEGORY_SOCIAL = 4
};

enum AmsEntityId : uint8_t {
    AMS_ENTITY_PLAYER = 0,
    AMS_ENTITY_QUEUE = 1,
    AMS_ENTITY_TRACK = 2
};

enum AmsTrackAttributeId : uint8_t {
    AMS_TRACK_ARTIST = 0,
    AMS_TRACK_ALBUM = 1,
    AMS_TRACK_TITLE = 2
};

String cleanDisplayText(const String& input) {
    String cleaned;
    cleaned.reserve(input.length());
    for (uint16_t i = 0; i < input.length();) {
        uint8_t byte = static_cast<uint8_t>(input.charAt(i));
        if (byte == '\r' || byte == '\n') { i++; continue; }
        if (byte == '\t') { cleaned += ' '; i++; continue; }
        if (byte >= 32 && byte <= 126) { cleaned += static_cast<char>(byte); i++; continue; }
        if (byte >= 0x80) {
            cleaned += '?';
            i++;
            while (i < input.length()) {
                uint8_t next = static_cast<uint8_t>(input.charAt(i));
                if ((next & 0xC0) == 0x80) { i++; }
                else { break; }
            }
            continue;
        }
        i++;
    }
    cleaned.trim();
    return cleaned;
}

String mapAppIdentifier(const String& appId) {
    if (appId.equalsIgnoreCase("com.apple.mobilephone")) return "Call";
    if (appId.equalsIgnoreCase("net.whatsapp.WhatsApp")) return "WhatsApp";
    if (appId.equalsIgnoreCase("com.google.ios.youtubemusic")) return "YouTube Music";
    if (appId.equalsIgnoreCase("com.apple.MobileSMS")) return "Messages";
    if (appId.equalsIgnoreCase("com.apple.facetime")) return "FaceTime";
    if (appId.equalsIgnoreCase("com.apple.Music")) return "Apple Music";
    if (appId.length() == 0) return "Notification";
    return appId;
}

void markDisplayDirty() {
    displayDirty = true;
}

void flushComposite() {
    uint8_t* src = compositeCanvas.getBuffer();
    const size_t pageCount = DISPLAY_HEIGHT / 8;
    for (size_t page = 0; page < pageCount; ++page) {
        memcpy(&leftBuffer[page * DISPLAY_HALF_WIDTH],
               &src[page * DISPLAY_WIDTH],
               DISPLAY_HALF_WIDTH);
        memcpy(&rightBuffer[page * DISPLAY_HALF_WIDTH],
               &src[page * DISPLAY_WIDTH + DISPLAY_HALF_WIDTH],
               DISPLAY_HALF_WIDTH);
    }

    oledLeft.clearDisplay();
    oledLeft.drawBitmap(0, 0, leftBuffer, DISPLAY_HALF_WIDTH, DISPLAY_HALF_HEIGHT, SSD1306_WHITE, SSD1306_BLACK);
    oledLeft.display();

    oledRight.clearDisplay();
    oledRight.drawBitmap(0, 0, rightBuffer, DISPLAY_HALF_WIDTH, DISPLAY_HALF_HEIGHT, SSD1306_WHITE, SSD1306_BLACK);
    oledRight.display();
}

void updateSongMetrics() {
    compositeCanvas.setTextSize(2);
    int16_t x1, y1;
    uint16_t w, h;
    compositeCanvas.getTextBounds(song.c_str(), 0, 0, &x1, &y1, &w, &h);
    songPixelWidth = static_cast<int>(w);
    scrollOffset = 0;
    lastScroll = millis();
}

void showNotification(const String& title, const String& message, const String& appName, uint32_t durationMs) {
    notificationTitle = cleanDisplayText(title);
    notificationMessage = cleanDisplayText(message);
    notificationApp = cleanDisplayText(mapAppIdentifier(appName));
    notificationActive = true;
    notificationExpiry = durationMs ? millis() + durationMs : 0;
    statusActive = false;
    markDisplayDirty();
}

void clearNotification() {
    notificationActive = false;
    notificationExpiry = 0;
    notificationApp = "";
    notificationTitle = "";
    notificationMessage = "";
    markDisplayDirty();
}

void showStatus(const String& message, uint32_t durationMs) {
    if (notificationActive) return;
    statusMessage = cleanDisplayText(message);
    statusActive = true;
    statusExpiry = durationMs ? millis() + durationMs : 0;
    markDisplayDirty();
}

void clearStatus() {
    statusActive = false;
    statusMessage = "";
    statusExpiry = 0;
    markDisplayDirty();
}

void renderNotification() {
    compositeCanvas.fillScreen(0);
    compositeCanvas.setTextWrap(false);
    compositeCanvas.setTextColor(1);

    compositeCanvas.setTextSize(1);
    compositeCanvas.setCursor(6, 6);
    compositeCanvas.print(notificationApp.length() ? notificationApp : "Notification");

    compositeCanvas.setTextSize(2);
    compositeCanvas.setCursor(6, 26);
    compositeCanvas.print(notificationTitle);

    compositeCanvas.setTextSize(1);
    compositeCanvas.setCursor(6, 50);
    compositeCanvas.print(notificationMessage);
}

void renderStatus() {
    compositeCanvas.fillScreen(0);
    compositeCanvas.setTextColor(1);
    compositeCanvas.setTextWrap(false);

    compositeCanvas.setTextSize(2);
    int16_t x1, y1;
    uint16_t w, h;
    compositeCanvas.getTextBounds(statusMessage.c_str(), 0, 0, &x1, &y1, &w, &h);
    int x = (DISPLAY_WIDTH - w) / 2;
    if (x < 0) x = 4;
    compositeCanvas.setCursor(x, 28);
    compositeCanvas.print(statusMessage);
}

void renderNowPlaying() {
    compositeCanvas.fillScreen(0);
    compositeCanvas.setTextColor(1);
    compositeCanvas.setTextWrap(false);

    compositeCanvas.setTextSize(1);
    compositeCanvas.setCursor(6, 8);
    compositeCanvas.print(artist.length() ? artist : "Unknown Artist");

    compositeCanvas.setCursor(6, 20);
    compositeCanvas.print(album.length() ? album : "Unknown Album");

    compositeCanvas.setTextSize(2);
    int16_t x1, y1;
    uint16_t w, h;
    compositeCanvas.getTextBounds(song.c_str(), 0, 0, &x1, &y1, &w, &h);
    int available = DISPLAY_WIDTH - 12;
    int cursorX = 6;
    if (w <= available) {
        cursorX = (DISPLAY_WIDTH - w) / 2;
    } else {
        cursorX = 6 - scrollOffset;
    }
    compositeCanvas.setCursor(cursorX, 42);
    compositeCanvas.print(song.length() ? song : "Ready");
}

void renderDisplay() {
    if (!displayDirty) return;

    if (notificationActive) {
        renderNotification();
    } else if (statusActive) {
        renderStatus();
    } else {
        renderNowPlaying();
    }

    flushComposite();
    displayDirty = false;
}

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

// ===== BLE HELPERS =====
void resetRemoteState() {
    artist = "";
    album = "";
    song = "";
    updateSongMetrics();
    amsReady = false;
    ancsReady = false;
    notificationActive = false;
    statusActive = false;
    notificationMessage = "";
    notificationTitle = "";
    notificationApp = "";
    statusMessage = "";
    markDisplayDirty();
}

void requestAmsAttribute(uint8_t entity, uint8_t attribute, uint16_t maxLen) {
    if (!amsEntityAttribute) return;
    uint8_t payload[4];
    payload[0] = entity;
    payload[1] = attribute;
    payload[2] = static_cast<uint8_t>(maxLen & 0xFF);
    payload[3] = static_cast<uint8_t>((maxLen >> 8) & 0xFF);
    amsEntityAttribute->writeValue(payload, sizeof(payload), true);
}

void subscribeAmsTrackAttributes() {
    requestAmsAttribute(AMS_ENTITY_TRACK, AMS_TRACK_TITLE, 80);
    requestAmsAttribute(AMS_ENTITY_TRACK, AMS_TRACK_ARTIST, 80);
    requestAmsAttribute(AMS_ENTITY_TRACK, AMS_TRACK_ALBUM, 80);
}

void handleAmsEntityUpdate(uint8_t* data, size_t length) {
    if (length < 3) return;
    uint8_t entity = data[0];
    uint8_t attribute = data[1];
    size_t valueLen = length - 2;
    String value = valueLen ? String((const char*)&data[2], valueLen) : String("");
    value = cleanDisplayText(value);

    if (entity == AMS_ENTITY_TRACK) {
        switch (attribute) {
            case AMS_TRACK_ARTIST:
                if (artist != value) { artist = value; markDisplayDirty(); }
                break;
            case AMS_TRACK_ALBUM:
                if (album != value) { album = value; markDisplayDirty(); }
                break;
            case AMS_TRACK_TITLE:
                if (song != value) { song = value; updateSongMetrics(); markDisplayDirty(); }
                break;
            default:
                break;
        }
    }
}

void handleNotificationSource(uint8_t* data, size_t length) {
    if (length < 8) return;
    uint8_t eventId = data[0];
    uint8_t categoryId = data[2];
    currentNotificationCategory = categoryId;
    uint32_t uid = static_cast<uint32_t>(data[4]) |
                   (static_cast<uint32_t>(data[5]) << 8) |
                   (static_cast<uint32_t>(data[6]) << 16) |
                   (static_cast<uint32_t>(data[7]) << 24);

    if (eventId == ANCS_EVENT_ADDED || eventId == ANCS_EVENT_MODIFIED) {
        currentNotificationUid = uid;
        requestAmsAttribute(AMS_ENTITY_TRACK, AMS_TRACK_TITLE, 80); // keep media fresh
        if (ancsControlPoint) {
            uint8_t payload[32];
            size_t index = 0;
            payload[index++] = 0x00; // Get Notification Attributes
            payload[index++] = static_cast<uint8_t>(uid & 0xFF);
            payload[index++] = static_cast<uint8_t>((uid >> 8) & 0xFF);
            payload[index++] = static_cast<uint8_t>((uid >> 16) & 0xFF);
            payload[index++] = static_cast<uint8_t>((uid >> 24) & 0xFF);
            payload[index++] = 0x00; // App Identifier
            payload[index++] = 0x01; payload[index++] = 0x40; payload[index++] = 0x00; // Title max 64
            payload[index++] = 0x02; payload[index++] = 0x40; payload[index++] = 0x00; // Subtitle max 64
            payload[index++] = 0x03; payload[index++] = 0x80; payload[index++] = 0x00; // Message max 128
            payload[index++] = 0x04; payload[index++] = 0x00; payload[index++] = 0x00; // Message Size
            ancsControlPoint->writeValue(payload, index, true);
        }
    } else if (eventId == ANCS_EVENT_REMOVED) {
        if (uid == currentNotificationUid) {
            clearNotification();
        }
    }
}

void handleAncsData(const uint8_t* data, size_t length) {
    if (length < 5) return;

    size_t index = 0;
    uint32_t uid = static_cast<uint32_t>(data[index]) |
                   (static_cast<uint32_t>(data[index + 1]) << 8) |
                   (static_cast<uint32_t>(data[index + 2]) << 16) |
                   (static_cast<uint32_t>(data[index + 3]) << 24);
    index += 4;

    if (uid != currentNotificationUid) {
        return;
    }

    String appId;
    String title;
    String subtitle;
    String message;

    while (index + 3 <= length) {
        uint8_t attrId = data[index++];
        uint16_t attrLen = static_cast<uint16_t>(data[index]) | (static_cast<uint16_t>(data[index + 1]) << 8);
        index += 2;
        if (index + attrLen > length) break;
        String value = attrLen ? String((const char*)&data[index], attrLen) : String("");
        index += attrLen;

        value = cleanDisplayText(value);

        switch (attrId) {
            case 0x00: appId = value; break;
            case 0x01: title = value; break;
            case 0x02: subtitle = value; break;
            case 0x03: message = value; break;
            default: break;
        }
    }

    String displayTitle = title.length() ? title : subtitle;
    if (displayTitle.length() == 0) {
        if (currentNotificationCategory == ANCS_CATEGORY_INCOMING_CALL) {
            displayTitle = "Incoming Call";
        } else {
            displayTitle = "Notification";
        }
    }

    if (currentNotificationCategory == ANCS_CATEGORY_INCOMING_CALL) {
        showNotification(displayTitle, message.length() ? message : "", appId, 0);
    } else {
        showNotification(displayTitle, message, appId, 8000);
    }
}

void sendAmsCommand(uint8_t commandId) {
    if (!amsReady || !amsRemoteCommand) return;
    amsRemoteCommand->writeValue(&commandId, 1, true);
}

void initializeRemoteServices(NimBLEClient* client) {
    amsRemoteCommand = nullptr;
    amsEntityUpdate = nullptr;
    amsEntityAttribute = nullptr;
    ancsControlPoint = nullptr;
    ancsDataSource = nullptr;

    NimBLERemoteService* ancsService = client->getService(ANCS_SERVICE_UUID);
    if (ancsService) {
        NimBLERemoteCharacteristic* notificationSource = ancsService->getCharacteristic(ANCS_NOTIFICATION_SOURCE_UUID);
        ancsControlPoint = ancsService->getCharacteristic(ANCS_CONTROL_POINT_UUID);
        ancsDataSource = ancsService->getCharacteristic(ANCS_DATA_SOURCE_UUID);

        if (notificationSource && notificationSource->canNotify()) {
            notificationSource->subscribe(true, [](NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
                handleNotificationSource(data, length);
            });
        }
        if (ancsDataSource && ancsDataSource->canNotify()) {
            ancsDataSource->subscribe(true, [](NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
                handleAncsData(data, length);
            });
        }
        ancsReady = (ancsControlPoint != nullptr && ancsDataSource != nullptr);
    }

    NimBLERemoteService* amsService = client->getService(AMS_SERVICE_UUID);
    if (amsService) {
        amsRemoteCommand = amsService->getCharacteristic(AMS_REMOTE_COMMAND_UUID);
        amsEntityUpdate = amsService->getCharacteristic(AMS_ENTITY_UPDATE_UUID);
        amsEntityAttribute = amsService->getCharacteristic(AMS_ENTITY_ATTRIBUTE_UUID);

        if (amsEntityUpdate && amsEntityUpdate->canNotify()) {
            amsEntityUpdate->subscribe(true, [](NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
                handleAmsEntityUpdate(data, length);
            });
        }
        if (amsEntityAttribute) {
            subscribeAmsTrackAttributes();
        }
        amsReady = (amsRemoteCommand != nullptr);
    }

    if (amsReady || ancsReady) {
        showStatus("iPhone linked", 2000);
    } else {
        showStatus("Waiting services", 1500);
    }
}

void requestAppleConnectionParams(NimBLEClient* client) {
    if (!client) return;
    if (client->updateConnParams(24, 40, 0, 100)) {
        Serial.println("Requested Apple-friendly connection params.");
    } else {
        Serial.println("Failed to request connection params.");
    }
}

class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* client) override {
        Serial.println("Connected to iPhone as GATT client");
        requestAppleConnectionParams(client);
    }

    void onDisconnect(NimBLEClient* client) override {
        Serial.println("Client role disconnected");
        resetRemoteState();
    }
};

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
        Serial.println("iPhone connected to advertising peripheral");
        iosConnected = true;
        if (pServerAddress) {
            delete pServerAddress;
        }
        pServerAddress = new NimBLEAddress(desc->peer_ota_addr);
        server->getAdvertising()->stop();
        NimBLEDevice::startSecurity(desc->conn_handle);
        showStatus("Securing...", 1500);
    }

    void onDisconnect(NimBLEServer* server) override {
        Serial.println("iPhone disconnected");
        iosConnected = false;
        resetRemoteState();
        if (pServerAddress) {
            delete pServerAddress;
            pServerAddress = nullptr;
        }
        server->getAdvertising()->start();
        showStatus("Advertising", 0);
    }

    void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
        Serial.println("Link encrypted. Initializing services...");
        if (!pServerAddress) return;

        if (!pClient) {
            pClient = NimBLEDevice::createClient();
            pClient->setClientCallbacks(new ClientCallbacks());
            pClient->setConnectionParams(24, 40, 0, 100);
            pClient->setConnectTimeout(10);
        }

        if (!pClient->isConnected()) {
            if (!pClient->connect(*pServerAddress)) {
                Serial.println("Failed to connect back as client");
                return;
            }
        }

        initializeRemoteServices(pClient);
    }
};

void showBoot() {
    compositeCanvas.fillScreen(0);
    compositeCanvas.setTextColor(1);
    compositeCanvas.setTextSize(3);
    compositeCanvas.setCursor(36, 18);
    compositeCanvas.print("BIKE");
    compositeCanvas.setCursor(160, 18);
    compositeCanvas.print("OS");
    compositeCanvas.setTextSize(1);
    compositeCanvas.setCursor(92, 50);
    compositeCanvas.print("Booting...");
    flushComposite();
    delay(1500);
}

// ===== SETUP =====
void setup() {
    Serial.begin(115200);
    WiFi.mode(WIFI_OFF);

    pinMode(JOY_SW_PIN, INPUT_PULLUP);
    analogReadResolution(12);

    WireTop.begin(TOP_SDA, TOP_SCL);
    WireBot.begin(BOT_SDA, BOT_SCL);

    oledLeft.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
    oledRight.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
    oledLeft.clearDisplay();
    oledRight.clearDisplay();
    oledLeft.display();
    oledRight.display();

    showBoot();
    updateSongMetrics();

    Serial.println("Initializing NimBLE...");
    NimBLEDevice::init("BikeRemote");
    NimBLEDevice::setMTU(185);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    uint8_t keyMask = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    NimBLEDevice::setSecurityInitKey(keyMask);
    NimBLEDevice::setSecurityRespKey(keyMask);

    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(new ServerCallbacks());

    calibrateCenter();
    showStatus("Advertising", 0);

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->setName("BikeRemote");
    advertising->addServiceUUID(ANCS_SERVICE_UUID);
    advertising->addServiceUUID(AMS_SERVICE_UUID);
    advertising->start();
}

bool inDZ(float val) { return fabsf(val) < DEADZONE; }
bool beyond(float val) { return fabsf(val) > TRIGGER_THRESH; }

// ===== LOOP =====
void loop() {
    uint32_t now = millis();

    if (notificationActive && notificationExpiry && now > notificationExpiry) {
        clearNotification();
    }
    if (statusActive && statusExpiry && now > statusExpiry) {
        clearStatus();
    }
    if (!notificationActive && !statusActive && songPixelWidth > DISPLAY_WIDTH - 32) {
        if (now - lastScroll > 150) {
            scrollOffset++;
            if (scrollOffset > songPixelWidth + 24) {
                scrollOffset = 0;
            }
            lastScroll = now;
            markDisplayDirty();
        }
    }

    smoothRead();
    bool btnDown = (digitalRead(JOY_SW_PIN) == LOW);
    if (btnDown && !btnWasDown) btnDownAt = now;
    if (btnDown && (now - btnDownAt > BOND_CLEAR_HOLD_MS)) {
        Serial.println("Long press: clearing bonds");
        NimBLEDevice::deleteAllBonds();
        showStatus("Bonds cleared", 1200);
        delay(1200);
        ESP.restart();
    }

    if (btnDown && (now - tBtn > 280)) {
        if (!btnWasDown) {
            tBtn = now;
            sendAmsCommand(AMS_CMD_TOGGLE_PLAY_PAUSE);
            showStatus("Play/Pause", 800);
        }
    }
    btnWasDown = btnDown;

    if (!inDZ(emaY) && beyond(emaY)) {
        if (emaY < 0 && now - tUp > VOL_REPEAT_MS) {
            sendAmsCommand(AMS_CMD_VOLUME_UP);
            showStatus("Vol +", 600);
            tUp = now;
        } else if (emaY > 0 && now - tDn > VOL_REPEAT_MS) {
            sendAmsCommand(AMS_CMD_VOLUME_DOWN);
            showStatus("Vol -", 600);
            tDn = now;
        }
    }

    if (!inDZ(emaX) && beyond(emaX)) {
        if (emaX < 0 && now - tR > ACTION_COOLDOWN) {
            sendAmsCommand(AMS_CMD_NEXT_TRACK);
            showStatus("Next >>", 800);
            tR = now;
        } else if (emaX > 0 && now - tL > ACTION_COOLDOWN) {
            sendAmsCommand(AMS_CMD_PREVIOUS_TRACK);
            showStatus("<< Prev", 800);
            tL = now;
        }
    }

    renderDisplay();
    delay(6);
}
