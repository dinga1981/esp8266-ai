import Foundation
import Network

/// Lightweight bridge health, network-path monitoring and a bounded access
/// log. It deliberately avoids logging response bodies or user settings.
final class BridgeDiagnostics {
    static let shared = BridgeDiagnostics()

    private let queue = DispatchQueue(label: "aiclock.bridge-diagnostics")
    private let startedAt = Date()
    private var pathMonitor: NWPathMonitor?
    private var networkStatus = "unknown"
    private var requestCount: UInt64 = 0
    private var errorCount: UInt64 = 0
    private var lastPath = ""
    private var lastIP = ""
    private var lastStatus = 0
    private var lastBytes = 0
    private var lastDurationMs = 0
    private var lastRequestAt: Date?
    private var pathRequestAt: [String: Date] = [:]
    private var bootReportCount: UInt64 = 0
    private var lastBootReportAt: Date?

    private let logURL: URL = {
        let fm = FileManager.default
        let base = fm.urls(for: .libraryDirectory, in: .userDomainMask).first!
            .appendingPathComponent("Logs/AIClockBridge", isDirectory: true)
        try? fm.createDirectory(at: base, withIntermediateDirectories: true)
        return base.appendingPathComponent("access.log")
    }()
    private lazy var bootReportURL = logURL.deletingLastPathComponent()
        .appendingPathComponent("boot-reports.jsonl")

    private init() {}

    func startNetworkMonitoring(onRestored: @escaping () -> Void) {
        let monitor = NWPathMonitor()
        pathMonitor = monitor
        monitor.pathUpdateHandler = { [weak self] path in
            guard let self else { return }
            self.queue.async {
                let old = self.networkStatus
                self.networkStatus = path.status == .satisfied ? "online" : "offline"
                self.appendLog("network status=\(self.networkStatus) expensive=\(path.isExpensive) constrained=\(path.isConstrained)")
                if self.networkStatus == "online", old != "online" {
                    DispatchQueue.main.asyncAfter(deadline: .now() + 3, execute: onRestored)
                }
            }
        }
        monitor.start(queue: queue)
    }

    func record(path: String, ip: String, status: Int, bytes: Int, duration: TimeInterval) {
        queue.async {
            self.requestCount += 1
            if !(200...299).contains(status) { self.errorCount += 1 }
            self.lastPath = path
            self.lastIP = ip
            self.lastStatus = status
            self.lastBytes = bytes
            self.lastDurationMs = Int((duration * 1000).rounded())
            self.lastRequestAt = Date()
            self.pathRequestAt[path] = self.lastRequestAt
            self.appendLog("ip=\(ip.isEmpty ? "-" : ip) path=\(path) status=\(status) bytes=\(bytes) duration_ms=\(self.lastDurationMs)")
        }
    }

    func wasRecentlyRequested(paths: [String], within seconds: TimeInterval) -> Bool {
        queue.sync {
            let newest = paths.compactMap { pathRequestAt[$0] }.max() ?? .distantPast
            return Date().timeIntervalSince(newest) <= seconds
        }
    }

    func healthData() -> Data {
        queue.sync {
            let now = Date()
            let formatter = ISO8601DateFormatter()
            let object: [String: Any] = [
                "ok": true,
                "bridge_version": aiClockBridgeVersion,
                "uptime_s": Int(now.timeIntervalSince(startedAt)),
                "network": networkStatus,
                "request_count": requestCount,
                "error_count": errorCount,
                "last_path": lastPath,
                "last_device_ip": lastIP,
                "last_status": lastStatus,
                "last_bytes": lastBytes,
                "last_duration_ms": lastDurationMs,
                "last_request_at": lastRequestAt.map(formatter.string(from:)) ?? "",
                "boot_report_count": bootReportCount,
                "last_boot_report_at": lastBootReportAt.map(formatter.string(from:)) ?? "",
                "ts": Int(now.timeIntervalSince1970),
            ]
            return (try? JSONSerialization.data(withJSONObject: object)) ?? Data("{\"ok\":true}".utf8)
        }
    }

    func recordBootReport(_ body: Data, remoteIP: String = "") -> Bool {
        guard body.count <= 16_384,
              let object = try? JSONSerialization.jsonObject(with: body) as? [String: Any],
              object["boot_count"] is NSNumber,
              object["reset_reason"] is String else { return false }
        queue.async {
            self.bootReportCount += 1
            self.lastBootReportAt = Date()
            var stored = object
            stored["received_at"] = Int(Date().timeIntervalSince1970)
            if !remoteIP.isEmpty { stored["remote_ip"] = remoteIP }
            if let data = try? JSONSerialization.data(withJSONObject: stored),
               let line = String(data: data, encoding: .utf8) {
                self.append(line + "\n", to: self.bootReportURL, maxBytes: 2 * 1024 * 1024)
            }
            self.appendLog("boot_report boot=\(object["boot_count"] ?? "?") reset=\(object["reset_reason"] ?? "?")")
        }
        return true
    }

    private func appendLog(_ message: String) {
        let line = "\(ISO8601DateFormatter().string(from: Date())) \(message)\n"
        append(line, to: logURL, maxBytes: 2 * 1024 * 1024)
    }

    private func append(_ text: String, to url: URL, maxBytes: Int) {
        rotateIfNeeded(url: url, maxBytes: maxBytes)
        let data = Data(text.utf8)
        if !FileManager.default.fileExists(atPath: url.path) {
            FileManager.default.createFile(atPath: url.path, contents: data)
            return
        }
        guard let handle = try? FileHandle(forWritingTo: url) else { return }
        defer { try? handle.close() }
        do {
            try handle.seekToEnd()
            try handle.write(contentsOf: data)
        } catch {}
    }

    private func rotateIfNeeded(url: URL, maxBytes: Int) {
        let attributes = try? FileManager.default.attributesOfItem(atPath: url.path)
        let size = (attributes?[.size] as? NSNumber)?.intValue ?? 0
        guard size >= maxBytes else { return }
        let old = url.appendingPathExtension("1")
        try? FileManager.default.removeItem(at: old)
        try? FileManager.default.moveItem(at: url, to: old)
    }
}
