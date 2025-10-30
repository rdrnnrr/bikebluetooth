import Foundation
import Dispatch
import Darwin

/// Lightweight wrapper around the private MediaRemote framework.
/// Fetches the system-wide now playing dictionary so we can surface metadata from apps like YouTube Music.
enum MediaRemoteReader {
    /// Fetches the most recent now playing info dictionary using MediaRemote.
    /// Returns `nil` if the bridge is unavailable or no metadata is available within `timeout`.
    static func nowPlayingInfo(timeout: TimeInterval = 1.5) -> [String: Any]? {
        #if targetEnvironment(simulator)
        // MediaRemote is not accessible on Simulator; avoid noisy logs and return nil.
        return nil
        #else
        return _deviceNowPlayingInfo(timeout: timeout)
        #endif
    }
}

#if !targetEnvironment(simulator)
private extension MediaRemoteReader {
    typealias InfoBlock = @convention(block) (CFDictionary?) -> Void
    typealias GetNowPlayingInfoFunction = @convention(c) (DispatchQueue?, InfoBlock) -> Void
    typealias GetNowPlayingInfoWithOptionsFunction = @convention(c) (DispatchQueue?, CFDictionary?, InfoBlock) -> Void

    static let libraryPath = "/System/Library/PrivateFrameworks/MediaRemote.framework/MediaRemote"
    static let libraryHandle: UnsafeMutableRawPointer? = {
        dlopen(libraryPath, RTLD_NOW)
    }()

    static let getNowPlayingInfo: GetNowPlayingInfoFunction? = {
        guard
            let handle = libraryHandle,
            let symbol = dlsym(handle, "MRMediaRemoteGetNowPlayingInfo")
        else {
            return nil
        }
        return unsafeBitCast(symbol, to: GetNowPlayingInfoFunction.self)
    }()

    static let getNowPlayingInfoWithOptions: GetNowPlayingInfoWithOptionsFunction? = {
        guard
            let handle = libraryHandle,
            let symbol = dlsym(handle, "MRMediaRemoteGetNowPlayingInfoWithOptions")
        else {
            return nil
        }
        return unsafeBitCast(symbol, to: GetNowPlayingInfoWithOptionsFunction.self)
    }()

    static let sourceIsNowPlayingAppKey = "kMRMediaRemoteOptionSourceIsNowPlayingApp" as CFString
    static let nowPlayingClientBundleIDKey = "kMRMediaRemoteOptionNowPlayingClientBundleIdentifier" as CFString

    static func _deviceNowPlayingInfo(timeout: TimeInterval) -> [String: Any]? {
        guard getNowPlayingInfo != nil || getNowPlayingInfoWithOptions != nil else { return nil }

        var result: [String: Any]?
        let semaphore = DispatchSemaphore(value: 0)
        let callback: InfoBlock = { dictionary in
            defer { semaphore.signal() }
            if let dictionary, let bridged = dictionary as? [String: Any], !bridged.isEmpty {
                result = bridged
            }
        }

        if let getNowPlayingInfoWithOptions {
            let options = [
                sourceIsNowPlayingAppKey: kCFBooleanTrue,
                nowPlayingClientBundleIDKey: ""
            ] as CFDictionary
            getNowPlayingInfoWithOptions(DispatchQueue.main, options, callback)
        } else if let getNowPlayingInfo {
            getNowPlayingInfo(DispatchQueue.main, callback)
        }

        _ = semaphore.wait(timeout: .now() + timeout)

        return result
    }
}
#endif
