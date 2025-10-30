import AppIntents
import Foundation

@available(iOS 16.0, macOS 13.0, *)
struct GetNowPlayingSongIntent: AppIntent {
    static var title: LocalizedStringResource = "Get Now Playing Song"
    static var description = IntentDescription("Returns the most recent song captured by JuiceBox Companion for use in Shortcuts.")

    func perform() async throws -> some IntentResult & ReturnsValue<NowPlayingSongEntity> {
        let sharedSong = NowPlayingSharedStore.loadSong() ?? .empty
        let entity = NowPlayingSongEntity(song: sharedSong)

        if entity.isEmpty {
            return .result(
                value: entity,
                dialog: IntentDialog(stringLiteral: "No song information is currently available.")
            )
        }

        let dialogText: String
        if entity.artist.isEmpty {
            dialogText = "Now playing \(entity.title)."
        } else if entity.title.isEmpty {
            dialogText = "Now playing music by \(entity.artist)."
        } else {
            dialogText = "Now playing \(entity.title) by \(entity.artist)."
        }

        return .result(
            value: entity,
            dialog: IntentDialog(stringLiteral: dialogText)
        )
    }
}

@available(iOS 16.0, macOS 13.0, *)
struct NowPlayingSongEntity: AppEntity, Codable, Sendable, Equatable {
    static var typeDisplayRepresentation = TypeDisplayRepresentation(name: "Song")
    static var defaultQuery = NowPlayingSongEntityQuery()
    var id: String { identifier }
    var artist: String
    var album: String
    var title: String
    var capturedAt: Date

    private let identifier: String

    var displayRepresentation: DisplayRepresentation {
        if !title.isEmpty && !artist.isEmpty {
            return DisplayRepresentation(title: LocalizedStringResource(stringLiteral: title), subtitle: LocalizedStringResource(stringLiteral: artist))
        }

        if !title.isEmpty {
            return DisplayRepresentation(title: LocalizedStringResource(stringLiteral: title))
        }

        if !artist.isEmpty {
            return DisplayRepresentation(title: LocalizedStringResource(stringLiteral: artist))
        }

        return DisplayRepresentation(title: LocalizedStringResource(stringLiteral: "Unknown Song"))
    }

    var isEmpty: Bool {
        artist.isEmpty && album.isEmpty && title.isEmpty
    }

    init(song: NowPlayingSharedSong) {
        self.artist = song.artist
        self.album = song.album
        self.title = song.title
        self.capturedAt = song.capturedAt
        self.identifier = Self.makeIdentifier(song: song)
    }

    private static func makeIdentifier(song: NowPlayingSharedSong) -> String {
        if song.capturedAt == .distantPast {
            return "now-playing-empty"
        }

        let timestamp = ISO8601DateFormatter().string(from: song.capturedAt)
        return "now-playing-\(timestamp)"
    }
}

@available(iOS 16.0, macOS 13.0, *)
struct NowPlayingSongEntityQuery: EntityQuery {
    func entities(for identifiers: [NowPlayingSongEntity.ID]) async throws -> [NowPlayingSongEntity] {
        guard
            let entity = Self.currentSong(),
            identifiers.contains(entity.id)
        else {
            return []
        }

        return [entity]
    }

    func suggestedEntities() async throws -> [NowPlayingSongEntity] {
        guard let entity = Self.currentSong(), !entity.isEmpty else {
            return []
        }

        return [entity]
    }

    func defaultResult() async -> NowPlayingSongEntity? {
        guard let entity = Self.currentSong(), !entity.isEmpty else {
            return nil
        }

        return entity
    }

    private static func currentSong() -> NowPlayingSongEntity? {
        guard let song = NowPlayingSharedStore.loadSong() else { return nil }
        return NowPlayingSongEntity(song: song)
    }
}
