/*
 * BikeHID — ESP32 + NimBLE-Arduino (Arduino-ESP32 3.3.x)
 * Joystick HID media remote with auto-range calibration.
 *
 * Controls:
 * - Button short: Play/Pause
 * - Button long (~700ms): Voice (Siri)
 * - X left/right: Prev / Next
 * - Y up/down:    Vol- / Vol+
 */

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <math.h>

// ---------------- Pins ----------------
#define JOY_X_PIN 35
#define JOY_Y_PIN 34
#define JOY_SW_PIN 32   // active LOW (INPUT_PULLUP)

// ---------------- Options ----------------
#define INVERT_X    1   // *** FIX: Changed to 1 based on log (Left movement = -1.00) ***
#define INVERT_Y    0
#define DEBUG_PRINT 0   // SET BACK TO 0 FOR NORMAL USE

// Raw jitter control
const float EMA_ALPHA        = 0.18f;
const int   DEADZONE_RAW     = 350;

// % thresholds relative to learned travel
const float THRESH_PCT_X     = 0.30f;
const float THRESH_PCT_Y     = 0.30f;
const float RELEASE_PCT      = 0.18f;

// Timing
const uint16_t ARM_DWELL_MS       = 50;
const uint16_t VOL_REPEAT_MS      = 150;
const uint16_t ACTION_COOLDOWN_MS = 240;

// Button filters
const uint16_t BTN_DEBOUNCE_MS       = 25;
const uint16_t BTN_MIN_LOW_MS        = 10;
const uint8_t  BTN_SAMPLE_NS         = 6;
const uint16_t BTN_SAMPLE_SPACING_MS = 1;

// Cross-input brakes
const uint16_t MUTEX_XY_AFTER_BTN_MS  = 350;
const uint16_t MUTEX_BTN_AFTER_XY_MS  = 300;

// Neutral requirement (light)
const float    QUIET_PCT           = 0.15f;
const uint16_t QUIET_BEFORE_ARM_MS = 20;

// Auto-center + auto-span
const float TRIM_BAND_PCT = 0.05f;
const float TRIM_ALPHA    = 0.002f;
const float PEAK_DECAY    = 0.995f;
const float PEAK_FLOOR    = 220.0f;

// Long-press -> Siri
const uint32_t SIRI_HOLD_MS = 700;

// HID usages (Consumer Page 0x0C)
#define CC_PLAY_PAUSE  0x00CD
#define CC_SCAN_NEXT   0x00B5
#define CC_SCAN_PREV   0x00B6
#define CC_VOL_UP      0x00E9
#define CC_VOL_DOWN    0x00EA
#define CC_VOICE_CMD   0x029C

// ---------------- State ----------------
float emaX=0, emaY=0, nx=0, ny=0;
int midX=2048, midY=2048, lastRx=2048, lastRy=2048;
float peakX=400, peakY=400;

uint32_t tUp=0, tDn=0, tL=0, tR=0;
uint32_t lastBtnActionAt=0, lastXYActionAt=0;

bool btnWasDown=false, btnEligible=false; // btnEligible will be treated as always true
uint32_t btnLowStart=0, btnDownAt=0;

bool volArmed=false; uint32_t volArmStart=0; bool yUpHold=false, yDnHold=false;
bool xArmed=false;  uint32_t xArmStart=0;

uint32_t quietStart=0;

NimBLEHIDDevice* gHid=nullptr;
NimBLECharacteristic* gInputReport=nullptr;
NimBLECharacteristic* gBattChar=nullptr;

// ---------------- Helpers ----------------
static inline bool inRawDZ(int v){ return abs(v) < DEADZONE_RAW; }
static inline float clamp1(float v){ return v<-1? -1 : (v>1? 1 : v); }

void calibrateCenter(){
  long sx=0, sy=0; for(int i=0;i<24;i++){ sx+=analogRead(JOY_X_PIN); sy+=analogRead(JOY_Y_PIN); delay(4); }
  midX=sx/24; midY=sy/24; emaX=emaY=0;
}

void smoothAndNormalize(){
  int rx=analogRead(JOY_X_PIN), ry=analogRead(JOY_Y_PIN);
  lastRx=rx; lastRy=ry;

  int dx=rx-midX, dy=ry-midY;
  if(inRawDZ(dx)) dx=0;
  if(inRawDZ(dy)) dy=0;

  emaX=(1-EMA_ALPHA)*emaX + EMA_ALPHA*(float)dx;
  emaY=(1-EMA_ALPHA)*emaY + EMA_ALPHA*(float)dy;

  // Learn span via decaying peak
  float ax=fabsf(emaX), ay=fabsf(emaY);
  peakX = fmaxf(peakX*PEAK_DECAY, ax);
  peakY = fmaxf(peakY*PEAK_DECAY, ay);
  if (peakX < PEAK_FLOOR) peakX = PEAK_FLOOR;
  if (peakY < PEAK_FLOOR) peakY = PEAK_FLOOR;

  // Normalize to learned span
  nx = clamp1(emaX / peakX);
  ny = clamp1(emaY / peakY);
#if INVERT_X
  nx = -nx; // *** Now inverted for correct direction ***
#endif
#if INVERT_Y
  ny = -ny;
#endif

  // Quiet gate timing
  if (fabsf(nx)<=QUIET_PCT && fabsf(ny)<=QUIET_PCT) {
    if (!quietStart) quietStart=millis();
  } else quietStart=0;

  // Slow auto-center when calm & idle
  if (quietStart && (millis()-quietStart)>250 &&
      fabsf(nx)<=TRIM_BAND_PCT && fabsf(ny)<=TRIM_BAND_PCT &&
      !volArmed && !xArmed &&
      (millis()-lastBtnActionAt>400) && (millis()-lastXYActionAt>400)) {
    midX = (int)((1.0f-TRIM_ALPHA)*midX + TRIM_ALPHA*lastRx);
    midY = (int)((1.0f-TRIM_ALPHA)*midY + TRIM_ALPHA*lastRy);
  }
}

// Majority-sampled, debounced, min-low button
bool sampleButtonRawLow(){
  uint8_t lows=0;
  for(uint8_t i=0;i<BTN_SAMPLE_NS;i++){
    lows += (digitalRead(JOY_SW_PIN)==LOW);
    delay(BTN_SAMPLE_SPACING_MS);
  }
  return lows > (BTN_SAMPLE_NS/2);
}
bool readButtonStable(uint32_t now){
  static bool stableLow=false, lastStableLow=false; static uint32_t lastEdgeAt=0;
  bool lowNow = sampleButtonRawLow();
  if (lowNow != stableLow){ stableLow = lowNow; lastEdgeAt = now; }
  if (now - lastEdgeAt < BTN_DEBOUNCE_MS) return lastStableLow;

  if (stableLow){
    if (!btnLowStart) btnLowStart = now;
    if (now - btnLowStart >= BTN_MIN_LOW_MS) { lastStableLow=true; return true; }
    return false;
  } else { btnLowStart=0; lastStableLow=false; return false; }
}

void sendConsumer(uint16_t usage){
  if(!gInputReport) return;
  uint8_t m[2]={(uint8_t)(usage&0xFF),(uint8_t)((usage>>8)&0xFF)};
  gInputReport->setValue(m,2); gInputReport->notify();
  // We need a slight delay to ensure the OS registers the key-down before key-up
  delay(10);
  m[0]=0; m[1]=0; gInputReport->setValue(m,2); gInputReport->notify();
}

// ---------------- GAP callbacks ----------------
class ServerCallbacks: public NimBLEServerCallbacks{
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
    Serial.printf("central connected, handle=%u\n", info.getConnHandle());
    NimBLEDevice::startSecurity(info.getConnHandle());
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    Serial.printf("central disconnected, reason=%d\n", reason);
    NimBLEDevice::getAdvertising()->start();
  }
  void onAuthenticationComplete(NimBLEConnInfo&) override { Serial.println("bonded/encrypted"); }
};

// ---------------- Setup ----------------
void setup(){
  Serial.begin(115200);
  pinMode(JOY_SW_PIN, INPUT_PULLUP);
  analogReadResolution(12);
  calibrateCenter();

  NimBLEDevice::init("BikeHID");
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RPA_PUBLIC_DEFAULT);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setMTU(185);
  NimBLEDevice::setSecurityAuth(true,false,true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  uint8_t keyMask=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;
  NimBLEDevice::setSecurityInitKey(keyMask);
  NimBLEDevice::setSecurityRespKey(keyMask);

  NimBLEServer* server=NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  // DIS
  NimBLEService* dis=server->createService("180A");
  dis->createCharacteristic("2A29", NIMBLE_PROPERTY::READ)->setValue("BikeRemote");
  dis->createCharacteristic("2A24", NIMBLE_PROPERTY::READ)->setValue("Model-CC");
  dis->start();

  // Battery (optional)
  NimBLEService* batt=server->createService("180F");
  gBattChar=batt->createCharacteristic("2A19", NIMBLE_PROPERTY::READ|NIMBLE_PROPERTY::NOTIFY);
  uint8_t lvl=100; gBattChar->setValue(&lvl,1);
  batt->start();

  // HID
  gHid=new NimBLEHIDDevice(server);
  gHid->setManufacturer("BikeRemote");
  gHid->setHidInfo(0x00,0x00);

  const uint8_t reportMap[]={
    0x05,0x0C, 0x09,0x01, 0xA1,0x01,        // Consumer Control
    0x85,0x01, 0x15,0x00, 0x26,0x9C,0x02,   // Report ID 1, Logical Max 0x29C
    0x19,0x00, 0x2A,0x9C,0x02,
    0x95,0x01, 0x75,0x10, 0x81,0x00,        // 16-bit input
    0xC0
  };
  gHid->setReportMap((uint8_t*)reportMap, sizeof(reportMap));

  gInputReport=gHid->getInputReport(1);
  if(gInputReport){
    NimBLEDescriptor* repRef=gInputReport->createDescriptor("2908", NIMBLE_PROPERTY::READ);
    const char ref[2]={0x01,0x01}; repRef->setValue(std::string(ref,2));
  }

  gHid->startServices();

  NimBLEAdvertising* adv=NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setFlags(0x06);
  advData.setAppearance(0x03C0);
  advData.setName("BikeHID");
  advData.addServiceUUID(gHid->getHidService()->getUUID());
  advData.addServiceUUID("180F");
  adv->setAdvertisementData(advData);
  adv->start();

  Serial.println("Advertising as BikeHID (HID Consumer Control)");
}

// ---------------- Loop ----------------
void loop(){
  uint32_t now=millis();
  smoothAndNormalize();

  // Button neutral gate
  bool quietOK = (quietStart && (now-quietStart)>=QUIET_BEFORE_ARM_MS);
  // *** FIX: Always allow button actions (btnEligible=true) unless cleared by a specific action
  if (!btnWasDown) btnEligible = true; // Set to true only if button is not currently down

  // Stable button (and block soon after XY)
  bool btnLowStable = readButtonStable(now);
  bool btnDown = btnLowStable;
  if (btnDown && !btnWasDown) btnDownAt=now;

  // Long-press -> Siri
  static bool siriFired=false;
  if (btnDown && !siriFired && (now-btnDownAt>=SIRI_HOLD_MS) && // Removed btnEligible check
      (now - lastXYActionAt >= MUTEX_BTN_AFTER_XY_MS)) {
    Serial.println("[BTN] Voice Command (Siri)");
    sendConsumer(CC_VOICE_CMD);
    lastBtnActionAt = now;
    siriFired=true;
  }
  if (!btnDown) siriFired=false;

  // Release -> short press (Play/Pause)
  if (!btnDown && btnWasDown) {
    if ((now - btnDownAt) < SIRI_HOLD_MS &&
        !siriFired) { // Removed btnEligible check
      Serial.println("[BTN] Play/Pause");
      sendConsumer(CC_PLAY_PAUSE);
      lastBtnActionAt=now;
    }
    btnEligible=false; 
  }
  btnWasDown = btnDown;

  bool allowXY = (now - lastBtnActionAt >= MUTEX_XY_AFTER_BTN_MS);

  // -------- Volume (Y) --------
  float ay=fabsf(ny);
  if (!volArmed) {
    if (allowXY && ay>=THRESH_PCT_Y) {
      if (!volArmStart) volArmStart=now;
      if (now - volArmStart >= ARM_DWELL_MS) volArmed=true;
    } else volArmStart=0;
  }
  if (volArmed && ay<=RELEASE_PCT) { volArmed=false; volArmStart=0; yUpHold=yDnHold=false; }

  if (allowXY && volArmed) {
    if (ny>0) {
      yDnHold=false;
      if (!yUpHold || (now - tUp >= VOL_REPEAT_MS)) {
        Serial.println("[JOY Y] Vol+");
        sendConsumer(CC_VOL_UP);
        yUpHold=true; tUp=now; lastXYActionAt=now;
      }
    } else if (ny<0) {
      yUpHold=false;
      if (!yDnHold || (now - tDn >= VOL_REPEAT_MS)) {
        Serial.println("[JOY Y] Vol-");
        sendConsumer(CC_VOL_DOWN);
        yDnHold=true; tDn=now; lastXYActionAt=now;
      }
    }
  }

  // -------- Prev/Next (X) --------
  float ax=fabsf(nx);
  if (!xArmed) {
    if (allowXY && ax>=THRESH_PCT_X && ay<=RELEASE_PCT && !btnDown) { 
      if (!xArmStart) xArmStart=now;
      if (now - xArmStart >= ARM_DWELL_MS) xArmed=true;
    } else xArmStart=0;
  }
  if (xArmed && ax<=RELEASE_PCT) { xArmed=false; xArmStart=0; }

  if (allowXY && xArmed) {
    if (nx<0 && (now - tL >= ACTION_COOLDOWN_MS)) {
      Serial.println("[JOY X] Prev");
      sendConsumer(CC_SCAN_PREV);
      tL=now; lastXYActionAt=now;
    } else if (nx>0 && (now - tR >= ACTION_COOLDOWN_MS)) {
      Serial.println("[JOY X] Next");
      sendConsumer(CC_SCAN_NEXT);
      tR=now; lastXYActionAt=now;
    }
  }

#if DEBUG_PRINT
  static uint32_t dbg=0;
  if (now-dbg>250){ dbg=now;
    Serial.printf("nx=%.2f ny=%.2f peakX=%.1f peakY=%.1f quiet=%d | btnDown=%d btnEligible=%d siriFired=%d\n",
      nx,ny,peakX,peakY,(int)(quietStart!=0), (int)btnDown, (int)btnEligible, (int)siriFired);
  }
#endif

  delay(5);
}