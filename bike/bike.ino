#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <NimBLEDevice.h>
#include <NimBLEUtils.h>
#include <math.h>
#include <string>
#include <vector>
#include <limits.h>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <WiFi.h>
#include <esp_log.h>
#include <esp_gap_ble_api.h>
#include "ble_notification.h"

// ===== PINS =====
#define JOY_X_PIN 34
#define JOY_Y_PIN 35
#define JOY_SW_PIN 32
#define OLED_ADDR 0x3C
#define TOP_SDA 21
#define TOP_SCL 22
#define BOT_SDA 25
#define BOT_SCL 26

constexpr int DISPLAY_TOTAL_WIDTH = 256;
constexpr int DISPLAY_TOTAL_HEIGHT = 64;
constexpr int DISPLAY_HALF_WIDTH = DISPLAY_TOTAL_WIDTH / 2;
constexpr int DISPLAY_FONT_STEP = 6; // 5px glyph +1px spacing in default font

TwoWire WireTop = TwoWire(0);
TwoWire WireBot = TwoWire(1);

Adafruit_SSD1306 oledLeft(DISPLAY_HALF_WIDTH, DISPLAY_TOTAL_HEIGHT, &WireTop, -1);
Adafruit_SSD1306 oledRight(DISPLAY_HALF_WIDTH, DISPLAY_TOTAL_HEIGHT, &WireBot, -1);

// ===== BLE CONSTANTS =====
static const NimBLEUUID ANCS_SERVICE_UUID("7905F431-B5CE-4E99-A40F-4B1E122D00D0");
static const NimBLEUUID ANCS_NOTIFICATION_SOURCE_UUID("9FBF120D-6301-42D9-8C58-25E699A21DBD");
static const NimBLEUUID ANCS_CONTROL_POINT_UUID("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9");
static const NimBLEUUID ANCS_DATA_SOURCE_UUID("22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB");
static const NimBLEUUID AMS_SERVICE_UUID("89D3502B-0F36-433A-8EF4-C502AD55F8DC");
static const NimBLEUUID AMS_REMOTE_COMMAND_UUID("9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2");
static const NimBLEUUID AMS_ENTITY_UPDATE_UUID("2F7CABCE-808D-411F-9A0C-BB92BA96C102");
static const NimBLEUUID AMS_ENTITY_ATTRIBUTE_UUID("C6B2F38C-23AB-46D8-A6AB-A3A870B5A1D6");

enum AmsEntityId : uint8_t { AMSEntityIDPlayer = 0, AMSEntityIDQueue = 1, AMSEntityIDTrack = 2 };
enum AmsPlayerAttributeId : uint8_t { AMSPlayerAttributeIDName = 0, AMSPlayerAttributeIDPlaybackInfo = 1, AMSPlayerAttributeIDVolume = 2 };
enum AmsTrackAttributeId : uint8_t { AMSTrackAttributeIDArtist = 0, AMSTrackAttributeIDAlbum = 1, AMSTrackAttributeIDTitle = 2, AMSTrackAttributeIDDuration = 3 };

bool iosConnected = false;
bool amsReady = false;
bool ancsReady = false;

NimBLEClient* pClient = nullptr;
NimBLEAddress* pServerAddress = nullptr;
bool pendingClientConnect = false;
uint32_t nextClientAttemptAt = 0;

NimBLERemoteCharacteristic* ancsControlPoint = nullptr;
NimBLERemoteCharacteristic* ancsDataSource = nullptr;
NimBLERemoteCharacteristic* ancsNotificationSource = nullptr;
NimBLERemoteCharacteristic* amsRemoteCommand = nullptr;
NimBLERemoteCharacteristic* amsEntityUpdate = nullptr;
NimBLERemoteCharacteristic* amsEntityAttribute = nullptr;

// ===== JOYSTICK & STATE (Unchanged) =====
const float EMA_ALPHA = 0.18f; const int DEADZONE = 350; const int TRIGGER_THRESH = 1200;
const uint16_t VOL_REPEAT_MS = 120; const uint16_t ACTION_COOLDOWN = 220; const uint32_t BOND_CLEAR_HOLD_MS = 3000;
float emaX = 0, emaY = 0; int midX = 2048, midY = 2048;
uint32_t tUp=0, tDn=0, tL=0, tR=0, tBtn=0; bool btnWasDown = false; uint32_t btnDownAt = 0;
String playerName = ""; String artist = ""; String album = ""; String song = "";
String notificationApp = ""; String notificationTitle = ""; String notificationMessage = "";
uint32_t notificationExpiry = 0; uint32_t statusExpiry = 0; uint32_t lastScroll = 0;
int scrollOffset = 0; bool notificationActive = false; bool statusActive = false;
uint32_t currentNotificationUid = 0; uint8_t currentNotificationCategory = 0;
enum AmsRemoteCommand : uint8_t { AMS_CMD_PLAY = 0x00, AMS_CMD_PAUSE = 0x01, AMS_CMD_TOGGLE_PLAY_PAUSE = 0x02, AMS_CMD_NEXT_TRACK = 0x03, AMS_CMD_PREVIOUS_TRACK = 0x04, AMS_CMD_VOLUME_UP = 0x05, AMS_CMD_VOLUME_DOWN = 0x06 };

// ===== UI Functions =====
void clearDisplays() {
    oledLeft.clearDisplay();
    oledRight.clearDisplay();
}

void flushDisplays() {
    oledLeft.display();
    oledRight.display();
}

void ensureDisplayDefaults() {
    oledLeft.setTextColor(SSD1306_WHITE);
    oledRight.setTextColor(SSD1306_WHITE);
    oledLeft.setTextWrap(false);
    oledRight.setTextWrap(false);
}

int textPixelWidth(const String &text, uint8_t size = 1) {
    return text.length() * DISPLAY_FONT_STEP * size;
}

void drawAcross(const String &text, int16_t x, int16_t y, uint8_t size = 1) {
    if (text.isEmpty()) {
        return;
    }
    int16_t cursorX = x;
    const int16_t step = DISPLAY_FONT_STEP * size;
    ensureDisplayDefaults();
    for (uint16_t i = 0; i < text.length(); ++i) {
        char c = text.charAt(i);
        if (cursorX <= -step) {
            cursorX += step;
            continue;
        }
        if (cursorX >= DISPLAY_TOTAL_WIDTH) {
            break;
        }
        Adafruit_SSD1306 *target = (cursorX < DISPLAY_HALF_WIDTH) ? &oledLeft : &oledRight;
        int16_t localX = (cursorX < DISPLAY_HALF_WIDTH) ? cursorX : cursorX - DISPLAY_HALF_WIDTH;
        target->setTextSize(size);
        target->setCursor(localX, y);
        target->write(c);
        cursorX += step;
    }
}

std::vector<String> wrapText(const String &text, uint8_t maxChars) {
    std::vector<String> lines;
    if (text.isEmpty()) {
        return lines;
    }
    String remaining = text;
    while (remaining.length() > maxChars) {
        int breakIndex = remaining.lastIndexOf(' ', maxChars);
        if (breakIndex <= 0) {
            breakIndex = maxChars;
        }
        lines.push_back(remaining.substring(0, breakIndex));
        remaining.remove(0, breakIndex);
        remaining.trim();
    }
    if (!remaining.isEmpty()) {
        lines.push_back(remaining);
    }
    return lines;
}

String cleanDisplayText(const String &input) {
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

void clearTopArea() {
    oledLeft.fillRect(0, 0, DISPLAY_HALF_WIDTH, 32, SSD1306_BLACK);
    oledRight.fillRect(0, 0, DISPLAY_HALF_WIDTH, 32, SSD1306_BLACK);
}

void clearBottomArea() {
    oledLeft.fillRect(0, 32, DISPLAY_HALF_WIDTH, 32, SSD1306_BLACK);
    oledRight.fillRect(0, 32, DISPLAY_HALF_WIDTH, 32, SSD1306_BLACK);
}

void drawNowPlayingTopLines() {
    if (notificationActive) {
        return;
    }
    clearTopArea();
    String header = playerName.length() ? playerName : String("Media");
    drawAcross(header, 0, 0, 1);
    drawAcross(artist.length() ? artist : String("Unknown Artist"), 0, 12, 1);
    drawAcross(album.length() ? album : String("Unknown Album"), 0, 24, 1);
}

bool drawBottomSong(bool clearArea) {
    if (notificationActive || statusActive) {
        return false;
    }
    int titleWidth = textPixelWidth(song, 2);
    int16_t baseY = 40;
    bool shouldRedraw = clearArea;
    if (titleWidth > DISPLAY_TOTAL_WIDTH) {
        if (millis() - lastScroll > 120) {
            scrollOffset += 2;
            if (scrollOffset > titleWidth + 32) {
                scrollOffset = 0;
            }
            lastScroll = millis();
            shouldRedraw = true;
        }
    } else {
        if (clearArea) {
            scrollOffset = 0;
            lastScroll = millis();
            shouldRedraw = true;
        }
    }

    if (!shouldRedraw) {
        return false;
    }

    clearBottomArea();

    if (titleWidth <= DISPLAY_TOTAL_WIDTH) {
        int startX = (DISPLAY_TOTAL_WIDTH - titleWidth) / 2;
        if (startX < 0) {
            startX = 0;
        }
        drawAcross(song, startX, baseY, 2);
    } else {
        drawAcross(song, -scrollOffset, baseY, 2);
    }
    return true;
}

void updateNowPlayingDisplay() {
    if (notificationActive) {
        return;
    }
    clearDisplays();
    drawNowPlayingTopLines();
    drawBottomSong(true);
    flushDisplays();
}

void showNotification(const String &title, const String &message, const String &appName, uint32_t durationMs) {
    notificationTitle = title;
    notificationMessage = message;
    notificationApp = appName;
    notificationActive = true;
    notificationExpiry = durationMs ? millis() + durationMs : 0;

    clearDisplays();
    drawAcross(appName.length() ? appName : String("Notification"), 0, 0, 1);

    if (!title.isEmpty()) {
        uint8_t textSize = (currentNotificationCategory == CategoryIDIncomingCall) ? 2 : 1;
        int startY = (textSize == 2) ? 18 : 14;
        drawAcross(title, 0, startY, textSize);
    }

    std::vector<String> lines = wrapText(message, 42);
    int16_t y = currentNotificationCategory == CategoryIDIncomingCall ? 40 : 28;
    for (size_t i = 0; i < lines.size() && y < DISPLAY_TOTAL_HEIGHT - 8; ++i) {
        drawAcross(lines[i], 0, y, 1);
        y += 12;
    }

    flushDisplays();
    statusActive = false;
}

void clearNotification() {
    notificationActive = false;
    notificationExpiry = 0;
    notificationApp.clear();
    notificationTitle.clear();
    notificationMessage.clear();
    scrollOffset = 0;
    if (!statusActive) {
        updateNowPlayingDisplay();
    }
}

void drawBottomStatic(const String &text) {
    clearDisplays();
    drawAcross(text, (DISPLAY_TOTAL_WIDTH - textPixelWidth(text, 2)) / 2, 24, 2);
    flushDisplays();
}

void showStatus(const String &message, uint32_t durationMs) {
    if (notificationActive) {
        return;
    }
    drawBottomStatic(message);
    statusActive = true;
    statusExpiry = durationMs ? millis() + durationMs : 0;
}

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


String friendlyAppName(const String &identifier) {
    if (identifier.equalsIgnoreCase("com.apple.mobilephone")) return "Phone";
    if (identifier.equalsIgnoreCase("com.apple.facetime")) return "FaceTime";
    if (identifier.equalsIgnoreCase("com.whatsapp")) return "WhatsApp";
    if (identifier.equalsIgnoreCase("com.google.ios.youtubemusic")) return "YouTube Music";
    if (identifier.equalsIgnoreCase("com.apple.Music")) return "Apple Music";
    if (identifier.equalsIgnoreCase("com.spotify.client")) return "Spotify";
    int lastDot = identifier.lastIndexOf('.');
    if (lastDot >= 0 && lastDot + 1 < identifier.length()) {
        String friendly = identifier.substring(lastDot + 1);
        friendly.replace('.', ' ');
        if (friendly.length()) {
            friendly.setCharAt(0, static_cast<char>(toupper(static_cast<unsigned char>(friendly.charAt(0)))));
        }
        return friendly;
    }
    return identifier.length() ? identifier : String("Notification");
}

void connectToPeerLater(uint32_t delayMs) {
    pendingClientConnect = true;
    nextClientAttemptAt = millis() + delayMs;
}

class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient *client) override {
        Serial.println("Client link established. Waiting for encryption...");
    }

    void onDisconnect(NimBLEClient *client) override {
        Serial.println("Client disconnected.");
        amsReady = false;
        ancsReady = false;
        amsRemoteCommand = nullptr;
        amsEntityUpdate = nullptr;
        amsEntityAttribute = nullptr;
        ancsControlPoint = nullptr;
        ancsDataSource = nullptr;
        ancsNotificationSource = nullptr;
        if (!notificationActive) {
            playerName = "";
            artist = "";
            album = "";
            song = "";
            updateNowPlayingDisplay();
        }
        if (iosConnected) {
            showStatus("Reconnecting", 1000);
            connectToPeerLater(1500);
        }
    }
};

static ClientCallbacks clientCallbacksInstance;

void requestAppleConnectionParams(NimBLEClient *client) {
    if (!client) {
        return;
    }
    if (client->updateConnParams(24, 40, 0, 100)) {
        Serial.println("Requested iOS compliant connection parameters.");
    } else {
        Serial.println("Failed to update connection parameters.");
    }
}

void subscribeAmsAttributes() {
    if (!amsEntityAttribute) {
        return;
    }
    const uint16_t longField = 160;
    const uint16_t mediumField = 96;
    uint8_t artistReq[] = {AMSEntityIDTrack, AMSTrackAttributeIDArtist, static_cast<uint8_t>(mediumField & 0xFF), static_cast<uint8_t>(mediumField >> 8)};
    uint8_t albumReq[] = {AMSEntityIDTrack, AMSTrackAttributeIDAlbum, static_cast<uint8_t>(mediumField & 0xFF), static_cast<uint8_t>(mediumField >> 8)};
    uint8_t titleReq[] = {AMSEntityIDTrack, AMSTrackAttributeIDTitle, static_cast<uint8_t>(longField & 0xFF), static_cast<uint8_t>(longField >> 8)};
    uint8_t playerReq[] = {AMSEntityIDPlayer, AMSPlayerAttributeIDName, static_cast<uint8_t>(mediumField & 0xFF), static_cast<uint8_t>(mediumField >> 8)};
    amsEntityAttribute->writeValue(artistReq, sizeof(artistReq), true);
    amsEntityAttribute->writeValue(albumReq, sizeof(albumReq), true);
    amsEntityAttribute->writeValue(titleReq, sizeof(titleReq), true);
    amsEntityAttribute->writeValue(playerReq, sizeof(playerReq), true);
}

void handleAmsEntityUpdate(NimBLERemoteCharacteristic *, uint8_t *data, size_t length, bool) {
    if (length < 3) {
        return;
    }
    const uint8_t entity = data[0];
    const uint8_t attribute = data[1];
    String value;
    if (length > 3) {
        value = cleanDisplayText(String(reinterpret_cast<const char *>(data + 3), length - 3));
    }

    bool displayNeedsRefresh = false;
    switch (entity) {
        case AMSEntityIDPlayer:
            if (attribute == AMSPlayerAttributeIDName && playerName != value) {
                playerName = value;
                displayNeedsRefresh = true;
            }
            break;
        case AMSEntityIDTrack:
            if (attribute == AMSTrackAttributeIDArtist && artist != value) {
                artist = value;
                displayNeedsRefresh = true;
            } else if (attribute == AMSTrackAttributeIDAlbum && album != value) {
                album = value;
                displayNeedsRefresh = true;
            } else if (attribute == AMSTrackAttributeIDTitle && song != value) {
                song = value;
                displayNeedsRefresh = true;
            }
            break;
        default:
            break;
    }

    if (!notificationActive && displayNeedsRefresh) {
        updateNowPlayingDisplay();
    }
}

void requestAncsAttributes(uint32_t uid) {
    if (!ancsControlPoint) {
        return;
    }
    std::vector<uint8_t> command;
    command.reserve(20);
    command.push_back(ANCS::CommandIDGetNotificationAttributes);
    command.push_back(static_cast<uint8_t>(uid & 0xFF));
    command.push_back(static_cast<uint8_t>((uid >> 8) & 0xFF));
    command.push_back(static_cast<uint8_t>((uid >> 16) & 0xFF));
    command.push_back(static_cast<uint8_t>((uid >> 24) & 0xFF));
    command.push_back(ANCS::NotificationAttributeIDAppIdentifier);
    command.push_back(ANCS::NotificationAttributeIDTitle);
    command.push_back(0x80);
    command.push_back(0x00);
    command.push_back(ANCS::NotificationAttributeIDSubtitle);
    command.push_back(0x80);
    command.push_back(0x00);
    command.push_back(ANCS::NotificationAttributeIDMessage);
    command.push_back(0xC0);
    command.push_back(0x00);
    command.push_back(ANCS::NotificationAttributeIDDate);
    ancsControlPoint->writeValue(command.data(), command.size(), true);
}

void handleAncsNotification(NimBLERemoteCharacteristic *, uint8_t *data, size_t length, bool) {
    if (length < 8) {
        return;
    }
    uint8_t eventId = data[0];
    uint8_t category = data[2];
    uint32_t uid = static_cast<uint32_t>(data[4]) |
                   (static_cast<uint32_t>(data[5]) << 8) |
                   (static_cast<uint32_t>(data[6]) << 16) |
                   (static_cast<uint32_t>(data[7]) << 24);

    if (eventId == ANCS::EventIDNotificationRemoved) {
        if (notificationActive && currentNotificationUid == uid) {
            clearNotification();
        }
        return;
    }

    currentNotificationUid = uid;
    currentNotificationCategory = category;
    requestAncsAttributes(uid);
}

void handleAncsData(NimBLERemoteCharacteristic *, uint8_t *data, size_t length, bool) {
    if (length < 5) {
        return;
    }
    if (data[0] != ANCS::CommandIDGetNotificationAttributes) {
        return;
    }
    uint32_t uid = static_cast<uint32_t>(data[1]) |
                   (static_cast<uint32_t>(data[2]) << 8) |
                   (static_cast<uint32_t>(data[3]) << 16) |
                   (static_cast<uint32_t>(data[4]) << 24);
    if (uid != currentNotificationUid) {
        return;
    }

    String appId;
    String title;
    String subtitle;
    String message;
    size_t index = 5;
    while (index + 2 < length) {
        uint8_t attributeId = data[index++];
        uint16_t attrLength = static_cast<uint16_t>(data[index]) | (static_cast<uint16_t>(data[index + 1]) << 8);
        index += 2;
        if (index + attrLength > length) {
            break;
        }
        String value = cleanDisplayText(String(reinterpret_cast<const char *>(data + index), attrLength));
        switch (attributeId) {
            case ANCS::NotificationAttributeIDAppIdentifier:
                appId = value;
                break;
            case ANCS::NotificationAttributeIDTitle:
                title = value;
                break;
            case ANCS::NotificationAttributeIDSubtitle:
                subtitle = value;
                break;
            case ANCS::NotificationAttributeIDMessage:
                message = value;
                break;
            default:
                break;
        }
        index += attrLength;
    }

    String appName = friendlyAppName(appId);
    String headline = title.length() ? title : subtitle;
    String body = message;

    if (currentNotificationCategory == CategoryIDIncomingCall) {
        if (headline.isEmpty()) {
            headline = subtitle.length() ? subtitle : String("Incoming call");
        }
        if (body.isEmpty()) {
            body = String("Answer on phone");
        }
        showNotification(headline, body, appName, 0);
        return;
    }

    if (headline.isEmpty()) {
        headline = appName;
    }
    if (body.isEmpty()) {
        body = subtitle;
    }
    showNotification(headline, body, appName, 12000);
}

void onSecurityEstablished(NimBLEClient *client) {
    Serial.println("Client link encrypted. Discovering services...");
    ancsReady = false;
    amsReady = false;

    NimBLERemoteService *ancsService = client->getService(ANCS_SERVICE_UUID);
    if (ancsService) {
        ancsControlPoint = ancsService->getCharacteristic(ANCS_CONTROL_POINT_UUID);
        ancsDataSource = ancsService->getCharacteristic(ANCS_DATA_SOURCE_UUID);
        ancsNotificationSource = ancsService->getCharacteristic(ANCS_NOTIFICATION_SOURCE_UUID);
        if (ancsControlPoint && ancsDataSource && ancsNotificationSource) {
            if (ancsNotificationSource->canNotify()) {
                ancsNotificationSource->subscribe(true, handleAncsNotification);
            }
            if (ancsDataSource->canNotify()) {
                ancsDataSource->subscribe(true, handleAncsData);
            }
            ancsReady = true;
            Serial.println("ANCS ready.");
        }
    } else {
        Serial.println("ANCS service not found.");
    }

    NimBLERemoteService *amsService = client->getService(AMS_SERVICE_UUID);
    if (amsService) {
        amsRemoteCommand = amsService->getCharacteristic(AMS_REMOTE_COMMAND_UUID);
        amsEntityUpdate = amsService->getCharacteristic(AMS_ENTITY_UPDATE_UUID);
        amsEntityAttribute = amsService->getCharacteristic(AMS_ENTITY_ATTRIBUTE_UUID);
        if (amsEntityUpdate && amsEntityUpdate->canNotify()) {
            amsEntityUpdate->subscribe(true, handleAmsEntityUpdate);
        }
        if (amsEntityAttribute) {
            subscribeAmsAttributes();
        }
        amsReady = amsRemoteCommand != nullptr;
        Serial.println("AMS ready.");
    } else {
        Serial.println("AMS service not found.");
    }

    if (!notificationActive) {
        updateNowPlayingDisplay();
    }
    showStatus("Connected", 800);
    requestAppleConnectionParams(client);
}

int gapEventHandler(struct ble_gap_event *event, void *) {
    if (!event) {
        return 0;
    }
    switch (event->type) {
        case BLE_GAP_EVENT_ENC_CHANGE: {
            Serial.printf("GAP encryption change: handle=%u status=%d (%s)\n",
                          event->enc_change.conn_handle,
                          event->enc_change.status,
                          NimBLEUtils::returnCodeToString(event->enc_change.status));
            NimBLEClient *client = NimBLEDevice::getClientByConnHandle(event->enc_change.conn_handle);
            if (client) {
                if (event->enc_change.status == 0) {
                    onSecurityEstablished(client);
                } else {
                    client->disconnect();
                }
            }
            break;
        }
        case BLE_GAP_EVENT_CONN_UPDATE:
            Serial.printf("GAP conn update: status=%d (%s)\n",
                          event->conn_update.status,
                          NimBLEUtils::returnCodeToString(event->conn_update.status));
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            Serial.printf("GAP disconnect: reason=%d (%s)\n",
                          event->disconnect.reason,
                          NimBLEUtils::returnCodeToString(event->disconnect.reason));
            break;
        default:
            break;
    }
    return 0;
}

void ensureClientConnection() {
    if (!pendingClientConnect) {
        return;
    }
    if (!iosConnected || !pServerAddress) {
        pendingClientConnect = false;
        return;
    }
    if (pClient && (pClient->isConnected() || pClient->isConnecting())) {
        pendingClientConnect = false;
        return;
    }

    if (!pClient) {
        pClient = NimBLEDevice::createClient();
        pClient->setClientCallbacks(&clientCallbacksInstance);
        pClient->setConnectionParams(24, 40, 0, 100);
        pClient->setConnectTimeout(10);
    }

    Serial.println("Connecting to iPhone as BLE client...");
    if (!pClient->connect(*pServerAddress, false)) {
        Serial.println("Client connection attempt failed.");
        NimBLEDevice::deleteClient(pClient);
        pClient = nullptr;
        connectToPeerLater(2000);
        return;
    }
    Serial.println("Client connected, awaiting encryption.");
    pendingClientConnect = false;
}
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *pServer, ble_gap_conn_desc *desc) override {
        Serial.println("iPhone connected.");
        iosConnected = true;
        if (pServerAddress) {
            delete pServerAddress;
        }
        pServerAddress = new NimBLEAddress(desc->peer_ota_addr);
        pServer->getAdvertising()->stop();
        NimBLEDevice::startSecurity(desc->conn_handle);
        showStatus("Securing", 600);
    }

    void onDisconnect(NimBLEServer *pServer) override {
        Serial.println("iPhone disconnected");
        iosConnected = false;
        pendingClientConnect = false;
        ancsReady = false;
        amsReady = false;
        if (pServerAddress) {
            delete pServerAddress;
            pServerAddress = nullptr;
        }
        if (notificationActive) {
            clearNotification();
        }
        showStatus("Advertising", 0);
        pServer->getAdvertising()->start();
    }

    void onAuthenticationComplete(ble_gap_conn_desc *) override {
        Serial.println("Server link encrypted. Scheduling client connection.");
        connectToPeerLater(200);
    }
};

void sendAmsCommand(uint8_t commandId) {
    if (!amsReady || !amsRemoteCommand) {
        return;
    }
    amsRemoteCommand->writeValue(&commandId, 1, false);
}

void showBoot() {
    clearDisplays();
    drawAcross("BIKE OS", (DISPLAY_TOTAL_WIDTH - textPixelWidth("BIKE OS", 2)) / 2, 18, 2);
    drawAcross("Booting...", (DISPLAY_TOTAL_WIDTH - textPixelWidth("Booting...", 1)) / 2, 42, 1);
    flushDisplays();
    delay(1500);
}

// --- SETUP ---
void setup() {
    Serial.begin(115200);
    WiFi.mode(WIFI_OFF);
    pinMode(JOY_SW_PIN, INPUT_PULLUP);
    analogReadResolution(12);
    WireTop.begin(TOP_SDA, TOP_SCL);
    WireBot.begin(BOT_SDA, BOT_SCL);
    oledLeft.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
    oledRight.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
    ensureDisplayDefaults();
    showBoot();

    Serial.println("Initializing NimBLE...");
    NimBLEDevice::init("Remote");
    NimBLEDevice::setMTU(185);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEDevice::setCustomGapHandler(gapEventHandler);

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

// ===== LOOP =====
bool inDZ(float val) { return fabsf(val) < DEADZONE; }
bool beyond(float val) { return fabsf(val) > TRIGGER_THRESH; }

void loop(){
  uint32_t now = millis();
  if(pendingClientConnect && now >= nextClientAttemptAt) {
    ensureClientConnection();
  }

  if(notificationActive && notificationExpiry && now > notificationExpiry) {
    clearNotification();
  }

  if(statusActive && statusExpiry && now > statusExpiry) {
    statusActive = false;
    if(!notificationActive) {
      if(song.length()) {
        if(drawBottomSong(true)) {
          flushDisplays();
        }
      } else {
        updateNowPlayingDisplay();
      }
    }
  }

  if(song.length() > 0 && !notificationActive && !statusActive) {
    if(drawBottomSong(false)) {
      flushDisplays();
    }
  }

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