import AppIntents

@available(iOS 16.0, macOS 13.0, *)
@main
struct JuiceBoxCompanionIntentsExtension: AppIntentsExtension { }

@available(iOS 16.0, macOS 13.0, *)
struct JuiceBoxCompanionShortcuts: AppShortcutsProvider {
    static var shortcutTileColor: ShortcutTileColor { .blue }

    static var appShortcuts: [AppShortcut] {
        AppShortcut(
            intent: GetNowPlayingSongIntent(),
            phrases: [
                "Get JuiceBox Companion song",
                "What is JuiceBox Companion playing",
                "Get current JuiceBox song"
            ],
            shortTitle: "Current Song",
            systemImageName: "music.note"
        )
    }
}
