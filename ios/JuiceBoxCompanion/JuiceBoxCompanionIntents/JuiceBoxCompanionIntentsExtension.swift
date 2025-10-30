import AppIntents

@available(iOS 16.0, macOS 13.0, *)
@main
struct JuiceBoxCompanionIntentsExtension: AppIntentsExtension { }

#if swift(>=5.9)
@available(iOS 16.0, macOS 13.0, *)
typealias JuiceBoxAppShortcutsCollection = [AppShortcut]
#else
@available(iOS 16.0, macOS 13.0, *)
typealias JuiceBoxAppShortcutsCollection = AppShortcut
#endif

@available(iOS 16.0, macOS 13.0, *)
struct JuiceBoxCompanionShortcuts: AppShortcutsProvider {
    static var shortcutTileColor: ShortcutTileColor { .blue }

    static var appShortcutsTitle: LocalizedStringResource { "JuiceBox Companion" }

    private static var nowPlayingShortcut: AppShortcut {
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
    static var appShortcuts: JuiceBoxAppShortcutsCollection {
        [nowPlayingShortcut]
    }
#else
    static var appShortcuts: JuiceBoxAppShortcutsCollection {
        nowPlayingShortcut
    }
#endif
}
