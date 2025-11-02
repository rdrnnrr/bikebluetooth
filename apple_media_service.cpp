#include "apple_media_service.h"

#include <BLEClient.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLESecurity.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace AppleMediaService {
namespace {

constexpr char kServiceUuid[] = "89D3502B-0F36-433A-8EF4-C502AD55F8DC";
constexpr char kRemoteCommandUuid[] = "9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2";
constexpr char kEntityUpdateUuid[] = "2F7CABCE-808D-411F-9A0C-BB92BA96C102";
constexpr char kEntityAttributeUuid[] = "C6B2F38C-23AB-46D8-A6AB-A3A870BBD5D7";

constexpr uint8_t kEntityPlayer = 0;
constexpr uint8_t kEntityQueue = 1;
constexpr uint8_t kEntityTrack = 2;

constexpr uint8_t kPlayerAttrName = 0;
constexpr uint8_t kPlayerAttrPlaybackInfo = 1;
constexpr uint8_t kPlayerAttrVolume = 2;

constexpr uint8_t kQueueAttrIndex = 0;
constexpr uint8_t kQueueAttrCount = 1;
constexpr uint8_t kQueueAttrShuffleMode = 2;
constexpr uint8_t kQueueAttrRepeatMode = 3;

constexpr uint8_t kTrackAttrArtist = 0;
constexpr uint8_t kTrackAttrAlbum = 1;
constexpr uint8_t kTrackAttrTitle = 2;
constexpr uint8_t kTrackAttrDuration = 3;

constexpr uint16_t kCccdUuid = 0x2902;

BLEClient *g_client = nullptr;
BLESecurity *g_security = nullptr;
BLERemoteCharacteristic *g_remoteCommand = nullptr;
BLERemoteCharacteristic *g_entityUpdate = nullptr;
BLERemoteCharacteristic *g_entityAttribute = nullptr;
UpdateCallback g_callback = nullptr;
MediaStatus g_status;
MediaStatus g_previousStatus;
bool g_hasPrevious = false;

// Forward declaration
void handleEntityUpdate(BLERemoteCharacteristic *characteristic, uint8_t *data, size_t length, bool isNotify);

void resetState() {
  g_remoteCommand = nullptr;
  g_entityUpdate = nullptr;
  g_entityAttribute = nullptr;
  g_callback = nullptr;
  g_hasPrevious = false;
  g_status = MediaStatus{};
  g_previousStatus = MediaStatus{};
}

void cleanupClient() {
  if (g_entityUpdate) {
    g_entityUpdate->registerForNotify(nullptr);
  }
  if (g_client) {
    if (g_client->isConnected()) {
      g_client->disconnect();
    }
    delete g_client;
    g_client = nullptr;
  }
  if (g_security) {
    delete g_security;
    g_security = nullptr;
  }
  resetState();
}

String stringFromPayload(const uint8_t *data, size_t length) {
  if (length == 0) {
    return String();
  }
  std::string temp(reinterpret_cast<const char *>(data), length);
  return String(temp.c_str());
}

void parsePlaybackInfo(const String &value, MediaStatus &status) {
  int firstComma = value.indexOf(',');
  if (firstComma < 0) {
    return;
  }
  int secondComma = value.indexOf(',', firstComma + 1);
  if (secondComma < 0) {
    return;
  }

  String stateStr = value.substring(0, firstComma);
  String rateStr = value.substring(firstComma + 1, secondComma);
  String elapsedStr = value.substring(secondComma + 1);

  int state = stateStr.toInt();
  status.playing = (state == 1);
  status.rewinding = (state == 2);
  status.fastForwarding = (state == 3);
  status.playbackRate = rateStr.toFloat();
  status.elapsedTimeSeconds = elapsedStr.toFloat();
}

void handleEntityUpdate(BLERemoteCharacteristic *, uint8_t *data, size_t length, bool) {
  if (length < 3) {
    return;
  }

  const uint8_t entity = data[0];
  const uint8_t attribute = data[1];
  const uint8_t /*flags*/ unusedFlags = data[2];
  (void)unusedFlags;
  const String value = stringFromPayload(data + 3, length - 3);

  bool playbackChanged = false;
  bool trackChanged = false;
  bool queueChanged = false;

  switch (entity) {
    case kEntityPlayer:
      switch (attribute) {
        case kPlayerAttrName:
          if (g_status.playerName != value) {
            g_status.playerName = value;
          }
          break;
        case kPlayerAttrPlaybackInfo: {
          bool oldPlaying = g_status.playing;
          bool oldRewinding = g_status.rewinding;
          bool oldFastForwarding = g_status.fastForwarding;
          float oldRate = g_status.playbackRate;
          float oldElapsed = g_status.elapsedTimeSeconds;

          parsePlaybackInfo(value, g_status);
          playbackChanged = playbackChanged || (oldPlaying != g_status.playing) ||
                            (oldRewinding != g_status.rewinding) ||
                            (oldFastForwarding != g_status.fastForwarding) ||
                            (fabsf(oldRate - g_status.playbackRate) > 0.001f) ||
                            (fabsf(oldElapsed - g_status.elapsedTimeSeconds) > 0.001f);
          break;
        }
        case kPlayerAttrVolume: {
          float oldVolume = g_status.volume;
          g_status.volume = value.toFloat();
          playbackChanged = playbackChanged || (fabsf(oldVolume - g_status.volume) > 0.001f);
          break;
        }
        default:
          break;
      }
      break;

    case kEntityQueue:
      switch (attribute) {
        case kQueueAttrIndex: {
          uint8_t newIndex = static_cast<uint8_t>(value.toInt());
          queueChanged = queueChanged || (newIndex != g_status.queueIndex);
          g_status.queueIndex = newIndex;
          break;
        }
        case kQueueAttrCount: {
          uint8_t newCount = static_cast<uint8_t>(value.toInt());
          queueChanged = queueChanged || (newCount != g_status.queueCount);
          g_status.queueCount = newCount;
          break;
        }
        case kQueueAttrShuffleMode: {
          uint8_t newShuffle = static_cast<uint8_t>(value.toInt());
          queueChanged = queueChanged || (newShuffle != g_status.shuffleMode);
          g_status.shuffleMode = newShuffle;
          break;
        }
        case kQueueAttrRepeatMode: {
          uint8_t newRepeat = static_cast<uint8_t>(value.toInt());
          queueChanged = queueChanged || (newRepeat != g_status.repeatMode);
          g_status.repeatMode = newRepeat;
          break;
        }
        default:
          break;
      }
      break;

    case kEntityTrack:
      switch (attribute) {
        case kTrackAttrArtist:
          if (g_status.artist != value) {
            g_status.artist = value;
            trackChanged = true;
          }
          break;
        case kTrackAttrAlbum:
          if (g_status.album != value) {
            g_status.album = value;
            trackChanged = true;
          }
          break;
        case kTrackAttrTitle:
          if (g_status.title != value) {
            g_status.title = value;
            trackChanged = true;
          }
          break;
        case kTrackAttrDuration: {
          float newDuration = value.toFloat();
          trackChanged = trackChanged || (fabsf(newDuration - g_status.durationSeconds) > 0.001f);
          g_status.durationSeconds = newDuration;
          break;
        }
        default:
          break;
      }
      break;

    default:
      break;
  }

  UpdateFlags flags{};
  flags.playbackChanged = playbackChanged;
  flags.queueChanged = queueChanged;
  flags.trackChanged = trackChanged;

  if (!g_hasPrevious) {
    flags.playbackChanged = true;
    flags.queueChanged = true;
    flags.trackChanged = true;
  }

  if (g_callback && (flags.playbackChanged || flags.queueChanged || flags.trackChanged)) {
    g_callback(g_status, flags);
  }

  g_previousStatus = g_status;
  g_hasPrevious = true;
}

bool enableNotifications(BLERemoteCharacteristic *characteristic) {
  if (!characteristic) {
    return false;
  }
  auto descriptor = characteristic->getDescriptor(BLEUUID((uint16_t)kCccdUuid));
  if (!descriptor) {
    return false;
  }
  uint8_t enableNotify[] = {0x01, 0x00};
  descriptor->writeValue(enableNotify, sizeof(enableNotify), true);
  return true;
}

}  // namespace

bool start(const BLEAddress &address, UpdateCallback callback) {
  stop();

  g_client = BLEDevice::createClient();
  if (!g_client) {
    return false;
  }

  g_security = new BLESecurity();
  g_security->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
  g_security->setCapability(ESP_IO_CAP_IO);
  g_security->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  if (!g_client->connect(address)) {
    cleanupClient();
    return false;
  }

  auto service = g_client->getService(BLEUUID(kServiceUuid));
  if (!service) {
    cleanupClient();
    return false;
  }

  g_remoteCommand = service->getCharacteristic(BLEUUID(kRemoteCommandUuid));
  g_entityUpdate = service->getCharacteristic(BLEUUID(kEntityUpdateUuid));
  g_entityAttribute = service->getCharacteristic(BLEUUID(kEntityAttributeUuid));

  if (!g_remoteCommand || !g_entityUpdate || !g_entityAttribute) {
    cleanupClient();
    return false;
  }

  if (!enableNotifications(g_entityUpdate)) {
    cleanupClient();
    return false;
  }

  g_entityUpdate->registerForNotify(handleEntityUpdate);

  g_callback = callback;

  // Subscribe to player, queue, and track attributes.
  uint8_t playerSubscription[] = {kEntityPlayer, kPlayerAttrName, kPlayerAttrPlaybackInfo, kPlayerAttrVolume};
  uint8_t queueSubscription[] = {kEntityQueue, kQueueAttrIndex, kQueueAttrCount, kQueueAttrShuffleMode, kQueueAttrRepeatMode};
  uint8_t trackSubscription[] = {kEntityTrack, kTrackAttrArtist, kTrackAttrAlbum, kTrackAttrTitle, kTrackAttrDuration};

  g_entityUpdate->writeValue(playerSubscription, sizeof(playerSubscription), true);
  g_entityUpdate->writeValue(queueSubscription, sizeof(queueSubscription), true);
  g_entityUpdate->writeValue(trackSubscription, sizeof(trackSubscription), true);

  return true;
}

void stop() {
  cleanupClient();
}

bool isActive() {
  return g_client != nullptr && g_client->isConnected();
}

bool sendCommand(RemoteCommandID command) {
  if (!g_remoteCommand) {
    return false;
  }
  uint8_t cmd = static_cast<uint8_t>(command);
  g_remoteCommand->writeValue(&cmd, sizeof(cmd), true);
  return true;
}

}  // namespace AppleMediaService
