import Foundation
import Combine
import MediaPlayer
import UIKit

final class NowPlayingManager: ObservableObject {
    struct Song: Equatable {
        var artist: String
        var album: String
        var title: String

        static let empty = Song(artist: "", album: "", title: "")
    }

    private static let authorizationMessage = "Enable Media & Apple Music access in Settings to monitor playback."
    private static let nowPlayingInfoNotifications: [Notification.Name] = [
        Notification.Name("MPNowPlayingInfoDidChange"),
        Notification.Name("MPNowPlayingInfoCenterDidChangeNotification"),
        Notification.Name("MPNowPlayingInfoCenterNowPlayingInfoDidChange")
    ]
    private static let nowPlayingTitleKeys: [String] = [
        MPMediaItemPropertyTitle,
        "kMRMediaRemoteNowPlayingInfoTitle",
        "kMRMediaRemoteNowPlayingInfoContentTitle",
        "title",
        "song"
    ]
    private static let nowPlayingArtistKeys: [String] = [
        MPMediaItemPropertyArtist,
        MPMediaItemPropertyAlbumArtist,
        "kMRMediaRemoteNowPlayingInfoArtist",
        "kMRMediaRemoteNowPlayingInfoAlbumArtist",
        "kMRMediaRemoteNowPlayingInfoContentAuthor",
        "artist",
        "subtitle"
    ]
    private static let nowPlayingAlbumKeys: [String] = [
        MPMediaItemPropertyAlbumTitle,
        "kMRMediaRemoteNowPlayingInfoAlbum",
        "kMRMediaRemoteNowPlayingInfoContentCollection",
        "album",
        "collection"
    ]

    @Published private(set) var currentSong: Song = .empty
    @Published private(set) var authorizationError: String?

    private var cancellables = Set<AnyCancellable>()
    private let musicPlayer = MPMusicPlayerController.systemMusicPlayer
    private var wantsMonitoring = false
    private var notificationsActive = false
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
        guard wantsMonitoring else { return }

        configureMusicPlayerAuthorization()

        guard !notificationsActive else {
            updateNowPlayingMetadata()
            return
        }

        notificationsActive = true

        for name in Self.nowPlayingInfoNotifications {
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

        updateNowPlayingMetadata()
    }

    private func configureMusicPlayerAuthorization() {
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
        if songFromNowPlayingInfo(MPNowPlayingInfoCenter.default().nowPlayingInfo) == nil {
            authorizationError = Self.authorizationMessage
            currentSong = .empty
        }
    }

    private func updateNowPlayingMetadata() {
        let infoSong = songFromNowPlayingInfo(MPNowPlayingInfoCenter.default().nowPlayingInfo)
        let playerSong = musicPlayerMonitoringActive ? songFromMediaItem(musicPlayer.nowPlayingItem) : nil
        let song = infoSong ?? playerSong ?? .empty

        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            if song != .empty {
                self.authorizationError = nil
            }
            self.currentSong = song
        }
    }

    private func songFromNowPlayingInfo(_ info: [String: Any]?) -> Song? {
        guard let info = info else { return nil }

        var dictionaries: [[String: Any]] = [info]
        if let contentItem = info["kMRMediaRemoteNowPlayingInfoContentItem"] as? [String: Any] {
            dictionaries.append(contentItem)
        }
        if let queueEntry = info["kMRMediaRemoteNowPlayingInfoQueueEntry"] as? [String: Any] {
            dictionaries.append(queueEntry)
        }

        var title = ""
        var artist = ""
        var album = ""

        for dictionary in dictionaries {
            if title.isEmpty {
                title = value(for: Self.nowPlayingTitleKeys, in: dictionary) ?? ""
            }
            if artist.isEmpty {
                artist = value(for: Self.nowPlayingArtistKeys, in: dictionary) ?? ""
            }
            if album.isEmpty {
                album = value(for: Self.nowPlayingAlbumKeys, in: dictionary) ?? ""
            }
        }

        if title.isEmpty && artist.isEmpty && album.isEmpty {
            return nil
        }

        return Song(artist: artist, album: album, title: title)
    }

    private func songFromMediaItem(_ item: MPMediaItem?) -> Song? {
        guard let item = item else { return nil }

        let title = item.title?.trimmed ?? ""
        let artist = item.artist?.trimmed ?? ""
        let album = item.albumTitle?.trimmed ?? ""

        if title.isEmpty && artist.isEmpty && album.isEmpty {
            return nil
        }

        return Song(artist: artist, album: album, title: title)
    }

    private func value(for keys: [String], in dictionary: [String: Any]) -> String? {
        for key in keys {
            if let string = normalizedString(dictionary[key]) {
                return string
            }
        }
        return nil
    }

    private func normalizedString(_ value: Any?) -> String? {
        guard let value else { return nil }

        if let string = value as? String {
            let trimmed = string.trimmed
            return trimmed.isEmpty ? nil : trimmed
        }

        if let string = value as? NSString {
            let trimmed = string.trimmingCharacters(in: .whitespacesAndNewlines)
            return trimmed.isEmpty ? nil : trimmed
        }

        if let attributed = value as? NSAttributedString {
            let trimmed = attributed.string.trimmed
            return trimmed.isEmpty ? nil : trimmed
        }

        if let url = value as? URL {
            let trimmed = url.lastPathComponent.trimmingCharacters(in: .whitespacesAndNewlines)
            return trimmed.isEmpty ? nil : trimmed
        }

        if let describable = value as? CustomStringConvertible {
            let trimmed = describable.description.trimmingCharacters(in: .whitespacesAndNewlines)
            return trimmed.isEmpty ? nil : trimmed
        }

        return nil
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
