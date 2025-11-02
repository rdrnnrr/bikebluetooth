
#include "ams_ble_client.h"

#include "BLEAddress.h"
#include "BLEDevice.h"
#include "BLEUtils.h"
#include "BLE2902.h"

#include "esp_log.h"

static char LOG_TAG[] = "AMSBLEClient";

static AMSBLEClient *sharedInstance;

const BLEUUID AMSService(AMSService_UUID);
const BLEUUID AMSCharacteristic_Remote_Command(AMSCharacteristic_Remote_Command_UUID);
const BLEUUID AMSCharacteristic_Entity_Update(AMSCharacteristic_Entity_Update_UUID);
const BLEUUID AMSCharacteristic_Entity_Attribute(AMSCharacteristic_Entity_Attribute_UUID);

static const char *amsEntityIDToString(AMSEntityID_t id)
{
    switch (id)
    {
    case AMSEntityIDPlayer:
        return "AMSEntityIDPlayer";
    case AMSEntityIDQueue:
        return "AMSEntityIDQueue";
    case AMSEntityIDTrack:
        return "AMSEntityIDTrack";
    default:
        return "AMSEntityID???";
    }
}

static const char *amsTrackAttributeIDToString(AMSTrackAttributeID_t id)
{
    switch (id)
    {
    case AMSTrackAttributeIDAlbum:
        return "AMSTrackAttributeIDAlbum";
    case AMSTrackAttributeIDArtist:
        return "AMSTrackAttributeIDArtist";
    case AMSTrackAttributeIDDuration:
        return "AMSTrackAttributeIDDuration";
    case AMSTrackAttributeIDTitle:
        return "AMSTrackAttributeIDTitle";
    default:
        return "AMSTrackAttributeID???";
    }
}

static const char *amsPlayerAttributeIDToString(AMSPlayerAttributeID_t id)
{
    switch (id)
    {
    case AMSPlayerAttributeIDName:
        return "AMSPlayerAttributeIDName";
    case AMSPlayerAttributeIDPlaybackInfo:
        return "AMSPlayerAttributeIDPlaybackInfo";
    case AMSPlayerAttributeIDVolume:
        return "AMSPlayerAttributeIDVolume";
    default:
        return "AMSPlayerAttributeID???";
    }
}

static const char *amsRemoteCommandIDToString(AMSRemoteCommandID_t id)
{
    switch (id)
    {
    case AMSRemoteCommandIDPlay:
        return "Play";
    case AMSRemoteCommandIDPause:
        return "Pause";
    case AMSRemoteCommandIDTogglePlayPause:
        return "TogglePlayPause";
    case AMSRemoteCommandIDNextTrack:
        return "NextTrack";
    case AMSRemoteCommandIDPreviousTrack:
        return "PreviousTrack";
    case AMSRemoteCommandIDVolumeUp:
        return "VolumeUp";
    case AMSRemoteCommandIDVolumeDown:
        return "VolumeDown";
    case AMSRemoteCommandIDAdvanceRepeatMode:
        return "AdvanceRepeatMode";
    case AMSRemoteCommandIDAdvanceShuffleMode:
        return "AdvanceShuffleMode";
    case AMSRemoteCommandIDSkipForward:
        return "SkipForward";
    case AMSRemoteCommandIDSkipBackward:
        return "SkipBackward";
    case AMSRemoteCommandIDLikeTrack:
        return "LikeTrack";
    case AMSRemoteCommandIDDislikeTrack:
        return "DislikeTrack";
    case AMSRemoteCommandIDBookmarkTrack:
        return "BookmarkTrack";
    default:
        return "AMSRemoteCommandID???";
    }
}

static void amsEntityUpdateNotifyCallback(
    BLERemoteCharacteristic *pDataSourceCharacteristic,
    uint8_t *pData,
    size_t length,
    bool isNotify)
{
    AMSEntityUpdateNotification_t *data = (AMSEntityUpdateNotification_t *)pData;
    std::string value = std::string((const char *)(pData + 3), length - 3);
    ESP_LOGI(LOG_TAG, "amsEntityUpdateNotifyCallback (%d) %s/%s (%x): %s", length,
             amsEntityIDToString((AMSEntityID_t)data->EntityID),
             data->EntityID == AMSEntityIDPlayer
                 ? amsPlayerAttributeIDToString((AMSPlayerAttributeID_t)data->AttributeID)
                 : amsTrackAttributeIDToString((AMSTrackAttributeID_t)data->AttributeID),
             data->EntityUpdateFlags,
             value.c_str());
    sharedInstance->onEntityUpdateNotification(data, value);
}

static void amsRemoteCommandNotifyCallback(
    BLERemoteCharacteristic *pNotificationSourceCharacteristic,
    uint8_t *pData,
    size_t length,
    bool isNotify)
{
    ESP_LOGI(LOG_TAG, "amsRemoteCommandNotifyCallback (%d commands)", length);
    for (int i = 0; i < length; i++)
        ESP_LOGD(LOG_TAG, "     cmd: %s", amsRemoteCommandIDToString((AMSRemoteCommandID_t)pData[i]));
    uint32_t commandBitMask = 0;
    for (int i = 0; i < length; i++)
    {
        assert(pData[i] < 32);
        commandBitMask |= (1 << pData[i]);
    }
    sharedInstance->setAvailableCommands(commandBitMask);
}

AMSBLEClient::AMSBLEClient()
{
    assert(sharedInstance == nullptr);
    sharedInstance = this;
}

AMSBLEClient::~AMSBLEClient()
{
    sharedInstance = nullptr;
}

void AMSBLEClient::setup(BLEClient *pClient)
{
    ESP_LOGD(LOG_TAG, "setup");

    BLERemoteService *pAmsService = pClient->getService(AMSService);
    if (pAmsService == nullptr)
    {
        ESP_LOGW(LOG_TAG, "Failed to find service UUID: AMSService");
        return;
    }
    pRemoteCommand = pAmsService->getCharacteristic(AMSCharacteristic_Remote_Command);
    if (pRemoteCommand == nullptr)
    {
        ESP_LOGW(LOG_TAG, "Failed to find characteristic UUID: AMSCharacteristic_Remote_Command");
        return;
    }
    BLERemoteCharacteristic *pEntityUpdate = pAmsService->getCharacteristic(AMSCharacteristic_Entity_Update);
    if (pEntityUpdate == nullptr)
    {
        ESP_LOGW(LOG_TAG, "Failed to find characteristic UUID: AMSCharacteristic_Entity_Update");
        return;
    }
    BLERemoteCharacteristic *pEntityAttribute = pAmsService->getCharacteristic(AMSCharacteristic_Entity_Attribute);
    if (pEntityAttribute == nullptr)
    {
        ESP_LOGW(LOG_TAG, "Failed to find characteristic UUID: AMSCharacteristic_Entity_Attribute");
        return;
    }

    const uint8_t trackStuff[] = {AMSEntityIDTrack, AMSTrackAttributeIDArtist, AMSTrackAttributeIDAlbum, AMSTrackAttributeIDTitle, AMSTrackAttributeIDDuration};
    pEntityUpdate->registerForNotify(amsEntityUpdateNotifyCallback);
    pEntityUpdate->writeValue((uint8_t *)trackStuff, 5, true);
    const uint8_t appStuff[] = {AMSEntityIDPlayer, AMSPlayerAttributeIDName, AMSPlayerAttributeIDPlaybackInfo, AMSPlayerAttributeIDVolume};
    pEntityUpdate->writeValue((uint8_t *)appStuff, 4, true);
    pRemoteCommand->registerForNotify(amsRemoteCommandNotifyCallback);
}

bool AMSBLEClient::performCommand(AMSRemoteCommandID_t cmd)
{
    if (!pRemoteCommand || !isCommandAvailable(cmd))
        return false;
    pRemoteCommand->writeValue((uint8_t *)&cmd, 1, true);
    return true;
}

void AMSBLEClient::onEntityUpdateNotification(const AMSEntityUpdateNotification_t *notification, const std::string &value)
{
    switch (notification->EntityID)
    {
    case AMSEntityIDPlayer:
        if (onPlayerUpdateCB != nullptr)
            onPlayerUpdateCB((AMSPlayerAttributeID_t)notification->AttributeID, value, onPlayerUpdateCBUserData);
        break;
    case AMSEntityIDTrack:
        if (onTrackUpdateCB != nullptr)
            onTrackUpdateCB((AMSTrackAttributeID_t)notification->AttributeID, value, onTrackUpdateCBUserData);
        break;
    default:
        ESP_LOGI(LOG_TAG, "Ignoring wrong entity id notification %d", notification->EntityID);
        break;
    }
}
