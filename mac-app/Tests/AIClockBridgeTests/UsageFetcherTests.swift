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
        XCTAssertEqual(usage.weeklyResetAt ?? 0, now + 3_600, accuracy: 0.001)
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

    func testCodexSnapshotIncludesAbsoluteWeeklyResetTimeAndOffset() throws {
        let epoch = 1_786_786_074
        var codex = CodexStatus()
        codex.weeklyPct = 42.5
        codex.weeklyResetMin = 9_000
        codex.weeklyResetAt = epoch
        let snapshot = Snapshot(claude: ClaudeStatus(), codex: codex, ts: epoch - 60)

        let object = try XCTUnwrap(JSONSerialization.jsonObject(with: snapshot.jsonData())
            as? [String: Any])
        let encodedCodex = try XCTUnwrap(object["codex"] as? [String: Any])
        XCTAssertEqual((encodedCodex["weekly_reset_at"] as? NSNumber)?.intValue, epoch)
        let expectedOffset = TimeZone.current.secondsFromGMT(
            for: Date(timeIntervalSince1970: TimeInterval(epoch)))
        XCTAssertEqual((encodedCodex["weekly_reset_utc_offset_sec"] as? NSNumber)?.intValue,
                       expectedOffset)
    }
}
