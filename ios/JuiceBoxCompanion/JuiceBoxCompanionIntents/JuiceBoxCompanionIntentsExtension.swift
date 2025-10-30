import AppIntents

@main
struct JuiceBoxCompanionIntentsExtension: AppIntentsExtension {
    func configuration() -> AppIntentsExtensionConfiguration {
        AppIntentsExtensionConfiguration(
            displayName: "JuiceBox Companion Intents",
            description: "Expose the most recent song captured by JuiceBox Companion to Shortcuts."
        )
    }
}
