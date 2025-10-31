import Foundation
import Dispatch
import Darwin

/// Lightweight wrapper around the private MediaRemote framework.
/// Fetches the system-wide now playing dictionary so we can surface metadata from apps like YouTube Music.
///
/// IMPORTANT:
/// - This file is compiled in only when the build flag `USE_MEDIAREMOTE` is defined
///   (e.g., in Debug/AdHoc). For App Store builds, do not define the flag.
enum MediaRemoteReader {
    /// Minimum delay between attempts after a nil/denied result, to reduce device log noise.
    private static let retryBackoff: TimeInterval = 30

    /// Tracks the earliest time we will attempt another MediaRemote read after a denial/empty result.
    private static var nextAllowedAttempt: TimeInterval = 0

    /// Fetches the most recent now playing info dictionary using MediaRemote.
    /// Returns `nil` if the bridge is unavailable, the app is not allowed, or if throttled by backoff.
    static func nowPlayingInfo(timeout: TimeInterval = 1.5) -> [String: Any]? {
        #if targetEnvironment(simulator)
        // MediaRemote is not accessible on Simulator; avoid noisy logs and return nil.
        return nil
        #else
        #if USE_MEDIAREMOTE
        let now = CFAbsoluteTimeGetCurrent()
        if now < nextAllowedAttempt {
            return nil
        }

        let result = _deviceNowPlayingInfo(timeout: timeout)
        if result == nil {
            // Back off further attempts for a short period to reduce repeated system logs.
            nextAllowedAttempt = now + retryBackoff
        } else {
            // On success, clear any backoff to allow responsive subsequent reads.
            nextAllowedAttempt = 0
        }
        return result
        #else
        // Compiled out for builds without the flag (e.g., App Store).
        return nil
        #endif
        #endif
    }
}

#if !targetEnvironment(simulator)
#if USE_MEDIAREMOTE
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
#endif
