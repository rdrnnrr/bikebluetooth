import SwiftUI

@main
struct JuiceBoxCompanionApp: App {
    @StateObject private var bluetoothManager = BluetoothManager()
    @StateObject private var nowPlayingManager = NowPlayingManager()
    @Environment(\.scenePhase) private var scenePhase

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
        .onChange(of: scenePhase) { newPhase in
            if newPhase == .active {
                bluetoothManager.startScanning()
                nowPlayingManager.beginMonitoring()
            }
        }
    }
}
