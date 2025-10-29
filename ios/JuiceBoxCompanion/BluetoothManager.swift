import Foundation
import CoreBluetooth
import Combine

struct Song: Codable {
    let title: String
    let artist: String
    let album: String?

    func encodedData() -> Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = .sortedKeys
        return (try? encoder.encode(self)) ?? Data()
    }
}

final class BluetoothManager: NSObject, ObservableObject {
    private let uartServiceUUID = CBUUID(string: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
    private let writeCharacteristicUUID = CBUUID(string: "6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
    private let notifyCharacteristicUUID = CBUUID(string: "6E400003-B5A3-F393-E0A9-E50E24DCCA9E")

    private lazy var centralManager: CBCentralManager = CBCentralManager(delegate: self, queue: .main)
    private var connectedPeripheral: CBPeripheral?
    private var writeCharacteristic: CBCharacteristic?
    private var notifyCharacteristic: CBCharacteristic?

    @Published var lastMessage: String = ""

    func connect() {
        _ = centralManager
    }

    func send(song: Song) {
        guard let peripheral = connectedPeripheral,
              let writeCharacteristic = writeCharacteristic else {
            return
        }

        let payload = song.encodedData()
        guard !payload.isEmpty else { return }

        peripheral.writeValue(payload, for: writeCharacteristic, type: .withResponse)
    }
}

extension BluetoothManager: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn {
            central.scanForPeripherals(withServices: [uartServiceUUID], options: nil)
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String : Any], rssi RSSI: NSNumber) {
        connectedPeripheral = peripheral
        peripheral.delegate = self
        central.stopScan()
        central.connect(peripheral, options: nil)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        peripheral.discoverServices([uartServiceUUID])
    }
}

extension BluetoothManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard error == nil, let services = peripheral.services else { return }
        for service in services where service.uuid == uartServiceUUID {
            peripheral.discoverCharacteristics([writeCharacteristicUUID, notifyCharacteristicUUID], for: service)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard error == nil, service.uuid == uartServiceUUID, let characteristics = service.characteristics else {
            return
        }

        for characteristic in characteristics {
            switch characteristic.uuid {
            case writeCharacteristicUUID:
                writeCharacteristic = characteristic
            case notifyCharacteristicUUID:
                notifyCharacteristic = characteristic
                peripheral.setNotifyValue(true, for: characteristic)
            default:
                continue
            }
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard error == nil, characteristic.uuid == notifyCharacteristicUUID, let data = characteristic.value,
              let message = String(data: data, encoding: .utf8) else {
            return
        }

        DispatchQueue.main.async { [weak self] in
            self?.lastMessage = message
        }
    }
}
