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
    private let nowPlayingInfoCenter = MPNowPlayingInfoCenter.default()
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
        notificationsActive = false
        cancellables.removeAll()
    }

    private func updateFromNowPlayingInfo() {
        guard notificationsActive else { return }

        let info = nowPlayingInfoCenter.nowPlayingInfo ?? [:]
        let title = info[MPMediaItemPropertyTitle] as? String ?? ""
        let artist = info[MPMediaItemPropertyArtist] as? String ?? ""
        let album = info[MPMediaItemPropertyAlbumTitle] as? String ?? ""

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
        guard wantsMonitoring, !notificationsActive else {
            updateFromNowPlayingInfo()
            return
        }

        notificationsActive = true

        // Removed nonexistent MPNowPlayingInfoCenterNowPlayingInfoDidChange notification.

        NotificationCenter.default.publisher(for: UIApplication.willEnterForegroundNotification)
            .sink { [weak self] _ in
                self?.updateFromNowPlayingInfo()
            }
            .store(in: &cancellables)

        Timer.publish(every: 5, on: .main, in: .common)
            .autoconnect()
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
