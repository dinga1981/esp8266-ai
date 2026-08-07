<p align="center">
  <img src="docs/images/logo.svg" width="72" alt="logo">
</p>

<h1 align="center">ESP8266 AI 行情小屏增强版</h1>

<p align="center">Codex 状态 · 多市场报价 · K 线行情 · 日期天气 · 自定义轮播</p>

<p align="center">
  <a href="https://github.com/dinga1981/esp8266-ai/releases/latest">下载最新版</a> ·
  <a href="CHANGELOG.md">版本记录</a> ·
  <a href="https://github.com/pengchujin/esp8266-ai">原作者项目</a>
</p>

> 当前增强版：`v0.5.7`。本项目从
> [`pengchujin/esp8266-ai`](https://github.com/pengchujin/esp8266-ai) 的
> `v0.4.9` 分支发展，增强版版本号与上游版本号独立。上游后续功能会按需移植，
> 不会自动合并覆盖本项目功能。

这是为 240×240 ST7789 小电视屏幕设计的 ESP8266 固件和 macOS 菜单栏桥接程序。
设备通过局域网从 Mac 获取 Codex 状态、行情和天气数据，再由 ESP8266 独立绘制各个页面。

## v0.5.7 主要功能

- **Codex 状态与额度**：Nothing 风格点阵数字、5 小时/周额度、重置倒计时和等待审批提醒。
- **四行报价**：一页最多显示 4 个 A 股、港股、美股、国内外期货、伦敦金、离岸人民币或美元指数。
- **全屏 K 线**：支持 1、5、60 分钟周期，最多收藏 15 个标的，支持添加和删除收藏。
- **多市场行情**：BTC/ETH、A 股、港股、美股、韩股、主要指数、国内期货、海外期货、伦敦金、离岸人民币和美元指数。
- **稳定轮换**：K 线下一标的提前加载，完整校验后一次切屏；接口失败时跳过，不拖住整个轮换。
- **日期天气**：城市、月日、星期、时分秒、当前天气与温度，以及今日和明日预报。
- **按星期自动轮播**：工作日和周末可分别选择页面及 5/10/30/60/120 秒间隔；进入 K 线页后会等待收藏完整展示一轮。
- **音乐和桌宠**：保留上游音乐显示、自定义 GIF 桌宠、亮度控制和实时屏幕预览。

`v0.5.7` 的菜单中暂时隐藏 Claude 页面和网速页面，但底层代码仍保留。增强功能目前仅适配
macOS 桥接程序；仓库中的 Windows 桥接程序沿用上游代码，不包含上述新增行情和天气能力。

## 硬件要求

- ESP8266 / ESP-12E / NodeMCU v2 兼容板
- 240×240 ST7789 SPI 屏幕
- 项目所用 SD2 小电视引脚配置见 `firmware/platformio.ini`

本固件面向 **ESP8266**，不适用于 ESP32-C3，也不能直接刷入采用不同引脚定义的开发板。

## 快速使用

1. 从 [Releases](https://github.com/dinga1981/esp8266-ai/releases/latest) 下载对应版本的 Web 刷机包和 Mac 桥接程序。
2. 使用 Chrome 或 Edge 打开刷机包中的网页，选择 ESP8266 串口并烧录；从旧版本升级时不要擦除设备，以保留 Wi-Fi 和设置。
3. 首次启动后连接设备创建的 `AI-Clock-Setup` 热点，在 `192.168.4.1` 完成 Wi-Fi 配置。
4. 解压并启动 `AIClockBridge.app`，允许本地网络访问。
5. 右键菜单配置四行报价、K 线收藏、天气城市以及工作日/周末自动轮播。

macOS 首次阻止打开时，可在“系统设置 → 隐私与安全性”中选择允许。

## 行情代码

### 四行报价

用英文逗号分隔，设备最多显示前 4 项：

```text
sh000001,hk00700,usAAPL,fxXAUUSD
fxXAUUSD,sfAU0,fxUSDCNH,fxDXY
```

| 前缀 | 市场 | 示例 |
|---|---|---|
| `sh` / `sz` / `bj` | A 股 | `sh600519` |
| `hk` | 港股 | `hk00700` |
| `us` | 美股 | `usAAPL` |
| `fx` | 贵金属/汇率/美元指数 | `fxXAUUSD`、`fxUSDCNH`、`fxDXY` |
| `sf` / `df` / `zf` / `cf` / `gf` | 国内期货市场 | `sfAU0`、`dfI0`、`cfIF0` |
| `hf` | 海外期货 | `hfGC`、`hfCL` |

### K 线收藏

右键菜单选择“搜索/添加 K线标的…”。支持常见代码和中文别名，例如：

```text
BTC  ETH  sh000001  hk00700  usAAPL  kr005930
fxXAUUSD  fxUSDCNH  fxDXY  sfAU2608  sfAU0  hfGC
```

行情来自公开免密接口，可能存在延迟、限流或临时不可用，仅适合信息展示，不应用作自动交易依据。

## 本地构建

### macOS 桥接程序

需要 macOS 12 或更高版本和 Swift 5.9：

```bash
cd mac-app
swift test
swift build -c release --arch arm64
```

### ESP8266 固件

需要 PlatformIO：

```bash
cd firmware
pio run
```

默认环境为 `nodemcuv2`。实际刷写前请确认屏幕引脚和 Flash 布局与目标硬件一致。

## 目录

```text
firmware/     ESP8266 固件（PlatformIO + Arduino）
mac-app/      macOS 菜单栏桥接（Swift Package Manager）
windows-app/  上游 Windows 桥接代码，未同步本增强版功能
tools/        图片和精灵图辅助工具
docs/         硬件、接口和开发文档
```

## 版本与上游关系

- 上游作者：[pengchujin](https://github.com/pengchujin)
- 上游仓库：[pengchujin/esp8266-ai](https://github.com/pengchujin/esp8266-ai)
- 增强版分叉基线：上游 `v0.4.9`
- 上游 `v0.4.10`、`v0.4.11` 中与 Claude/Codex 额度页相关的改动已在增强版 `v0.5.5` 中按需移植。
- `v0.5.1` 至 `v0.5.7` 是根据本地留存源码快照重建的历史提交；它们不是开发当时自动保存的原始 Git 提交。

详细说明见 [NOTICE.md](NOTICE.md) 和 [CHANGELOG.md](CHANGELOG.md)。

## 授权说明

上游仓库目前未提供标准 `LICENSE` 文件，因此本仓库不擅自为整套派生代码添加新的许可证。
原始代码、图片和文档的权利归其各自作者所有；使用、再分发或商用前，请同时核对上游说明并自行确认授权范围。
