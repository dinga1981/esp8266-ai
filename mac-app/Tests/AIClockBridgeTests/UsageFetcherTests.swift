import XCTest
@testable import AIClockBridge

final class UsageFetcherTests: XCTestCase {
    func testWeeklyOnlyCodexCacheRestoresOnColdStart() throws {
        let key = "usage_cache_codex"
        let now = Date().timeIntervalSince1970
        let cached: [String: Any] = [
            "at": now,
            "wPct": 42.5,
            "wResetAt": now + 3_600,
        ]
        let data = try JSONSerialization.data(withJSONObject: cached)
        UserDefaults.standard.set(data, forKey: key)
        defer { UserDefaults.standard.removeObject(forKey: key) }

        let usage = UsageFetcher().codex
        XCTAssertEqual(usage.weeklyPct, 42.5)
        XCTAssertNil(usage.primaryPct)
        XCTAssertNotNil(usage.weeklyResetMin)
        XCTAssertTrue((58...60).contains(usage.weeklyResetMin!))
    }

    func testQuotaCacheOlderThanOneDayIsIgnored() throws {
        let key = "usage_cache_codex"
        let now = Date().timeIntervalSince1970
        let cached: [String: Any] = [
            "at": now - 25 * 3_600,
            "wPct": 99.0,
            "wResetAt": now + 3_600,
        ]
        let data = try JSONSerialization.data(withJSONObject: cached)
        UserDefaults.standard.set(data, forKey: key)
        defer { UserDefaults.standard.removeObject(forKey: key) }

        XCTAssertNil(UsageFetcher().codex.weeklyPct)
    }
}
