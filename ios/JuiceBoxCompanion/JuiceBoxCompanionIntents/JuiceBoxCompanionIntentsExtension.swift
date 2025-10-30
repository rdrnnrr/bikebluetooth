import AppIntents

@available(iOS 16.0, macOS 13.0, *)
@main
struct JuiceBoxCompanionIntentsExtension: AppIntentsExtension {
    var configuration: AppIntentsExtensionConfiguration {
        AppIntentsExtensionConfiguration(
            displayName: "JuiceBox Companion Intents",
            description: "Expose the most recent song captured by JuiceBox Companion to Shortcuts."
        )
    }
}
