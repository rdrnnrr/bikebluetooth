import Foundation
import CoreBluetooth
import QuartzCore

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
    private let deviceNameKeyword = "juicebox"
    private let centralRestoreIdentifier = "com.juicebox.remote.central"
    private let knownPeripheralKey = "BluetoothManager.knownPeripheral"

    private var central: CBCentralManager!
    private var discoveredPeripheral: CBPeripheral?
    private var txCharacteristic: CBCharacteristic?
    private var rxCharacteristic: CBCharacteristic?

    private var lastSentPayload = SongPayload.empty
    private var lastSeenPeripheralNames: [UUID: [String]] = [:]
    private var pendingScanRequest = false
    private var pendingConnection: CBPeripheral?
    private var scanFallbackWorkItem: DispatchWorkItem?
    private var pendingServiceDiscovery = false

    // Scan state/coalescing
    private var isScanning = false
    private var lastScanStartTime: TimeInterval = 0
    private let minScanRestartInterval: TimeInterval = 1.0

    // Send throttling/debounce
    private var pendingSendPayload: SongPayload?
    private var pendingSendForce: Bool = false
    private var sendDebounceWorkItem: DispatchWorkItem?
    private var lastSendTime: TimeInterval = 0
    private let minSendInterval: TimeInterval = 0.5
    private let sendDebounceDelay: TimeInterval = 0.2

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

        // Avoid rapid duplicate scan starts
        let now = CACurrentMediaTime()
        if isScanning && (now - lastScanStartTime) < minScanRestartInterval {
            return
        }

        scanFallbackWorkItem?.cancel()
        scanFallbackWorkItem = nil

        if attemptRetrieveConnectedPeripheral() {
            errorMessage = nil
            pendingScanRequest = false
            return
        }

        stopScan()

        let hasKnown = UserDefaults.standard.string(forKey: knownPeripheralKey) != nil

        if hasKnown {
            startScan(withServices: nil)
            connectionDescription = "Scanning for remote…"
            scheduleScanFallback()
        } else {
            startScan(withServices: [serviceUUID])
            scheduleScanFallback()
            connectionDescription = "Scanning for remote…"
        }

        errorMessage = nil
        pendingScanRequest = false
        print("DEBUG: Started scanning (hasKnown=\(hasKnown))")
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
        guard central.state == .poweredOn else {
            pendingConnection = peripheral
            pendingScanRequest = true
            connectionDescription = "Waiting for Bluetooth…"
            isBusy = true
            return
        }

        pendingConnection = nil
        central.connect(peripheral, options: nil)
        isBusy = true
        print("DEBUG: Attempting reconnect to \(peripheral.identifier)")
    }

    func disconnect() {
        if let peripheral = discoveredPeripheral {
            central.cancelPeripheralConnection(peripheral)
        }
        resetState()
        startScanning()
    }

    // Public API stays the same; internally route through debounced/throttled sender
    func send(song: SongPayload, force: Bool = false) {
        guard canSend else { return }

        // Avoid spamming identical payloads unless forced
        if !force && song.formatted == lastSentPayload.formatted {
            return
        }

        // Coalesce pending payloads; latest wins, force if any caller forces
        pendingSendPayload = song
        pendingSendForce = pendingSendForce || force

        // Debounce multiple send requests quickly arriving
        sendDebounceWorkItem?.cancel()
        let work = DispatchWorkItem { [weak self] in
            self?.performThrottledSend()
        }
        sendDebounceWorkItem = work
        DispatchQueue.main.asyncAfter(deadline: .now() + sendDebounceDelay, execute: work)
    }

    private func performThrottledSend() {
        guard canSend, let payload = pendingSendPayload else { return }

        let now = CACurrentMediaTime()
        let force = pendingSendForce

        // Respect min interval unless explicitly forced
        if !force && (now - lastSendTime) < minSendInterval {
            let remaining = minSendInterval - (now - lastSendTime)
            sendDebounceWorkItem?.cancel()
            let work = DispatchWorkItem { [weak self] in
                self?.performThrottledSend()
            }
            sendDebounceWorkItem = work
            DispatchQueue.main.asyncAfter(deadline: .now() + remaining, execute: work)
            return
        }

        // Clear pending state before actual write
        pendingSendPayload = nil
        pendingSendForce = false
        lastSendTime = now

        guard let peripheral = discoveredPeripheral, let rxCharacteristic = rxCharacteristic else { return }
        guard let data = (payload.formatted + "\n").data(using: .utf8) else { return }

        let maxLength = max(peripheral.maximumWriteValueLength(for: .withResponse), 20)
        var offset = 0
        while offset < data.count {
            let chunkSize = min(maxLength, data.count - offset)
            let chunk = data.subdata(in: offset..<(offset + chunkSize))
            peripheral.writeValue(chunk, for: rxCharacteristic, type: .withResponse)
            offset += chunkSize
        }

        lastSentPayload = payload
    }

    private func resetState() {
        isConnected = false
        txCharacteristic = nil
        rxCharacteristic = nil
        lastSentPayload = .empty
        pendingConnection = nil
        connectionDescription = "Scanning for remote…"
        errorMessage = nil
        scanFallbackWorkItem?.cancel()
        scanFallbackWorkItem = nil

        // Clear pending send state
        pendingSendPayload = nil
        pendingSendForce = false
        sendDebounceWorkItem?.cancel()
        sendDebounceWorkItem = nil
        lastSendTime = 0

        // Scan state
        stopScan()

        print("DEBUG: Reset Bluetooth state")
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
            scanFallbackWorkItem?.cancel()
            scanFallbackWorkItem = nil
            discoveredPeripheral = peripheral
            connectionDescription = "Reconnecting to remote…"
            peripheral.delegate = self
            stopScan()
            central.connect(peripheral, options: nil)
            isBusy = true
            errorMessage = nil
            print("DEBUG: Restoring known peripheral \(uuid)")
        }
    }

    @discardableResult
    private func attemptRetrieveConnectedPeripheral() -> Bool {
        guard let central = central, central.state == .poweredOn else { return false }

        let peripherals = central.retrieveConnectedPeripherals(withServices: [serviceUUID])
        for peripheral in peripherals where isTargetPeripheral(peripheral) {
            scanFallbackWorkItem?.cancel()
            scanFallbackWorkItem = nil
            stopScan()
            discoveredPeripheral = peripheral
            peripheral.delegate = self
            errorMessage = nil

            if peripheral.state == .connected {
                isConnected = true
                connectionDescription = "Connected"
                peripheral.discoverServices([serviceUUID])
                print("DEBUG: Found already-connected peripheral \(peripheral.identifier)")
            } else {
                connectionDescription = "Reconnecting…"
                isBusy = true
                central.connect(peripheral, options: nil)
                print("DEBUG: Connecting to retrieved peripheral \(peripheral.identifier)")
            }

            return true
        }

        return false
    }

    private func scheduleScanFallback() {
        let fallback = DispatchWorkItem { [weak self] in
            guard
                let self = self,
                let central = self.central,
                central.state == .poweredOn,
                !(self.discoveredPeripheral?.state == .connected || self.discoveredPeripheral?.state == .connecting)
            else { return }

            self.scanFallbackWorkItem = nil
            self.stopScan()
            self.startScan(withServices: nil)
            self.connectionDescription = "Scanning for remote…"
            print("DEBUG: Scan fallback to unfiltered")
        }

        scanFallbackWorkItem = fallback
        DispatchQueue.main.asyncAfter(deadline: .now() + 5, execute: fallback)
    }

    private func isTargetPeripheral(_ peripheral: CBPeripheral, advertisementData: [String: Any] = [:]) -> Bool {
        let candidates = updateLastSeenNames(for: peripheral, advertisementData: advertisementData)

        // 1) Always accept by stored identifier (previously paired device)
        if let storedIdentifier = UserDefaults.standard.string(forKey: knownPeripheralKey),
           peripheral.identifier.uuidString == storedIdentifier {
            return true
        }

        // 2) Prefer name-based matching — accept if any candidate contains "juicebox"
        if candidates.contains(where: { $0.contains(deviceNameKeyword) }) {
            print("DEBUG: Name match for \(peripheral.identifier) candidates=\(candidates)")
            return true
        }

        // 3) Fallback to service list if present in advertisement (not required for name match)
        let advertisedServices = advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] ?? []
        let overflowServices = advertisementData[CBAdvertisementDataOverflowServiceUUIDsKey] as? [CBUUID] ?? []
        let includesService = advertisedServices.contains(serviceUUID) || overflowServices.contains(serviceUUID)
        if includesService {
            print("DEBUG: Service UUID match for \(peripheral.identifier) advertisedServices=\(advertisedServices)")
            return true
        }

        // Debug log for diagnostics when we skip a device
        #if DEBUG
        let localName = (advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? peripheral.name ?? "<nil>"
        print("DEBUG: Skipping peripheral \(peripheral.identifier) name=\(localName) candidates=\(candidates) advServices=\(advertisedServices)")
        #endif

        return false
    }

    @discardableResult
    private func updateLastSeenNames(for peripheral: CBPeripheral, advertisementData: [String: Any]) -> [String] {
        var candidates = lastSeenPeripheralNames[peripheral.identifier] ?? []

        if let name = peripheral.name?.lowercased(), !name.isEmpty {
            if !candidates.contains(name) {
                candidates.append(name)
            }
        }

        if let advertisedName = (advertisementData[CBAdvertisementDataLocalNameKey] as? String)?.lowercased(),
           !advertisedName.isEmpty,
           !candidates.contains(advertisedName) {
            candidates.append(advertisedName)
        }

        if !candidates.isEmpty {
            lastSeenPeripheralNames[peripheral.identifier] = candidates
        }

        return candidates
    }

    private func hasExpectedName(for peripheral: CBPeripheral) -> Bool {
        if let storedIdentifier = UserDefaults.standard.string(forKey: knownPeripheralKey),
           peripheral.identifier.uuidString == storedIdentifier {
            return true
        }

        var candidates = lastSeenPeripheralNames[peripheral.identifier] ?? []

        if let currentName = peripheral.name?.lowercased(), !currentName.isEmpty {
            candidates.append(currentName)
        }

        let match = candidates.contains { $0.contains(deviceNameKeyword) }
        if match {
            print("DEBUG: Name match for \(peripheral.identifier) candidates=\(candidates)")
        }
        return match
    }
}

// MARK: - Scan helpers
private extension BluetoothManager {
    func startScan(withServices services: [CBUUID]?) {
        guard central.state == .poweredOn else { return }
        let options: [String: Any] = [CBCentralManagerScanOptionAllowDuplicatesKey: false]
        central.scanForPeripherals(withServices: services, options: options)
        isScanning = true
        lastScanStartTime = CACurrentMediaTime()
    }

    func stopScan() {
        guard isScanning else { return }
        guard central.state == .poweredOn else { return }
        central.stopScan()
        isScanning = false
    }
}

extension BluetoothManager: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            // If we deferred service discovery during restoration, do it now.
            if pendingServiceDiscovery,
               let peripheral = discoveredPeripheral,
               peripheral.state == .connected {
                pendingServiceDiscovery = false
                connectionDescription = "Discovering services…"
                print("DEBUG: Resuming deferred service discovery for \(peripheral.identifier)")
                peripheral.discoverServices([serviceUUID])
            }

            if let pendingConnection {
                self.pendingConnection = nil
                pendingScanRequest = false
                central.connect(pendingConnection, options: nil)
                isBusy = true
                print("DEBUG: Resuming pending connection \(pendingConnection.identifier)")
            } else if pendingScanRequest || !isConnected {
                startScanning()
            }
            attemptRestoreKnownPeripheral()
            attemptRetrieveConnectedPeripheral()
        case .poweredOff:
            connectionDescription = "Turn on Bluetooth"
            errorMessage = nil
            stopScan()
        case .unauthorized:
            connectionDescription = "Bluetooth access denied"
            errorMessage = "Enable Bluetooth permissions in Settings"
            stopScan()
        case .unsupported:
            connectionDescription = "Bluetooth unsupported"
            errorMessage = "This device cannot connect to the remote"
            stopScan()
        default:
            connectionDescription = "Bluetooth unavailable"
            errorMessage = nil
            stopScan()
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String : Any], rssi RSSI: NSNumber) {
        guard isTargetPeripheral(peripheral, advertisementData: advertisementData) else { return }
        discoveredPeripheral = peripheral
        guard central.state == .poweredOn else {
            pendingConnection = peripheral
            pendingScanRequest = true
            connectionDescription = "Waiting for Bluetooth…"
            isBusy = true
            print("DEBUG: Queued connection for \(peripheral.identifier) (Bluetooth not powered on)")
            return
        }

        stopScan()
        scanFallbackWorkItem?.cancel()
        scanFallbackWorkItem = nil
        pendingConnection = nil
        central.connect(peripheral, options: nil)
        isBusy = true
        connectionDescription = "Connecting…"
        errorMessage = nil
        print("DEBUG: Discovered and connecting to \(peripheral.identifier) name=\(peripheral.name ?? "<nil>")")
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        pendingConnection = nil
        discoveredPeripheral = peripheral
        peripheral.delegate = self
        connectionDescription = "Discovering services…"
        peripheral.discoverServices([serviceUUID])
        print("DEBUG: Connected to \(peripheral.identifier)")
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        pendingConnection = nil
        errorMessage = error?.localizedDescription ?? "Failed to connect"
        connectionDescription = "Tap Connect to retry"
        isBusy = false
        print("DEBUG: Failed to connect \(peripheral.identifier) error=\(error?.localizedDescription ?? "<none>")")
        startScanning()
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        resetState()
        connectionDescription = "Disconnected"
        isBusy = false
        if let error = error {
            errorMessage = error.localizedDescription
        }
        print("DEBUG: Disconnected \(peripheral.identifier) error=\(error?.localizedDescription ?? "<none>")")
        startScanning()
    }

    func centralManager(_ central: CBCentralManager, willRestoreState dict: [String : Any]) {
        if let peripherals = dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral] {
            for peripheral in peripherals where isTargetPeripheral(peripheral) {
                scanFallbackWorkItem?.cancel()
                scanFallbackWorkItem = nil
                discoveredPeripheral = peripheral
                peripheral.delegate = self
                if peripheral.state == .connected {
                    isConnected = true
                    connectionDescription = "Connected"
                    // If we can't safely discover yet, defer until poweredOn.
                    if central.state == .poweredOn {
                        connectionDescription = "Discovering services…"
                        peripheral.discoverServices([serviceUUID])
                        print("DEBUG: Will restore connected peripheral \(peripheral.identifier) and discover services")
                    } else {
                        pendingServiceDiscovery = true
                        print("DEBUG: Will restore connected peripheral \(peripheral.identifier) (defer service discovery)")
                    }
                } else if peripheral.state == .connecting {
                    connectionDescription = "Reconnecting…"
                    isBusy = true
                    print("DEBUG: Will restore connecting peripheral \(peripheral.identifier)")
                } else if central.state == .poweredOn {
                    central.connect(peripheral, options: nil)
                    isBusy = true
                    print("DEBUG: Will restore and connect \(peripheral.identifier)")
                } else {
                    pendingConnection = peripheral
                    pendingScanRequest = true
                    connectionDescription = "Waiting for Bluetooth…"
                    isBusy = true
                    print("DEBUG: Will restore and queue \(peripheral.identifier)")
                }
                break
            }
        }

        // If the system indicates scanning state, request a scan after power-on instead of starting immediately.
        if discoveredPeripheral == nil,
           let scannedServices = dict[CBCentralManagerRestoredStateScanServicesKey] as? [CBUUID],
           !scannedServices.isEmpty {
            pendingScanRequest = true
            print("DEBUG: Will restore scanning with services \(scannedServices) (deferred until poweredOn)")
        }
    }
}

extension BluetoothManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error = error {
            errorMessage = error.localizedDescription
            isBusy = false
            print("DEBUG: Discover services error=\(error.localizedDescription)")
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
            print("DEBUG: Discover characteristics error=\(error.localizedDescription)")
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
        let hadConnection = isConnected
        isConnected = txCharacteristic != nil && rxCharacteristic != nil
        isBusy = false
        connectionDescription = isConnected ? "Connected" : "Missing UART characteristic"
        print("DEBUG: Characteristics set tx=\(txCharacteristic != nil) rx=\(rxCharacteristic != nil)")

        if isConnected && !hadConnection && shouldPersistPeripheral(peripheral) {
            UserDefaults.standard.set(peripheral.identifier.uuidString, forKey: knownPeripheralKey)
            print("DEBUG: Persisted known peripheral \(peripheral.identifier)")
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error = error {
            errorMessage = error.localizedDescription
            print("DEBUG: Update value error=\(error.localizedDescription)")
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
                // Force to bypass throttle so the remote gets the payload promptly on request.
                send(song: lastSentPayload, force: true)
            }
            if shouldPersistPeripheral(peripheral) {
                UserDefaults.standard.set(peripheral.identifier.uuidString, forKey: knownPeripheralKey)
            }
        default:
            print("Received UART message: \(message)")
        }
    }
}

private extension BluetoothManager {
    func shouldPersistPeripheral(_ peripheral: CBPeripheral) -> Bool {
        if let storedIdentifier = UserDefaults.standard.string(forKey: knownPeripheralKey),
           storedIdentifier == peripheral.identifier.uuidString {
            return true
        }

        return hasExpectedName(for: peripheral)
    }
}
