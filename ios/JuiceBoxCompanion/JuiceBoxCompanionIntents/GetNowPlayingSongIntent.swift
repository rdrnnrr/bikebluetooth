import AppIntents
import Foundation

struct GetNowPlayingSongIntent: AppIntent {
    static var title: LocalizedStringResource = "Get Now Playing Song"
    static var description = IntentDescription("Returns the most recent song captured by JuiceBox Companion for use in Shortcuts.")

    func perform() async throws -> some IntentResult & ReturnsValue<NowPlayingSongIntentResponse> {
        let song = NowPlayingSharedStore.loadSong() ?? .empty
        let response = NowPlayingSongIntentResponse(from: song)

        if song == .empty {
            return .result(value: response, dialog: IntentDialog("No song information is currently available."))
        }

        let dialogText: String
        if song.artist.isEmpty {
            dialogText = "Now playing \(song.title)."
        } else if song.title.isEmpty {
            dialogText = "Now playing music by \(song.artist)."
        } else {
            dialogText = "Now playing \(song.title) by \(song.artist)."
        }

        return .result(value: response, dialog: IntentDialog(dialogText))
    }
}

struct NowPlayingSongIntentResponse: Codable, Sendable, Equatable {
    var artist: String
    var album: String
    var title: String
    var capturedAt: Date

    init(artist: String, album: String, title: String, capturedAt: Date) {
        self.artist = artist
        self.album = album
        self.title = title
        self.capturedAt = capturedAt
    }

    init(from song: NowPlayingSharedSong) {
        self.init(artist: song.artist, album: song.album, title: song.title, capturedAt: song.capturedAt)
    }
}
