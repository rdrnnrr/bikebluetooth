# Apple ANCS and AMS ESP32 Integration Guide

This document summarizes how the repository's ESP32 components connect to Apple's Notification Center Service (ANCS) and Apple Media Service (AMS), and outlines a minimal setup you can adapt in your own project.

## ANCS overview

Apple's ANCS exposes three fixed characteristics: Notification Source, Control Point, and Data Source. The ESP32 client in this repository discovers those characteristics on connection and subscribes to their notifications so it can read incoming events and request extra attributes (title, message, app identifier, etc.).【F:ancs_ble_client.cpp†L17-L87】

The `ANCSBLEClient` class handles this flow:

* `setup()` locates the ANCS service, registers for notifications on the Notification Source and Data Source characteristics, and enables the Client Characteristic Configuration Descriptor (CCCD) required for notifications.【F:ancs_ble_client.cpp†L52-L83】
* When a notification summary arrives on the Notification Source, the client schedules follow-up Control Point reads to pull the app ID, title, body text, and timestamp, then stores the results in `ANCSNotificationQueue` until the notification is complete.【F:ancs_ble_client.cpp†L89-L142】
* Parsed notifications trigger the callback you provide via `setNotificationArrivedCallback`, allowing your application to react to new alerts.【F:ancs_ble_client.cpp†L31-L133】

## AMS overview

The Apple Media Service uses a separate GATT service with characteristics for issuing remote commands and subscribing to media state changes. `apple_media_service.cpp` contains a lightweight client that:

* Connects to the AMS service and caches the Remote Command, Entity Update, and Entity Attribute characteristics.【F:apple_media_service.cpp†L44-L92】
* Enables notifications on the Entity Update characteristic so the device receives playback, queue, and track updates.【F:apple_media_service.cpp†L93-L115】
* Subscribes to player, queue, and track attributes to keep a `MediaStatus` record current, then notifies your application through the `UpdateCallback` whenever playback, queue, or track data change.【F:apple_media_service.cpp†L117-L207】
* Exposes `sendCommand()` so you can map UI events to standard AMS remote commands such as play/pause or next track.【F:apple_media_service.cpp†L214-L220】

The public header `apple_media_service.h` lists the supported `RemoteCommandID` values and the `MediaStatus` fields you will receive in callbacks.【F:apple_media_service.h†L9-L48】

## ESP32 helper library structure

`BLENotifications` (declared in `esp32notifications.h`) is the main entry point for Arduino-style applications. It wraps the ANCS and AMS clients, managing advertising, security, and callbacks for you.【F:esp32notifications.h†L9-L79】 Calling `begin()` sets up the BLE stack, configures privacy, and starts advertising for iOS devices that implement ANCS.【F:esp32notifications.cpp†L77-L123】 When an iPhone connects, `BLENotifications` creates the ANCS, AMS, and CTS client instances, applies the recommended LE Secure Connections bonding security parameters, and spawns the FreeRTOS tasks that fetch notification metadata.【F:esp32notifications.cpp†L31-L74】

If you want to issue AMS commands (play/pause, skip, etc.) or listen for media updates, use `setOnAMSTrackUpdateCB`, `setOnAMSPlayerUpdateCB`, and `amsCommand()` on the `BLENotifications` instance.【F:esp32notifications.h†L56-L75】 These functions forward to the AMS client so you can connect UI controls to remote command IDs.

## Minimal integration steps

1. **Install the library** – Copy the `esp32notifications.*`, `ancs_ble_client.*`, `apple_media_service.*`, and supporting headers (`ble_notification.h`, `ble_security.*`) into your Arduino project or PlatformIO component.
2. **Initialize BLE** – In your sketch, create a global `BLENotifications` object and call `begin("My ESP32")` during setup to configure security, start advertising, and initialize the client helpers.【F:esp32notifications.cpp†L77-L123】
3. **Handle callbacks** – Register notification and media callbacks before starting your main loop. For example, use `setNotificationCallback()` to receive notification data and `setOnAMSTrackUpdateCB()` to be notified when the currently playing track changes.【F:esp32notifications.h†L41-L75】
4. **Implement UI actions** – Call `amsCommand()` with a value from `AppleMediaService::RemoteCommandID` when a user interacts with playback controls.【F:apple_media_service.h†L9-L22】【F:esp32notifications.h†L67-L73】
5. **Advertise after disconnects** – When you receive a `StateDisconnected` callback, call `startAdvertising()` so the iOS device can reconnect.【F:esp32notifications.h†L19-L40】【F:esp32notifications.cpp†L125-L173】

Following these steps gives you a working ESP32 implementation of Apple's notification and media BLE services using the components already present in this repository.
