import AppKit
import Foundation

// Watchlist quotes routed by a stable, user-facing market prefix:
//   sh/sz/bj/hk/us -> Tencent stocks
//   fxXAUUSD       -> Sina international quote hf_XAU
//   sf/df/zf/gf    -> Sina commodity futures nf_<CODE>
//   cf             -> Sina financial futures nf_<CODE> (different field layout)
//   hf             -> Sina international futures hf_<CODE>
// Both endpoints are key-free, GB18030-encoded and unofficial. Sina requires a
// finance.sina.com.cn Referer. Provider-specific codes never leak into settings,
// so a future provider change will not invalidate the user's watchlist.
// Polled every 5s; the device and the mirror both render the pre-formatted
// strings so the firmware stays dumb (and ASCII-only: it shows the code, the
// CJK name is sent separately as an RGB565 strip).
final class StockMonitor {
    struct Row {
        let code: String  // ASCII display code: "600519", "00700", "AAPL"
        let name: String  // CJK name, mirror + device-rendered strip
        let price: String // pre-formatted
        let pct: String   // "+1.24%"
        let up: Int       // 1 rising / -1 falling / 0 flat
    }

    static let symbolsKey = "stock_symbols"
    /// Comma-separated market-prefixed symbols.
    static var symbols: [String] {
        get {
            let raw = UserDefaults.standard.string(forKey: symbolsKey) ?? "sh000001"
            return raw.replacingOccurrences(of: "，", with: ",") // CN comma happens
                .split(separator: ",")
                .map { normalize(String($0)) }
                .filter { !$0.isEmpty }
        }
        set { UserDefaults.standard.set(newValue.joined(separator: ","), forKey: symbolsKey) }
    }

    enum QuoteProvider: Equatable {
        case tencentStock
        case sinaInternational
        case sinaDomesticFutures
        case sinaFinancialFutures
        case sinaForex
    }

    struct QuoteSymbol {
        let key: String          // stable normalized user symbol, lowercase
        let prefix: String       // user-facing market prefix
        let code: String         // user-facing body, uppercase
        let provider: QuoteProvider
        let providerCode: String // Tencent/Sina request symbol

        var displayCode: String { code }
        var fixedPriceDecimals: Int? {
            // London spot gold is conventionally quoted to cents; the legacy
            // magnitude formatter would otherwise reduce a 4000.xx quote to 0.1.
            if prefix == "fx" && code == "XAUUSD" { return 2 }
            if prefix == "fx" && code == "USDCNH" { return 4 }
            if prefix == "fx" && code == "DXY" { return 3 }
            return nil
        }
    }

    /// Market prefix must be lowercase but ticker/contract bodies stay uppercase.
    /// HK codes are zero-padded to 5 digits for Tencent.
    static func normalize(_ s: String) -> String {
        let t = s.trimmingCharacters(in: .whitespaces)
        guard t.count > 2 else { return t.lowercased() }
        let prefix = t.prefix(2).lowercased()
        var body = String(t.dropFirst(2)).uppercased()
        if prefix == "hk", body.count < 5, body.allSatisfy({ $0.isNumber }) {
            body = String(repeating: "0", count: 5 - body.count) + body
        }
        return prefix + body
    }

    /// Converts the stable user syntax into a provider-specific request symbol.
    /// `fx` is intentionally allow-listed: spot symbols do not have a universal
    /// Sina naming rule, so unsupported pairs must not silently fetch a different
    /// instrument.
    static func parseSymbol(_ input: String) -> QuoteSymbol? {
        let normalized = normalize(input)
        guard normalized.count > 2 else { return nil }
        let prefix = String(normalized.prefix(2)).lowercased()
        let code = String(normalized.dropFirst(2)).uppercased()
        guard !code.isEmpty else { return nil }

        let provider: QuoteProvider
        let providerCode: String
        switch prefix {
        case "sh", "sz", "bj", "hk", "us":
            provider = .tencentStock
            providerCode = prefix + code
        case "fx":
            switch code {
            case "XAUUSD": provider = .sinaInternational; providerCode = "hf_XAU"
            case "USDCNH": provider = .sinaForex; providerCode = "fx_susdcnh"
            case "DXY": provider = .sinaForex; providerCode = "DINIW"
            default: return nil
            }
        case "sf", "df", "zf", "gf":
            provider = .sinaDomesticFutures
            providerCode = "nf_" + code
        case "cf":
            provider = .sinaFinancialFutures
            providerCode = "nf_" + code
        case "hf":
            provider = .sinaInternational
            providerCode = "hf_" + code
        default:
            return nil
        }
        return QuoteSymbol(key: normalized.lowercased(), prefix: prefix, code: code,
                           provider: provider, providerCode: providerCode)
    }

    private let lock = NSLock()
    private var rows: [Row] = []
    private var rowsBySymbol: [String: Row] = [:]
    private var fetchGeneration = 0
    private var timer: Timer?

    // The device's font is ASCII-only, so CJK company names go down as Mac-
    // rendered RGB565 strips (same trick as the music title strip): one
    // NAME_W x NAME_H strip per row, wire format [1 byte count][strips...].
    // names_rev in /stock tells the device when to re-fetch.
    static let nameW = 156, nameH = 16
    private var namesRev = 0
    private var namesData = Data([0])
    private var lastNamesKey = ""

    func start() {
        fetch()
        timer = Timer.scheduledTimer(withTimeInterval: 5.0, repeats: true) { [weak self] _ in
            self?.fetch()
        }
    }

    var snapshot: [Row] {
        lock.lock()
        defer { lock.unlock() }
        return rows
    }

    func jsonData() -> Data {
        let stocks = snapshot.map { r -> [String: Any] in
            ["code": r.code, "name": r.name, "price": r.price, "pct": r.pct, "up": r.up]
        }
        lock.lock()
        let rev = namesRev
        lock.unlock()
        let dict: [String: Any] = ["stocks": stocks, "names_rev": rev]
        return (try? JSONSerialization.data(withJSONObject: dict)) ?? Data("{}".utf8)
    }

    /// [1 byte count][NAME_W x NAME_H RGB565 big-endian per row...]
    func namesRGB565() -> Data {
        lock.lock()
        defer { lock.unlock() }
        return namesData
    }

    private func renderNamesIfNeeded(_ parsed: [Row]) {
        let key = parsed.prefix(4).map { $0.name }.joined(separator: "\n")
        lock.lock()
        let dirty = key != lastNamesKey
        lock.unlock()
        guard dirty else { return }
        var data = Data([UInt8(min(parsed.count, 4))])
        for row in parsed.prefix(4) {
            data.append(Self.renderNameStrip(row.name) ?? Data(count: Self.nameW * Self.nameH * 2))
        }
        lock.lock()
        lastNamesKey = key
        namesData = data
        namesRev += 1
        lock.unlock()
    }

    private static func renderNameStrip(_ name: String) -> Data? {
        let w = nameW, h = nameH
        guard let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8,
                                  bytesPerRow: w * 4, space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else {
            return nil
        }
        ctx.setFillColor(CGColor(red: 0, green: 0, blue: 0, alpha: 1))
        ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(cgContext: ctx, flipped: false)
        let style = NSMutableParagraphStyle()
        style.alignment = .right // sits left of the row's right edge, code is on the left
        style.lineBreakMode = .byTruncatingTail
        (name as NSString).draw(in: NSRect(x: 0, y: 1, width: w, height: h - 1), withAttributes: [
            .font: NSFont.systemFont(ofSize: 12, weight: .medium),
            .foregroundColor: NSColor(white: 0.72, alpha: 1),
            .paragraphStyle: style,
        ])
        NSGraphicsContext.restoreGraphicsState()
        guard let rendered = ctx.data else { return nil }
        let px = rendered.bindMemory(to: UInt8.self, capacity: w * h * 4)
        var out = Data(capacity: w * h * 2)
        for i in 0..<(w * h) {
            let v = (UInt16(px[i * 4] & 0xF8) << 8) | (UInt16(px[i * 4 + 1] & 0xFC) << 3)
                | UInt16(px[i * 4 + 2] >> 3)
            out.append(UInt8((v >> 8) & 0xFF))
            out.append(UInt8(v & 0xFF))
        }
        return out
    }

    private func fetch() {
        let symbols = Self.symbols.compactMap(Self.parseSymbol)
        lock.lock()
        fetchGeneration += 1
        let generation = fetchGeneration
        lock.unlock()
        guard !symbols.isEmpty else {
            lock.lock(); rows = []; rowsBySymbol = [:]; lock.unlock()
            renderNamesIfNeeded([])
            return
        }

        let tencent = symbols.filter { $0.provider == .tencentStock }
        let sina = symbols.filter { $0.provider != .tencentStock }
        let group = DispatchGroup()
        let resultLock = NSLock()
        var successfulKeys = Set<String>()
        var fetchedRows: [String: Row] = [:]

        func record(_ requested: [QuoteSymbol], result: Result<[String: Row], Error>) {
            resultLock.lock()
            defer { resultLock.unlock() }
            if case let .success(parsed) = result {
                successfulKeys.formUnion(requested.map(\.key))
                fetchedRows.merge(parsed) { _, new in new }
            }
        }

        if !tencent.isEmpty {
            group.enter()
            Self.fetchTencent(tencent) { result in
                record(tencent, result: result)
                group.leave()
            }
        }
        if !sina.isEmpty {
            group.enter()
            Self.fetchSina(sina) { result in
                record(sina, result: result)
                group.leave()
            }
        }

        group.notify(queue: .global(qos: .utility)) { [weak self] in
            guard let self = self else { return }
            resultLock.lock()
            let completedKeys = successfulKeys
            let newRows = fetchedRows
            resultLock.unlock()

            self.lock.lock()
            guard generation == self.fetchGeneration else {
                self.lock.unlock()
                return
            }
            // A successful provider response replaces all symbols requested from
            // that provider (including invalid/empty instruments). A failed batch
            // keeps its previous rows, matching the old transient-error behavior.
            for key in completedKeys { self.rowsBySymbol.removeValue(forKey: key) }
            self.rowsBySymbol.merge(newRows) { _, new in new }
            let currentKeys = Set(symbols.map(\.key))
            self.rowsBySymbol = self.rowsBySymbol.filter { currentKeys.contains($0.key) }
            let ordered = symbols.compactMap { self.rowsBySymbol[$0.key] }
            self.rows = ordered
            self.lock.unlock()
            self.renderNamesIfNeeded(ordered)
        }
    }

    private enum FetchError: Error { case invalidURL, noData, httpStatus(Int) }

    private static func fetchTencent(
        _ symbols: [QuoteSymbol], completion: @escaping (Result<[String: Row], Error>) -> Void
    ) {
        let query = symbols.map(\.providerCode).joined(separator: ",")
        request("https://qt.gtimg.cn/q=" + query) { result in
            completion(result.map { parseTencent(text: $0, symbols: symbols) })
        }
    }

    private static func fetchSina(
        _ symbols: [QuoteSymbol], completion: @escaping (Result<[String: Row], Error>) -> Void
    ) {
        let query = symbols.map(\.providerCode).joined(separator: ",")
        request("https://hq.sinajs.cn/list=" + query,
                referer: "https://finance.sina.com.cn/") { result in
            completion(result.map { parseSina(text: $0, symbols: symbols) })
        }
    }

    private static func request(
        _ urlString: String, referer: String? = nil,
        completion: @escaping (Result<String, Error>) -> Void
    ) {
        guard let url = URL(string: urlString) else {
            completion(.failure(FetchError.invalidURL)); return
        }
        var req = URLRequest(url: url)
        req.timeoutInterval = 5
        if let referer = referer { req.setValue(referer, forHTTPHeaderField: "Referer") }
        URLSession.shared.dataTask(with: req) { data, response, error in
            if let error = error { completion(.failure(error)); return }
            if let status = (response as? HTTPURLResponse)?.statusCode, status != 200 {
                completion(.failure(FetchError.httpStatus(status))); return
            }
            guard let data = data else { completion(.failure(FetchError.noData)); return }
            completion(.success(decodeGB18030(data)))
        }.resume()
    }

    private static func decodeGB18030(_ data: Data) -> String {
        let gbk = String.Encoding(rawValue: CFStringConvertEncodingToNSStringEncoding(
            CFStringEncoding(CFStringEncodings.GB_18030_2000.rawValue)))
        return String(data: data, encoding: gbk)
            ?? String(data: data, encoding: .isoLatin1) ?? ""
    }

    /// Response is lines of `v_sh600519="1~贵州茅台~600519~1212.00~...";`
    /// fields split by "~": [1]=name [3]=price [31]=change [32]=change%.
    static func parseTencent(text: String, symbols: [QuoteSymbol]) -> [String: Row] {
        let requested = Dictionary(grouping: symbols) { $0.providerCode.lowercased() }
        var result: [String: Row] = [:]
        for line in text.split(whereSeparator: { $0.isNewline }) {
            guard let eq = line.firstIndex(of: "="), line.hasPrefix("v_") else { continue }
            let symbol = String(line[line.index(line.startIndex, offsetBy: 2)..<eq]).lowercased()
            guard let requestedSymbols = requested[symbol] else { continue }
            let f = line[line.index(after: eq)...]
                .trimmingCharacters(in: CharacterSet(charactersIn: "\";"))
                .components(separatedBy: "~")
            guard f.count > 32, let price = Double(f[3]), let chg = Double(f[31]),
                  let pct = Double(f[32]) else { continue }
            for requestedSymbol in requestedSymbols {
                result[requestedSymbol.key] = Row(
                    code: requestedSymbol.displayCode,
                    name: f[1],
                    price: formatPrice(price),
                    pct: String(format: "%+.2f%%", pct),
                    up: chg > 0 ? 1 : (chg < 0 ? -1 : 0))
            }
        }
        return result
    }

    /// Sina international fields used here: [0]=last [7]=previous close
    /// [13]=name. Commodity futures: [0]=name [8]=last [10]=previous settlement.
    /// CFFEX uses another layout: [3]=last [14]=previous settlement [last]=name.
    /// The endpoint is unofficial, so fixtures cover these positions in tests.
    static func parseSina(text: String, symbols: [QuoteSymbol]) -> [String: Row] {
        let requested = Dictionary(grouping: symbols) { $0.providerCode.lowercased() }
        var result: [String: Row] = [:]
        for rawLine in text.split(whereSeparator: { $0.isNewline }) {
            let line = String(rawLine)
            guard let eq = line.firstIndex(of: "=") else { continue }
            let lhs = String(line[..<eq])
            guard let marker = lhs.range(of: "hq_str_", options: .caseInsensitive) else { continue }
            let providerCode = String(lhs[marker.upperBound...]).lowercased()
            guard let requestedSymbols = requested[providerCode],
                  let symbol = requestedSymbols.first else { continue }
            let fields = line[line.index(after: eq)...]
                .trimmingCharacters(in: CharacterSet(charactersIn: "\";"))
                .components(separatedBy: ",")

            let name: String
            let price: Double
            let reference: Double
            switch symbol.provider {
            case .sinaInternational:
                guard fields.count > 13, let p = Double(fields[0]),
                      let ref = Double(fields[7]), p > 0, ref > 0 else { continue }
                name = fields[13]
                price = p
                reference = ref
            case .sinaDomesticFutures:
                guard fields.count > 17, let p = Double(fields[8]),
                      let ref = Double(fields[10]), p > 0, ref > 0 else { continue }
                name = fields[0]
                price = p
                reference = ref
            case .sinaFinancialFutures:
                guard fields.count > 14, let p = Double(fields[3]),
                      let ref = Double(fields[14]), p > 0, ref > 0,
                      let last = fields.last, !last.isEmpty else { continue }
                name = last
                price = p
                reference = ref
            case .sinaForex:
                guard fields.count > 9, let p = Double(fields[8]),
                      let ref = Double(fields[3]), p > 0, ref > 0 else { continue }
                name = fields[9].isEmpty ? symbol.displayCode : fields[9]
                price = p
                reference = ref
            case .tencentStock:
                continue
            }
            let change = price - reference
            let pct = change / reference * 100
            for requestedSymbol in requestedSymbols {
                result[requestedSymbol.key] = Row(
                    code: requestedSymbol.displayCode,
                    name: name,
                    price: formatPrice(price, for: requestedSymbol),
                    pct: String(format: "%+.2f%%", pct),
                    up: change > 0 ? 1 : (change < 0 ? -1 : 0))
            }
        }
        return result
    }

    static func formatPrice(_ p: Double) -> String {
        if p >= 10000 { return String(format: "%.0f", p) }
        if p >= 1000 { return String(format: "%.1f", p) }
        return String(format: "%.2f", p)
    }

    static func formatPrice(_ p: Double, for symbol: QuoteSymbol) -> String {
        if let decimals = symbol.fixedPriceDecimals {
            return String(format: "%.*f", decimals, p)
        }
        return formatPrice(p)
    }
}
