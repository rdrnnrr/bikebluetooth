import AppIntents

@available(iOS 16.0, macOS 13.0, *)
@main
struct JuiceBoxCompanionIntentsExtension: AppIntentsExtension { }

@available(iOS 16.0, macOS 13.0, *)
struct JuiceBoxCompanionShortcuts: AppShortcutsProvider {
    static var shortcutTileColor: ShortcutTileColor { .blue }

    static var appShortcutsTitle: LocalizedStringResource { "JuiceBox Companion" }

    private static func makeNowPlayingShortcut() -> AppShortcut {
        AppShortcut(
            intent: GetNowPlayingSongIntent(),
            phrases: [
                "Get ${applicationName} song",
                "What is ${applicationName} playing",
                "Get current ${applicationName} song"
            ],
            shortTitle: "Current Song",
            systemImageName: "music.note"
        )
    }

#if swift(>=5.9)
    static var appShortcuts: [AppShortcut] {
        [makeNowPlayingShortcut()]
    }
#else
    static var appShortcuts: AppShortcut {
        makeNowPlayingShortcut()
    }
#endif
}
