import XCTest
@testable import AIClockBridge

final class WeatherMonitorTests: XCTestCase {
    func testWeatherRLEEnvelopeRoundTripsAndChecksCRC() throws {
        var raw = Data()
        for index in 0..<WeatherMonitor.weatherTextPixelCount {
            let value: UInt16
            if index < 12_000 {
                value = 0x0000
            } else {
                value = index.isMultiple(of: 7) ? 0xFFFF : 0x10E2
            }
            raw.append(UInt8(value >> 8))
            raw.append(UInt8(value & 0xFF))
        }

        let revision: UInt32 = 0x1234_5678
        let packed = WeatherMonitor.packWeatherRLE(raw, revision: revision)
        XCTAssertEqual(String(decoding: packed.prefix(4), as: UTF8.self), "WTR1")
        XCTAssertLessThan(packed.count, raw.count)

        func u32(_ offset: Int) -> UInt32 {
            packed[offset..<(offset + 4)].reduce(0) { ($0 << 8) | UInt32($1) }
        }
        XCTAssertEqual(u32(4), revision)
        XCTAssertEqual(u32(8), UInt32(WeatherMonitor.weatherTextPixelCount))
        let payload = packed.dropFirst(16)
        XCTAssertEqual(u32(12), WeatherMonitor.crc32(Data(payload)))

        let decoded = try XCTUnwrap(WeatherMonitor.unpackWeatherRLE(packed))
        XCTAssertEqual(decoded.revision, revision)
        XCTAssertEqual(decoded.raw, raw)

        var corrupted = packed
        corrupted[corrupted.count - 1] ^= 0x01
        XCTAssertNil(WeatherMonitor.unpackWeatherRLE(corrupted))
    }

    func testWeatherTextRevisionIsStableAndContentSensitive() {
        let first = WeatherMonitor.stableTextRevision("济南|8月29日 周六|晴|今天 晴|明天 多云")
        XCTAssertEqual(first,
                       WeatherMonitor.stableTextRevision("济南|8月29日 周六|晴|今天 晴|明天 多云"))
        XCTAssertNotEqual(first,
                          WeatherMonitor.stableTextRevision("济南|8月30日 周日|晴|今天 晴|明天 多云"))
        XCTAssertNotEqual(first, 0)
    }
}
