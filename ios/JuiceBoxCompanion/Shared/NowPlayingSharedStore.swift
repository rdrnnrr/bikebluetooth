import Foundation

public struct NowPlayingSharedSong: Codable, Equatable, Sendable {
    public var artist: String
    public var album: String
    public var title: String
    public var capturedAt: Date

    public init(artist: String, album: String, title: String, capturedAt: Date = Date()) {
        self.artist = artist
        self.album = album
        self.title = title
        self.capturedAt = capturedAt
    }

    public static let empty = NowPlayingSharedSong(artist: "", album: "", title: "", capturedAt: .distantPast)
}

public enum NowPlayingSharedStore {
    private static let suiteName = "group.com.juiceboxcompanion.shared"
    private static let songKey = "NowPlayingSharedStore.song"

    private static var defaults: UserDefaults? {
        UserDefaults(suiteName: suiteName)
    }

    private static var isAppGroupAvailable: Bool {
        defaults != nil
    }

    public static func save(song: NowPlayingSharedSong?) {
        guard isAppGroupAvailable else {
            // Silently no-op if the app group container is not available (e.g., Simulator or missing entitlement).
            return
        }

        guard let defaults else { return }

        if let song, let data = try? JSONEncoder().encode(song) {
            defaults.set(data, forKey: songKey)
        } else {
            defaults.removeObject(forKey: songKey)
        }
    }

    public static func loadSong() -> NowPlayingSharedSong? {
        guard isAppGroupAvailable, let defaults else {
            return nil
        }

        guard let data = defaults.data(forKey: songKey),
              let song = try? JSONDecoder().decode(NowPlayingSharedSong.self, from: data) else {
            return nil
        }

        return song
    }
}
