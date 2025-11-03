/*
 * Bike Remote — ESP32 (Arduino core 3.3.x, NimBLE-Arduino 2.3+)
 * Two SSD1306 128x32 displays (both 0x3C) on separate I2C buses
 * Joystick on 34/35 + button on 32
 * BLE multi-role: act as Peripheral for pairing, then scan & connect back as Client
 * Connects to iPhone using a COPY of the advertised device (preserves addr type)
 * Controls: Press=Play/Pause, X=Prev/Next, Y=Vol-/Vol+
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <NimBLEDevice.h>
#include <math.h>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

// =================== Pins & Display ===================
#define JOY_X_PIN   34
#define JOY_Y_PIN   35
#define JOY_SW_PIN  32

#define OLED_ADDR   0x3C
#define TOP_SDA     21
#define TOP_SCL     22
#define BOT_SDA     25
#define BOT_SCL     26

TwoWire WireTop = TwoWire(0);
TwoWire WireBot = TwoWire(1);

Adafruit_SSD1306 oledTop(128, 32, &WireTop, -1);
Adafruit_SSD1306 oledBot(128, 32, &WireBot, -1);

// =================== BLE UUIDs ===================
// ANCS
static const NimBLEUUID UUID_ANCS_SVC("7905F431-B5CE-4E99-A40F-4B1E122D00D0");
static const NimBLEUUID UUID_ANCS_NOTIFICATION_SOURCE("9FBF120D-6301-42D9-8C58-25E699A21DBD");
static const NimBLEUUID UUID_ANCS_CONTROL_POINT("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9");
static const NimBLEUUID UUID_ANCS_DATA_SOURCE("22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB");
// AMS
static const NimBLEUUID UUID_AMS_SVC("89D3502B-0F36-433A-8EF4-C502AD55F8DC");
static const NimBLEUUID UUID_AMS_REMOTE_COMMAND("9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2");
static const NimBLEUUID UUID_AMS_ENTITY_UPDATE("2F7CABCE-808D-411F-9A0C-BB92BA96C102");
static const NimBLEUUID UUID_AMS_ENTITY_ATTRIBUTE("C6B2F38C-23AB-46D8-A6AB-A3A870B5A1D6");

// =================== State ===================
struct LinkState {
  bool serverLinked = false;   // iPhone connected to our peripheral
  bool clientLinked = false;   // we connected back to iPhone
  bool amsReady     = false;
  bool ancsReady    = false;
} gLink;

NimBLEServer* gServer = nullptr;
NimBLEClient* gClient = nullptr;
NimBLEScan*   gScan   = nullptr;

NimBLEAddress gPeerAddr;      // learned from server-side connection
bool          havePeerAddr = false;

// Safe scanner/connect scheduling
bool     scanStartPending = false;
uint32_t scanStartAtMs    = 0;
bool     scanStopPending  = false;

bool     connectPending   = false;          // request a client connect in loop
NimBLEAdvertisedDevice* gPendingDev = nullptr; // heap copy of advertised device

const uint32_t CONNECT_BACKOFF_MS = 8000;   // wait before retrying a failed peer
struct FailedPeer {
  String address;
  uint32_t retryUntil;
  FailedPeer(const String& addr, uint32_t until)
      : address(addr), retryUntil(until) {}
};
std::vector<FailedPeer> gFailedPeers;

// AMS/ANCS handles
NimBLERemoteCharacteristic* chAmsCmd   = nullptr;
NimBLERemoteCharacteristic* chAmsUpd   = nullptr;
NimBLERemoteCharacteristic* chAmsAttr  = nullptr;
NimBLERemoteCharacteristic* chAncsCtrl = nullptr;
NimBLERemoteCharacteristic* chAncsData = nullptr;
NimBLERemoteCharacteristic* chAncsSrc  = nullptr;

// =================== UI State ===================
String nowArtist, nowAlbum, nowTitle;
String statusMsg;
bool   statusActive = false;
uint32_t statusUntil = 0;

String notifApp, notifTitle, notifMsg;
bool   notifActive = false;
uint32_t notifUntil = 0;

uint32_t lastScroll = 0;
int scrollOffset = 0;
int songPixelWidth = 0;

// =================== Joystick control ===================
const float EMA_ALPHA = 0.18f;
const int DEADZONE = 350;
const int TRIGGER_THRESH = 1200;
const uint16_t VOL_REPEAT_MS = 120;
const uint16_t ACTION_COOLDOWN = 220;
const uint32_t BOND_CLEAR_HOLD_MS = 3000;

float emaX = 0, emaY = 0;
int midX = 2048, midY = 2048;
uint32_t tUp=0, tDn=0, tL=0, tR=0, tBtn=0;
bool btnWasDown = false; 
uint32_t btnDownAt = 0;

// One diag ticker (avoid redeclaration)
uint32_t gDiagTick = 0;

// =================== Helpers ===================
static inline bool inDZ(float v){ return fabsf(v) < DEADZONE; }
static inline bool beyond(float v){ return fabsf(v) > TRIGGER_THRESH; }

String cleanText(const String& in) {
  String out; out.reserve(in.length());
  for (uint16_t i=0;i<in.length();) {
    uint8_t b = (uint8_t)in[i];
    if (b=='\r' || b=='\n') { i++; continue; }
    if (b=='\t') { out+=' '; i++; continue; }
    if (b>=32 && b<=126) { out+=(char)b; i++; continue; }
    if (b>=0x80) {
      out+='?'; i++;
      while (i<in.length()) {
        uint8_t n = (uint8_t)in[i];
        if ((n&0xC0)==0x80) i++; else break;
      }
      continue;
    }
    i++;
  }
  out.trim();
  return out;
}

String mapAppId(const String& appId) {
  if (appId.equalsIgnoreCase("com.apple.mobilephone")) return "Call";
  if (appId.equalsIgnoreCase("net.whatsapp.WhatsApp")) return "WhatsApp";
  if (appId.equalsIgnoreCase("com.google.ios.youtubemusic")) return "YT Music";
  if (appId.equalsIgnoreCase("com.apple.MobileSMS")) return "Messages";
  if (appId.equalsIgnoreCase("com.apple.facetime")) return "FaceTime";
  if (appId.equalsIgnoreCase("com.apple.Music")) return "Apple Music";
  if (!appId.length()) return "Notification";
  return appId;
}

void setStatus(const String& msg, uint32_t ms=1500) {
  statusMsg = cleanText(msg);
  statusActive = true;
  statusUntil = ms ? millis()+ms : 0;
}

void clearStatus() { statusActive=false; statusMsg=""; statusUntil=0; }

void showNotif(const String& title, const String& body, const String& app, uint32_t ms=8000) {
  notifTitle = cleanText(title);
  notifMsg   = cleanText(body);
  notifApp   = cleanText(mapAppId(app));
  notifActive=true;
  notifUntil = ms ? millis()+ms : 0;
}
void clearNotif() { notifActive=false; notifTitle=""; notifMsg=""; notifApp=""; notifUntil=0; }

void pruneFailedPeers(uint32_t now) {
  if (gFailedPeers.empty()) return;
  gFailedPeers.erase(
      std::remove_if(gFailedPeers.begin(), gFailedPeers.end(),
                     [now](const FailedPeer& peer) { return now >= peer.retryUntil; }),
      gFailedPeers.end());
}

bool recentlyFailed(const NimBLEAdvertisedDevice* dev) {
  if (!dev) return false;
  uint32_t now = millis();
  pruneFailedPeers(now);
  String addr = String(dev->getAddress().toString().c_str());
  for (const auto& peer : gFailedPeers) {
    if (peer.address == addr) {
      return true;
    }
  }
  return false;
}

void rememberFailure(const NimBLEAdvertisedDevice* dev) {
  if (!dev) return;
  uint32_t now = millis();
  pruneFailedPeers(now);
  String addr = String(dev->getAddress().toString().c_str());
  bool updated = false;
  for (auto& peer : gFailedPeers) {
    if (peer.address == addr) {
      peer.retryUntil = now + CONNECT_BACKOFF_MS;
      updated = true;
      break;
    }
  }
  if (!updated) {
    gFailedPeers.emplace_back(addr, now + CONNECT_BACKOFF_MS);
  }
}

void clearFailureFor(const NimBLEAdvertisedDevice* dev) {
  if (!dev || gFailedPeers.empty()) return;
  String addr = String(dev->getAddress().toString().c_str());
  gFailedPeers.erase(
      std::remove_if(gFailedPeers.begin(), gFailedPeers.end(),
                     [&addr](const FailedPeer& peer) { return peer.address == addr; }),
      gFailedPeers.end());
}

// =================== Display ===================
void drawTopNowPlaying() {
  oledTop.clearDisplay();
  oledTop.setTextColor(SSD1306_WHITE);
  oledTop.setTextSize(1);
  oledTop.setCursor(0,0);
  oledTop.println(nowArtist.length()?nowArtist:"Unknown Artist");
  oledTop.setCursor(0,16);
  if (!gLink.clientLinked) oledTop.println("Waiting link...");
  else oledTop.println(nowAlbum.length()?nowAlbum:"Unknown Album");
  oledTop.display();
}

void drawBottomScroll() {
  oledBot.clearDisplay();
  oledBot.setTextColor(SSD1306_WHITE);
  oledBot.setTextSize(2);

  int16_t x1,y1; uint16_t w,h;
  oledBot.getTextBounds(nowTitle.c_str(), 0, 0, &x1, &y1, &w, &h);

  if ((int)w > 128) {
    oledBot.setCursor(-scrollOffset, 8);
    oledBot.print(nowTitle);
    if (millis()-lastScroll > 150) {
      scrollOffset++;
      if (scrollOffset > (int)w + 24) scrollOffset = 0;
      lastScroll = millis();
    }
  } else {
    int x = (128 - (int)w)/2; if (x < 0) x = 0;
    oledBot.setCursor(x, 8);
    oledBot.print(nowTitle.length()?nowTitle:"Ready");
  }
  oledBot.display();
}

void drawBottomStatic(const char* txt) {
  oledBot.clearDisplay();
  oledBot.setTextColor(SSD1306_WHITE);
  oledBot.setTextSize(2);
  int16_t x1,y1; uint16_t w,h;
  oledBot.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  int x = (128 - (int)w)/2, y = (32 - (int)h)/2;
  if (x<0) x=0; if (y<0) y=0;
  oledBot.setCursor(x,y);
  oledBot.print(txt);
  oledBot.display();
}

void drawNotif() {
  oledTop.clearDisplay();
  oledTop.setTextColor(SSD1306_WHITE);
  oledTop.setTextSize(1);
  oledTop.setCursor(0,0);
  oledTop.println(notifApp.length()?notifApp:"Notification");
  oledTop.setCursor(0,16);
  String t = notifTitle; if (t.length()>20) t = t.substring(0,20);
  oledTop.print(t);
  oledTop.display();

  oledBot.clearDisplay();
  oledBot.setTextColor(SSD1306_WHITE);
  oledBot.setTextSize(1);
  oledBot.setCursor(0,0);
  String l1, l2;
  if (notifMsg.length() <= 21) { l1=notifMsg; l2=""; }
  else {
    l1 = notifMsg.substring(0,21);
    uint16_t endIndex = notifMsg.length()>42 ? 42 : notifMsg.length();
    l2 = notifMsg.substring(21, endIndex);
  }
  oledBot.println(l1);
  oledBot.setCursor(0,16);
  oledBot.println(l2);
  oledBot.display();
}

void refreshDisplay() {
  if (notifActive) { drawNotif(); return; }
  if (statusActive) { drawTopNowPlaying(); drawBottomStatic(statusMsg.c_str()); return; }
  drawTopNowPlaying();
  drawBottomScroll();
}

void updateSongMetrics() {
  oledBot.setTextSize(2);
  int16_t x1,y1; uint16_t w,h;
  oledBot.getTextBounds(nowTitle.c_str(), 0, 0, &x1, &y1, &w, &h);
  songPixelWidth = (int)w;
  scrollOffset = 0;
  lastScroll = millis();
}

// =================== AMS helpers ===================
enum AmsRemoteCommand : uint8_t {
  AMS_CMD_PLAY=0, AMS_CMD_PAUSE=1, AMS_CMD_TOGGLE=2, AMS_CMD_NEXT=3, AMS_CMD_PREV=4, AMS_CMD_VOL_UP=5, AMS_CMD_VOL_DOWN=6
};
enum AmsEntityId : uint8_t { AMS_ENTITY_PLAYER=0, AMS_ENTITY_QUEUE=1, AMS_ENTITY_TRACK=2 };
enum AmsTrackAttributeId : uint8_t { AMS_TRACK_ARTIST=0, AMS_TRACK_ALBUM=1, AMS_TRACK_TITLE=2 };

void amsSend(uint8_t cmd) {
  if (!gLink.amsReady || !chAmsCmd) return;
  Serial.printf("[AMS] cmd=%u\n", cmd);
  chAmsCmd->writeValue(&cmd, 1, true);
}
void amsRequestAttr(uint8_t entity, uint8_t attr, uint16_t maxLen) {
  if (!chAmsAttr) return;
  uint8_t p[4];
  p[0]=entity; p[1]=attr; p[2]=(uint8_t)(maxLen & 0xFF); p[3]=(uint8_t)((maxLen>>8)&0xFF);
  chAmsAttr->writeValue(p, sizeof(p), true);
}
void amsSubscribeTrack() {
  amsRequestAttr(AMS_ENTITY_TRACK, AMS_TRACK_TITLE, 80);
  amsRequestAttr(AMS_ENTITY_TRACK, AMS_TRACK_ARTIST, 80);
  amsRequestAttr(AMS_ENTITY_TRACK, AMS_TRACK_ALBUM, 80);
}

// =================== ANCS helpers ===================
enum AncsEventId : uint8_t { ANCS_EVENT_ADDED=0, ANCS_EVENT_MODIFIED=1, ANCS_EVENT_REMOVED=2 };
enum AncsCategoryId : uint8_t { ANCS_CATEGORY_OTHER=0, ANCS_CATEGORY_INCOMING_CALL=1 };

uint32_t currentNotifUid = 0;
uint8_t  currentNotifCategory = 0;

void ancsHandleNotificationSource(uint8_t* data, size_t len) {
  if (len < 8) return;
  uint8_t eventId = data[0];
  uint8_t categoryId = data[2];
  currentNotifCategory = categoryId;
  uint32_t uid = (uint32_t)data[4] | ((uint32_t)data[5]<<8) | ((uint32_t)data[6]<<16) | ((uint32_t)data[7]<<24);

  if (eventId==ANCS_EVENT_ADDED || eventId==ANCS_EVENT_MODIFIED) {
    currentNotifUid = uid;
    if (chAncsCtrl) {
      uint8_t p[32]; size_t idx=0;
      p[idx++]=0x00; // Get Notification Attributes
      p[idx++]=(uint8_t)(uid & 0xFF);
      p[idx++]=(uint8_t)((uid>>8)&0xFF);
      p[idx++]=(uint8_t)((uid>>16)&0xFF);
      p[idx++]=(uint8_t)((uid>>24)&0xFF);
      p[idx++]=0x00; // App Identifier
      p[idx++]=0x01; p[idx++]=0x40; p[idx++]=0x00; // Title 64
      p[idx++]=0x02; p[idx++]=0x40; p[idx++]=0x00; // Subtitle 64
      p[idx++]=0x03; p[idx++]=0x80; p[idx++]=0x00; // Message 128
      p[idx++]=0x04; p[idx++]=0x00; p[idx++]=0x00; // Message Size
      chAncsCtrl->writeValue(p, idx, true);
    }
    amsRequestAttr(AMS_ENTITY_TRACK, AMS_TRACK_TITLE, 80);
  } else if (eventId==ANCS_EVENT_REMOVED) {
    if (uid==currentNotifUid) clearNotif();
  }
}

void ancsHandleData(const uint8_t* data, size_t len) {
  if (len < 5) return;
  size_t i=0;
  uint32_t uid = (uint32_t)data[i] | ((uint32_t)data[i+1]<<8) | ((uint32_t)data[i+2]<<16) | ((uint32_t)data[i+3]<<24);
  i+=4;
  if (uid != currentNotifUid) return;

  String appId, title, subtitle, message;

  while (i+3 <= len) {
    uint8_t attrId = data[i++];
    uint16_t attrLen = (uint16_t)data[i] | ((uint16_t)data[i+1]<<8);
    i+=2;
    if (i+attrLen > len) break;
    String v = attrLen ? String((const char*)&data[i], attrLen) : String("");
    i += attrLen;

    v = cleanText(v);
    switch (attrId) {
      case 0x00: appId=v; break;
      case 0x01: title=v; break;
      case 0x02: subtitle=v; break;
      case 0x03: message=v; break;
      default: break;
    }
  }

  String displayTitle = title.length()?title:subtitle;
  if (!displayTitle.length()) {
    displayTitle = (currentNotifCategory==ANCS_CATEGORY_INCOMING_CALL) ? "Incoming Call" : "Notification";
  }
  if (currentNotifCategory==ANCS_CATEGORY_INCOMING_CALL) showNotif(displayTitle, message, appId, 0);
  else showNotif(displayTitle, message, appId, 8000);
}

// =================== Scanner scheduling ===================
void scheduleScanStart(uint32_t delayMs = 0) {
  scanStopPending = false;
  scanStartPending = true;
  scanStartAtMs = millis() + delayMs;
}
void scheduleScanStop() {
  scanStartPending = false;
  scanStopPending = true;
}

// =================== BLE Callbacks ===================
class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pClient) override {
    gLink.clientLinked = true;
    scanStartPending = false;
    scheduleScanStop();
    Serial.println("[CLIENT] Connected");
  }
  void onDisconnect(NimBLEClient* pClient, int reason) override {
    gLink.clientLinked = false;
    gLink.amsReady = false;
    gLink.ancsReady = false;
    Serial.printf("[CLIENT] Disconnected, reason=%d\n", reason);
    scheduleScanStart(600);
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    gLink.serverLinked = true;
    gPeerAddr = connInfo.getAddress();
    havePeerAddr = true;
    gFailedPeers.clear();
    Serial.printf("iPhone connected to our peripheral (handle=%u)\n", connInfo.getConnHandle());
    setStatus("Securing...", 1200);
    NimBLEDevice::startSecurity(connInfo.getConnHandle());
  }
  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    gLink.serverLinked = false;
    Serial.printf("iPhone disconnected (server), reason=%d\n", reason);
    clearNotif();
    setStatus("Advertising", 0);
    pServer->getAdvertising()->start();
  }
  void onAuthenticationComplete(NimBLEConnInfo& /*connInfo*/) override {
    Serial.println("Link encrypted. Scanning to back-connect...");
    gFailedPeers.clear();
    // Use scan (connectable + Apple mfg) to get a proper RPA + type
    scheduleScanStart(200);
  }
};

// NimBLE 2.3 signatures:
class ScanCallbacks : public NimBLEScanCallbacks {
  static bool isApple(const std::string& mfg) {
    if (mfg.size() < 2) return false;
    const uint8_t* d = (const uint8_t*)mfg.data();
    return (d[0] == 0x4C && d[1] == 0x00); // 0x004C little-endian
  }
  void onDiscovered(const NimBLEAdvertisedDevice* dev) override {
    if (!dev->isConnectable()) return;
    if (gLink.clientLinked) return;

    // Prefer exact peer address if we have it
    if (havePeerAddr && dev->getAddress().equals(gPeerAddr)) {
      Serial.println("[SCAN] Found same iPhone address; scheduling connect");
      if (gPendingDev) { delete gPendingDev; gPendingDev = nullptr; }
      gPendingDev = new NimBLEAdvertisedDevice(*dev); // COPY (keeps addr type)
      scheduleScanStop();
      connectPending = true;
      return;
    }
    // Else Apple device (likely your iPhone)
    if (dev->haveManufacturerData() && isApple(dev->getManufacturerData())) {
      if (!gLink.serverLinked && recentlyFailed(dev)) {
        Serial.printf("[SCAN] Skipping Apple device %s (recent failure)\n",
                      dev->getAddress().toString().c_str());
        return;
      }
      Serial.printf("[SCAN] Found Apple device %s; scheduling connect\n", dev->getAddress().toString().c_str());
      if (gPendingDev) { delete gPendingDev; gPendingDev = nullptr; }
      gPendingDev = new NimBLEAdvertisedDevice(*dev); // COPY
      scheduleScanStop();
      connectPending = true;
    }
  }
  void onScanEnd(const NimBLEScanResults& /*results*/, int /*reason*/) override {
    // handled in loop
  }
};

// =================== Input ===================
void calibrateCenter() {
  long sx=0, sy=0;
  for (int i=0;i<24;i++){ sx+=analogRead(JOY_X_PIN); sy+=analogRead(JOY_Y_PIN); delay(4); }
  midX=sx/24; midY=sy/24; emaX=0; emaY=0;
}
void smoothRead() {
  int rx=analogRead(JOY_X_PIN);
  int ry=analogRead(JOY_Y_PIN);
  emaX = (1.0f-EMA_ALPHA)*emaX + EMA_ALPHA*(rx-midX);
  emaY = (1.0f-EMA_ALPHA)*emaY + EMA_ALPHA*(ry-midY);
}

// =================== Setup ===================
void setup(){
  Serial.begin(115200);

  pinMode(JOY_SW_PIN, INPUT_PULLUP);
  analogReadResolution(12);

  WireTop.begin(TOP_SDA, TOP_SCL);
  WireBot.begin(BOT_SDA, BOT_SCL);

  if (!oledTop.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) Serial.println("Top OLED init failed");
  if (!oledBot.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) Serial.println("Bottom OLED init failed");

  // rotate 180°
  oledTop.setRotation(2);
  oledBot.setRotation(2);
  oledTop.clearDisplay(); oledTop.display();
  oledBot.clearDisplay(); oledBot.display();

  // Boot screen
  oledTop.setTextColor(SSD1306_WHITE);
  oledTop.setTextSize(2);
  oledTop.setCursor(16,8); oledTop.print("BIKE OS");
  oledTop.display();
  oledBot.setTextColor(SSD1306_WHITE);
  oledBot.setTextSize(1);
  oledBot.setCursor(28,10); oledBot.print("Booting...");
  oledBot.display();
  delay(700);

  // ---- NimBLE init (order matters) ----
  NimBLEDevice::init("BikeRemote");              // 1) init
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RPA_PUBLIC_DEFAULT); // use RPA identity
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setMTU(185);
  NimBLEDevice::setSecurityAuth(true,true,true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  uint8_t keyMask = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  NimBLEDevice::setSecurityInitKey(keyMask);
  NimBLEDevice::setSecurityRespKey(keyMask);

  // Server/peripheral
  gServer = NimBLEDevice::createServer();
  gServer->setCallbacks(new ServerCallbacks());

  // Advertise
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName("BikeRemote");
  adv->addServiceUUID(UUID_ANCS_SVC);
  adv->addServiceUUID(UUID_AMS_SVC);
  adv->start();

  // Scanner for client back-connect
  gScan = NimBLEDevice::getScan();
  gScan->setScanCallbacks(new ScanCallbacks(), false);
  gScan->setInterval(45);
  gScan->setWindow(30);
  gScan->setActiveScan(true);

  calibrateCenter();
  setStatus("Advertising", 0);
  refreshDisplay();

  // Kick off scanning after boot
  scheduleScanStart(400);
}

// =================== Loop ===================
void loop(){
  uint32_t now = millis();

  // Expire overlays
  if (notifActive && notifUntil && now > notifUntil) clearNotif();
  if (statusActive && statusUntil && now > statusUntil) clearStatus();

  // Safe scan stop/start outside callbacks
  if (scanStopPending) {
    scanStopPending = false;
    if (gScan->isScanning()) gScan->stop();
  }
  if (scanStartPending && now >= scanStartAtMs) {
    scanStartPending = false;
    if (!gScan->isScanning()) { Serial.println("[SCAN] start"); gScan->start(0, false); }
  }

  // Handle requested client connect using the COPIED advertised device
  if (connectPending) {
    connectPending = false;

    if (!gPendingDev) {
      // nothing to connect to; rescan
      scheduleScanStart(300);
    } else {
      if (!gClient) {
        gClient = NimBLEDevice::createClient();
        gClient->setClientCallbacks(new ClientCallbacks());
        gClient->setConnectionParams(24, 40, 0, 100);
        gClient->setConnectTimeout(10);
      }
      if (gClient->isConnected()) gClient->disconnect();

      Serial.printf("Connecting back to iPhone at %s ...\n", gPendingDev->getAddress().toString().c_str());
      bool ok = gClient->connect(gPendingDev); // preserves addr TYPE (public/random)
      if (ok) {
        Serial.println("[CLIENT] Connected; discovering AMS/ANCS...");

        clearFailureFor(gPendingDev);

        gLink.clientLinked = true;
        gLink.amsReady = gLink.ancsReady = false;
        chAmsCmd = chAmsUpd = chAmsAttr = nullptr;
        chAncsCtrl = chAncsData = chAncsSrc = nullptr;

        // AMS
        if (auto svc = gClient->getService(UUID_AMS_SVC)) {
          chAmsCmd  = svc->getCharacteristic(UUID_AMS_REMOTE_COMMAND);
          chAmsUpd  = svc->getCharacteristic(UUID_AMS_ENTITY_UPDATE);
          chAmsAttr = svc->getCharacteristic(UUID_AMS_ENTITY_ATTRIBUTE);
          if (chAmsUpd && chAmsUpd->canNotify()) {
            chAmsUpd->subscribe(true, [](NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
              if (len < 3) return;
              uint8_t entity = data[0], attr=data[1];
              String value = (len>2)? String((const char*)&data[2], len-2):String("");
              value = cleanText(value);
              if (entity==AMS_ENTITY_TRACK) {
                if (attr==AMS_TRACK_ARTIST) { if (nowArtist!=value){ nowArtist=value; refreshDisplay(); } }
                else if (attr==AMS_TRACK_ALBUM) { if (nowAlbum!=value){ nowAlbum=value; refreshDisplay(); } }
                else if (attr==AMS_TRACK_TITLE) { if (nowTitle!=value){ nowTitle=value; updateSongMetrics(); refreshDisplay(); } }
              }
            });
          }
          if (chAmsAttr) amsSubscribeTrack();
          gLink.amsReady = (chAmsCmd != nullptr);
        }

        // ANCS
        if (auto svcN = gClient->getService(UUID_ANCS_SVC)) {
          chAncsSrc  = svcN->getCharacteristic(UUID_ANCS_NOTIFICATION_SOURCE);
          chAncsCtrl = svcN->getCharacteristic(UUID_ANCS_CONTROL_POINT);
          chAncsData = svcN->getCharacteristic(UUID_ANCS_DATA_SOURCE);
          if (chAncsSrc && chAncsSrc->canNotify()) {
            chAncsSrc->subscribe(true, [](NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
              ancsHandleNotificationSource(data, len);
            });
          }
          if (chAncsData && chAncsData->canNotify()) {
            chAncsData->subscribe(true, [](NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
              ancsHandleData(data, len);
            });
          }
          gLink.ancsReady = (chAncsCtrl && chAncsData);
        }

        Serial.printf("[CLIENT] Ready AMS:%d ANCS:%d\n", gLink.amsReady, gLink.ancsReady);
        setStatus("iPhone linked", 1200);
        refreshDisplay();
      } else {
        Serial.println("Back-connect failed; will scan");
        rememberFailure(gPendingDev);
        scheduleScanStart(400);
      }
      delete gPendingDev; gPendingDev=nullptr;
    }
  }

  // Inputs
  smoothRead();

  // Button: short press = toggle, long press clears bonds and restarts
  bool btnDown = (digitalRead(JOY_SW_PIN) == LOW);
  if (btnDown && !btnWasDown) btnDownAt = now;
  if (btnDown && (now - btnDownAt > BOND_CLEAR_HOLD_MS)) {
    Serial.println("Long press: Clearing bonds + restart");
    NimBLEDevice::deleteAllBonds();
    setStatus("Bonds cleared", 1200);
    refreshDisplay();
    delay(1200);
    ESP.restart();
  }
  if (btnDown && (now - tBtn > 280)) {
    if (!btnWasDown) {
      tBtn = now;
      amsSend(AMS_CMD_TOGGLE);
      setStatus("Play/Pause", 800);
      refreshDisplay();
    }
  }
  btnWasDown = btnDown;

  // Vol +/- (Y)
  if (!inDZ(emaY) && beyond(emaY)) {
    if (emaY < 0 && now - tUp > VOL_REPEAT_MS) {
      amsSend(AMS_CMD_VOL_UP);
      setStatus("Vol +", 600);
      refreshDisplay();
      tUp = now;
    } else if (emaY > 0 && now - tDn > VOL_REPEAT_MS) {
      amsSend(AMS_CMD_VOL_DOWN);
      setStatus("Vol -", 600);
      refreshDisplay();
      tDn = now;
    }
  }

  // Next/Prev (X)
  if (!inDZ(emaX) && beyond(emaX)) {
    if (emaX < 0 && now - tR > ACTION_COOLDOWN) {
      amsSend(AMS_CMD_NEXT);
      setStatus("Next >", 800);
      refreshDisplay();
      tR = now;
    } else if (emaX > 0 && now - tL > ACTION_COOLDOWN) {
      amsSend(AMS_CMD_PREV);
      setStatus("< Prev", 800);
      refreshDisplay();
      tL = now;
    }
  }

  // Scroll song if no overlays
  if (!notifActive && !statusActive) {
    if (songPixelWidth > 128 && (now - lastScroll > 150)) {
      scrollOffset++;
      if (scrollOffset > songPixelWidth + 24) scrollOffset = 0;
      lastScroll = now;
    }
  }

  // Periodic diagnostics (single ticker)
  if (now - gDiagTick > 2000) {
    gDiagTick = now;
    Serial.printf("[DIAG] srv=%d cli=%d ams=%d ancs=%d scan=%d\n",
      gLink.serverLinked?1:0, gLink.clientLinked?1:0,
      gLink.amsReady?1:0, gLink.ancsReady?1:0,
      gScan->isScanning()?1:0);
  }

  refreshDisplay();
  delay(6);
}
