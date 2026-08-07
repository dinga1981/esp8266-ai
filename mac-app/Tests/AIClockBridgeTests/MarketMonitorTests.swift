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
}
