import AppKit

// Entry point. Runs as an "accessory" app (menu-bar only, no Dock icon, no main
// window) and starts the /status HTTP server that the ESP8266 clock polls.
// Protocol-only smoke test; it does not start the menu-bar app or open ports.
if CommandLine.arguments.count >= 2,
   CommandLine.arguments[1] == "--test-weather-protocol" {
    let monitor = WeatherMonitor()
    let metadata = monitor.jsonData()
    let raw = monitor.textRGB565()
    let packed = monitor.textRLE()
    var corrupted = packed
    if !corrupted.isEmpty { corrupted[corrupted.count - 1] ^= 0x01 }
    guard let object = try? JSONSerialization.jsonObject(with: metadata) as? [String: Any],
          let metadataRevision = (object["text_rev"] as? NSNumber)?.uint32Value,
          let decoded = WeatherMonitor.unpackWeatherRLE(packed),
          decoded.revision == metadataRevision,
          decoded.raw == raw,
          WeatherMonitor.unpackWeatherRLE(corrupted) == nil else {
        print("weather protocol self-test failed")
        exit(1)
    }
    print("weather protocol ok rev=\(metadataRevision) raw=\(raw.count) rle=\(packed.count)")
    exit(0)
}

// Protocol-only smoke test for the four-row quote name-strip transport. It
// verifies both the stable content revision and the backward-compatible SNR1
// RLE payload without starting the app or opening the bridge port.
if CommandLine.arguments.count >= 2,
   CommandLine.arguments[1] == "--test-stock-name-protocol" {
    let key = "沪金\n伦敦金\n离岸人民币\n美元指数"
    let revision = StockMonitor.stableNamesRevision(key)
    var raw = Data([1])
    for index in 0..<(StockMonitor.nameW * StockMonitor.nameH) {
        let pixel: UInt16 = index < 2_300 ? 0 : (index.isMultiple(of: 2) ? 0x7BEF : 0)
        raw.append(UInt8(pixel >> 8))
        raw.append(UInt8(pixel & 0xFF))
    }
    let packed = StockMonitor.packNamesRLE(raw)
    var decoded = Data([packed.count >= 5 ? packed[4] : 0])
    var offset = 5
    while offset + 2 < packed.count {
        for _ in 0..<Int(packed[offset]) {
            decoded.append(packed[offset + 1])
            decoded.append(packed[offset + 2])
        }
        offset += 3
    }
    guard revision > 0,
          revision == StockMonitor.stableNamesRevision(key),
          revision != StockMonitor.stableNamesRevision(key + "2"),
          Array(packed.prefix(5)) == [0x53, 0x4E, 0x52, 0x31, 1],
          packed.count < raw.count,
          decoded == raw else {
        print("stock name protocol self-test failed")
        exit(1)
    }
    print("stock name protocol ok rev=\(revision) raw=\(raw.count) rle=\(packed.count)")
    exit(0)
}

// Headless smoke test for the petdex -> GIF -> device pipeline (same code the
// pet picker window uses): AIClockBridge --test-pet <slug> <claude|codex> <host>
if CommandLine.arguments.count >= 4, CommandLine.arguments[1] == "--test-pet" {
    let slug = CommandLine.arguments[2]
    let slot = CommandLine.arguments[3]
    if CommandLine.arguments.count >= 5 { DeviceClient.host = CommandLine.arguments[4] }
    let size = slot == "claude" ? (w: 111, h: 120) : (w: 120, h: 120)
    let state = PetdexService.states.first { $0.id == "running" }!
    PetdexService.loadManifest { result in
        guard case let .success(pets) = result, let pet = pets.first(where: { $0.slug == slug }) else {
            print("manifest load failed or slug not found"); exit(1)
        }
        print("pet: \(pet.displayName) \(pet.spritesheetUrl)")
        PetdexService.downloadSpritesheet(pet) { result in
            guard case let .success(sheet) = result else { print("sheet download failed"); exit(1) }
            print("sheet: \(sheet.width)x\(sheet.height)")
            guard let gif = PetdexService.buildGif(sheet: sheet, state: state,
                                                   targetW: size.w, targetH: size.h) else {
                print("gif build failed"); exit(1)
            }
            print("gif: \(gif.count) bytes, uploading to \(DeviceClient.host) slot \(slot)...")
            DeviceClient.uploadGif(gif, slot: slot) { error in
                print(error.map { "upload failed: \($0.localizedDescription)" } ?? "upload ok")
                exit(error == nil ? 0 : 1)
            }
        }
    }
    RunLoop.main.run() // completions land on the main queue; exit() above ends us
    exit(0)
}

// The environment override is intentionally undocumented UI-wise; it lets
// release verification run beside an installed bridge without disturbing the
// user's live port 8765 service.
let port = UInt16(ProcessInfo.processInfo.environment["AI_CLOCK_BRIDGE_PORT"] ?? "") ?? 8765
let service = StatusService()
let usage = UsageFetcher()
service.usage = usage
let netMonitor = NetSpeedMonitor()
netMonitor.start()
let nowPlaying = NowPlayingMonitor()
nowPlaying.start()
service.musicPlayingProvider = { nowPlaying.snapshot.playing }

let stockMonitor = StockMonitor()
stockMonitor.start()
let marketMonitor = MarketMonitor()
marketMonitor.start()
let weatherMonitor = WeatherMonitor()
weatherMonitor.start()
let bridgeDiagnostics = BridgeDiagnostics.shared

// Wired fallback: if the clock is plugged in over USB, push status/net down
// the serial line (works around AP client isolation; no WiFi setup needed).
let serialLink = SerialLink(service: service, netMonitor: netMonitor, stockMonitor: stockMonitor,
                            weatherMonitor: weatherMonitor)
serialLink.start()

let server = HTTPServer(port: port, routes: [
    "/": { service.snapshot().jsonData() },
    "/status": { service.snapshot().jsonData() },
    "/health": { bridgeDiagnostics.healthData() },
    "/net": {
        let stats = SystemStatsMonitor.shared.snapshot()
        return netMonitor.jsonData(cpu: stats.cpu, mem: stats.mem)
    },
    "/music": { nowPlaying.jsonData() },
    "/stock": { stockMonitor.jsonData() },
    "/market": { marketMonitor.jsonData() },
    "/market/version": { marketMonitor.frameVersionJSON },
    "/weather": { weatherMonitor.jsonData() },
], binaryRoutes: [
    "/music/cover.raw": { nowPlaying.coverRGB565 },
    "/music/text.raw": { nowPlaying.textRGB565 },
    "/stock/names.raw": { stockMonitor.namesRGB565() },
    "/stock/names.rle": { stockMonitor.namesRLE() },
    "/market/frame.rle": { marketMonitor.packedFrameEnvelope },
    "/market/frame.pal": { marketMonitor.paletteFrameEnvelope },
    "/market/frame.raw": { marketMonitor.frameEnvelope },
    "/weather/text.raw": { weatherMonitor.textRGB565() },
    "/weather/text.rle": { weatherMonitor.textRLE() },
], postRoutes: [
    // Claude Code / Codex hooks push lifecycle events here (see README §7):
    // curl -d '{"agent":"claude","event":"PreToolUse"}' http://127.0.0.1:8765/event
    "/event": { body in
        if let obj = try? JSONSerialization.jsonObject(with: body) as? [String: Any],
           let agent = obj["agent"] as? String, let event = obj["event"] as? String {
            service.recordEvent(agent: agent, event: event, message: obj["message"] as? String)
            return Data("{\"ok\":true}".utf8)
        }
        return Data("{\"ok\":false}".utf8)
    },
    "/device/boot-report": { body in
        let ok = bridgeDiagnostics.recordBootReport(body)
        return Data(ok ? "{\"ok\":true}".utf8 : "{\"ok\":false}".utf8)
    },
])
// Passive discovery: the clock polls us, so its source IP identifies it.
// Remember it (for auto-pairing / DHCP-change self-healing) and adopt it
// outright when no device is configured yet.
server.onRequest = { path, ip in
    let deviceRoutes = ["/status", "/health", "/net", "/music", "/stock",
                        "/stock/names.raw", "/stock/names.rle",
                        "/market/version", "/market/frame.pal", "/market/frame.rle",
                        "/weather", "/weather/text.raw", "/weather/text.rle",
                        "/device/boot-report"]
    guard deviceRoutes.contains(path),
          ip != "127.0.0.1", ip != "::1", !ip.isEmpty else { return }
    DeviceClient.devicePollAt = Date()
    DeviceClient.lastSeenIP = ip
    if DeviceClient.host.isEmpty { DeviceClient.host = ip }
}
server.onResponse = { path, ip, status, bytes, duration in
    bridgeDiagnostics.record(path: path, ip: ip, status: status,
                             bytes: bytes, duration: duration)
}
// Active fallback for when the passive route can't fire at all (fresh /
// erased device knows no bridge host, so it never polls anyone): if the
// device stays silent, find it ourselves and hand it our address.
Timer.scheduledTimer(withTimeInterval: 60, repeats: true) { _ in
    DeviceClient.healPairingIfNeeded(port: port)
}

do {
    try server.start()
    FileHandle.standardError.write(Data("[bridge] serving /status on 0.0.0.0:\(port)\n".utf8))
} catch {
    FileHandle.standardError.write(Data("[bridge] failed to bind port \(port): \(error)\n".utf8))
}

bridgeDiagnostics.startNetworkMonitoring {
    DeviceClient.recoverAfterNetworkChange(port: port)
}

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let menuBar = MenuBarController(service: service, usage: usage, netMonitor: netMonitor,
                                nowPlaying: nowPlaying, stockMonitor: stockMonitor,
                                market: marketMonitor, weather: weatherMonitor, port: port)
_ = menuBar // retain
usage.startAutoRefresh()
app.run()
