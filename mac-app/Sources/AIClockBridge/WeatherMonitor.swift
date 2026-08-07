import Foundation

struct WeatherSnapshot {
    var city = "SHANGHAI"
    var timeZone = "Asia/Shanghai"
    var utcOffsetSeconds = 8 * 3600
    var currentTemperature = 0.0
    var currentCode = 0
    var todayHigh = 0.0
    var todayLow = 0.0
    var todayCode = 0
    var tomorrowHigh = 0.0
    var tomorrowLow = 0.0
    var tomorrowCode = 0
    var updatedAt: Date?
    var error: String?

    var currentText: String { WeatherMonitor.conditionText(currentCode) }
    var todayText: String { WeatherMonitor.conditionText(todayCode) }
    var tomorrowText: String { WeatherMonitor.conditionText(tomorrowCode) }
    var hasData: Bool { updatedAt != nil }
}

/// Key-free weather source for the independent 240x240 date/weather page.
/// The bridge resolves a city once, persists its coordinates, then refreshes
/// Open-Meteo every 15 minutes. The last good snapshot survives transient
/// network failures so the clock never clears a useful forecast.
final class WeatherMonitor {
    private struct Location: Codable {
        var query: String
        var displayName: String
        var latitude: Double
        var longitude: Double
        var timeZone: String
    }

    private static let locationKey = "weather_location_v1"
    private static let defaultLocation = Location(query: "Shanghai", displayName: "SHANGHAI",
                                                  latitude: 31.2304, longitude: 121.4737,
                                                  timeZone: "Asia/Shanghai")
    private let lock = NSLock()
    private var value = WeatherSnapshot()
    private var location: Location
    private var timer: Timer?

    init() {
        if let data = UserDefaults.standard.data(forKey: Self.locationKey),
           let saved = try? JSONDecoder().decode(Location.self, from: data) {
            location = saved
        } else {
            location = Self.defaultLocation
        }
        value.city = location.displayName
        value.timeZone = location.timeZone
    }

    var snapshot: WeatherSnapshot {
        lock.lock(); defer { lock.unlock() }
        return value
    }

    var configuredCity: String { location.query }

    func start() {
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 15 * 60, repeats: true) { [weak self] _ in
            self?.refresh()
        }
    }

    func refresh(completion: ((Error?) -> Void)? = nil) {
        fetchForecast(for: location, completion: completion)
    }

    func setCity(_ query: String, completion: @escaping (Result<String, Error>) -> Void) {
        let trimmed = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else {
            completion(.failure(Self.error("城市名称不能为空")))
            return
        }
        var components = URLComponents(string: "https://geocoding-api.open-meteo.com/v1/search")!
        components.queryItems = [
            URLQueryItem(name: "name", value: trimmed),
            URLQueryItem(name: "count", value: "1"),
            URLQueryItem(name: "language", value: "en"),
            URLQueryItem(name: "format", value: "json"),
        ]
        var request = URLRequest(url: components.url!)
        request.timeoutInterval = 12
        URLSession.shared.dataTask(with: request) { [weak self] data, response, networkError in
            if let networkError {
                DispatchQueue.main.async { completion(.failure(networkError)) }
                return
            }
            guard (response as? HTTPURLResponse)?.statusCode == 200, let data,
                  let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let result = (root["results"] as? [[String: Any]])?.first,
                  let latitude = (result["latitude"] as? NSNumber)?.doubleValue,
                  let longitude = (result["longitude"] as? NSNumber)?.doubleValue else {
                DispatchQueue.main.async { completion(.failure(Self.error("没有找到这个城市"))) }
                return
            }
            let name = result["name"] as? String ?? trimmed
            let admin = result["admin1"] as? String
            let country = result["country"] as? String
            let detail = [name, admin, country].compactMap { $0 }.joined(separator: ", ")
            let newLocation = Location(query: trimmed, displayName: Self.asciiCity(name),
                                       latitude: latitude, longitude: longitude,
                                       timeZone: result["timezone"] as? String ?? "auto")
            self?.fetchForecast(for: newLocation) { error in
                DispatchQueue.main.async {
                    if let error { completion(.failure(error)); return }
                    guard let self else { return }
                    self.location = newLocation
                    if let encoded = try? JSONEncoder().encode(newLocation) {
                        UserDefaults.standard.set(encoded, forKey: Self.locationKey)
                    }
                    completion(.success(detail))
                }
            }
        }.resume()
    }

    func jsonData() -> Data {
        let snap = snapshot
        let object: [String: Any] = [
            "city": snap.city,
            "timezone": snap.timeZone,
            "utc_offset_seconds": snap.utcOffsetSeconds,
            "schedule_utc_offset_seconds": TimeZone.current.secondsFromGMT(for: Date()),
            "server_unix": Int(Date().timeIntervalSince1970),
            "valid": snap.hasData,
            "stale": snap.updatedAt.map { Date().timeIntervalSince($0) > 45 * 60 } ?? true,
            "current_temp": snap.currentTemperature,
            "current_code": snap.currentCode,
            "current_text": snap.currentText,
            "today_high": snap.todayHigh,
            "today_low": snap.todayLow,
            "today_code": snap.todayCode,
            "today_text": snap.todayText,
            "tomorrow_high": snap.tomorrowHigh,
            "tomorrow_low": snap.tomorrowLow,
            "tomorrow_code": snap.tomorrowCode,
            "tomorrow_text": snap.tomorrowText,
            "updated_unix": Int(snap.updatedAt?.timeIntervalSince1970 ?? 0),
        ]
        return (try? JSONSerialization.data(withJSONObject: object)) ?? Data("{}".utf8)
    }

    private func fetchForecast(for target: Location, completion: ((Error?) -> Void)?) {
        var components = URLComponents(string: "https://api.open-meteo.com/v1/forecast")!
        components.queryItems = [
            URLQueryItem(name: "latitude", value: String(target.latitude)),
            URLQueryItem(name: "longitude", value: String(target.longitude)),
            URLQueryItem(name: "current", value: "temperature_2m,weather_code"),
            URLQueryItem(name: "daily", value: "weather_code,temperature_2m_max,temperature_2m_min"),
            URLQueryItem(name: "timezone", value: "auto"),
            URLQueryItem(name: "forecast_days", value: "2"),
        ]
        var request = URLRequest(url: components.url!)
        request.timeoutInterval = 12
        URLSession.shared.dataTask(with: request) { [weak self] data, response, networkError in
            let finish: (Error?) -> Void = { error in DispatchQueue.main.async { completion?(error) } }
            if let networkError { self?.record(error: networkError); finish(networkError); return }
            guard (response as? HTTPURLResponse)?.statusCode == 200, let data,
                  let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let current = root["current"] as? [String: Any],
                  let daily = root["daily"] as? [String: Any],
                  let temperature = (current["temperature_2m"] as? NSNumber)?.doubleValue,
                  let currentCode = (current["weather_code"] as? NSNumber)?.intValue,
                  let codes = daily["weather_code"] as? [NSNumber], codes.count >= 2,
                  let highs = daily["temperature_2m_max"] as? [NSNumber], highs.count >= 2,
                  let lows = daily["temperature_2m_min"] as? [NSNumber], lows.count >= 2 else {
                let error = Self.error("天气服务返回了无法识别的数据")
                self?.record(error: error); finish(error); return
            }
            var next = WeatherSnapshot()
            next.city = target.displayName
            next.timeZone = root["timezone"] as? String ?? target.timeZone
            next.utcOffsetSeconds = (root["utc_offset_seconds"] as? NSNumber)?.intValue ?? 0
            next.currentTemperature = temperature
            next.currentCode = currentCode
            next.todayCode = codes[0].intValue
            next.todayHigh = highs[0].doubleValue
            next.todayLow = lows[0].doubleValue
            next.tomorrowCode = codes[1].intValue
            next.tomorrowHigh = highs[1].doubleValue
            next.tomorrowLow = lows[1].doubleValue
            next.updatedAt = Date()
            self?.lock.lock(); self?.value = next; self?.lock.unlock()
            finish(nil)
        }.resume()
    }

    private func record(error: Error) {
        lock.lock(); value.error = error.localizedDescription; lock.unlock()
    }

    static func conditionText(_ code: Int) -> String {
        switch code {
        case 0: return "SUNNY"
        case 1, 2: return "PARTLY"
        case 3: return "CLOUDY"
        case 45, 48: return "FOG"
        case 51, 53, 55, 56, 57: return "DRIZZLE"
        case 61, 63, 65, 66, 67: return "RAIN"
        case 71, 73, 75, 77, 85, 86: return "SNOW"
        case 80, 81, 82: return "SHOWERS"
        case 95, 96, 99: return "STORM"
        default: return "WEATHER"
        }
    }

    private static func asciiCity(_ value: String) -> String {
        let latin = value.applyingTransform(.toLatin, reverse: false)?
            .applyingTransform(.stripDiacritics, reverse: false) ?? value
        let allowed = latin.uppercased().unicodeScalars.filter {
            CharacterSet(charactersIn: "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -").contains($0)
        }
        let cleaned = String(String.UnicodeScalarView(allowed))
            .split(whereSeparator: { $0 == " " }).joined(separator: " ")
        return String((cleaned.isEmpty ? "CITY" : cleaned).prefix(18))
    }

    private static func error(_ message: String) -> NSError {
        NSError(domain: "AIClockWeather", code: 1,
                userInfo: [NSLocalizedDescriptionKey: message])
    }
}
