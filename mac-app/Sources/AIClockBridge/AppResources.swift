import Foundation

/// Resolves SwiftPM resources both while developing and after the executable
/// has been wrapped in a conventional macOS .app bundle.
enum AppResources {
    static let bundle: Bundle = {
        if let resourcesURL = Bundle.main.resourceURL {
            let packagedURL = resourcesURL.appendingPathComponent(
                "AIClockBridge_AIClockBridge.bundle",
                isDirectory: true
            )
            if let packagedBundle = Bundle(url: packagedURL) {
                return packagedBundle
            }
        }
        return Bundle.module
    }()
}
