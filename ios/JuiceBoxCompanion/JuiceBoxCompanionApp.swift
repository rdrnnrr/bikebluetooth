import SwiftUI

@main
struct JuiceBoxCompanionApp: App {
    @StateObject private var bluetoothManager = BluetoothManager()
    @StateObject private var nowPlayingManager = NowPlayingManager()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(bluetoothManager)
                .environmentObject(nowPlayingManager)
                .onAppear {
                    bluetoothManager.startScanning()
                    nowPlayingManager.beginMonitoring()
                }
        }
    }
}
