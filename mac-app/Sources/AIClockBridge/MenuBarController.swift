import AppKit
import UniformTypeIdentifiers

// Menu-bar item: a retro Macintosh icon (drawn in code, template so it adapts
// to light/dark menu bars). Left click opens a live mirror of the ESP8266
// screen (MirrorPopover); right click opens the control menu with usage
// meters and device remote control. No quota text lives in the bar itself.
final class MenuBarController: NSObject, NSMenuDelegate {
    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
    private let service: StatusService
    private let usage: UsageFetcher
    private let port: UInt16
    private let market: MarketMonitor
    private let weather: WeatherMonitor
    private let controlMenu = NSMenu()
    private let mirrorPopover: MirrorPopoverController

    private let claudeUsageItem = NSMenuItem(title: "Claude …", action: nil, keyEquivalent: "")
    private let codexUsageItem = NSMenuItem(title: "Codex …", action: nil, keyEquivalent: "")
    private let deviceInfoItem = NSMenuItem(title: "设备：未设置", action: nil, keyEquivalent: "")
    private var modeItems: [String: NSMenuItem] = [:]
    private var marketInstrumentItems: [String: NSMenuItem] = [:]
    private let instrumentMenu = NSMenu()

    init(service: StatusService, usage: UsageFetcher, netMonitor: NetSpeedMonitor,
         nowPlaying: NowPlayingMonitor, stockMonitor: StockMonitor,
         market: MarketMonitor, weather: WeatherMonitor, port: UInt16) {
        self.service = service
        self.usage = usage
        self.port = port
        self.market = market
        self.weather = weather
        self.mirrorPopover = MirrorPopoverController(service: service, netMonitor: netMonitor,
                                                     nowPlaying: nowPlaying, stockMonitor: stockMonitor,
                                                     market: market, weather: weather)
        super.init()
        buildMenu()
        if let button = statusItem.button {
            button.image = Self.retroMacIcon()
            button.target = self
            button.action = #selector(statusItemClicked)
            button.sendAction(on: [.leftMouseUp, .rightMouseUp])
        }
    }

    /// User-supplied device logo (bezel + dark screen + smiley + green status
    /// dot). Full-color, so NOT a template image — it keeps its colors in
    /// both light and dark menu bars.
    private static func retroMacIcon() -> NSImage {
        guard let img = Bundle.module.image(forResource: "happy-mac") else {
            return NSImage(size: NSSize(width: 18, height: 18))
        }
        img.size = NSSize(width: 18, height: 18)
        img.isTemplate = false
        return img
    }

    /// Left click -> mirror popover; right click -> control menu.
    @objc private func statusItemClicked() {
        guard let button = statusItem.button else { return }
        let event = NSApp.currentEvent
        if event?.type == .rightMouseUp || event?.modifierFlags.contains(.control) == true {
            statusItem.menu = controlMenu
            button.performClick(nil)
            statusItem.menu = nil // detach so left click keeps toggling the popover
        } else {
            mirrorPopover.toggle(relativeTo: button)
        }
    }

    // MARK: - menu construction

    private func buildMenu() {
        let menu = controlMenu
        menu.delegate = self

        claudeUsageItem.isEnabled = false
        codexUsageItem.isEnabled = false
        menu.addItem(claudeUsageItem)
        menu.addItem(codexUsageItem)
        menu.addItem(.separator())

        deviceInfoItem.isEnabled = false
        menu.addItem(deviceInfoItem)

        menu.addItem(makeItem("自动查找并配对设备", #selector(autoPairAction)))
        menu.addItem(makeItem("设置设备地址…", #selector(setDeviceAddress)))
        menu.addItem(makeItem("打开设备网页", #selector(openDevicePage)))

        let displayMenu = NSMenu()
        for (title, mode) in [("自动轮播（按星期配置）", "auto"),
                              ("固定 Codex", "codex"), ("音乐播放", "music"),
                              ("四行报价", "stock"), ("K线行情", "market"),
                              ("日期天气", "weather")] {
            let item = NSMenuItem(title: title, action: #selector(setDisplayMode(_:)), keyEquivalent: "")
            item.target = self
            item.representedObject = mode
            modeItems[mode] = item
            displayMenu.addItem(item)
        }
        let displayItem = NSMenuItem(title: "屏幕显示", action: nil, keyEquivalent: "")
        displayItem.submenu = displayMenu
        menu.addItem(displayItem)
        menu.addItem(makeItem("设置工作日/周末自动轮播…", #selector(configureAutoCycle)))
        // (屏幕亮度在左键弹出的镜像页底部，做成滑条了)

        menu.addItem(makeItem("设置自选行情…", #selector(setStockSymbols)))
        menu.addItem(makeItem("设置天气城市…", #selector(setWeatherCity)))

        let marketIntervalMenu = NSMenu()
        for interval in MarketInterval.allCases {
            let item = NSMenuItem(title: interval.rawValue,
                                  action: #selector(setMarketInterval(_:)), keyEquivalent: "")
            item.target = self
            item.representedObject = interval.rawValue
            item.state = market.snapshot.interval == interval ? .on : .off
            marketIntervalMenu.addItem(item)
        }
        let marketIntervalItem = NSMenuItem(title: "K线周期", action: nil, keyEquivalent: "")
        marketIntervalItem.submenu = marketIntervalMenu
        menu.addItem(marketIntervalItem)

        let refreshMenu = NSMenu()
        for interval in MarketRefreshInterval.allCases {
            let item = NSMenuItem(title: interval.title,
                                  action: #selector(setMarketRefreshInterval(_:)), keyEquivalent: "")
            item.target = self
            item.representedObject = interval.rawValue
            item.state = market.selectedRefreshInterval == interval ? .on : .off
            refreshMenu.addItem(item)
        }
        let refreshItem = NSMenuItem(title: "K线轮换与刷新", action: nil, keyEquivalent: "")
        refreshItem.submenu = refreshMenu
        menu.addItem(refreshItem)

        rebuildMarketInstrumentMenu()
        let instrumentItem = NSMenuItem(title: "K线收藏标的", action: nil, keyEquivalent: "")
        instrumentItem.submenu = instrumentMenu
        menu.addItem(instrumentItem)
        menu.addItem(makeItem("搜索/添加 K线标的…", #selector(searchMarket)))
        menu.addItem(makeItem("删除 K线收藏…", #selector(removeMarketFavorite)))

        menu.addItem(makeItem("更换桌宠动画…（petdex）", #selector(openPetPicker)))

        let resetMenu = NSMenu()
        for (title, slot) in [("Claude 恢复默认", "claude"), ("Codex 恢复默认", "codex")] {
            let item = NSMenuItem(title: title, action: #selector(resetSprite(_:)), keyEquivalent: "")
            item.target = self
            item.representedObject = slot
            resetMenu.addItem(item)
        }
        let resetItem = NSMenuItem(title: "恢复默认动画", action: nil, keyEquivalent: "")
        resetItem.submenu = resetMenu
        menu.addItem(resetItem)

        menu.addItem(makeItem("把本机设为设备桥接", #selector(pointBridgeHere)))
        let settingsMenu = NSMenu()
        settingsMenu.addItem(makeItem("导出全部设置…", #selector(exportSettings)))
        settingsMenu.addItem(makeItem("从备份一键恢复…", #selector(restoreSettings)))
        let settingsItem = NSMenuItem(title: "设置备份与恢复", action: nil, keyEquivalent: "")
        settingsItem.submenu = settingsMenu
        menu.addItem(settingsItem)
        menu.addItem(.separator())
        menu.addItem(makeItem("刷新", #selector(refreshAction), key: "r"))
        menu.addItem(makeItem("桥接服务地址", #selector(showAddress)))
        menu.addItem(.separator())
        menu.addItem(NSMenuItem(title: "退出", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q"))
    }

    private func makeItem(_ title: String, _ action: Selector, key: String = "") -> NSMenuItem {
        let item = NSMenuItem(title: title, action: action, keyEquivalent: key)
        item.target = self
        return item
    }

    // MARK: - refresh

    func menuWillOpen(_ menu: NSMenu) {
        usage.refresh()
        refreshUsageLines()
        refreshDeviceSection()
    }

    private func refreshUsageLines() {
        claudeUsageItem.title = Self.usageLine(name: "Claude", u: usage.claude, weeklyLabel: "7天")
        codexUsageItem.title = Self.usageLine(name: "Codex", u: usage.codex, weeklyLabel: "周")
    }

    private static func usageLine(name: String, u: ProviderUsage, weeklyLabel: String) -> String {
        if let err = u.error, u.primaryPct == nil { return "\(name)：\(err)" }
        var parts: [String] = []
        if let p = u.primaryPct {
            var s = "5h \(Int(p))%"
            if let m = u.primaryResetMin { s += "（\(fmtMin(m))后重置）" }
            parts.append(s)
        }
        if let p = u.weeklyPct {
            var s = "\(weeklyLabel) \(Int(p))%"
            if let m = u.weeklyResetMin { s += "（\(fmtMin(m))）" }
            parts.append(s)
        }
        return parts.isEmpty ? "\(name)：额度未知" : "\(name)　" + parts.joined(separator: "　")
    }

    private static func fmtMin(_ min: Int) -> String {
        if min >= 48 * 60 { return "\(min / (24 * 60))天" }
        if min >= 60 { return "\(min / 60)h\(min % 60 > 0 ? "\(min % 60)m" : "")" }
        return "\(min)m"
    }

    private func refreshDeviceSection() {
        let host = DeviceClient.host
        guard !host.isEmpty else {
            deviceInfoItem.title = "设备：未设置地址"
            modeItems.values.forEach { $0.state = .off }
            return
        }
        deviceInfoItem.title = "设备：\(host)（连接中…）"
        DeviceClient.fetchInfo { [weak self] result in
            guard let self = self else { return }
            switch result {
            case let .success(info):
                let sprites = [info.claudeCustomSprite ? "C:自定义" : "C:默认",
                               info.codexCustomSprite ? "X:自定义" : "X:默认"]
                let effectiveTitle = Self.displayTitle(info.effective, showing: info.showing)
                let showing = info.mode == "auto" ? "自动：\(effectiveTitle)" : effectiveTitle
                self.deviceInfoItem.title =
                    "设备：\(info.ip) · 正在显示 \(showing) · \(sprites.joined(separator: " "))"
                for (mode, item) in self.modeItems { item.state = mode == info.mode ? .on : .off }
            case .failure:
                self.deviceInfoItem.title = "设备：\(host)（无法连接）"
                self.modeItems.values.forEach { $0.state = .off }
                // self-heal: the device may have moved to a new DHCP address;
                // if it recently polled us from a different IP, adopt that.
                let seen = DeviceClient.lastSeenIP
                if !seen.isEmpty, !host.hasPrefix(seen) {
                    DeviceClient.verifyDevice(ip: seen) { ok in
                        if ok {
                            DeviceClient.host = seen
                            self.refreshDeviceSection()
                        }
                    }
                }
            }
        }
    }

    // MARK: - pairing

    @objc private func autoPairAction() {
        deviceInfoItem.title = "设备：正在查找…"
        DeviceClient.autoPair(progress: { [weak self] msg in
            self?.deviceInfoItem.title = "设备：\(msg)"
        }, completion: { [weak self] ip in
            if let ip = ip {
                Self.toast("配对成功", "已找到设备并配对：\(ip)")
                self?.refreshDeviceSection()
            } else {
                Self.toast("未找到设备", """
                局域网内没有发现 ESP8266 时钟。请确认：
                1. 设备已通电并连上同一个 WiFi（首次使用需通过 AI-Clock-Setup 热点配网）
                2. 路由器未开启"客户端隔离"
                """)
                self?.refreshDeviceSection()
            }
        })
    }

    // MARK: - actions

    @objc private func refreshAction() {
        usage.refresh()
        refreshUsageLines()
        refreshDeviceSection()
    }

    @objc private func setDeviceAddress() {
        let alert = NSAlert()
        alert.messageText = "设备地址"
        alert.informativeText = "ESP8266 时钟的 IP（设备开机时屏幕上会显示，例如 192.168.1.50）"
        let input = NSTextField(frame: NSRect(x: 0, y: 0, width: 240, height: 24))
        input.stringValue = DeviceClient.host
        input.placeholderString = "192.168.1.50"
        alert.accessoryView = input
        alert.addButton(withTitle: "保存")
        alert.addButton(withTitle: "取消")
        NSApp.activate(ignoringOtherApps: true)
        if alert.runModal() == .alertFirstButtonReturn {
            DeviceClient.host = input.stringValue.trimmingCharacters(in: .whitespaces)
            refreshDeviceSection()
        }
    }

    @objc private func openDevicePage() {
        guard let url = DeviceClient.baseURL else {
            setDeviceAddress()
            return
        }
        NSWorkspace.shared.open(url)
    }

    @objc private func exportSettings() {
        guard DeviceClient.baseURL != nil else {
            Self.toast("无法备份", "请先配对设备；完整备份需要同时读取设备端设置。")
            return
        }
        let panel = NSSavePanel()
        panel.title = "导出 AI Clock 全部设置"
        panel.nameFieldStringValue = "AIClock-设置备份-\(Self.backupDate()).json"
        panel.allowedContentTypes = [.json]
        NSApp.activate(ignoringOtherApps: true)
        guard panel.runModal() == .OK, let url = panel.url else { return }
        DeviceClient.fetchSettings { result in
            do {
                let device = try result.get()
                let data = try SettingsBackup.makeData(device: device)
                try data.write(to: url, options: .atomic)
                Self.toast("备份完成", "Mac 行情、天气和设备显示设置已保存。\n\n未包含 Wi-Fi 密码、登录凭据和桌宠动画文件。")
            } catch {
                Self.toast("备份失败", error.localizedDescription)
            }
        }
    }

    @objc private func restoreSettings() {
        guard DeviceClient.baseURL != nil else {
            Self.toast("无法恢复", "请先配对要恢复设置的设备。")
            return
        }
        let panel = NSOpenPanel()
        panel.title = "选择 AI Clock 设置备份"
        panel.allowedContentTypes = [.json]
        panel.allowsMultipleSelection = false
        NSApp.activate(ignoringOtherApps: true)
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            let contents = try SettingsBackup.parse(Data(contentsOf: url))
            let confirm = NSAlert()
            confirm.messageText = "恢复这份设置？"
            confirm.informativeText = "将覆盖当前行情收藏、天气城市、亮度、显示模式和工作日/周末轮播设置。Wi-Fi 和桌宠动画不会改变。"
            confirm.addButton(withTitle: "一键恢复")
            confirm.addButton(withTitle: "取消")
            guard confirm.runModal() == .alertFirstButtonReturn else { return }
            var deviceSettings = contents.device
            if let localIP = DeviceClient.localIPv4() {
                deviceSettings["bridge"] = "\(localIP):\(port)"
            }
            DeviceClient.restoreSettings(deviceSettings) { [weak self] error in
                guard let self else { return }
                if let error {
                    Self.toast("恢复失败", "设备端没有修改：\(error.localizedDescription)")
                    return
                }
                do {
                    try SettingsBackup.applyMac(contents.mac)
                    self.market.reloadConfiguredSettings()
                    self.weather.reloadConfiguredLocation()
                    self.rebuildMarketInstrumentMenu()
                    self.refreshDeviceSection()
                    Self.toast("恢复完成", "Mac 与设备设置均已恢复并立即生效。")
                } catch {
                    Self.toast("部分恢复", "设备设置已恢复，但 Mac 设置失败：\(error.localizedDescription)")
                }
            }
        } catch {
            Self.toast("无法读取备份", error.localizedDescription)
        }
    }

    private static func backupDate() -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyy-MM-dd"
        return formatter.string(from: Date())
    }

    @objc private func setDisplayMode(_ sender: NSMenuItem) {
        guard let mode = sender.representedObject as? String else { return }
        DeviceClient.setDisplayMode(mode) { [weak self] error in
            if let error = error {
                Self.toast("切换失败", error.localizedDescription)
            } else {
                self?.refreshDeviceSection()
            }
        }
    }

    @objc private func configureAutoCycle() {
        DeviceClient.fetchInfo { result in
            guard case let .success(info) = result else {
                Self.toast("无法读取设置", "请确认设备在线且已安装 v0.5.2 或更高版本固件。")
                return
            }
            let pageOptions = [
                ("codex", "Codex"), ("music", "音乐"), ("stock", "四行报价"),
                ("market", "K线行情"), ("weather", "日期天气"),
            ]
            let stack = NSStackView()
            stack.orientation = .vertical
            stack.alignment = .leading
            stack.spacing = 9
            stack.setFrameSize(NSSize(width: 430, height: 255))
            let intervals = [5, 10, 15, 20, 30, 60, 120]

            func makeSchedule(title: String, selected: [String], seconds: Int)
                -> (NSView, [(String, NSButton)], NSPopUpButton) {
                let group = NSStackView()
                group.orientation = .vertical
                group.alignment = .leading
                group.spacing = 6
                let heading = NSTextField(labelWithString: title)
                heading.font = .boldSystemFont(ofSize: 13)
                group.addArrangedSubview(heading)
                let pages = NSStackView()
                pages.orientation = .horizontal
                pages.spacing = 9
                var checks: [(String, NSButton)] = []
                for (id, name) in pageOptions {
                    let check = NSButton(checkboxWithTitle: name, target: nil, action: nil)
                    check.state = selected.contains(id) ? .on : .off
                    pages.addArrangedSubview(check)
                    checks.append((id, check))
                }
                group.addArrangedSubview(pages)
                let intervalRow = NSStackView()
                intervalRow.orientation = .horizontal
                intervalRow.spacing = 10
                intervalRow.addArrangedSubview(NSTextField(labelWithString: "页面切换时间"))
                let popup = NSPopUpButton()
                popup.addItems(withTitles: intervals.map { "\($0) 秒" })
                popup.selectItem(at: intervals.firstIndex(of: seconds) ?? 1)
                intervalRow.addArrangedSubview(popup)
                group.addArrangedSubview(intervalRow)
                return (group, checks, popup)
            }

            let weekday = makeSchedule(title: "周一至周五", selected: info.weekdayAutoPages,
                                       seconds: info.weekdayAutoSeconds)
            let weekend = makeSchedule(title: "周六至周日", selected: info.weekendAutoPages,
                                       seconds: info.weekendAutoSeconds)
            stack.addArrangedSubview(weekday.0)
            let separator = NSBox()
            separator.boxType = .separator
            separator.widthAnchor.constraint(equalToConstant: 430).isActive = true
            stack.addArrangedSubview(separator)
            stack.addArrangedSubview(weekend.0)

            let alert = NSAlert()
            alert.messageText = "按星期设置自动轮播"
            alert.informativeText = "两组可选择不同页面和时间；K线页仍会等待全部收藏完整展示一轮。"
            alert.accessoryView = stack
            alert.addButton(withTitle: "保存并切换到自动")
            alert.addButton(withTitle: "取消")
            NSApp.activate(ignoringOtherApps: true)
            guard alert.runModal() == .alertFirstButtonReturn else { return }
            let weekdayPages = weekday.1.filter { $0.1.state == .on }.map { $0.0 }
            let weekendPages = weekend.1.filter { $0.1.state == .on }.map { $0.0 }
            guard !weekdayPages.isEmpty, !weekendPages.isEmpty else {
                Self.toast("无法保存", "工作日和周末都要至少选择一个轮播页面。")
                return
            }
            let weekdaySeconds = intervals[max(0, weekday.2.indexOfSelectedItem)]
            let weekendSeconds = intervals[max(0, weekend.2.indexOfSelectedItem)]
            DeviceClient.setAutoSchedule(weekdayPages: weekdayPages,
                                         weekdaySeconds: weekdaySeconds,
                                         weekendPages: weekendPages,
                                         weekendSeconds: weekendSeconds) { error in
                if let error {
                    Self.toast("保存失败", error.localizedDescription)
                } else {
                    DeviceClient.setDisplayMode("auto") { modeError in
                        if let modeError { Self.toast("切换失败", modeError.localizedDescription) }
                    }
                }
            }
        }
    }

    @objc private func setWeatherCity() {
        let alert = NSAlert()
        alert.messageText = "设置天气城市"
        alert.informativeText = "输入中文或英文城市名。保存后会自动匹配城市，并更新当地时间与天气。"
        let input = NSTextField(frame: NSRect(x: 0, y: 0, width: 300, height: 24))
        input.stringValue = weather.configuredCity
        input.placeholderString = "例如：上海、杭州、Tokyo"
        alert.accessoryView = input
        alert.addButton(withTitle: "查找并保存")
        alert.addButton(withTitle: "取消")
        NSApp.activate(ignoringOtherApps: true)
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        let city = input.stringValue
        weather.setCity(city) { result in
            switch result {
            case let .success(detail): Self.toast("天气城市已更新", detail)
            case let .failure(error): Self.toast("城市设置失败", error.localizedDescription)
            }
        }
    }

    @objc private func setStockSymbols() {
        let alert = NSAlert()
        alert.messageText = "自选行情"
        alert.informativeText = "逗号分隔：sh/sz/bj=A股、hk=港股、us=美股；fxXAUUSD=伦敦金、fxUSDCNH=离岸人民币、fxDXY=美元指数；sf/df/zf/cf/gf=国内期货；hf=海外期货\n例如 fxXAUUSD,sfAU0,fxUSDCNH,fxDXY（设备最多显示 4 项）"
        let input = NSTextField(frame: NSRect(x: 0, y: 0, width: 280, height: 24))
        input.stringValue = StockMonitor.symbols.joined(separator: ",")
        input.placeholderString = "sh000001,fxXAUUSD,sfAU0"
        alert.accessoryView = input
        alert.addButton(withTitle: "保存")
        alert.addButton(withTitle: "取消")
        NSApp.activate(ignoringOtherApps: true)
        if alert.runModal() == .alertFirstButtonReturn {
            StockMonitor.symbols = input.stringValue.split(separator: ",")
                .map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
        }
    }

    @objc private func setMarketInterval(_ sender: NSMenuItem) {
        guard let raw = sender.representedObject as? String,
              let interval = MarketInterval(rawValue: raw) else { return }
        market.setInterval(interval)
        sender.menu?.items.forEach { $0.state = $0 === sender ? .on : .off }
    }

    @objc private func setMarketRefreshInterval(_ sender: NSMenuItem) {
        guard let raw = sender.representedObject as? String,
              let interval = MarketRefreshInterval(rawValue: raw) else { return }
        market.setRefreshInterval(interval)
        sender.menu?.items.forEach { $0.state = $0 === sender ? .on : .off }
    }

    @objc private func setMarketInstrument(_ sender: NSMenuItem) {
        guard let id = sender.representedObject as? String,
              let instrument = market.favorites.first(where: { $0.id == id }) else { return }
        market.setInstrument(instrument)
        updateMarketInstrumentStates()
    }

    @objc private func searchMarket() {
        let alert = NSAlert()
        alert.messageText = "搜索/添加 K线标的"
        alert.informativeText = "支持 BTC/ETH、sh/sz/bj、hk、us、kr；以及 fxXAUUSD、fxUSDCNH、fxDXY、sfAU0、hfGC 等行情代码。"
        let input = NSTextField(frame: NSRect(x: 0, y: 0, width: 320, height: 24))
        input.placeholderString = "AAPL / hk00700 / fxXAUUSD / sfAU0"
        alert.accessoryView = input
        alert.addButton(withTitle: "显示并收藏")
        alert.addButton(withTitle: "仅显示")
        alert.addButton(withTitle: "取消")
        NSApp.activate(ignoringOtherApps: true)
        let response = alert.runModal()
        guard response == .alertFirstButtonReturn || response == .alertSecondButtonReturn else { return }
        guard let instrument = MarketInstrument.parse(input.stringValue) else {
            Self.toast("无法识别", "请检查市场前缀和代码。")
            return
        }
        if response == .alertFirstButtonReturn, !market.addFavorite(instrument) {
            Self.toast("收藏已满", "K线收藏最多 15 个；本次已仅显示该标的。")
        }
        market.setInstrument(instrument)
        rebuildMarketInstrumentMenu()
    }

    @objc private func removeMarketFavorite() {
        let favorites = market.favorites
        guard favorites.count > 1 else {
            Self.toast("无法删除", "K线行情至少需要保留一个收藏标的。")
            return
        }
        let popup = NSPopUpButton(frame: NSRect(x: 0, y: 0, width: 300, height: 26))
        popup.addItems(withTitles: favorites.map(\.menuTitle))
        if let selected = favorites.firstIndex(where: { $0.id == market.instrument.id }) {
            popup.selectItem(at: selected)
        }
        let alert = NSAlert()
        alert.messageText = "删除 K线收藏"
        alert.informativeText = "删除后，该标的将不再参与自动轮换。"
        alert.accessoryView = popup
        alert.addButton(withTitle: "删除")
        alert.addButton(withTitle: "取消")
        NSApp.activate(ignoringOtherApps: true)
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        let index = max(0, popup.indexOfSelectedItem)
        guard market.removeFavorite(id: favorites[index].id) else {
            Self.toast("无法删除", "K线行情至少需要保留一个收藏标的。")
            return
        }
        rebuildMarketInstrumentMenu()
    }

    private func rebuildMarketInstrumentMenu() {
        instrumentMenu.removeAllItems()
        marketInstrumentItems.removeAll()
        for instrument in market.favorites {
            let item = NSMenuItem(title: instrument.menuTitle,
                                  action: #selector(setMarketInstrument(_:)), keyEquivalent: "")
            item.target = self
            item.representedObject = instrument.id
            marketInstrumentItems[instrument.id] = item
            instrumentMenu.addItem(item)
        }
        updateMarketInstrumentStates()
    }

    private func updateMarketInstrumentStates() {
        marketInstrumentItems.values.forEach { $0.state = .off }
        marketInstrumentItems[market.instrument.id]?.state = .on
    }

    @objc private func openPetPicker() {
        if DeviceClient.host.isEmpty { setDeviceAddress() }
        PetPickerWindowController.shared.show()
    }

    @objc private func resetSprite(_ sender: NSMenuItem) {
        guard let slot = sender.representedObject as? String else { return }
        DeviceClient.resetSprite(slot: slot) { [weak self] error in
            if let error = error {
                Self.toast("恢复失败", error.localizedDescription)
            } else {
                self?.refreshDeviceSection()
            }
        }
    }

    @objc private func pointBridgeHere() {
        guard let ip = DeviceClient.localIPv4() else {
            Self.toast("失败", "获取本机局域网 IP 失败")
            return
        }
        let bridge = "\(ip):\(port)"
        DeviceClient.setBridgeHost(bridge) { error in
            if let error = error {
                Self.toast("设置失败", error.localizedDescription)
            } else {
                Self.toast("已设置", "设备将从 http://\(bridge)/status 拉取状态")
            }
        }
    }

    @objc private func showAddress() {
        let ip = DeviceClient.localIPv4() ?? "<本机局域网IP>"
        Self.toast("桥接服务地址", "http://\(ip):\(port)/status\n\n设备端 Bridge host 填：\(ip):\(port)")
    }

    private static func toast(_ title: String, _ text: String) {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = text
        NSApp.activate(ignoringOtherApps: true)
        alert.runModal()
    }

    private static func displayTitle(_ effective: String, showing: String) -> String {
        switch effective {
        case "claude": return "Claude"
        case "codex": return "Codex"
        case "net": return "网速"
        case "music": return "音乐"
        case "stock": return "四行报价"
        case "market": return "K线行情"
        case "weather": return "日期天气"
        default: return showing == "claude" ? "Claude" : "Codex"
        }
    }
}
