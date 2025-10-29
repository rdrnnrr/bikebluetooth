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
        handleAuthorization(status: MPMediaLibrary.authorizationStatus())
    }

    func stopMonitoring() {
        wantsMonitoring = false
        if notificationsActive {
            musicPlayer.endGeneratingPlaybackNotifications()
            notificationsActive = false
        }
        cancellables.removeAll()
    }

    private func updateFromNowPlayingInfo() {
        guard let item = musicPlayer.nowPlayingItem else {
            DispatchQueue.main.async {
                self.currentSong = .empty
            }
            return
        }

        let song = Song(
            artist: item.artist ?? "",
            album: item.albumTitle ?? "",
            title: item.title ?? ""
        )

        DispatchQueue.main.async {
            self.currentSong = song
        }
    }

    private func handleAuthorization(status: MPMediaLibraryAuthorizationStatus) {
        switch status {
        case .authorized:
            authorizationError = nil
            startPlaybackNotificationsIfNeeded()
        case .notDetermined:
            authorizationError = nil
            MPMediaLibrary.requestAuthorization { [weak self] newStatus in
                DispatchQueue.main.async {
                    self?.handleAuthorization(status: newStatus)
                }
            }
        case .restricted, .denied:
            authorizationError = "Enable Media & Apple Music access in Settings to share playback."
            stopMonitoring()
            DispatchQueue.main.async {
                self.currentSong = .empty
            }
        @unknown default:
            authorizationError = "Playback permissions are unavailable."
            stopMonitoring()
        }
    }

    private func startPlaybackNotificationsIfNeeded() {
        guard wantsMonitoring, !notificationsActive else { return }

        notificationsActive = true
        musicPlayer.beginGeneratingPlaybackNotifications()

        NotificationCenter.default.publisher(for: .MPMusicPlayerControllerNowPlayingItemDidChange)
            .merge(with: NotificationCenter.default.publisher(for: .MPMusicPlayerControllerPlaybackStateDidChange))
            .sink { [weak self] _ in
                self?.updateFromNowPlayingInfo()
            }
            .store(in: &cancellables)

        updateFromNowPlayingInfo()
    }

    @MainActor
    func openSettings() {
        guard let url = URL(string: UIApplication.openSettingsURLString) else { return }
        UIApplication.shared.open(url)
    }
}
