#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <array>
#include <cmath>
#include <cstring>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "esp32notifications.h"
#include "ble_notification.h"
#include "ams_ble_client.h"

namespace {
constexpr int kSingleDisplayWidth = 128;
constexpr int kDisplayHeight = 64;
constexpr int kCombinedWidth = 256;
constexpr uint8_t kLeftDisplayAddress = 0x3C;
constexpr uint8_t kRightDisplayAddress = 0x3D;

constexpr uint8_t kButtonPlayPause = 12;
constexpr uint8_t kButtonNext = 13;
constexpr uint8_t kButtonPrevious = 14;
constexpr uint8_t kButtonVolumeUp = 27;
constexpr uint8_t kButtonVolumeDown = 26;

constexpr char kWhatsAppAppId[] = "net.whatsapp.WhatsApp";
constexpr char kPhoneAppId[] = "com.apple.mobilephone";

Adafruit_SSD1306 g_leftDisplay(kSingleDisplayWidth, kDisplayHeight, &Wire, -1);
Adafruit_SSD1306 g_rightDisplay(kSingleDisplayWidth, kDisplayHeight, &Wire, -1);

class DualOLEDDisplay {
 public:
  DualOLEDDisplay(Adafruit_SSD1306 &left, Adafruit_SSD1306 &right)
      : left_(left), right_(right), canvas_(kCombinedWidth, kDisplayHeight) {}

  bool begin() {
    if (!left_.begin(SSD1306_SWITCHCAPVCC, kLeftDisplayAddress)) {
      return false;
    }
    if (!right_.begin(SSD1306_SWITCHCAPVCC, kRightDisplayAddress)) {
      return false;
    }
    left_.clearDisplay();
    left_.display();
    right_.clearDisplay();
    right_.display();
    canvas_.fillScreen(0);
    canvas_.setTextWrap(false);
    canvas_.setTextColor(1);
    return true;
  }

  void clear() { canvas_.fillScreen(0); }

  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
    canvas_.drawFastHLine(x, y, w, color);
  }

  void drawCenteredText(int16_t y, const String &text, uint8_t size) {
    canvas_.setTextSize(size);
    int16_t x1, y1;
    uint16_t w, h;
    canvas_.getTextBounds(text.c_str(), 0, y, &x1, &y1, &w, &h);
    int16_t x = (width() - static_cast<int16_t>(w)) / 2;
    if (x < 0) {
      x = 0;
    }
    canvas_.setCursor(x, y);
    canvas_.print(text);
  }

  void drawWrappedText(int16_t x, int16_t y, const String &text, uint8_t size,
                       int16_t maxWidth) {
    canvas_.setTextSize(size);
    const int16_t charWidth = 6 * size;
    const int16_t lineHeight = 8 * size;
    int16_t cursorX = x;
    int16_t cursorY = y;
    canvas_.setCursor(cursorX, cursorY);

    for (size_t i = 0; i < text.length(); ++i) {
      char c = text.charAt(i);
      if (c == '\n') {
        cursorY += lineHeight;
        cursorX = x;
        canvas_.setCursor(cursorX, cursorY);
        continue;
      }

      if (cursorX + charWidth > x + maxWidth) {
        cursorY += lineHeight;
        cursorX = x;
        canvas_.setCursor(cursorX, cursorY);
        if (c == ' ') {
          continue;
        }
      }

      canvas_.write(static_cast<uint8_t>(c));
      cursorX = canvas_.getCursorX();
    }
  }

  void flush() {
    const uint8_t *buffer = canvas_.getBuffer();
    constexpr int fullRowBytes = kCombinedWidth / 8;
    constexpr int halfRowBytes = kSingleDisplayWidth / 8;
    for (int row = 0; row < kDisplayHeight; ++row) {
      const uint8_t *rowPtr = buffer + (row * fullRowBytes);
      std::memcpy(leftBuffer_.data() + row * halfRowBytes, rowPtr,
                  halfRowBytes);
      std::memcpy(rightBuffer_.data() + row * halfRowBytes,
                  rowPtr + halfRowBytes, halfRowBytes);
    }

    left_.clearDisplay();
    left_.drawBitmap(0, 0, leftBuffer_.data(), kSingleDisplayWidth,
                     kDisplayHeight, SSD1306_WHITE);
    left_.display();

    right_.clearDisplay();
    right_.drawBitmap(0, 0, rightBuffer_.data(), kSingleDisplayWidth,
                      kDisplayHeight, SSD1306_WHITE);
    right_.display();
  }

  int16_t width() const { return kCombinedWidth; }
  int16_t height() const { return kDisplayHeight; }

 private:
  Adafruit_SSD1306 &left_;
  Adafruit_SSD1306 &right_;
  GFXcanvas1 canvas_;
  std::array<uint8_t, kDisplayHeight * (kSingleDisplayWidth / 8)> leftBuffer_{};
  std::array<uint8_t, kDisplayHeight * (kSingleDisplayWidth / 8)> rightBuffer_{};
};

DualOLEDDisplay g_display(g_leftDisplay, g_rightDisplay);
BLENotifications g_notifications;

struct NotificationState {
  bool active = false;
  String appId;
  String title;
  String message;
  uint32_t uuid = 0;
  time_t timestamp = 0;
};

struct MediaState {
  String playerName;
  String title;
  String artist;
  String album;
  bool playing = false;
  bool rewinding = false;
  bool fastForwarding = false;
  float playbackRate = 0.0f;
  float elapsedSeconds = 0.0f;
  float durationSeconds = 0.0f;
  float volume = 0.0f;
  bool hasTrack = false;
};

struct SharedState {
  bool connected = false;
  MediaState media;
  NotificationState call;
  NotificationState whatsapp;
  NotificationState general;
};

SharedState g_state;
volatile bool g_displayDirty = true;
portMUX_TYPE g_stateMux = portMUX_INITIALIZER_UNLOCKED;

String friendlyAppName(const String &appId) {
  if (appId.equalsIgnoreCase(kWhatsAppAppId)) {
    return F("WhatsApp");
  }
  if (appId.equalsIgnoreCase(kPhoneAppId)) {
    return F("Phone");
  }
  int lastDot = appId.lastIndexOf('.');
  if (lastDot >= 0 && lastDot < static_cast<int>(appId.length()) - 1) {
    return appId.substring(lastDot + 1);
  }
  return appId;
}

void setNotificationState(NotificationState &state, const Notification *note) {
  state.active = true;
  state.uuid = note->uuid;
  state.timestamp = note->time;
  state.title = String(note->title.c_str());
  state.message = String(note->message.c_str());
  state.appId = String(note->type.c_str());
}

void clearNotificationState(NotificationState &state) {
  state.active = false;
  state.uuid = 0;
  state.timestamp = 0;
  state.title = "";
  state.message = "";
  state.appId = "";
}

void markDisplayDirty() {
  portENTER_CRITICAL(&g_stateMux);
  g_displayDirty = true;
  portEXIT_CRITICAL(&g_stateMux);
}

void handleNotification(const Notification *note, const void *) {
  portENTER_CRITICAL(&g_stateMux);
  String appId(note->type.c_str());
  if (note->category == CategoryIDIncomingCall) {
    setNotificationState(g_state.call, note);
  } else if (appId.equalsIgnoreCase(kWhatsAppAppId)) {
    setNotificationState(g_state.whatsapp, note);
  } else {
    setNotificationState(g_state.general, note);
  }
  g_displayDirty = true;
  portEXIT_CRITICAL(&g_stateMux);
}

void handleNotificationRemoved(const Notification *note, const void *) {
  portENTER_CRITICAL(&g_stateMux);
  if (note->uuid == g_state.call.uuid) {
    clearNotificationState(g_state.call);
  }
  if (note->uuid == g_state.whatsapp.uuid) {
    clearNotificationState(g_state.whatsapp);
  }
  if (note->uuid == g_state.general.uuid) {
    clearNotificationState(g_state.general);
  }
  g_displayDirty = true;
  portEXIT_CRITICAL(&g_stateMux);
}

void parsePlaybackInfo(MediaState &media, const std::string &value) {
  String payload(value.c_str());
  int firstComma = payload.indexOf(',');
  if (firstComma < 0) {
    return;
  }
  int secondComma = payload.indexOf(',', firstComma + 1);
  if (secondComma < 0) {
    return;
  }

  int state = payload.substring(0, firstComma).toInt();
  media.playing = state == AMSPlaybackStatePlaying;
  media.rewinding = state == AMSPlaybackStateRewinding;
  media.fastForwarding = state == AMSPlaybackStateFastForwarding;
  media.playbackRate = payload.substring(firstComma + 1, secondComma).toFloat();
  media.elapsedSeconds = payload.substring(secondComma + 1).toFloat();
}

void handleTrackUpdate(AMSTrackAttributeID_t attribute, const std::string &value,
                       const void *) {
  portENTER_CRITICAL(&g_stateMux);
  switch (attribute) {
    case AMSTrackAttributeIDTitle:
      g_state.media.title = String(value.c_str());
      g_state.media.hasTrack = true;
      break;
    case AMSTrackAttributeIDArtist:
      g_state.media.artist = String(value.c_str());
      break;
    case AMSTrackAttributeIDAlbum:
      g_state.media.album = String(value.c_str());
      break;
    case AMSTrackAttributeIDDuration:
      g_state.media.durationSeconds = String(value.c_str()).toFloat();
      break;
  }
  g_displayDirty = true;
  portEXIT_CRITICAL(&g_stateMux);
}

void handlePlayerUpdate(AMSPlayerAttributeID_t attribute, const std::string &value,
                        const void *) {
  portENTER_CRITICAL(&g_stateMux);
  switch (attribute) {
    case AMSPlayerAttributeIDName:
      g_state.media.playerName = String(value.c_str());
      break;
    case AMSPlayerAttributeIDPlaybackInfo:
      parsePlaybackInfo(g_state.media, value);
      break;
    case AMSPlayerAttributeIDVolume:
      g_state.media.volume = String(value.c_str()).toFloat();
      break;
  }
  g_displayDirty = true;
  portEXIT_CRITICAL(&g_stateMux);
}

void handleConnectionState(BLENotifications::State state, const void *) {
  portENTER_CRITICAL(&g_stateMux);
  g_state.connected = state == BLENotifications::StateConnected;
  if (!g_state.connected) {
    clearNotificationState(g_state.call);
    clearNotificationState(g_state.whatsapp);
    clearNotificationState(g_state.general);
    g_state.media = MediaState();
  }
  g_displayDirty = true;
  portEXIT_CRITICAL(&g_stateMux);
}

void sendMediaCommand(AMSRemoteCommandID_t command) {
  AMSBLEClient *client = nullptr;
  bool connected = false;
  portENTER_CRITICAL(&g_stateMux);
  connected = g_state.connected;
  client = g_notifications.clientAMS;
  portEXIT_CRITICAL(&g_stateMux);
  if (!connected || client == nullptr) {
    return;
  }
  if (!client->isCommandAvailable(command)) {
    return;
  }
  client->performCommand(command);
}

void handleButtons() {
  static bool lastPlayPause = HIGH;
  static bool lastNext = HIGH;
  static bool lastPrevious = HIGH;
  static bool lastVolUp = HIGH;
  static bool lastVolDown = HIGH;

  bool playPause = digitalRead(kButtonPlayPause);
  bool next = digitalRead(kButtonNext);
  bool previous = digitalRead(kButtonPrevious);
  bool volUp = digitalRead(kButtonVolumeUp);
  bool volDown = digitalRead(kButtonVolumeDown);

  if (lastPlayPause == HIGH && playPause == LOW) {
    sendMediaCommand(AMSRemoteCommandIDTogglePlayPause);
  }
  if (lastNext == HIGH && next == LOW) {
    sendMediaCommand(AMSRemoteCommandIDNextTrack);
  }
  if (lastPrevious == HIGH && previous == LOW) {
    sendMediaCommand(AMSRemoteCommandIDPreviousTrack);
  }
  if (lastVolUp == HIGH && volUp == LOW) {
    sendMediaCommand(AMSRemoteCommandIDVolumeUp);
  }
  if (lastVolDown == HIGH && volDown == LOW) {
    sendMediaCommand(AMSRemoteCommandIDVolumeDown);
  }

  lastPlayPause = playPause;
  lastNext = next;
  lastPrevious = previous;
  lastVolUp = volUp;
  lastVolDown = volDown;
}

void renderDisplay() {
  SharedState snapshot;
  bool dirty = false;
  portENTER_CRITICAL(&g_stateMux);
  dirty = g_displayDirty;
  if (dirty) {
    snapshot = g_state;
    g_displayDirty = false;
  }
  portEXIT_CRITICAL(&g_stateMux);

  if (!dirty) {
    return;
  }

  g_display.clear();

  String statusLine = snapshot.connected ? F("iPhone connected")
                                         : F("Waiting for iPhone...");
  g_display.drawWrappedText(0, 0, statusLine, 1, g_display.width());

  String playbackLine;
  if (snapshot.media.playing) {
    playbackLine = F("Playing");
  } else if (snapshot.media.rewinding) {
    playbackLine = F("Rewinding");
  } else if (snapshot.media.fastForwarding) {
    playbackLine = F("Fast forward");
  } else {
    playbackLine = F("Paused");
  }
  if (snapshot.media.playerName.length() > 0) {
    playbackLine += F(" - ");
    playbackLine += snapshot.media.playerName;
  }
  g_display.drawWrappedText(0, 10, playbackLine, 1, g_display.width());

  String title = snapshot.media.hasTrack ? snapshot.media.title
                                         : F("Waiting for track...");
  g_display.drawWrappedText(0, 20, title, 2, g_display.width());

  const NotificationState *activeNotification = nullptr;
  String notificationLabel;
  if (snapshot.call.active) {
    activeNotification = &snapshot.call;
    notificationLabel = F("Incoming call");
  } else if (snapshot.whatsapp.active) {
    activeNotification = &snapshot.whatsapp;
    notificationLabel = F("WhatsApp");
  } else if (snapshot.general.active) {
    activeNotification = &snapshot.general;
    notificationLabel = friendlyAppName(snapshot.general.appId);
    if (notificationLabel.length() == 0) {
      notificationLabel = F("Notification");
    }
  }

  if (activeNotification) {
    g_display.drawWrappedText(0, 36, notificationLabel, 1, g_display.width());
    if (activeNotification->title.length() > 0) {
      g_display.drawWrappedText(0, 44, activeNotification->title, 2,
                                g_display.width());
    }
    if (activeNotification->message.length() > 0) {
      g_display.drawWrappedText(0, 56, activeNotification->message, 1,
                                g_display.width());
    }
  } else {
    if (snapshot.media.artist.length() > 0 ||
        snapshot.media.album.length() > 0) {
      String bottomLine = snapshot.media.artist;
      if (snapshot.media.album.length() > 0) {
        if (bottomLine.length() > 0) {
          bottomLine += F(" - ");
        }
        bottomLine += snapshot.media.album;
      }
      g_display.drawWrappedText(0, 40, bottomLine, 1, g_display.width());
    } else {
      g_display.drawWrappedText(0, 44, F("No notifications"), 1,
                                g_display.width());
    }
  }

  g_display.flush();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Wire.begin();

  if (!g_display.begin()) {
    Serial.println("Failed to initialise OLED displays");
    while (true) {
      delay(1000);
    }
  }

  pinMode(kButtonPlayPause, INPUT_PULLUP);
  pinMode(kButtonNext, INPUT_PULLUP);
  pinMode(kButtonPrevious, INPUT_PULLUP);
  pinMode(kButtonVolumeUp, INPUT_PULLUP);
  pinMode(kButtonVolumeDown, INPUT_PULLUP);

  g_display.clear();
  g_display.drawCenteredText(24, F("Bike Bluetooth"), 2);
  g_display.flush();

  g_notifications.begin("BikeBluetooth");
  g_notifications.setConnectionStateChangedCallback(handleConnectionState);
  g_notifications.setNotificationCallback(handleNotification);
  g_notifications.setRemovedCallback(handleNotificationRemoved);
  g_notifications.setOnAMSTrackUpdateCB(handleTrackUpdate);
  g_notifications.setOnAMSPlayerUpdateCB(handlePlayerUpdate);

  markDisplayDirty();
}

void loop() {
  handleButtons();
  renderDisplay();
  delay(20);
}
