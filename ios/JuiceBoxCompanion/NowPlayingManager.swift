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

    private static let authorizationMessage = "Enable Media & Apple Music access in Settings to monitor Apple Music playback."
    private static let nowPlayingInfoCenterDidChangeNotification = Notification.Name("MPNowPlayingInfoCenterNowPlayingInfoDidChange")
    private static let nowPlayingInfoTitleKeys: [String] = [
        MPMediaItemPropertyTitle,
        "kMRMediaRemoteNowPlayingInfoTitle",
        "title",
        "song",
        "trackName"
    ]
    private static let nowPlayingInfoArtistKeys: [String] = [
        MPMediaItemPropertyArtist,
        MPMediaItemPropertyAlbumArtist,
        "kMRMediaRemoteNowPlayingInfoArtist",
        "kMRMediaRemoteNowPlayingInfoAlbumArtist",
        "artist",
        "subtitle",
        "performer",
        "kMRMediaRemoteNowPlayingInfoPerformer"
    ]
    private static let nowPlayingInfoAlbumKeys: [String] = [
        MPMediaItemPropertyAlbumTitle,
        "kMRMediaRemoteNowPlayingInfoAlbum",
        "album",
        "collection"
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

            NotificationCenter.default.publisher(
                for: Self.nowPlayingInfoCenterDidChangeNotification,
                object: MPNowPlayingInfoCenter.default()
            )
                .sink { [weak self] _ in
                    self?.updateNowPlayingMetadata()
                }
                .store(in: &cancellables)

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
        if songFromNowPlayingInfo(MPNowPlayingInfoCenter.default().nowPlayingInfo) == nil {
            authorizationError = Self.authorizationMessage
            currentSong = .empty
        }
    }

    private func updateNowPlayingMetadata() {
        guard notificationsActive else { return }

        let infoSong = songFromNowPlayingInfo(MPNowPlayingInfoCenter.default().nowPlayingInfo)
        let playerSong = musicPlayerMonitoringActive ? songFromMediaItem(musicPlayer.nowPlayingItem) : nil
        let song = infoSong ?? playerSong ?? .empty

        DispatchQueue.main.async {
            if song != .empty {
                self.authorizationError = nil
            }
            self.currentSong = song
        }
    }

    private func songFromNowPlayingInfo(_ info: [String: Any]?) -> Song? {
        guard let info = info else { return nil }

        let title = normalizedString(for: Self.nowPlayingInfoTitleKeys, in: info)
        let artist = normalizedString(for: Self.nowPlayingInfoArtistKeys, in: info)
        let album = normalizedString(for: Self.nowPlayingInfoAlbumKeys, in: info)

        guard [title, artist, album].contains(where: { !$0.isEmpty }) else { return nil }

        return Song(artist: artist, album: album, title: title)
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
    func normalizedString(for keys: [String], in info: [String: Any]) -> String {
        for key in keys {
            if let value = info[key] as? String {
                let trimmed = value.trimmed
                if !trimmed.isEmpty { return trimmed }
            }

            if let value = info[key] as? NSString {
                let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
                if !trimmed.isEmpty { return trimmed }
            }

            if let value = info[key] as? NSAttributedString {
                let trimmed = value.string.trimmed
                if !trimmed.isEmpty { return trimmed }
            }
        }

        return ""
    }
}
