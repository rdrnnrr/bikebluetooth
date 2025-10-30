import AppIntents

@available(iOS 16.0, macOS 13.0, *)
@main
struct JuiceBoxCompanionIntentsExtension: AppIntentsExtension { }

@available(iOS 16.0, macOS 13.0, *)
struct JuiceBoxCompanionShortcuts: AppShortcutsProvider {
    static var shortcutTileColor: ShortcutTileColor { .blue }

    static var appShortcutsTitle: LocalizedStringResource { "JuiceBox Companion" }

    @AppShortcutsBuilder
    static var appShortcuts: [AppShortcut] {
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

        AppShortcut(
            intent: SetNowPlayingSongIntent(),
            phrases: [
                "Set ${applicationName} song",
                "Update ${applicationName} song info",
                "Share song with ${applicationName}"
            ],
            shortTitle: "Update Song",
            systemImageName: "square.and.pencil"
        )
    }
}
