
#ifndef AMS_BLE_CLIENT_H_
#define AMS_BLE_CLIENT_H_

#include <stdint.h>
#include "BLEClient.h"

#define AMSService_UUID "89D3502B-0F36-433A-8EF4-C502AD55F8DC"
#define AMSCharacteristic_Remote_Command_UUID "9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2"   // (writeable, notifiable)
#define AMSCharacteristic_Entity_Update_UUID "2F7CABCE-808D-411F-9A0C-BB92BA96C102"    // (writeable with response, notifiable)
#define AMSCharacteristic_Entity_Attribute_UUID "C6B2F38C-23AB-46D8-A6AB-A3A870BBD5D7" // (readable, writeable)

typedef struct __attribute__((__packed__))
{
    uint8_t RemoteCommandID;
} AMSRemoteCommand_t;

typedef struct __attribute__((__packed__))
{
    uint8_t EntityID;
    uint8_t AttributeID;
    uint8_t EntityUpdateFlags;
    char ValueFirstChar;
} AMSEntityUpdateNotification_t;

typedef struct __attribute__((__packed__))
{
    uint8_t EntityID;
    uint8_t FirstAttributeID;
} AMSEntityUpdateCommand_t;

typedef struct __attribute__((__packed__))
{
    uint8_t EntityID;
    uint8_t AttributeID;
} AMSEntityAttribute_t;

typedef enum
{
    AMSPlayerAttributeIDName = 0, /* A string containing the localized name of the app. */
    /**
    *   A concatenation of three comma-separated values:
    *   PlaybackState: a string that represents the integer value of the playback state
    *   PlaybackRate: a string that represents the floating point value of the playback rate.
    *   ElapsedTime: a string that represents the floating point value of the elapsed time of the current track, in seconds, at the moment the value was sent to the MR.
    */
    AMSPlayerAttributeIDPlaybackInfo = 1,
    AMSPlayerAttributeIDVolume = 2, /* A string that represents the floating point value of the volume, ranging from 0 (silent) to 1 (full volume). */
} AMSPlayerAttributeID_t;

typedef enum
{
    AMSPlaybackStatePaused = 0,
    AMSPlaybackStatePlaying = 1,
    AMSPlaybackStateRewinding = 2,
    AMSPlaybackStateFastForwarding = 3,
} AMSPlayerAttributeIDPlaybackInfoState_t;

typedef enum
{
    AMSRemoteCommandIDPlay = 0,
    AMSRemoteCommandIDPause = 1,
    AMSRemoteCommandIDTogglePlayPause = 2,
    AMSRemoteCommandIDNextTrack = 3,
    AMSRemoteCommandIDPreviousTrack = 4,
    AMSRemoteCommandIDVolumeUp = 5,
    AMSRemoteCommandIDVolumeDown = 6,
    AMSRemoteCommandIDAdvanceRepeatMode = 7,
    AMSRemoteCommandIDAdvanceShuffleMode = 8,
    AMSRemoteCommandIDSkipForward = 9,
    AMSRemoteCommandIDSkipBackward = 10,
    AMSRemoteCommandIDLikeTrack = 11,
    AMSRemoteCommandIDDislikeTrack = 12,
    AMSRemoteCommandIDBookmarkTrack = 13,
} AMSRemoteCommandID_t;

typedef enum
{
    AMSErr_Invalid_State = 0xA0,    /* The MR has not properly set up the AMS, e.g. it wrote to the Entity Update or Entity Attribute characteristic without subscribing to GATT notifications for the Entity Update characteristic. */
    AMSErr_Invalid_Command = 0xA1,  /* The command was improperly formatted. */
    AMSErr_Absent_Attribute = 0xA2, /* The corresponding attribute is empty. */
} AMSErrorCodes_t;

typedef enum
{
    AMSEntityIDPlayer = 0,
    AMSEntityIDQueue = 1,
    AMSEntityIDTrack = 2,
} AMSEntityID_t;

typedef enum
{
    AMSRepeatModeOff = 0,
    AMSRepeatModeOne = 1,
    AMSRepeatModeAll = 2,
} AMSRepeatMode_t;

typedef enum
{
    AMSShuffleModeOff = 0,
    AMSShuffleModeOne = 1,
    AMSShuffleModeAll = 2,
} AMSShuffleMode_t;

typedef enum
{
    AMSTrackAttributeIDArtist = 0,
    AMSTrackAttributeIDAlbum = 1,
    AMSTrackAttributeIDTitle = 2,
    AMSTrackAttributeIDDuration = 3, /* A string containing the floating point value of the total duration of the track in seconds. */
} AMSTrackAttributeID_t;

typedef void (*ams_track_updated_t)(const AMSTrackAttributeID_t attribute, const std::string &value, const void *userData);
typedef void (*ams_player_updated_t)(const AMSPlayerAttributeID_t attribute, const std::string &value, const void *userData);

class AMSBLEClient
{
private:
    ams_track_updated_t onTrackUpdateCB = nullptr;
    const void *onTrackUpdateCBUserData = nullptr;
    ams_player_updated_t onPlayerUpdateCB = nullptr;
    const void *onPlayerUpdateCBUserData = nullptr;
    class BLERemoteCharacteristic *pRemoteCommand = nullptr;
    uint32_t pAvailableCommands = 0;

public:
    AMSBLEClient();
    ~AMSBLEClient();
    void setup(BLEClient *pClient);
    bool isCommandAvailable(AMSRemoteCommandID_t cmd) { return (pAvailableCommands & (1 << cmd)) != 0; };
    void setAvailableCommands(uint32_t flags) { pAvailableCommands = flags; };
    void onEntityUpdateNotification(const AMSEntityUpdateNotification_t *notification, const std::string &value);
    void setOnTrackUpdateCB(ams_track_updated_t cb, const void *userData = nullptr)
    {
        onTrackUpdateCB = cb;
        onTrackUpdateCBUserData = userData;
    }
    void setOnPlayerUpdateCB(ams_player_updated_t cb, const void *userData = nullptr)
    {
        onPlayerUpdateCB = cb;
        onPlayerUpdateCBUserData = userData;
    }
    bool performCommand(AMSRemoteCommandID_t cmd);
};

#endif