import Foundation
import CoreBluetooth

final class BluetoothManager: NSObject, ObservableObject {
    struct SongPayload: Equatable {
        var artist: String
        var album: String
        var title: String

        var formatted: String {
            "SONG|\(artist.replacingOccurrences(of: "\n", with: " "))|\(album.replacingOccurrences(of: "\n", with: " "))|\(title.replacingOccurrences(of: "\n", with: " "))"
        }

        static let empty = SongPayload(artist: "", album: "", title: "")
    }

    @Published private(set) var isConnected = false
    @Published private(set) var isBusy = false
    @Published private(set) var connectionDescription = "Searching for JuiceBox Remote…"
    @Published private(set) var errorMessage: String?

    var canSend: Bool { isConnected && rxCharacteristic != nil }

    private let serviceUUID = CBUUID(string: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
    private let rxUUID = CBUUID(string: "6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
    private let txUUID = CBUUID(string: "6E400003-B5A3-F393-E0A9-E50E24DCCA9E")
    private let deviceName = "JuiceBox Remote"
    private let centralRestoreIdentifier = "com.juicebox.remote.central"
    private let knownPeripheralKey = "BluetoothManager.knownPeripheral"

    private var central: CBCentralManager!
    private var discoveredPeripheral: CBPeripheral?
    private var txCharacteristic: CBCharacteristic?
    private var rxCharacteristic: CBCharacteristic?

    private var lastSentPayload = SongPayload.empty
    private var pendingScanRequest = false

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
        central = CBCentralManager(
            delegate: self,
            queue: .main,
            options: [
                CBCentralManagerOptionRestoreIdentifierKey: centralRestoreIdentifier,
                CBCentralManagerOptionShowPowerAlertKey: true
            ]
        )
        attemptRestoreKnownPeripheral()
    }

    func startScanning() {
        guard central != nil else { return }
        guard central.state == .poweredOn else {
            pendingScanRequest = true
            return
        }

        central.scanForPeripherals(
            withServices: [serviceUUID],
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: false]
        )
        connectionDescription = "Scanning for remote…"
        errorMessage = nil
        pendingScanRequest = false
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

    func send(song: SongPayload, force: Bool = false) {
        guard canSend,
              let peripheral = discoveredPeripheral,
              let rxCharacteristic = rxCharacteristic else { return }

        // Avoid spamming identical payloads
        guard force || song.formatted != lastSentPayload.formatted else { return }

        guard let data = (song.formatted + "\n").data(using: .utf8) else { return }

        let maxLength = max(peripheral.maximumWriteValueLength(for: .withResponse), 20)
        var offset = 0
        while offset < data.count {
            let chunkSize = min(maxLength, data.count - offset)
            let chunk = data.subdata(in: offset..<(offset + chunkSize))
            peripheral.writeValue(chunk, for: rxCharacteristic, type: .withResponse)
            offset += chunkSize
        }

        lastSentPayload = song
    }

    private func resetState() {
        isConnected = false
        txCharacteristic = nil
        rxCharacteristic = nil
        lastSentPayload = .empty
        connectionDescription = "Scanning for remote…"
        errorMessage = nil
    }

    private func attemptRestoreKnownPeripheral() {
        if let existing = discoveredPeripheral, existing.state == .connecting || existing.state == .connected {
            return
        }

        guard let central = central,
              central.state == .poweredOn,
              let idString = UserDefaults.standard.string(forKey: knownPeripheralKey),
              let uuid = UUID(uuidString: idString) else { return }

        let peripherals = central.retrievePeripherals(withIdentifiers: [uuid])
        if let peripheral = peripherals.first {
            discoveredPeripheral = peripheral
            connectionDescription = "Reconnecting to remote…"
            peripheral.delegate = self
            central.connect(peripheral, options: nil)
            isBusy = true
        }
    }
}

extension BluetoothManager: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            if pendingScanRequest || !isConnected {
                startScanning()
            }
            attemptRestoreKnownPeripheral()
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
        discoveredPeripheral = peripheral
        peripheral.delegate = self
        peripheral.discoverServices([serviceUUID])
        connectionDescription = "Discovering services…"
        UserDefaults.standard.set(peripheral.identifier.uuidString, forKey: knownPeripheralKey)
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        errorMessage = error?.localizedDescription ?? "Failed to connect"
        connectionDescription = "Tap Connect to retry"
        isBusy = false
        startScanning()
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        resetState()
        connectionDescription = "Disconnected"
        isBusy = false
        if let error = error {
            errorMessage = error.localizedDescription
        }
        startScanning()
    }

    func centralManager(_ central: CBCentralManager, willRestoreState dict: [String : Any]) {
        if let peripherals = dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral] {
            for peripheral in peripherals where peripheral.name == deviceName {
                discoveredPeripheral = peripheral
                peripheral.delegate = self
                if peripheral.state == .connected {
                    isConnected = true
                    connectionDescription = "Connected"
                    peripheral.discoverServices([serviceUUID])
                } else if peripheral.state == .connecting {
                    connectionDescription = "Reconnecting…"
                    isBusy = true
                } else {
                    startScanning()
                }
                break
            }
        }

        if discoveredPeripheral == nil,
           let scannedServices = dict[CBCentralManagerRestoredStateScanServicesKey] as? [CBUUID],
           !scannedServices.isEmpty {
            pendingScanRequest = false
            startScanning()
        }
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
            switch characteristic.uuid {
            case txUUID:
                txCharacteristic = characteristic
                peripheral.setNotifyValue(true, for: characteristic)
            case rxUUID:
                rxCharacteristic = characteristic
            default:
                break
            }
        }
        isConnected = txCharacteristic != nil && rxCharacteristic != nil
        isBusy = false
        connectionDescription = isConnected ? "Connected" : "Missing UART characteristic"
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error = error {
            errorMessage = error.localizedDescription
            return
        }
        guard characteristic.uuid == txUUID,
              let data = characteristic.value,
              let message = String(data: data, encoding: .utf8) else { return }

        handleIncoming(message: message.trimmingCharacters(in: .whitespacesAndNewlines), from: peripheral)
    }
}

private extension BluetoothManager {
    func handleIncoming(message: String, from peripheral: CBPeripheral) {
        switch message {
        case "ACK":
            // Remote acknowledged the most recent payload; nothing else to do.
            break
        case "REQ|SONG":
            if lastSentPayload != .empty {
                send(song: lastSentPayload, force: true)
            }
        default:
            print("Received UART message: \(message)")
        }
    }
}
