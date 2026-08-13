import Foundation

/// Portable, human-readable backup for user choices on both sides of the
/// bridge. It intentionally excludes OAuth data, Wi-Fi passwords, transient
/// quote caches and custom sprite binaries.
enum SettingsBackup {
    static let schema = 1

    struct Contents {
        let mac: [String: Any]
        let device: [String: Any]
    }

    static func makeData(device: [String: Any]) throws -> Data {
        let defaults = UserDefaults.standard
        var mac: [String: Any] = [
            "device_host": DeviceClient.host,
            "stock_symbols": defaults.string(forKey: "stock_symbols") ?? "sh000001",
            "market_favorite_ids": defaults.stringArray(forKey: "market_favorite_ids") ?? [],
            "market_instrument_id": defaults.string(forKey: "market_instrument_id") ?? "",
            "market_interval": defaults.string(forKey: "btc_interval") ?? "5m",
            "market_refresh_interval": defaults.string(forKey: "market_refresh_interval") ?? "10",
        ]
        if let location = defaults.data(forKey: "weather_location_v1") {
            mac["weather_location_v1_base64"] = location.base64EncodedString()
        }
        let root: [String: Any] = [
            "schema": schema,
            "product": "esp8266-ai-mac-enhanced",
            "created_at": ISO8601DateFormatter().string(from: Date()),
            "bridge_version": aiClockBridgeVersion,
            "mac": mac,
            "device": device,
        ]
        return try JSONSerialization.data(withJSONObject: root, options: [.prettyPrinted, .sortedKeys])
    }

    static func parse(_ data: Data) throws -> Contents {
        guard let root = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              (root["schema"] as? NSNumber)?.intValue == schema,
              root["product"] as? String == "esp8266-ai-mac-enhanced",
              let mac = root["mac"] as? [String: Any],
              let device = root["device"] as? [String: Any],
              (device["schema"] as? NSNumber)?.intValue == 1 else {
            throw NSError(domain: "SettingsBackup", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "不是受支持的 AI Clock 设置备份文件"])
        }
        return Contents(mac: mac, device: device)
    }

    static func applyMac(_ mac: [String: Any]) throws {
        let defaults = UserDefaults.standard
        if let value = mac["stock_symbols"] as? String { defaults.set(value, forKey: "stock_symbols") }
        if let value = mac["market_favorite_ids"] as? [String] {
            defaults.set(value, forKey: "market_favorite_ids")
        }
        if let value = mac["market_instrument_id"] as? String {
            defaults.set(value, forKey: "market_instrument_id")
        }
        if let value = mac["market_interval"] as? String { defaults.set(value, forKey: "btc_interval") }
        if let value = mac["market_refresh_interval"] as? String {
            defaults.set(value, forKey: "market_refresh_interval")
        }
        if let encoded = mac["weather_location_v1_base64"] as? String,
           let data = Data(base64Encoded: encoded) {
            defaults.set(data, forKey: "weather_location_v1")
        } else {
            defaults.removeObject(forKey: "weather_location_v1")
        }
        // Restore is sent to the device currently paired by the user. Keep
        // that verified LAN address instead of replacing it with a stale DHCP
        // address stored on another Mac.
        if DeviceClient.host.isEmpty, let host = mac["device_host"] as? String, !host.isEmpty {
            DeviceClient.host = host
        }
    }
}
