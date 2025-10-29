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

    @Published private(set) var currentSong: Song = .empty
    @Published private(set) var authorizationError: String?

    private var cancellables = Set<AnyCancellable>()
    private let musicPlayer = MPMusicPlayerController.systemMusicPlayer
    private var notificationsActive = false
    private var wantsMonitoring = false

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
        if notificationsActive {
            musicPlayer.endGeneratingPlaybackNotifications()
        }
        notificationsActive = false
        cancellables.removeAll()
    }

    private func updateFromMusicPlayer() {
        guard notificationsActive else { return }

        let item = musicPlayer.nowPlayingItem
        let title = item?.title ?? ""
        let artist = item?.artist ?? ""
        let album = item?.albumTitle ?? ""

        let song = Song(artist: artist, album: album, title: title)

        DispatchQueue.main.async {
            if song == .empty {
                self.currentSong = .empty
            } else {
                self.currentSong = song
            }
        }
    }

    private func startMonitoringIfNeeded() {
        guard wantsMonitoring else {
            return
        }

        let status = MPMediaLibrary.authorizationStatus()

        switch status {
        case .authorized:
            break
        case .notDetermined:
            MPMediaLibrary.requestAuthorization { [weak self] newStatus in
                guard let self = self else { return }
                DispatchQueue.main.async {
                    if newStatus == .authorized {
                        self.authorizationError = nil
                        self.startMonitoringIfNeeded()
                    } else {
                        self.authorizationError = Self.authorizationMessage
                        self.currentSong = .empty
                    }
                }
            }
            return
        default:
            DispatchQueue.main.async {
                self.authorizationError = Self.authorizationMessage
                self.currentSong = .empty
            }
            return
        }

        guard !notificationsActive else {
            updateFromMusicPlayer()
            return
        }

        notificationsActive = true
        authorizationError = nil
        musicPlayer.beginGeneratingPlaybackNotifications()

        NotificationCenter.default.publisher(for: .MPMusicPlayerControllerNowPlayingItemDidChange, object: musicPlayer)
            .sink { [weak self] _ in
                self?.updateFromMusicPlayer()
            }
            .store(in: &cancellables)

        NotificationCenter.default.publisher(for: .MPMusicPlayerControllerPlaybackStateDidChange, object: musicPlayer)
            .sink { [weak self] _ in
                self?.updateFromMusicPlayer()
            }
            .store(in: &cancellables)

        NotificationCenter.default.publisher(for: UIApplication.willEnterForegroundNotification)
            .sink { [weak self] _ in
                self?.updateFromMusicPlayer()
            }
            .store(in: &cancellables)

        Timer.publish(every: 5, on: .main, in: .common)
            .autoconnect()
            .sink { [weak self] _ in
                self?.updateFromMusicPlayer()
            }
            .store(in: &cancellables)

        updateFromMusicPlayer()
    }

    @MainActor
    func openSettings() {
        guard let url = URL(string: UIApplication.openSettingsURLString) else { return }
        UIApplication.shared.open(url)
    }
}
