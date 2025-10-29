import Foundation
import Combine
import MediaPlayer
import UIKit
import Darwin

final class NowPlayingManager: ObservableObject {
    struct Song: Equatable {
        var artist: String
        var album: String
        var title: String

        static let empty = Song(artist: "", album: "", title: "")
    }

    private static let authorizationMessage = "Enable Media & Apple Music access in Settings to monitor Apple Music playback."
    private static let nowPlayingInfoDidChangeNotification: Notification.Name = {
        if let typed = MPNowPlayingInfoCenter.typedDidChangeNotification {
            return typed
        }

        // Fallback for SDKs that omit the typed constant but still post the same name.
        return Notification.Name("MPNowPlayingInfoDidChange")
    }()
    private static let legacyNowPlayingInfoNotifications: [Notification.Name] = [
        Notification.Name("MPNowPlayingInfoCenterDidChangeNotification"),
        Notification.Name("MPNowPlayingInfoCenterNowPlayingInfoDidChange")
    ]
    private static let nowPlayingInfoTitleKeys: [String] = [
        MPMediaItemPropertyTitle,
        "kMRMediaRemoteNowPlayingInfoTitle",
        "title",
        "song",
        "trackName",
        "kMRMediaRemoteNowPlayingInfoContentTitle",
        "kMRMediaRemoteNowPlayingInfoQueueItem"
    ]
    private static let nowPlayingInfoArtistKeys: [String] = [
        MPMediaItemPropertyArtist,
        MPMediaItemPropertyAlbumArtist,
        "kMRMediaRemoteNowPlayingInfoArtist",
        "kMRMediaRemoteNowPlayingInfoAlbumArtist",
        "artist",
        "subtitle",
        "performer",
        "kMRMediaRemoteNowPlayingInfoPerformer",
        "kMRMediaRemoteNowPlayingInfoRadioStationName",
        "kMRMediaRemoteNowPlayingInfoContentAuthor",
        "kMRMediaRemoteNowPlayingInfoSubtitle"
    ]
    private static let nowPlayingInfoAlbumKeys: [String] = [
        MPMediaItemPropertyAlbumTitle,
        "kMRMediaRemoteNowPlayingInfoAlbum",
        "album",
        "collection",
        "kMRMediaRemoteNowPlayingInfoContentCollection",
        "kMRMediaRemoteNowPlayingInfoLocalizedAlbumName"
    ]

    @Published private(set) var currentSong: Song = .empty
    @Published private(set) var authorizationError: String?

    private var cancellables = Set<AnyCancellable>()
    private let musicPlayer = MPMusicPlayerController.systemMusicPlayer
    private var notificationsActive = false
    private var wantsMonitoring = false
    private var musicPlayerMonitoringActive = false

    init(preview: Bool = false) {
        if preview {
            currentSong = Song(artist: "Daft Punk", album: "Discovery", title: "Harder, Better, Faster, Stronger")
        }
    }

    deinit {
        stopMonitoring()
    }

    func beginMonitoring() {
        wantsMonitoring = true
        authorizationError = nil
        startMonitoringIfNeeded()
    }

    func stopMonitoring() {
        wantsMonitoring = false
        deactivateMusicPlayerMonitoring()
        notificationsActive = false
        cancellables.removeAll()
    }

    private func startMonitoringIfNeeded() {
        guard wantsMonitoring else {
            return
        }

        if !notificationsActive {
            notificationsActive = true

            var observedNames = Set<String>()

            ([Self.nowPlayingInfoDidChangeNotification] + Self.legacyNowPlayingInfoNotifications).forEach { name in
                guard observedNames.insert(name.rawValue).inserted else { return }

                NotificationCenter.default.publisher(for: name, object: nil)
                    .sink { [weak self] _ in
                        self?.updateNowPlayingMetadata()
                    }
                    .store(in: &cancellables)
            }

            NotificationCenter.default.publisher(for: UIApplication.willEnterForegroundNotification)
                .sink { [weak self] _ in
                    self?.updateNowPlayingMetadata()
                }
                .store(in: &cancellables)

            Timer.publish(every: 5, on: .main, in: .common)
                .autoconnect()
                .sink { [weak self] _ in
                    self?.updateNowPlayingMetadata()
                }
                .store(in: &cancellables)
        }

        configureMusicPlayerMonitoring()
        updateNowPlayingMetadata()
    }

    private func configureMusicPlayerMonitoring() {
        let status = MPMediaLibrary.authorizationStatus()

        switch status {
        case .authorized:
            activateMusicPlayerMonitoring()
        case .notDetermined:
            MPMediaLibrary.requestAuthorization { [weak self] newStatus in
                guard let self = self else { return }
                DispatchQueue.main.async {
                    if newStatus == .authorized {
                        self.authorizationError = nil
                        self.activateMusicPlayerMonitoring()
                        self.updateNowPlayingMetadata()
                    } else {
                        self.handleMediaLibraryDenied()
                    }
                }
            }
        default:
            DispatchQueue.main.async { [weak self] in
                self?.handleMediaLibraryDenied()
            }
        }
    }

    private func activateMusicPlayerMonitoring() {
        guard !musicPlayerMonitoringActive else { return }

        musicPlayerMonitoringActive = true
        musicPlayer.beginGeneratingPlaybackNotifications()

        NotificationCenter.default.publisher(for: .MPMusicPlayerControllerNowPlayingItemDidChange, object: musicPlayer)
            .sink { [weak self] _ in
                self?.updateNowPlayingMetadata()
            }
            .store(in: &cancellables)

        NotificationCenter.default.publisher(for: .MPMusicPlayerControllerPlaybackStateDidChange, object: musicPlayer)
            .sink { [weak self] _ in
                self?.updateNowPlayingMetadata()
            }
            .store(in: &cancellables)
    }

    private func deactivateMusicPlayerMonitoring() {
        guard musicPlayerMonitoringActive else { return }
        musicPlayer.endGeneratingPlaybackNotifications()
        musicPlayerMonitoringActive = false
    }

    private func handleMediaLibraryDenied() {
        if songFromNowPlayingInfo(activeNowPlayingInfo()) == nil {
            authorizationError = Self.authorizationMessage
            currentSong = .empty
        }
    }

    private func updateNowPlayingMetadata() {
        guard notificationsActive else { return }

        DispatchQueue.main.async { [weak self] in
            self?.refreshNowPlayingMetadataOnMainThread()
        }
    }

    private func refreshNowPlayingMetadataOnMainThread() {
        guard notificationsActive else { return }

        let infoSong = songFromNowPlayingInfo(activeNowPlayingInfo())
        let playerSong = musicPlayerMonitoringActive ? songFromMediaItem(musicPlayer.nowPlayingItem) : nil
        let song = infoSong ?? playerSong ?? .empty

        if song != .empty {
            authorizationError = nil
        }

        currentSong = song
    }

    private func songFromNowPlayingInfo(_ info: [String: Any]?) -> Song? {
        guard let info = info else { return nil }

        let title = normalizedString(for: Self.nowPlayingInfoTitleKeys, hints: Self.nowPlayingInfoTitleHints, in: info)
        let artist = normalizedString(for: Self.nowPlayingInfoArtistKeys, hints: Self.nowPlayingInfoArtistHints, in: info)
        let album = normalizedString(for: Self.nowPlayingInfoAlbumKeys, hints: Self.nowPlayingInfoAlbumHints, in: info)

        guard [title, artist, album].contains(where: { !$0.isEmpty }) else { return nil }

        return Song(artist: artist, album: album, title: title)
    }

    private func activeNowPlayingInfo() -> [String: Any]? {
        if let info = MPNowPlayingInfoCenter.default().nowPlayingInfo, !info.isEmpty {
            return info
        }

        return nil
    }

    private func songFromMediaItem(_ item: MPMediaItem?) -> Song? {
        guard let item = item else { return nil }

        let title = item.title?.trimmed ?? ""
        let artist = item.artist?.trimmed ?? ""
        let album = item.albumTitle?.trimmed ?? ""

        guard [title, artist, album].contains(where: { !$0.isEmpty }) else { return nil }

        return Song(artist: artist, album: album, title: title)
    }

    @MainActor
    func openSettings() {
        guard let url = URL(string: UIApplication.openSettingsURLString) else { return }
        UIApplication.shared.open(url)
    }
}

private extension String {
    var trimmed: String {
        trimmingCharacters(in: .whitespacesAndNewlines)
    }
}

private extension NowPlayingManager {
    static let nowPlayingInfoTitleHints = ["title", "song", "track", "name"]
    static let nowPlayingInfoArtistHints = ["artist", "performer", "subtitle", "channel", "singer"]
    static let nowPlayingInfoAlbumHints = ["album", "collection", "playlist"]

    func normalizedString(for keys: [String], hints: [String], in info: [String: Any]) -> String {
        for key in keys {
            if let normalized = normalizedValue(info[key]) {
                return normalized
            }
        }

        for (key, value) in info {
            let lowerKey = key.lowercased()
            if hints.contains(where: { lowerKey.contains($0) }), let normalized = normalizedValue(value) {
                return normalized
            }
        }

        return ""
    }

    func normalizedValue(_ value: Any?) -> String? {
        guard let value else { return nil }

        if let string = value as? String {
            let trimmed = string.trimmed
            return trimmed.isEmpty ? nil : trimmed
        }

        if let string = value as? NSString {
            let trimmed = string.trimmingCharacters(in: .whitespacesAndNewlines)
            return trimmed.isEmpty ? nil : trimmed
        }

        if let string = value as? NSAttributedString {
            let trimmed = string.string.trimmed
            return trimmed.isEmpty ? nil : trimmed
        }

        if let number = value as? NSNumber {
            let trimmed = number.stringValue.trimmed
            return trimmed.isEmpty ? nil : trimmed
        }

        if let dictionary = value as? [String: Any] {
            for nestedValue in dictionary.values {
                if let normalized = normalizedValue(nestedValue) {
                    return normalized
                }
            }
            return nil
        }

        if let array = value as? [Any] {
            for element in array {
                if let normalized = normalizedValue(element) {
                    return normalized
                }
            }
            return nil
        }

        if let describable = value as? CustomStringConvertible {
            let trimmed = describable.description.trimmed
            return trimmed.isEmpty ? nil : trimmed
        }

        return nil
    }
}

private extension MPNowPlayingInfoCenter {
    static var typedDidChangeNotification: Notification.Name? {
        guard let handle = dlopen("/System/Library/Frameworks/MediaPlayer.framework/MediaPlayer", RTLD_LAZY) else {
            return nil
        }

        defer { dlclose(handle) }

        guard let symbol = dlsym(handle, "MPNowPlayingInfoCenterDidChangeNotification") else {
            return nil
        }

        let pointer = symbol.assumingMemoryBound(to: Optional<AnyObject>.self)

        guard let rawString = pointer.pointee as? NSString else {
            return nil
        }

        let trimmed = rawString.trimmingCharacters(in: .whitespacesAndNewlines)

        guard !trimmed.isEmpty else { return nil }

        return Notification.Name(trimmed as String)
    }
}
