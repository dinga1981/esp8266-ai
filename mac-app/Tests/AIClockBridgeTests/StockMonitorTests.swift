import XCTest
@testable import AIClockBridge

final class StockMonitorTests: XCTestCase {
    func testMarketPrefixesRouteToProviderCodes() throws {
        let stock = try XCTUnwrap(StockMonitor.parseSymbol(" hk1810 "))
        XCTAssertEqual(stock.key, "hk01810")
        XCTAssertEqual(stock.provider, .tencentStock)
        XCTAssertEqual(stock.providerCode, "hk01810")

        let londonGold = try XCTUnwrap(StockMonitor.parseSymbol("fxxauusd"))
        XCTAssertEqual(londonGold.key, "fxxauusd")
        XCTAssertEqual(londonGold.provider, .sinaInternational)
        XCTAssertEqual(londonGold.providerCode, "hf_XAU")
        XCTAssertEqual(londonGold.displayCode, "XAUUSD")

        let shfeGold = try XCTUnwrap(StockMonitor.parseSymbol("sfau0"))
        XCTAssertEqual(shfeGold.provider, .sinaDomesticFutures)
        XCTAssertEqual(shfeGold.providerCode, "nf_AU0")

        let comexGold = try XCTUnwrap(StockMonitor.parseSymbol("hfgc"))
        XCTAssertEqual(comexGold.provider, .sinaInternational)
        XCTAssertEqual(comexGold.providerCode, "hf_GC")

        let cffex = try XCTUnwrap(StockMonitor.parseSymbol("cfIF0"))
        XCTAssertEqual(cffex.provider, .sinaFinancialFutures)
        XCTAssertEqual(cffex.providerCode, "nf_IF0")
    }

    func testUnsupportedSpotPairIsRejected() {
        XCTAssertNil(StockMonitor.parseSymbol("fxEURUSD"))
        XCTAssertNil(StockMonitor.parseSymbol("unknown"))
    }

    func testOffshoreRmbAndDollarIndexRouting() throws {
        let cnh = try XCTUnwrap(StockMonitor.parseSymbol("fxUSDCNH"))
        XCTAssertEqual(cnh.provider, .sinaForex)
        XCTAssertEqual(cnh.providerCode, "fx_susdcnh")
        XCTAssertEqual(cnh.fixedPriceDecimals, 4)

        let dxy = try XCTUnwrap(StockMonitor.parseSymbol("fxDXY"))
        XCTAssertEqual(dxy.provider, .sinaForex)
        XCTAssertEqual(dxy.providerCode, "DINIW")
        XCTAssertEqual(dxy.fixedPriceDecimals, 3)
    }

    func testTencentStockFixture() throws {
        let symbol = try XCTUnwrap(StockMonitor.parseSymbol("sh600519"))
        var fields = Array(repeating: "", count: 33)
        fields[1] = "贵州茅台"
        fields[3] = "1327.50"
        fields[31] = "74.50"
        fields[32] = "5.95"
        let text = "v_sh600519=\"\(fields.joined(separator: "~"))\";"

        let rows = StockMonitor.parseTencent(text: text, symbols: [symbol])
        let row = try XCTUnwrap(rows[symbol.key])
        XCTAssertEqual(row.code, "600519")
        XCTAssertEqual(row.name, "贵州茅台")
        XCTAssertEqual(row.price, "1327.5")
        XCTAssertEqual(row.pct, "+5.95%")
        XCTAssertEqual(row.up, 1)
    }

    func testSinaGoldFixturesAndReferencePrices() throws {
        let londonGold = try XCTUnwrap(StockMonitor.parseSymbol("fxXAUUSD"))
        let shfeGold = try XCTUnwrap(StockMonitor.parseSymbol("sfAU0"))
        let text = """
        var hq_str_hf_XAU="4005.64,4016.360,4005.64,4005.99,4040.52,3982.83,22:59:00,4016.36,4006.04,0,0,0,2026-07-20,伦敦金（现货黄金）";
        var hq_str_nf_AU0="黄金连续,225952,878.020,878.880,875.060,0.000,877.120,877.200,877.180,0.000,876.740,5,2,119894.000,20430,沪,黄金,2026-07-20,1";
        """

        let rows = StockMonitor.parseSina(text: text, symbols: [londonGold, shfeGold])
        let xau = try XCTUnwrap(rows[londonGold.key])
        XCTAssertEqual(xau.code, "XAUUSD")
        XCTAssertEqual(xau.name, "伦敦金（现货黄金）")
        XCTAssertEqual(xau.price, "4005.64")
        XCTAssertEqual(xau.pct, "-0.27%")
        XCTAssertEqual(xau.up, -1)

        let au = try XCTUnwrap(rows[shfeGold.key])
        XCTAssertEqual(au.code, "AU0")
        XCTAssertEqual(au.name, "黄金连续")
        XCTAssertEqual(au.price, "877.18")
        XCTAssertEqual(au.pct, "+0.05%")
        XCTAssertEqual(au.up, 1)
    }

    func testTwoUserSymbolsMayShareOneProviderCode() throws {
        let spotAlias = try XCTUnwrap(StockMonitor.parseSymbol("fxXAUUSD"))
        let providerStyleMarket = try XCTUnwrap(StockMonitor.parseSymbol("hfXAU"))
        let text = """
        var hq_str_hf_XAU="4005.64,4016.360,4005.64,4005.99,4040.52,3982.83,22:59:00,4016.36,4006.04,0,0,0,2026-07-20,伦敦金（现货黄金）";
        """

        let rows = StockMonitor.parseSina(
            text: text, symbols: [spotAlias, providerStyleMarket])
        XCTAssertNotNil(rows[spotAlias.key])
        XCTAssertNotNil(rows[providerStyleMarket.key])
    }

    func testCffexUsesItsDedicatedFieldLayout() throws {
        let cffex = try XCTUnwrap(StockMonitor.parseSymbol("cfIF0"))
        let text = """
        var hq_str_nf_IF0="4530.000,4590.000,4465.000,4545.800,118856,539099700.800,167899.000,4545.800,0.000,4950.200,4050.200,0.000,0.000,4473.800,4500.200,174134.000,4545.800,1,0.000,0,0.000,0,0.000,0,0.000,0,4546.000,4,0.000,0,0.000,0,0.000,0,0.000,0,2026-07-20,15:00:00,200,1,,,,,,,,,4535.738,沪深300指数期货连续";
        """

        let rows = StockMonitor.parseSina(text: text, symbols: [cffex])
        let row = try XCTUnwrap(rows[cffex.key])
        XCTAssertEqual(row.code, "IF0")
        XCTAssertEqual(row.name, "沪深300指数期货连续")
        XCTAssertEqual(row.price, "4545.8")
        XCTAssertEqual(row.pct, "+1.01%")
        XCTAssertEqual(row.up, 1)
    }
}
