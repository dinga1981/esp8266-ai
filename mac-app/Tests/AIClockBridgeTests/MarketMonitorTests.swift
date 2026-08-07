import XCTest
@testable import AIClockBridge

final class MarketMonitorTests: XCTestCase {
    func testLondonSpotGoldUsesOHLCForexFeed() throws {
        let instrument = try XCTUnwrap(MarketInstrument.parse("fxXAUUSD"))

        XCTAssertEqual(instrument.id, "fx-XAUUSD")
        XCTAssertEqual(instrument.region, .forex)
        XCTAssertEqual(instrument.providerCode, "fx_sxauusd")
        XCTAssertEqual(instrument.symbol, "XAU/USD")
        XCTAssertEqual(instrument.currency, "USD")
    }

    func testLondonSpotGoldAliasesShareTheOHLCFeed() throws {
        for alias in ["XAU", "XAUUSD", "伦敦金"] {
            let instrument = try XCTUnwrap(MarketInstrument.parse(alias))
            XCTAssertEqual(instrument.region, .forex)
            XCTAssertEqual(instrument.providerCode, "fx_sxauusd")
        }
    }

    func testSavedCustomFuturesAndForexIDsRestore() throws {
        let shfe = try XCTUnwrap(MarketInstrument.parse("sf-AU2608"))
        XCTAssertEqual(shfe.id, "sf-AU2608")
        XCTAssertEqual(shfe.providerCode, "AU2608")

        XCTAssertEqual(MarketInstrument.parse("fx-XAUUSD")?.id, "fx-XAUUSD")
        XCTAssertEqual(MarketInstrument.parse("fx-DXY")?.providerCode, "100.UDI")
    }

    func testFallbackPricePointsBecomeVisibleCandles() throws {
        let candles = MarketMonitor.candlesFromPricePoints([
            (1_000, 4_000.0), (1_060, 4_002.5), (1_120, 3_998.0),
        ])

        XCTAssertEqual(candles.count, 3)
        XCTAssertEqual(candles[1].open, 4_000.0)
        XCTAssertEqual(candles[1].close, 4_002.5)
        XCTAssertEqual(candles[1].high, 4_002.5)
        XCTAssertEqual(candles[1].low, 4_000.0)
        XCTAssertNotEqual(candles[1].open, candles[1].close)
    }
}
