import Foundation
import CoreBluetooth

final class BluetoothManager: NSObject, ObservableObject {
    struct SongPayload {
        var artist: String
        var album: String
        var title: String

        var formatted: String {
            "SONG|\(artist)|\(album)|\(title)"
        }

        static let empty = SongPayload(artist: "", album: "", title: "")
    }

    @Published private(set) var isConnected = false
    @Published private(set) var isBusy = false
    @Published private(set) var connectionDescription = "Searching for JuiceBox Remote…"
    @Published private(set) var errorMessage: String?

    var canSend: Bool { isConnected && txCharacteristic != nil }

    private let serviceUUID = CBUUID(string: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
    private let rxUUID = CBUUID(string: "6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
    private let txUUID = CBUUID(string: "6E400003-B5A3-F393-E0A9-E50E24DCCA9E")
    private let deviceName = "JuiceBox Remote"

    private var central: CBCentralManager!
    private var discoveredPeripheral: CBPeripheral?
    private var txCharacteristic: CBCharacteristic?

    private var lastSentPayload = SongPayload.empty

    init(preview: Bool = false) {
        super.init()
        if preview {
            central = CBCentralManager()
            isConnected = true
            connectionDescription = "Connected"
        }
    }

    override convenience init() {
        self.init(preview: false)
        central = CBCentralManager(delegate: self, queue: .main)
    }

    func startScanning() {
        guard central != nil else { return }
        if central.state == .poweredOn {
            central.scanForPeripherals(withServices: [serviceUUID], options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
            connectionDescription = "Scanning for remote…"
            errorMessage = nil
        }
    }

    func toggleConnection() {
        if isConnected {
            disconnect()
        } else {
            reconnect()
        }
    }

    func reconnect() {
        guard let peripheral = discoveredPeripheral else {
            startScanning()
            return
        }
        central.connect(peripheral, options: nil)
        isBusy = true
    }

    func disconnect() {
        if let peripheral = discoveredPeripheral {
            central.cancelPeripheralConnection(peripheral)
        }
        resetState()
        startScanning()
    }

    func send(song: SongPayload) {
        guard canSend,
              let peripheral = discoveredPeripheral,
              let txCharacteristic = txCharacteristic else { return }

        // Avoid spamming identical payloads
        guard song.formatted != lastSentPayload.formatted else { return }

        if let data = song.formatted.data(using: .utf8) {
            peripheral.writeValue(data, for: txCharacteristic, type: .withResponse)
            lastSentPayload = song
        }
    }

    private func resetState() {
        isConnected = false
        txCharacteristic = nil
        connectionDescription = "Scanning for remote…"
        errorMessage = nil
    }
}

extension BluetoothManager: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            startScanning()
        case .poweredOff:
            connectionDescription = "Turn on Bluetooth"
            errorMessage = nil
        case .unauthorized:
            connectionDescription = "Bluetooth access denied"
            errorMessage = "Enable Bluetooth permissions in Settings"
        case .unsupported:
            connectionDescription = "Bluetooth unsupported"
            errorMessage = "This device cannot connect to the remote"
        default:
            connectionDescription = "Bluetooth unavailable"
            errorMessage = nil
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String : Any], rssi RSSI: NSNumber) {
        guard peripheral.name == deviceName else { return }
        discoveredPeripheral = peripheral
        central.stopScan()
        central.connect(peripheral, options: nil)
        isBusy = true
        connectionDescription = "Connecting…"
        errorMessage = nil
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        peripheral.delegate = self
        peripheral.discoverServices([serviceUUID])
        connectionDescription = "Discovering services…"
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        errorMessage = error?.localizedDescription ?? "Failed to connect"
        connectionDescription = "Tap Connect to retry"
        isBusy = false
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        isConnected = false
        connectionDescription = "Disconnected"
        isBusy = false
        if let error = error {
            errorMessage = error.localizedDescription
        }
        startScanning()
    }
}

extension BluetoothManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error = error {
            errorMessage = error.localizedDescription
            isBusy = false
            return
        }
        guard let services = peripheral.services else { return }
        for service in services where service.uuid == serviceUUID {
            peripheral.discoverCharacteristics([txUUID, rxUUID], for: service)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        if let error = error {
            errorMessage = error.localizedDescription
            isBusy = false
            return
        }
        guard let characteristics = service.characteristics else { return }
        for characteristic in characteristics {
            if characteristic.uuid == txUUID {
                txCharacteristic = characteristic
            }
        }
        isConnected = txCharacteristic != nil
        isBusy = false
        connectionDescription = isConnected ? "Connected" : "Missing UART characteristic"
    }
}
