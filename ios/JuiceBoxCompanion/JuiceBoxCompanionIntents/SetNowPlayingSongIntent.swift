import AppIntents
import Foundation

@available(iOS 16.0, macOS 13.0, *)
struct SetNowPlayingSongIntent: AppIntent {
    static var title: LocalizedStringResource = "Update Now Playing Song"
    static var description = IntentDescription(
        "Manually set the song information shared with JuiceBox Companion. Useful when Shortcuts automation can read the current song from another app."
    )

    @Parameter(
        title: "Title",
        default: "",
        requestValueDialog: IntentDialog("What is the song title?")
    )
    var title: String

    @Parameter(
        title: "Artist",
        default: "",
        requestValueDialog: IntentDialog("Who is the artist?")
    )
    var artist: String

    @Parameter(
        title: "Album",
        default: "",
        requestValueDialog: IntentDialog("What album is it from?")
    )
    var album: String

    @Parameter(
        title: "Clear Existing Song",
        default: false,
        requestValueDialog: IntentDialog("Do you want to clear the saved song?")
    )
    var clearExistingSong: Bool

    func perform() async throws -> some IntentResult {
        let trimmedTitle = title.trimmedForIntent
        if clearExistingSong || trimmedTitle.isEmpty {
            NowPlayingSharedStore.save(song: nil)
            return .result(dialog: "Cleared the stored song.")
        }

        let song = NowPlayingSharedSong(
            artist: artist.trimmedForIntent,
            album: album.trimmedForIntent,
            title: trimmedTitle
        )

        NowPlayingSharedStore.save(song: song)
        return .result(dialog: "Saved \"\(song.title)\" by \(song.artist.isEmpty ? "Unknown Artist" : song.artist).")
    }
}

private extension String {
    var trimmedForIntent: String {
        trimmingCharacters(in: .whitespacesAndNewlines)
    }
}
