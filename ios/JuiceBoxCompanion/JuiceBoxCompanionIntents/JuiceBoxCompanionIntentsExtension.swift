import AppIntents

@available(iOS 16.0, macOS 13.0, *)
@main
struct JuiceBoxCompanionIntentsExtension: AppIntentsExtension { }

@available(iOS 16.0, macOS 13.0, *)
struct JuiceBoxCompanionShortcuts {
    static var tileColor: ShortcutTileColor { .blue }

    static var titleResource: LocalizedStringResource { "JuiceBox Companion" }

    static var shortcuts: [AppShortcut] {
        [nowPlayingShortcut]
    }

    static var nowPlayingShortcut: AppShortcut {
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
}

#if swift(>=5.9)
@available(iOS 16.0, macOS 13.0, *)
extension JuiceBoxCompanionShortcuts: AppShortcutsProvider {
    static var shortcutTileColor: ShortcutTileColor { tileColor }

    static var appShortcutsTitle: LocalizedStringResource { titleResource }

    static var appShortcuts: [AppShortcut] {
        shortcuts
    }
}
#else
@available(iOS 16.0, macOS 13.0, *)
extension JuiceBoxCompanionShortcuts: AppShortcutsProvider {
    static var shortcutTileColor: ShortcutTileColor { tileColor }

    static var appShortcutsTitle: LocalizedStringResource { titleResource }

    @AppShortcutsBuilder
    static var appShortcuts: AppShortcut {
        nowPlayingShortcut
    }
}
#endif
