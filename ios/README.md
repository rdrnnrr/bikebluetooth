# JuiceBox Companion iOS App

This SwiftUI project provides the companion app for the JuiceBox Bluetooth remote. The app:

- Scans for the `JuiceBox Remote` BLE device that advertises the Nordic UART service (`6E400001-B5A3-F393-E0A9-E50E24DCCA9E`).
- Connects automatically and keeps track of the UART TX characteristic used by the ESP32 remote.
- Listens to the system media queue via `MPMusicPlayerController` and pushes song metadata to the remote in the `SONG|Artist|Album|Title` format expected by `bike.ino`.
- Lets you manually resend the current track from the main screen if needed.

## Getting started

1. Open `ios/JuiceBoxCompanion.xcodeproj` (create a new SwiftUI App project in Xcode using these sources if the project file is missing).
2. Set the signing team and bundle identifier that suits your account.
3. Ensure the app has the **Bluetooth** (`NSBluetoothAlwaysUsageDescription`) and **Media Library** (`NSAppleMusicUsageDescription`) usage descriptions in the target Info.
4. Build and run on an iPhone. Simulator cannot access CoreBluetooth peripherals.

Once running, start playback in Apple Music or any app that updates the system's Now Playing info. The song metadata will sync to the bike remote automatically when connected.
