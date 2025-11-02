#include "esp_gap_ble_api.h"
#include <NimBLEDevice.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>
#include <Arduino.h>

// OLED display configuration
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET     -1

Adafruit_SSD1306 displayTop(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_SSD1306 displayBottom(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Button pins
#define BUTTON_PLAY_PAUSE 12
#define BUTTON_NEXT 13
#define BUTTON_PREV 14
#define BUTTON_VOL_UP 27
#define BUTTON_VOL_DOWN 26

// Bluetooth AMS service UUIDs and characteristics
static NimBLEUUID AMS_SERVICE_UUID("89D3502B-0F36-433A-8EF4-C502AD55F8DC");
static NimBLEUUID AMS_REMOTE_COMMAND_CHAR_UUID("9B3C81D8-9A0C-4D56-8A0A-9E3A6A9F5A7B");
static NimBLEUUID AMS_ENTITY_UPDATE_CHAR_UUID("2F7CABCE-808D-411F-9A0C-BB92BA96C102");
static NimBLEUUID AMS_ENTITY_ATTRIBUTE_CHAR_UUID("C6B2F38C-23AB-46D8-A6AB-A3A870BBD5D7");

NimBLERemoteCharacteristic* pRemoteCmdChar = nullptr;
NimBLERemoteCharacteristic* pEntityUpdateChar = nullptr;
NimBLERemoteCharacteristic* pEntityAttributeChar = nullptr;

bool deviceConnected = false;
bool doScan = false;

struct SongInfo {
  String title;
  String artist;
  String album;
};

SongInfo currentSong;

NimBLEClient* pClient = nullptr;

void updateDisplay() {
  displayTop.clearDisplay();
  displayTop.setTextSize(1);
  displayTop.setTextColor(SSD1306_WHITE);
  displayTop.setCursor(0, 0);

  if (deviceConnected) {
    displayTop.println("Connected");
    displayTop.println("Title: " + currentSong.title);
    displayTop.println("Artist: " + currentSong.artist);
    displayTop.println("Album: " + currentSong.album);
  } else {
    displayTop.println("Disconnected");
  }
  displayTop.display();
}

bool getPeerAddress(esp_bd_addr_t addrOut) {
    std::vector<NimBLEAddress> connected = NimBLEDevice::getConnectedDevices();
    if (connected.empty()) return false;
    NimBLEAddress peerAddr = connected[0];
    memcpy(addrOut, peerAddr.getNative(), 6);
    return true;
}

class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pClient) {
    deviceConnected = true;
    Serial.println("Connected to server");
  }

  void onDisconnect(NimBLEClient* pClient) {
    deviceConnected = false;
    Serial.println("Disconnected from server");
    doScan = true;
  }
};

class NotificationCallbacks : public NimBLERemoteCharacteristicCallbacks {
  void onNotify(NimBLERemoteCharacteristic* pRemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) override {
    if(pRemoteCharacteristic->getUUID().equals(AMS_ENTITY_ATTRIBUTE_CHAR_UUID)) {
      // Parse notification data for song info updates
      // Assuming a simple format for demo purposes
      // Real implementation would parse the AMS protocol properly
      // For example, pData[0] could be entity ID, pData[1] attribute ID, followed by UTF-8 string

      // For demo, just update currentSong.title with received data as string
      String str = "";
      for(size_t i=0; i<length; i++) {
        str += (char)pData[i];
      }
      currentSong.title = str;
      updateDisplay();
    }
  }
};

bool connectToServer(NimBLEAddress pAddress) {
  Serial.print("Forming a connection to ");
  Serial.println(pAddress.toString().c_str());

  pClient = NimBLEDevice::createClient();
  Serial.println("Created client");

  pClient->setClientCallbacks(new ClientCallbacks(), false);

  if(!pClient->connect(pAddress)) {
    Serial.println("Failed to connect");
    NimBLEDevice::deleteClient(pClient);
    return false;
  }

  NimBLERemoteService* pAmsService = pClient->getService(AMS_SERVICE_UUID);
  if(pAmsService == nullptr) {
    Serial.println("Failed to find AMS service");
    pClient->disconnect();
    return false;
  }

  pRemoteCmdChar = pAmsService->getCharacteristic(AMS_REMOTE_COMMAND_CHAR_UUID);
  pEntityUpdateChar = pAmsService->getCharacteristic(AMS_ENTITY_UPDATE_CHAR_UUID);
  pEntityAttributeChar = pAmsService->getCharacteristic(AMS_ENTITY_ATTRIBUTE_CHAR_UUID);

  if(pRemoteCmdChar == nullptr || pEntityUpdateChar == nullptr || pEntityAttributeChar == nullptr) {
    Serial.println("Failed to find AMS characteristics");
    pClient->disconnect();
    return false;
  }

  if(pEntityAttributeChar->canNotify()) {
    pEntityAttributeChar->subscribe(true, new NotificationCallbacks());
  }

  return true;
}

class AdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* advertisedDevice) {
    Serial.print("Found device: ");
    Serial.println(advertisedDevice->toString().c_str());

    if(advertisedDevice->isAdvertisingService(AMS_SERVICE_UUID)) {
      Serial.println("Found AMS device!");
      NimBLEDevice::getScan()->stop();
      if(connectToServer(advertisedDevice->getAddress())) {
        Serial.println("Connected to AMS device");
      } else {
        Serial.println("Failed to connect");
      }
      doScan = false;
    }
  }
};

void sendAmsCommand(uint8_t commandId) {
  if(pRemoteCmdChar == nullptr) return;

  uint8_t cmd[3];
  cmd[0] = 0; // Command ID
  cmd[1] = commandId;
  cmd[2] = 0; // Reserved
  pRemoteCmdChar->writeValue(cmd, 3, false);
}

void handleButtons() {
  static bool lastPlayPauseState = HIGH;
  static bool lastNextState = HIGH;
  static bool lastPrevState = HIGH;
  static bool lastVolUpState = HIGH;
  static bool lastVolDownState = HIGH;

  bool playPauseState = digitalRead(BUTTON_PLAY_PAUSE);
  bool nextState = digitalRead(BUTTON_NEXT);
  bool prevState = digitalRead(BUTTON_PREV);
  bool volUpState = digitalRead(BUTTON_VOL_UP);
  bool volDownState = digitalRead(BUTTON_VOL_DOWN);

  if(lastPlayPauseState == HIGH && playPauseState == LOW) {
    sendAmsCommand(0x01); // Play/Pause toggle
  }
  if(lastNextState == HIGH && nextState == LOW) {
    sendAmsCommand(0x02); // Next track
  }
  if(lastPrevState == HIGH && prevState == LOW) {
    sendAmsCommand(0x03); // Previous track
  }
  if(lastVolUpState == HIGH && volUpState == LOW) {
    sendAmsCommand(0x04); // Volume up
  }
  if(lastVolDownState == HIGH && volDownState == LOW) {
    sendAmsCommand(0x05); // Volume down
  }

  lastPlayPauseState = playPauseState;
  lastNextState = nextState;
  lastPrevState = prevState;
  lastVolUpState = volUpState;
  lastVolDownState = volDownState;
}

void setup() {
  Serial.begin(115200);
  Serial.println("Starting BLE AMS Client");

  pinMode(BUTTON_PLAY_PAUSE, INPUT_PULLUP);
  pinMode(BUTTON_NEXT, INPUT_PULLUP);
  pinMode(BUTTON_PREV, INPUT_PULLUP);
  pinMode(BUTTON_VOL_UP, INPUT_PULLUP);
  pinMode(BUTTON_VOL_DOWN, INPUT_PULLUP);

  if(!displayTop.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 allocation failed for top display");
    for(;;);
  }
  displayTop.clearDisplay();
  displayTop.display();

  if(!displayBottom.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
    Serial.println("SSD1306 allocation failed for bottom display");
    for(;;);
  }
  displayBottom.clearDisplay();
  displayBottom.display();

  NimBLEDevice::init("AMS Client");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9); // Max power

  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallbacks());
  pScan->setInterval(45);
  pScan->setWindow(15);
  pScan->setActiveScan(true);
  pScan->start(5, false);

  doScan = true;
}

void loop() {
  if(!deviceConnected && doScan) {
    NimBLEDevice::getScan()->start(0, nullptr, false);
  }

  handleButtons();

  delay(100);
}