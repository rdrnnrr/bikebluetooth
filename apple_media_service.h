#pragma once

#include <Arduino.h>
#include <BLEAddress.h>
#include <stdint.h>

namespace AppleMediaService {

enum class RemoteCommandID : uint8_t {
  Play = 0,
  Pause = 1,
  TogglePlayPause = 2,
  NextTrack = 3,
  PreviousTrack = 4,
  VolumeUp = 5,
  VolumeDown = 6,
  AdvanceRepeatMode = 7,
  AdvanceShuffleMode = 8,
  SkipForward = 9,
  SkipBackward = 10,
  LikeTrack = 11,
  DislikeTrack = 12,
  BookmarkTrack = 13
};

struct MediaStatus {
  bool playing = false;
  bool rewinding = false;
  bool fastForwarding = false;
  float playbackRate = 0.0f;
  float elapsedTimeSeconds = 0.0f;
  float durationSeconds = 0.0f;
  uint8_t shuffleMode = 0;
  uint8_t repeatMode = 0;
  uint8_t queueIndex = 0;
  uint8_t queueCount = 0;
  float volume = 0.0f;
  String playerName;
  String artist;
  String album;
  String title;
};

struct UpdateFlags {
  bool playbackChanged = false;
  bool trackChanged = false;
  bool queueChanged = false;
};

using UpdateCallback = void (*)(const MediaStatus &status, const UpdateFlags &flags);

bool start(const BLEAddress &address, UpdateCallback callback);
void stop();
bool isActive();
bool sendCommand(RemoteCommandID command);

}  // namespace AppleMediaService

