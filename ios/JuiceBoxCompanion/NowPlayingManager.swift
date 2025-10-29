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
    private static let remoteNowPlayingInfoNotifications: [Notification.Name] = [
        Notification.Name("kMRMediaRemoteNowPlayingInfoDidChangeNotification"),
        Notification.Name("kMRMediaRemoteNowPlayingInfoDidUpdate"),
        Notification.Name("kMRMediaRemoteNowPlayingInfoClientPlaybackQueueDidChange")
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

            ([Self.nowPlayingInfoDidChangeNotification]
                + Self.legacyNowPlayingInfoNotifications
                + Self.remoteNowPlayingInfoNotifications)
                .forEach { name in
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
            if let normalized = normalizedScalarValue(info[key]) {
                return normalized
            }
        }

        if let fallback = fallbackNormalizedString(in: info, preferredKeys: keys, hints: hints) {
            return fallback
        }

        return ""
    }

    func normalizedScalarValue(_ value: Any?) -> String? {
        guard let value else { return nil }

        if value is NSNumber || value is NSDate || value is NSNull {
            return nil
        }

        if value is [String: Any] || value is [Any] {
            return nil
        }

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

        if let url = value as? URL {
            let trimmed = url.lastPathComponent.trimmed
            return trimmed.isEmpty ? nil : trimmed
        }

        if let describable = value as? CustomStringConvertible {
            let description = describable.description.trimmed
            return description.isEmpty ? nil : description
        }

        return nil
    }

    func fallbackNormalizedString(in info: [String: Any], preferredKeys: [String], hints: [String]) -> String? {
        var bestCandidate: (value: String, score: Int)?
        var visited = Set<String>()

        collectCandidates(in: info, keyPath: [], preferredKeys: preferredKeys, hints: hints) { candidate, score in
            guard score > 0 else { return }
            let lowered = candidate.lowercased()
            guard visited.insert(lowered).inserted else { return }

            if let current = bestCandidate {
                if score > current.score {
                    bestCandidate = (candidate, score)
                }
            } else {
                bestCandidate = (candidate, score)
            }
        }

        return bestCandidate?.value
    }

    func collectCandidates(in value: Any, keyPath: [String], preferredKeys: [String], hints: [String], collector: (String, Int) -> Void) {
        if let string = normalizedScalarValue(value) {
            let score = score(for: string, keyPath: keyPath, preferredKeys: preferredKeys, hints: hints)
            guard score > 0 else { return }
            collector(string, score)
            return
        }

        if let dictionary = value as? [String: Any] {
            for (key, nested) in dictionary {
                collectCandidates(in: nested, keyPath: keyPath + [key], preferredKeys: preferredKeys, hints: hints, collector: collector)
            }
            return
        }

        if let array = value as? [Any] {
            for (index, element) in array.enumerated() {
                collectCandidates(in: element, keyPath: keyPath + ["[\(index)]"], preferredKeys: preferredKeys, hints: hints, collector: collector)
            }
        }
    }

    func score(for string: String, keyPath: [String], preferredKeys: [String], hints: [String]) -> Int {
        let trimmed = string.trimmed
        guard !trimmed.isEmpty else { return Int.min }

        var score = 0
        let lowercasedPath = keyPath.map { $0.lowercased() }
        let lowerHints = hints.map { $0.lowercased() }
        let lowerPreferredKeys = preferredKeys.map { $0.lowercased() }
        let lowerTrimmed = trimmed.lowercased()

        if lowercasedPath.contains(where: { component in
            lowerPreferredKeys.contains(where: { component.contains($0) })
        }) {
            score += 40
        }

        if lowercasedPath.contains(where: { component in
            lowerHints.contains(where: { component.contains($0) })
        }) {
            score += 30
        }

        if trimmed.contains("://") {
            score -= 40
        }

        if trimmed.contains("com.") {
            score -= 25
        }

        if trimmed.contains(".app") {
            score -= 20
        }

        if lowerTrimmed.contains("unknown") {
            score -= 50
        }

        if ["youtube music", "apple music", "spotify", "tidal", "pandora"].contains(lowerTrimmed) {
            score -= 25
        }

        if lowerTrimmed.contains("juce") || lowerTrimmed.contains("juicebox") {
            score -= 15
        }

        let wordCount = trimmed.split { $0.isWhitespace }.count
        if wordCount >= 2 {
            score += 10
        }

        let lengthBonus = min(trimmed.count, 60)
        score += lengthBonus

        let letterCount = trimmed.unicodeScalars.filter { CharacterSet.letters.contains($0) }.count
        if letterCount >= max(1, trimmed.count / 2) {
            score += 6
        }

        if trimmed == trimmed.uppercased() {
            score -= 6
        }

        if trimmed == trimmed.lowercased() {
            score -= 3
        }

        return score
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
