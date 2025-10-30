import AppIntents
import Foundation

@available(iOS 16.0, macOS 13.0, *)
struct AppGroupDiagnosticsIntent: AppIntent {
    static var title: LocalizedStringResource = "Test App Group"
    static var description = IntentDescription("Writes and reads a test value in the shared app group to verify entitlements.")

    func perform() async throws -> some IntentResult & ReturnsValue<String> {
        let suiteName = "group.com.juiceboxcompanion.shared"
        guard let defaults = UserDefaults(suiteName: suiteName) else {
            return .result(value: "Failed to open UserDefaults(suiteName:).", dialog: "App Group unavailable.")
        }
        let key = "Diagnostics.test"
        let value = "Wrote at \(ISO8601DateFormatter().string(from: Date()))"
        defaults.set(value, forKey: key)
        let roundTrip = defaults.string(forKey: key) ?? "<nil>"
        return .result(value: roundTrip, dialog: "App Group read: \(roundTrip)")
    }
}
