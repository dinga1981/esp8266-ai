// ESP8266 WiFi clock: shows local time plus live Claude Code / Codex CLI
// working status and usage quota, polled from a small bridge service that
// runs on the developer's Mac (see ../bridge/bridge.py).
//
// Display: 240x240 SPI ST7789 (TFT_eSPI). Pin mapping is set via build_flags
// in platformio.ini - edit those if your wiring differs.

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <Updater.h>
#include <WiFiClient.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <AnimatedGIF.h>

#include "config.h"
#include "img/claude_sprite.h"
#include "img/codex_sprite.h"
#include "img/claude_logo.h"
#include "img/codex_logo.h"

TFT_eSPI tft = TFT_eSPI();
ESP8266WebServer webServer(80);

// ---------- custom sprite storage (LittleFS) ----------
// Custom uploads replace the compiled-in default animation without needing a
// firmware rebuild. You POST a raw .gif straight to /sprite/claude or
// /sprite/codex (the device serves its own upload page at "/"); the ESP8266
// decodes and rescales the GIF *on-device* (AnimatedGIF, line-by-line so it
// never needs a full-canvas buffer) into the wire format below, which the
// display path then reads back frame-by-frame:
//   [1 byte frame count][frame0 bytes][frame1 bytes]...
// Each frame is exactly CLAUDE_SPRITE_W x H (or CODEX_SPRITE_W x H) RGB565
// pixels, byte order matching tools/convert_sprites.py's to_rgb565() so the
// compiled-in defaults and custom uploads share one draw path.
const char *CLAUDE_SPRITE_FILE = "/c.bin";
const char *CODEX_SPRITE_FILE = "/x.bin";
const char *CLAUDE_GIF_FILE = "/c.gif"; // raw upload, decoded then removed
const char *CODEX_GIF_FILE = "/x.gif";
const int MAX_CUSTOM_FRAMES = 8;
const size_t CLAUDE_FRAME_BYTES = (size_t)CLAUDE_SPRITE_W * CLAUDE_SPRITE_H * 2;
const size_t CODEX_FRAME_BYTES = (size_t)CODEX_SPRITE_W * CODEX_SPRITE_H * 2;

// We never hold a whole sprite frame in RAM. Decoding a GIF needs ~24KB of
// heap for AnimatedGIF's own buffers, which wouldn't fit alongside a static
// full-frame buffer (a 120x120 frame is ~28KB) on the ESP8266's ~80KB. So both
// the display path and the decoder work one screen-row at a time through these
// two small scratch rows (SCREEN_W is the widest we ever need).
uint16_t rowBuf[SCREEN_W];     // current row being drawn / decoded
uint16_t prevRowBuf[SCREEN_W]; // decode only: same row from the previous frame

bool claudeCustom = false;
int claudeCustomFrames = 0;
bool codexCustom = false;
int codexCustomFrames = 0;
uint32_t spriteRev = 0; // bumped on upload/reset so the Mac mirror re-fetches

const int SCREEN_CX = 120, SCREEN_CY = 120;
const int RING_MARGIN = 4;      // inset from screen edge
const int RING_THICKNESS = 10;  // ring bar thickness
const unsigned long ANIM_INTERVAL_MS = 120;  // sprite frame advance
const unsigned long FLASH_INTERVAL_MS = 400; // "urgent" flash speed
const unsigned long SWITCH_BOTH_MS = 2000;   // both apps working: alternate fast
const unsigned long SWITCH_IDLE_MS = 6000;   // neither working: alternate slow

enum ActiveApp { APP_CLAUDE, APP_CODEX };
ActiveApp currentApp = APP_CLAUDE;
unsigned long lastSwitchMs = 0;

// Display override, settable from the Mac app via POST /api/display:
// auto = follow working status, claude/codex = pin that app on screen,
// net/music = show Mac-side telemetry pages instead of the pet.
enum DisplayMode {
  MODE_AUTO, MODE_CLAUDE, MODE_CODEX, MODE_NET, MODE_MUSIC, MODE_STOCK, MODE_MARKET,
  MODE_WEATHER
};
DisplayMode displayMode = MODE_AUTO;

// ---------- reset / last-operation diagnostics ----------
// RTC user memory survives watchdog/software resets (but not necessarily a
// full power loss), so it can tell us which operation was active immediately
// before an unexpected restart without writing flash on every network poll.
enum RuntimeStage : uint8_t {
  STAGE_BOOT, STAGE_IDLE, STAGE_BRIDGE, STAGE_NET, STAGE_MUSIC, STAGE_STOCK,
  STAGE_WEATHER, STAGE_MARKET_META, STAGE_MARKET_FRAME, STAGE_WEB,
  STAGE_WIFI_RECOVERY, STAGE_OTA
};

enum RecoveryStep : uint8_t {
  RECOVERY_NONE, RECOVERY_QUICK_RECONNECT, RECOVERY_WIFI_REINIT,
  RECOVERY_RADIO_RESET, RECOVERY_PROTECTIVE_RESTART, RECOVERY_MANUAL,
  RECOVERY_AUTH_REJOIN
};

enum NetworkTask : uint8_t {
  NET_TASK_NONE, NET_TASK_BRIDGE, NET_TASK_NET, NET_TASK_MUSIC_META,
  NET_TASK_MUSIC_COVER, NET_TASK_MUSIC_TEXT, NET_TASK_STOCK_META,
  NET_TASK_STOCK_NAMES, NET_TASK_WEATHER_META, NET_TASK_WEATHER_TEXT,
  NET_TASK_MARKET_META, NET_TASK_MARKET_FRAME, NET_TASK_HEALTH,
  NET_TASK_BOOT_REPORT
};

enum RequestPhase : uint8_t {
  REQUEST_NONE, REQUEST_QUEUED, REQUEST_CONNECTING, REQUEST_SENDING,
  REQUEST_READING, REQUEST_PARSING, REQUEST_RENDERING, REQUEST_CLEANUP,
  REQUEST_COMPLETE, REQUEST_FAILED
};

struct RtcRuntimeDiag {
  uint32_t magic;
  uint32_t checksum;
  uint32_t sequence;
  uint8_t stage;
  uint8_t mode;
  uint8_t wifiStatus;
  uint8_t recoveryStep;
  uint8_t firstWifiDisconnectReason;
  uint8_t lastWifiDisconnectReason;
  int8_t firstWifiRssi;
  uint8_t firstWifiChannel;
  uint8_t firstDisconnectWasRecovery;
  uint8_t lastDisconnectWasRecovery;
  uint16_t recoveryMask;
  uint8_t firstWifiBssid[6];
  uint8_t reserved[2];
  uint32_t bridgeFailures;
  uint32_t wifiDisconnectCount;
  uint32_t wifiReconnectCount;
  int32_t lastBridgeHttpCode;
  uint32_t outageDurationMs;
  uint32_t quickRecoveryAtMs;
  uint32_t wifiReinitAtMs;
  uint32_t radioResetAtMs;
  uint32_t protectiveRestartAtMs;
  uint32_t authRejoinAtMs;
  uint32_t millisAtStage;
  uint32_t requestStartedAtMs;
  uint32_t requestDurationMs;
  uint32_t requestBytes;
  uint32_t requestFreeHeap;
  uint32_t requestMaxBlock;
  int32_t requestCode;
  uint8_t networkTask;
  uint8_t requestPhase;
  uint8_t requestSucceeded;
  uint8_t reserved3;
};
static_assert((sizeof(RtcRuntimeDiag) % 4) == 0, "RTC diagnostics must be word aligned");
static_assert(32 * 4 + sizeof(RtcRuntimeDiag) <= 512,
              "RTC diagnostics exceed ESP8266 user memory");

const uint32_t RTC_DIAG_MAGIC = 0x4149434CUL; // "AICL"
// ESP8266 eboot stores its 128-byte OTA copy command at the start of RTC user
// memory. Diagnostics must begin after word 31 or a stage update between
// Update.end() and ESP.restart() can silently cancel an otherwise valid OTA.
const uint32_t RTC_DIAG_WORD_OFFSET = 32;
const char *BOOT_COUNT_FILE = "/boot_count.txt";
const char *RESTART_HISTORY_FILE = "/restart_history.bin";
const uint32_t RESTART_HISTORY_MAGIC = 0x52485332UL; // RHS2
const uint8_t RESTART_HISTORY_CAPACITY = 4;

struct RestartHistoryEntry {
  uint32_t bootCount;
  uint32_t rtcSequence;
  uint32_t outageDurationMs;
  uint8_t stage;
  uint8_t mode;
  uint8_t recoveryStep;
  uint8_t firstReason;
  uint8_t lastReason;
  uint8_t firstWasRecovery;
  uint8_t lastWasRecovery;
  int8_t firstRssi;
  uint8_t firstChannel;
  uint8_t networkTask;
  uint8_t requestPhase;
  uint8_t requestSucceeded;
  int32_t requestCode;
  uint32_t requestDurationMs;
  uint32_t requestBytes;
  uint32_t requestFreeHeap;
  uint32_t requestMaxBlock;
  uint32_t quickRecoveryAtMs;
  uint32_t wifiReinitAtMs;
  uint32_t radioResetAtMs;
  uint32_t protectiveRestartAtMs;
  uint32_t authRejoinAtMs;
  char resetReason[24];
  char resetInfo[96];
};

struct RestartHistoryStore {
  uint32_t magic;
  uint32_t checksum;
  uint8_t count;
  uint8_t reserved[3];
  RestartHistoryEntry entries[RESTART_HISTORY_CAPACITY];
};

RestartHistoryStore restartHistory = {};
RtcRuntimeDiag rtcRuntimeDiag = {};
uint8_t previousRuntimeStage = STAGE_BOOT;
uint8_t previousRuntimeMode = MODE_AUTO;
uint8_t previousRecoveryStep = RECOVERY_NONE;
uint8_t previousFirstWifiDisconnectReason = 0;
uint8_t previousLastWifiDisconnectReason = 0;
int8_t previousFirstWifiRssi = 0;
uint8_t previousFirstWifiChannel = 0;
bool previousFirstDisconnectWasRecovery = false;
bool previousLastDisconnectWasRecovery = false;
uint16_t previousRecoveryMask = 0;
uint8_t previousFirstWifiBssid[6] = {};
uint32_t previousBridgeFailures = 0;
uint32_t previousWifiDisconnectCount = 0;
uint32_t previousWifiReconnectCount = 0;
int32_t previousBridgeHttpCode = 0;
uint32_t previousOutageDurationMs = 0;
uint32_t previousQuickRecoveryAtMs = 0;
uint32_t previousWifiReinitAtMs = 0;
uint32_t previousRadioResetAtMs = 0;
uint32_t previousProtectiveRestartAtMs = 0;
uint32_t previousAuthRejoinAtMs = 0;
uint8_t previousNetworkTask = NET_TASK_NONE;
uint8_t previousRequestPhase = REQUEST_NONE;
bool previousRequestSucceeded = false;
int32_t previousRequestCode = 0;
uint32_t previousRequestDurationMs = 0;
uint32_t previousRequestBytes = 0;
uint32_t previousRequestFreeHeap = 0;
uint32_t previousRequestMaxBlock = 0;
uint8_t diagnosticEffectiveMode = MODE_AUTO;
uint32_t bootCount = 0;
char lastResetReason[32] = "Unknown";
char lastResetInfo[200] = "Unknown";

uint32_t rtcDiagChecksum(const RtcRuntimeDiag &value) {
  RtcRuntimeDiag copy = value;
  copy.checksum = 0;
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&copy);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < sizeof(copy); ++i) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
}

uint32_t restartHistoryChecksum(const RestartHistoryStore &value) {
  RestartHistoryStore copy = value;
  copy.checksum = 0;
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&copy);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < sizeof(copy); ++i) { hash ^= bytes[i]; hash *= 16777619UL; }
  return hash;
}

void loadRestartHistory() {
  File f = LittleFS.open(RESTART_HISTORY_FILE, "r");
  if (!f || f.size() != sizeof(restartHistory) ||
      f.read(reinterpret_cast<uint8_t *>(&restartHistory), sizeof(restartHistory)) !=
          sizeof(restartHistory) || restartHistory.magic != RESTART_HISTORY_MAGIC ||
      restartHistory.count > RESTART_HISTORY_CAPACITY ||
      restartHistory.checksum != restartHistoryChecksum(restartHistory)) {
    restartHistory = {};
  }
  if (f) f.close();
}

void appendRestartHistory(const RtcRuntimeDiag &previous) {
  for (int i = RESTART_HISTORY_CAPACITY - 1; i > 0; --i) {
    restartHistory.entries[i] = restartHistory.entries[i - 1];
  }
  RestartHistoryEntry &entry = restartHistory.entries[0];
  entry = {};
  entry.bootCount = bootCount;
  entry.rtcSequence = previous.sequence;
  entry.outageDurationMs = previous.outageDurationMs;
  entry.stage = previous.stage;
  entry.mode = previous.mode;
  entry.recoveryStep = previous.recoveryStep;
  entry.firstReason = previous.firstWifiDisconnectReason;
  entry.lastReason = previous.lastWifiDisconnectReason;
  entry.firstWasRecovery = previous.firstDisconnectWasRecovery;
  entry.lastWasRecovery = previous.lastDisconnectWasRecovery;
  entry.firstRssi = previous.firstWifiRssi;
  entry.firstChannel = previous.firstWifiChannel;
  entry.networkTask = previous.networkTask;
  entry.requestPhase = previous.requestPhase;
  entry.requestSucceeded = previous.requestSucceeded;
  entry.requestCode = previous.requestCode;
  entry.requestDurationMs = previous.requestDurationMs;
  entry.requestBytes = previous.requestBytes;
  entry.requestFreeHeap = previous.requestFreeHeap;
  entry.requestMaxBlock = previous.requestMaxBlock;
  entry.quickRecoveryAtMs = previous.quickRecoveryAtMs;
  entry.wifiReinitAtMs = previous.wifiReinitAtMs;
  entry.radioResetAtMs = previous.radioResetAtMs;
  entry.protectiveRestartAtMs = previous.protectiveRestartAtMs;
  entry.authRejoinAtMs = previous.authRejoinAtMs;
  strlcpy(entry.resetReason, lastResetReason, sizeof(entry.resetReason));
  strlcpy(entry.resetInfo, lastResetInfo, sizeof(entry.resetInfo));
  restartHistory.count = min((uint8_t)(restartHistory.count + 1), RESTART_HISTORY_CAPACITY);
  restartHistory.magic = RESTART_HISTORY_MAGIC;
  restartHistory.checksum = restartHistoryChecksum(restartHistory);
  File f = LittleFS.open(RESTART_HISTORY_FILE, "w");
  if (f) {
    f.write(reinterpret_cast<const uint8_t *>(&restartHistory), sizeof(restartHistory));
    f.close();
  }
}

const char *runtimeStageName(uint8_t stage) {
  switch (stage) {
    case STAGE_BOOT: return "启动";
    case STAGE_IDLE: return "空闲";
    case STAGE_BRIDGE: return "桥接状态请求";
    case STAGE_NET: return "网速请求";
    case STAGE_MUSIC: return "音乐请求";
    case STAGE_STOCK: return "四行行情请求";
    case STAGE_WEATHER: return "天气请求";
    case STAGE_MARKET_META: return "K线元数据请求";
    case STAGE_MARKET_FRAME: return "K线帧请求";
    case STAGE_WEB: return "设备网页";
    case STAGE_WIFI_RECOVERY: return "Wi-Fi恢复";
    case STAGE_OTA: return "OTA升级";
    default: return "未知";
  }
}

const char *networkTaskName(uint8_t task) {
  switch (task) {
    case NET_TASK_BRIDGE: return "桥接状态";
    case NET_TASK_NET: return "网速";
    case NET_TASK_MUSIC_META: return "音乐元数据";
    case NET_TASK_MUSIC_COVER: return "音乐封面";
    case NET_TASK_MUSIC_TEXT: return "音乐文字";
    case NET_TASK_STOCK_META: return "四行报价";
    case NET_TASK_STOCK_NAMES: return "报价名称";
    case NET_TASK_WEATHER_META: return "天气元数据";
    case NET_TASK_WEATHER_TEXT: return "天气文字";
    case NET_TASK_MARKET_META: return "K线元数据";
    case NET_TASK_MARKET_FRAME: return "K线画面";
    case NET_TASK_HEALTH: return "桥接健康检查";
    case NET_TASK_BOOT_REPORT: return "启动诊断上报";
    default: return "无";
  }
}

const char *requestPhaseName(uint8_t phase) {
  switch (phase) {
    case REQUEST_QUEUED: return "排队";
    case REQUEST_CONNECTING: return "连接";
    case REQUEST_SENDING: return "发送";
    case REQUEST_READING: return "读取";
    case REQUEST_PARSING: return "解析";
    case REQUEST_RENDERING: return "绘制";
    case REQUEST_CLEANUP: return "清理";
    case REQUEST_COMPLETE: return "完成";
    case REQUEST_FAILED: return "失败";
    default: return "无";
  }
}

const char *runtimeModeName(uint8_t mode) {
  switch (mode) {
    case MODE_AUTO: return "自动";
    case MODE_CLAUDE: return "Claude";
    case MODE_CODEX: return "Codex";
    case MODE_NET: return "网速";
    case MODE_MUSIC: return "音乐";
    case MODE_STOCK: return "四行报价";
    case MODE_MARKET: return "K线行情";
    case MODE_WEATHER: return "天气";
    default: return "未知";
  }
}

const char *wifiStatusName(uint8_t status) {
  switch (status) {
    case WL_CONNECTED: return "已连接";
    case WL_NO_SSID_AVAIL: return "找不到Wi-Fi";
    case WL_CONNECT_FAILED: return "连接失败";
    case WL_CONNECTION_LOST: return "连接丢失";
    case WL_DISCONNECTED: return "已断开";
    case WL_IDLE_STATUS: return "连接中";
    default: return "未知";
  }
}

const char *recoveryStepName(uint8_t step) {
  switch (step) {
    case RECOVERY_AUTH_REJOIN: return "握手异常重新关联";
    case RECOVERY_QUICK_RECONNECT: return "快速重连";
    case RECOVERY_WIFI_REINIT: return "重新初始化无线网络";
    case RECOVERY_RADIO_RESET: return "射频关闭/唤醒";
    case RECOVERY_PROTECTIVE_RESTART: return "保护性重启";
    case RECOVERY_MANUAL: return "用户手动重连";
    default: return "无";
  }
}

const char *disconnectOriginName(bool recoveryGenerated) {
  return recoveryGenerated ? "固件恢复动作" : "外部/AP/协议栈";
}

String disconnectEventText(uint8_t reason, bool recoveryGenerated) {
  if (reason == 0) return "无记录";
  return String((unsigned)reason) + "（" + disconnectOriginName(recoveryGenerated) + "）";
}

String wifiBssidText(const uint8_t *bssid) {
  char value[18];
  snprintf(value, sizeof(value), "%02X:%02X:%02X:%02X:%02X:%02X",
           bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
  return String(value);
}

String recoveryTimelineText(uint32_t quick, uint32_t reinit,
                            uint32_t radio, uint32_t restart, uint32_t auth) {
  String value = "握手:";
  value += auth ? String(auth / 1000UL) + "s" : "--";
  value += " 快速:";
  value += quick ? String(quick / 1000UL) + "s" : "--";
  value += " 重置:";
  value += reinit ? String(reinit / 1000UL) + "s" : "--";
  value += " 射频:";
  value += radio ? String(radio / 1000UL) + "s" : "--";
  value += " 重启:";
  value += restart ? String(restart / 1000UL) + "s" : "--";
  return value;
}

void writeRtcRuntimeDiag() {
  rtcRuntimeDiag.magic = RTC_DIAG_MAGIC;
  rtcRuntimeDiag.checksum = rtcDiagChecksum(rtcRuntimeDiag);
  ESP.rtcUserMemoryWrite(RTC_DIAG_WORD_OFFSET, reinterpret_cast<uint32_t *>(&rtcRuntimeDiag),
                         sizeof(rtcRuntimeDiag));
}

void saveRuntimeStage(RuntimeStage stage) {
  if (rtcRuntimeDiag.stage == stage && rtcRuntimeDiag.mode == diagnosticEffectiveMode &&
      rtcRuntimeDiag.wifiStatus == (uint8_t)WiFi.status()) return;
  rtcRuntimeDiag.stage = stage;
  rtcRuntimeDiag.mode = diagnosticEffectiveMode;
  rtcRuntimeDiag.wifiStatus = (uint8_t)WiFi.status();
  rtcRuntimeDiag.millisAtStage = millis();
  writeRtcRuntimeDiag();
}

uint32_t networkRequestCount = 0;
uint32_t networkRequestFailures = 0;
uint32_t lastNetworkTaskFinishedMs = 0;
uint32_t nextNetworkTaskAtMs = 0;
NetworkTask activeNetworkTask = NET_TASK_NONE;
RequestPhase activeRequestPhase = REQUEST_NONE;
int32_t activeRequestCode = 0;
uint32_t activeRequestBytes = 0;

RuntimeStage runtimeStageForNetworkTask(NetworkTask task) {
  switch (task) {
    case NET_TASK_BRIDGE:
    case NET_TASK_HEALTH:
    case NET_TASK_BOOT_REPORT: return STAGE_BRIDGE;
    case NET_TASK_NET: return STAGE_NET;
    case NET_TASK_MUSIC_META:
    case NET_TASK_MUSIC_COVER:
    case NET_TASK_MUSIC_TEXT: return STAGE_MUSIC;
    case NET_TASK_STOCK_META:
    case NET_TASK_STOCK_NAMES: return STAGE_STOCK;
    case NET_TASK_WEATHER_META:
    case NET_TASK_WEATHER_TEXT: return STAGE_WEATHER;
    case NET_TASK_MARKET_META: return STAGE_MARKET_META;
    case NET_TASK_MARKET_FRAME: return STAGE_MARKET_FRAME;
    default: return STAGE_IDLE;
  }
}

void saveRequestBreadcrumb(RequestPhase phase, bool persist) {
  activeRequestPhase = phase;
  rtcRuntimeDiag.networkTask = (uint8_t)activeNetworkTask;
  rtcRuntimeDiag.requestPhase = (uint8_t)phase;
  rtcRuntimeDiag.requestCode = activeRequestCode;
  rtcRuntimeDiag.requestBytes = activeRequestBytes;
  rtcRuntimeDiag.requestFreeHeap = ESP.getFreeHeap();
  rtcRuntimeDiag.requestMaxBlock = ESP.getMaxFreeBlockSize();
  rtcRuntimeDiag.requestDurationMs = rtcRuntimeDiag.requestStartedAtMs == 0 ? 0 :
      millis() - rtcRuntimeDiag.requestStartedAtMs;
  if (persist) writeRtcRuntimeDiag();
}

void beginNetworkRequest(NetworkTask task) {
  activeNetworkTask = task;
  activeRequestCode = 0;
  activeRequestBytes = 0;
  networkRequestCount++;
  rtcRuntimeDiag.stage = runtimeStageForNetworkTask(task);
  rtcRuntimeDiag.mode = diagnosticEffectiveMode;
  rtcRuntimeDiag.wifiStatus = (uint8_t)WiFi.status();
  rtcRuntimeDiag.millisAtStage = millis();
  rtcRuntimeDiag.requestStartedAtMs = millis();
  rtcRuntimeDiag.requestSucceeded = 0;
  // Persist once at task start. Phase transitions stay in RAM so frequent RTC
  // writes cannot contend with the ESP8266 Wi-Fi driver's critical sections.
  saveRequestBreadcrumb(REQUEST_QUEUED, true);
  delay(1); // let the SDK resume after RTC access before opening a TCP connection
  Serial.printf("[network] #%lu %s queued heap=%u block=%u\n",
                (unsigned long)networkRequestCount, networkTaskName(task),
                ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
}

void markRequestPhase(RequestPhase phase) {
  if (activeNetworkTask != NET_TASK_NONE && activeRequestPhase != phase) {
    saveRequestBreadcrumb(phase, false);
  }
}

void noteRequestResult(int code, int size = -1) {
  activeRequestCode = code;
  if (size > 0) activeRequestBytes = (uint32_t)size;
  rtcRuntimeDiag.requestCode = code;
  rtcRuntimeDiag.requestBytes = activeRequestBytes;
}

void finishNetworkRequest(bool success) {
  const NetworkTask completedTask = activeNetworkTask;
  if (!success) networkRequestFailures++;
  rtcRuntimeDiag.requestSucceeded = success ? 1 : 0;
  // A second and final RTC write records the result. No intermediate network
  // phase writes are made while TCP is active.
  saveRequestBreadcrumb(success ? REQUEST_COMPLETE : REQUEST_FAILED, true);
  Serial.printf("[network] %s %s code=%ld bytes=%lu elapsed=%lums heap=%u\n",
                networkTaskName((uint8_t)activeNetworkTask), success ? "ok" : "failed",
                (long)activeRequestCode, (unsigned long)activeRequestBytes,
                (unsigned long)rtcRuntimeDiag.requestDurationMs, ESP.getFreeHeap());
  lastNetworkTaskFinishedMs = millis();
  const bool largeTransfer = completedTask == NET_TASK_MARKET_FRAME ||
      completedTask == NET_TASK_STOCK_NAMES || completedTask == NET_TASK_MUSIC_COVER ||
      completedTask == NET_TASK_MUSIC_TEXT || completedTask == NET_TASK_WEATHER_TEXT ||
      activeRequestBytes >= 4096;
  nextNetworkTaskAtMs = lastNetworkTaskFinishedMs +
      (largeTransfer ? NETWORK_LARGE_TASK_GAP_MS : NETWORK_TASK_GAP_MS);
  rtcRuntimeDiag.stage = STAGE_IDLE; // RAM only; the completed task stays in RTC
  rtcRuntimeDiag.millisAtStage = lastNetworkTaskFinishedMs;
  activeNetworkTask = NET_TASK_NONE;
  activeRequestPhase = REQUEST_NONE;
  delay(0);
}

void closeTrackedHttp(HTTPClient &http, WiFiClient &client) {
  markRequestPhase(REQUEST_CLEANUP);
  http.end();
  client.stop();
  delay(0);
}

void loadBootDiagnostics() {
  strlcpy(lastResetReason, ESP.getResetReason().c_str(), sizeof(lastResetReason));
  strlcpy(lastResetInfo, ESP.getResetInfo().c_str(), sizeof(lastResetInfo));

  RtcRuntimeDiag previous = {};
  const bool previousValid = ESP.rtcUserMemoryRead(
      RTC_DIAG_WORD_OFFSET, reinterpret_cast<uint32_t *>(&previous),
                            sizeof(previous)) &&
      previous.magic == RTC_DIAG_MAGIC && previous.checksum == rtcDiagChecksum(previous);
  if (previousValid) {
    previousRuntimeStage = previous.stage;
    previousRuntimeMode = previous.mode;
    previousRecoveryStep = previous.recoveryStep;
    previousFirstWifiDisconnectReason = previous.firstWifiDisconnectReason;
    previousLastWifiDisconnectReason = previous.lastWifiDisconnectReason;
    previousFirstWifiRssi = previous.firstWifiRssi;
    previousFirstWifiChannel = previous.firstWifiChannel;
    previousFirstDisconnectWasRecovery = previous.firstDisconnectWasRecovery != 0;
    previousLastDisconnectWasRecovery = previous.lastDisconnectWasRecovery != 0;
    previousRecoveryMask = previous.recoveryMask;
    memcpy(previousFirstWifiBssid, previous.firstWifiBssid, sizeof(previousFirstWifiBssid));
    previousBridgeFailures = previous.bridgeFailures;
    previousWifiDisconnectCount = previous.wifiDisconnectCount;
    previousWifiReconnectCount = previous.wifiReconnectCount;
    previousBridgeHttpCode = previous.lastBridgeHttpCode;
    previousOutageDurationMs = previous.outageDurationMs;
    previousQuickRecoveryAtMs = previous.quickRecoveryAtMs;
    previousWifiReinitAtMs = previous.wifiReinitAtMs;
    previousRadioResetAtMs = previous.radioResetAtMs;
    previousProtectiveRestartAtMs = previous.protectiveRestartAtMs;
    previousAuthRejoinAtMs = previous.authRejoinAtMs;
    previousNetworkTask = previous.networkTask;
    previousRequestPhase = previous.requestPhase;
    previousRequestSucceeded = previous.requestSucceeded != 0;
    previousRequestCode = previous.requestCode;
    previousRequestDurationMs = previous.requestDurationMs;
    previousRequestBytes = previous.requestBytes;
    previousRequestFreeHeap = previous.requestFreeHeap;
    previousRequestMaxBlock = previous.requestMaxBlock;
    rtcRuntimeDiag.sequence = previous.sequence + 1;
  } else {
    rtcRuntimeDiag.sequence = 1;
  }

  if (LittleFS.exists(BOOT_COUNT_FILE)) {
    File f = LittleFS.open(BOOT_COUNT_FILE, "r");
    if (f) { bootCount = (uint32_t)f.readString().toInt(); f.close(); }
  }
  bootCount++;
  File f = LittleFS.open(BOOT_COUNT_FILE, "w");
  if (f) { f.print(bootCount); f.close(); }
  loadRestartHistory();
  if (previousValid) appendRestartHistory(previous);
  saveRuntimeStage(STAGE_BOOT);
}

// AUTO is a user-configurable carousel. Bit N selects DisplayMode N; AUTO
// itself is never a carousel page. Default preserves the original two-pet
// experience until the user changes it from the Mac menu.
uint16_t weekdayAutoPageMask = (1U << MODE_CODEX);
uint16_t weekdayAutoCycleSeconds = AUTO_CYCLE_DEFAULT_SECONDS;
uint16_t weekendAutoPageMask = (1U << MODE_CODEX);
uint16_t weekendAutoCycleSeconds = AUTO_CYCLE_DEFAULT_SECONDS;
uint8_t autoPageIndex = 0;
unsigned long autoPageStartedMs = 0;
bool lastAutoScheduleWeekend = false;

// When AUTO and the Mac reports audio playing, the screen auto-switches to the
// music page and back when it stops — same spirit as the Claude/Codex auto
// switch. Only AUTO does this; a pinned mode is always honored as-is.
bool statusMusicPlaying = false;
DisplayMode lastEffectiveMode = MODE_AUTO;

// ---------- net speed mode state ----------
// Rendering is decoupled from the network: pollNet() fetches every 2s and
// only refills a queue of 250ms samples (the bridge samples at 4Hz and tags
// them with a running seq, so nothing is drawn twice or skipped). The sweep
// itself consumes exactly one queued sample every NET_DRAW_INTERVAL_MS, so
// the trace advances at a constant rate no matter how long HTTP takes.
const unsigned long NET_POLL_INTERVAL_MS = 2000; // queue refill cadence
const unsigned long NET_DRAW_INTERVAL_MS = 250;  // one chart step per bridge sample
const int NET_QUEUE = 32;
long netQRx[NET_QUEUE], netQTx[NET_QUEUE]; // ring buffer of pending samples
int netQHead = 0, netQCount = 0;
long netSeq = -1;                          // last bridge sample seq consumed into the queue
long netCurRx = 0, netCurTx = 0;           // smoothed readout for the header
int netCpuPct = -1, netMemPct = -1;        // Mac CPU/MEM row; -1 = bridge sends none (hidden)
String netLastCpuVal, netLastMemVal;       // change detection for the CPU/MEM values
bool netSysLabelsDrawn = false;
unsigned long lastNetPollMs = 0;
unsigned long lastNetDrawMs = 0;
bool netChromeDrawn = false;
bool netHeaderDirty = false;

// Chart layout (task-manager style scrolling area chart, newest at the right)
const int NET_CHART_X = 8, NET_CHART_Y = 60, NET_CHART_W = 224, NET_CHART_H = 128;
long netHistRx[NET_CHART_W], netHistTx[NET_CHART_W]; // one 250ms sample per column
long netScale = 10240;    // current "nice" full-scale value (whole chart shares it)
String netLastDl, netLastUl, netLastScaleText; // change detection for partial redraws

// ---------- music mode state ----------
const int MUSIC_COVER_W = 128;
const int MUSIC_COVER_H = 128;
// Title/artist come as a Mac-rendered bitmap strip (232x44) because the
// panel fonts are ASCII-only and CJK titles would render as blanks.
const int MUSIC_TEXT_W = 232;
const int MUSIC_TEXT_H = 44;
const int MUSIC_TEXT_X = 4, MUSIC_TEXT_Y = 150;
const unsigned long MUSIC_POLL_INTERVAL_MS = 2000;
// ---------- stock watchlist mode state ----------
// Rows come pre-formatted from the bridge (GET /stock or serial #STOCK):
// ASCII code + price/pct strings + up flag, so the firmware just paints.
const unsigned long STOCK_POLL_INTERVAL_MS = 5000;
const int MAX_STOCKS = 4;
struct StockRow {
  char code[16] = "";
  char price[16] = "";
  char pct[12] = "";
  int up = 0; // 1 rising (red, CN convention) / -1 falling (green) / 0 flat
};
StockRow stocks[MAX_STOCKS];
int stockCount = 0;
bool stockEverLoaded = false;
bool stockDirty = false;
bool stockChromeDrawn = false;
uint32_t stockLastCodeHash[MAX_STOCKS] = {}; // top line (code + CJK name strip)
uint32_t stockLastValHash[MAX_STOCKS] = {};  // value line (price + pct + direction)
unsigned long lastStockPollMs = 0;
bool stockNamesPending = false;
bool stockNamesDrawPending = false;
bool stockNamesCacheSupported = false;
bool stockNamesCacheValid = false;
uint8_t stockNamesCacheCount = 0;
uint8_t stockNamesFailures = 0;
unsigned long stockNamesRetryAtMs = 0;
unsigned long lastStockNamesAttemptMs = 0;
// CJK names come as Mac-rendered RGB565 strips (GET /stock/names.raw, one
// 156x16 strip per row). v0.5.17 caches the compressed stream in LittleFS;
// names_rev says when the content actually changed. Zero = not available.
const int STOCK_NAME_W = 156, STOCK_NAME_H = 16;
uint32_t stockNamesRev = 0;
uint32_t stockNamesDrawnRev = 0;
uint32_t stockNamesCachedRev = 0;
bool stockNamesPreferRle = true;

// ---------- full-screen K-line market mode ----------
// This is a separate page from the four-row stock page above. Unlike the
// ESP32-C3 implementation, the ESP8266 does not reserve a permanent 32 KB
// frame buffer. It allocates the compressed frame only during an update,
// validates the entire envelope and CRC, draws one row at a time through the
// existing 480-byte rowBuf, then immediately releases the allocation.
const unsigned long MARKET_POLL_INTERVAL_MS = 5000;
const unsigned long MARKET_HTTP_TIMEOUT_MS = 2500;
const unsigned long MARKET_READ_TIMEOUT_MS = 1200;
const unsigned long MARKET_TOTAL_TIMEOUT_MS = 5000;
const size_t MARKET_PACKED_HEADER_BYTES = 20;
const size_t MARKET_PACKED_MAX_BYTES = 30 * 1024;
const uint16_t MARKET_PALETTE_RGB565[16] PROGMEM = {
  0x0000, 0xFFFF, 0xC618, 0x7BEF, 0x39E7, 0x18C3, 0xF800, 0x07E0,
  0xFBE7, 0x37E7, 0xFFE0, 0xFD20, 0x07FF, 0x001F, 0xF81F, 0x8410
};
unsigned long lastMarketPollMs = 0;
uint64_t lastMarketFrameVersion = 0;
char lastMarketFrameSession[40] = "";
uint64_t pendingMarketFrameVersion = 0;
size_t pendingMarketFrameBytes = 0;
bool pendingMarketFramePalette4 = false;
unsigned long lastMarketFrameAttemptMs = 0;
bool marketAutoDwellKnown = false;
unsigned long marketAutoDwellMs = 0;

// ---------- date/weather mode ----------
struct WeatherState {
  float currentTemp = 0;
  float todayHigh = 0, todayLow = 0;
  float tomorrowHigh = 0, tomorrowLow = 0;
  int currentCode = 0, todayCode = 0, tomorrowCode = 0;
  long utcOffsetSeconds = 8 * 3600;
  long scheduleUtcOffsetSeconds = 8 * 3600;
  time_t serverUnix = 0;
  unsigned long syncedAtMs = 0;
  bool valid = false;
  bool stale = true;
};
WeatherState weather;
unsigned long lastWeatherPollMs = 0;
unsigned long lastWeatherClockMs = 0;
bool weatherChromeDrawn = false;
bool weatherDirty = false;
bool weatherTextPending = false;
bool weatherTextDrawPending = false;
bool weatherTextCacheValid = false;
bool weatherTextLegacyRaw = false;
uint32_t weatherTextRev = 0;
uint32_t weatherTextCachedRev = 0;
uint8_t weatherTextFailures = 0;
unsigned long weatherTextRetryAtMs = 0;
unsigned long lastWeatherTextAttemptMs = 0;

char musicTitle[72] = "";
char musicArtist[48] = "";
bool musicPlaying = false;
int musicElapsed = 0, musicDuration = 0;
int musicArtworkRev = -1;
int musicTextRev = -1;
bool musicHasArtwork = false;
bool musicChromeDrawn = false;
unsigned long lastMusicPollMs = 0;
bool musicCoverPending = false;
bool musicTextPending = false;
unsigned long lastMusicCoverAttemptMs = 0;
unsigned long lastMusicTextAttemptMs = 0;

int claudeFrame = 0;
int codexFrame = 0;
unsigned long lastAnimMs = 0;

bool flashOn = true;
unsigned long lastFlashMs = 0;

// Bridge host is not asked for during first-time WiFi setup: the Mac/Windows
// bridge discovers the device and pairs automatically (or set via /api/bridge).
String bridgeHost;
char bridgeVersion[24] = "--";

struct ClaudeStatus {
  char status[16] = "unknown";
  long tokensToday = 0;
  int sessionMin = 0;
  int sessionWindowMin = 300;
  float fiveHourPct = -1; // real OAuth quota from the bridge, -1 = unknown
  int fiveHourResetMin = -1; // minutes until the 5h window resets
  float sevenDayPct = -1;
  int sevenDayResetMin = -1; // minutes until the 7-day window resets
  bool needsInput = false; // waiting on a permission/approval prompt
};

struct CodexStatus {
  char status[16] = "unknown";
  long tokensToday = 0;
  float primaryPct = -1;
  int primaryResetMin = -1;
  float weeklyPct = -1;
  int weeklyResetMin = -1;
  int64_t weeklyResetAt = -1; // absolute Unix reset instant from the bridge
  int weeklyResetUtcOffsetSec = 0; // local offset at that instant (handles DST)
  bool needsInput = false;
};

ClaudeStatus claudeStatus;
CodexStatus codexStatus;

unsigned long lastPollMs = 0;
unsigned long lastSuccessMs = 0;
unsigned long lastBridgeHealthPollMs = 0;
int lastBridgeHealthHttpCode = 0;
bool lastBridgeHealthHealthy = false;
bool bootReportPending = true;
unsigned long lastBootReportAttemptMs = 0;
bool everPolled = false;
bool mainUiShown = false;      // false while the config-portal screen is up
bool webServerStarted = false; // deferred: port 80 clashes with the portal

// Heap health is sampled from the normal loop, outside the diagnostic web
// request. This keeps the admin page's own temporary allocations out of the
// displayed "current" value and makes long-term fragmentation visible.
uint32_t idleFreeHeap = 0;
uint32_t minimumIdleFreeHeap = 0;
uint32_t idleMaxFreeBlock = 0;
uint8_t idleHeapFragmentation = 0;
unsigned long lastHeapSampleMs = 0;

void sampleHeapHealth(bool force = false) {
  const unsigned long now = millis();
  if (!force && now - lastHeapSampleMs < 1000UL) return;
  lastHeapSampleMs = now;
  idleFreeHeap = ESP.getFreeHeap();
  idleMaxFreeBlock = ESP.getMaxFreeBlockSize();
  idleHeapFragmentation = ESP.getHeapFragmentation();
  if (minimumIdleFreeHeap == 0 || idleFreeHeap < minimumIdleFreeHeap) {
    minimumIdleFreeHeap = idleFreeHeap;
  }
}

// ---------- Wi-Fi / bridge recovery diagnostics ----------
WiFiEventHandler wifiDisconnectedEventHandler;
WiFiEventHandler wifiGotIpEventHandler;
uint32_t wifiDisconnectCount = 0;
uint32_t wifiReconnectCount = 0;
uint8_t firstWifiDisconnectReason = 0;
uint8_t lastWifiDisconnectReason = 0;
int8_t firstWifiDisconnectRssi = 0;
uint8_t firstWifiDisconnectChannel = 0;
uint8_t firstWifiDisconnectBssid[6] = {};
bool firstDisconnectWasRecovery = false;
bool lastDisconnectWasRecovery = false;
bool wifiOutageActive = false;
uint16_t wifiRecoveryMask = 0;
uint32_t lastWifiOutageDurationMs = 0;
uint32_t quickRecoveryAtMs = 0;
uint32_t wifiReinitAtMs = 0;
uint32_t radioResetAtMs = 0;
uint32_t protectiveRestartAtMs = 0;
uint32_t authRejoinAtMs = 0;
int8_t lastConnectedWifiRssi = 0;
uint8_t lastConnectedWifiChannel = 0;
uint8_t lastConnectedWifiBssid[6] = {};
RecoveryStep pendingDisconnectRecoveryStep = RECOVERY_NONE;
unsigned long pendingDisconnectRecoveryAtMs = 0;
unsigned long wifiDisconnectedSinceMs = 0;
bool wifiEverGotIp = false;
bool wifiSoftRecoveryDone = false;
bool wifiHardRecoveryDone = false;
bool wifiRadioRecoveryDone = false;
bool wifiAuthRecoveryPending = false;
bool wifiAuthRecoveryDone = false;
unsigned long wifiAuthRecoveryAtMs = 0;
bool webServerNeedsRestart = false;
const unsigned long WIFI_TRAFFIC_COOLDOWN_MS = NETWORK_STARTUP_GRACE_MS;
bool networkTrafficPaused = true;
unsigned long networkTrafficResumeAtMs = 0;

uint32_t bridgeConsecutiveFailures = 0;
uint32_t bridgeTotalFailures = 0;
int lastBridgeHttpCode = 0;
unsigned long bridgeFailureSinceMs = 0;
unsigned long lastBridgeSuccessAtMs = 0;
char lastRecoveryAction[56] = "尚未执行";
unsigned long manualReconnectAtMs = 0;

void pauseNetworkTrafficUntilGotIp() {
  networkTrafficPaused = true;
  networkTrafficResumeAtMs = 0;
}

void startNetworkTrafficCooldown() {
  networkTrafficPaused = true;
  networkTrafficResumeAtMs = millis() + WIFI_TRAFFIC_COOLDOWN_MS;
}

bool networkTrafficReady() {
  if (WiFi.status() != WL_CONNECTED || networkTrafficResumeAtMs == 0) return false;
  if (networkTrafficPaused &&
      (long)(millis() - networkTrafficResumeAtMs) >= 0) {
    networkTrafficPaused = false;
    Serial.println("[wifi] network traffic cooldown complete");
  }
  return !networkTrafficPaused;
}

void armRecoveryDisconnect(RecoveryStep step) {
  pauseNetworkTrafficUntilGotIp();
  pendingDisconnectRecoveryStep = step;
  pendingDisconnectRecoveryAtMs = millis();
}

void sampleConnectedWifiEvidence() {
  if (WiFi.status() != WL_CONNECTED) return;
  lastConnectedWifiRssi = (int8_t)constrain(WiFi.RSSI(), -127, 0);
  lastConnectedWifiChannel = (uint8_t)WiFi.channel();
  const uint8_t *bssid = WiFi.BSSID();
  if (bssid) memcpy(lastConnectedWifiBssid, bssid, sizeof(lastConnectedWifiBssid));
}

uint32_t currentWifiOutageDurationMs() {
  return wifiDisconnectedSinceMs == 0 ? lastWifiOutageDurationMs
                                      : millis() - wifiDisconnectedSinceMs;
}

void syncWifiEvidenceToRtc() {
  rtcRuntimeDiag.firstWifiDisconnectReason = firstWifiDisconnectReason;
  rtcRuntimeDiag.lastWifiDisconnectReason = lastWifiDisconnectReason;
  rtcRuntimeDiag.firstWifiRssi = firstWifiDisconnectRssi;
  rtcRuntimeDiag.firstWifiChannel = firstWifiDisconnectChannel;
  rtcRuntimeDiag.firstDisconnectWasRecovery = firstDisconnectWasRecovery ? 1 : 0;
  rtcRuntimeDiag.lastDisconnectWasRecovery = lastDisconnectWasRecovery ? 1 : 0;
  rtcRuntimeDiag.recoveryMask = wifiRecoveryMask;
  memcpy(rtcRuntimeDiag.firstWifiBssid, firstWifiDisconnectBssid,
         sizeof(rtcRuntimeDiag.firstWifiBssid));
  rtcRuntimeDiag.outageDurationMs = currentWifiOutageDurationMs();
  rtcRuntimeDiag.quickRecoveryAtMs = quickRecoveryAtMs;
  rtcRuntimeDiag.wifiReinitAtMs = wifiReinitAtMs;
  rtcRuntimeDiag.radioResetAtMs = radioResetAtMs;
  rtcRuntimeDiag.protectiveRestartAtMs = protectiveRestartAtMs;
  rtcRuntimeDiag.authRejoinAtMs = authRejoinAtMs;
}

void persistRecoverySnapshot(RecoveryStep step) {
  rtcRuntimeDiag.stage = STAGE_WIFI_RECOVERY;
  rtcRuntimeDiag.mode = diagnosticEffectiveMode;
  rtcRuntimeDiag.wifiStatus = (uint8_t)WiFi.status();
  rtcRuntimeDiag.recoveryStep = (uint8_t)step;
  const uint32_t elapsed = wifiDisconnectedSinceMs == 0 ? 0
                                                        : millis() - wifiDisconnectedSinceMs;
  wifiRecoveryMask |= (uint16_t)(1U << (uint8_t)step);
  if (step == RECOVERY_QUICK_RECONNECT) quickRecoveryAtMs = elapsed;
  else if (step == RECOVERY_WIFI_REINIT || step == RECOVERY_MANUAL) wifiReinitAtMs = elapsed;
  else if (step == RECOVERY_RADIO_RESET) radioResetAtMs = elapsed;
  else if (step == RECOVERY_PROTECTIVE_RESTART) protectiveRestartAtMs = elapsed;
  else if (step == RECOVERY_AUTH_REJOIN) authRejoinAtMs = elapsed;
  syncWifiEvidenceToRtc();
  rtcRuntimeDiag.bridgeFailures = bridgeConsecutiveFailures;
  rtcRuntimeDiag.wifiDisconnectCount = wifiDisconnectCount;
  rtcRuntimeDiag.wifiReconnectCount = wifiReconnectCount;
  rtcRuntimeDiag.lastBridgeHttpCode = lastBridgeHttpCode;
  rtcRuntimeDiag.millisAtStage = millis();
  writeRtcRuntimeDiag();
}

void setRecoveryAction(const char *action, RecoveryStep step) {
  strlcpy(lastRecoveryAction, action, sizeof(lastRecoveryAction));
  Serial.printf("[recovery] %s\n", lastRecoveryAction);
  persistRecoverySnapshot(step);
}

void recordBridgeSuccess() {
  lastBridgeSuccessAtMs = millis();
  bridgeConsecutiveFailures = 0;
  bridgeFailureSinceMs = 0;
  rtcRuntimeDiag.recoveryStep = RECOVERY_NONE;
  rtcRuntimeDiag.bridgeFailures = 0;
  rtcRuntimeDiag.lastBridgeHttpCode = lastBridgeHttpCode;
}

void recordBridgeFailure(int code) {
  lastBridgeHttpCode = code;
  bridgeConsecutiveFailures++;
  bridgeTotalFailures++;
  rtcRuntimeDiag.bridgeFailures = bridgeConsecutiveFailures;
  rtcRuntimeDiag.lastBridgeHttpCode = lastBridgeHttpCode;
  if (bridgeFailureSinceMs == 0) bridgeFailureSinceMs = max(millis(), 1UL);
}

void hardReconnectWiFi(const char *reason, RecoveryStep step = RECOVERY_WIFI_REINIT) {
  setRecoveryAction(reason, step);
  armRecoveryDisconnect(step);
  WiFi.disconnect(false);
  delay(0);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.begin();
  webServerNeedsRestart = webServerStarted;
}

void resetWiFiRadio(const char *reason) {
  setRecoveryAction(reason, RECOVERY_RADIO_RESET);
  armRecoveryDisconnect(RECOVERY_RADIO_RESET);
  WiFi.forceSleepBegin();
  delay(30);
  WiFi.forceSleepWake();
  delay(30);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.begin();
  webServerNeedsRestart = webServerStarted;
}

void maintainConnectivity(unsigned long nowMs) {
  if (!wifiEverGotIp && bridgeHost.length() == 0) return; // first-time config portal owns Wi-Fi
  sampleConnectedWifiEvidence();
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiDisconnectedSinceMs == 0) wifiDisconnectedSinceMs = max(nowMs, 1UL);
    const unsigned long downFor = nowMs - wifiDisconnectedSinceMs;
    if (wifiAuthRecoveryPending && !wifiAuthRecoveryDone &&
        (long)(nowMs - wifiAuthRecoveryAtMs) >= 0) {
      wifiAuthRecoveryDone = true;
      hardReconnectWiFi("WPA握手异常：重新关联接入点", RECOVERY_AUTH_REJOIN);
    }
    if (downFor >= 15000UL && !wifiSoftRecoveryDone) {
      wifiSoftRecoveryDone = true;
      setRecoveryAction("Wi-Fi断线15秒：尝试快速重连", RECOVERY_QUICK_RECONNECT);
      armRecoveryDisconnect(RECOVERY_QUICK_RECONNECT);
      WiFi.reconnect();
    }
    if (downFor >= 60000UL && !wifiHardRecoveryDone) {
      wifiHardRecoveryDone = true;
      hardReconnectWiFi("Wi-Fi断线60秒：重新初始化无线网络");
    }
    if (downFor >= 180000UL && !wifiRadioRecoveryDone) {
      wifiRadioRecoveryDone = true;
      resetWiFiRadio("Wi-Fi断线3分钟：关闭并唤醒无线射频");
    }
    if (downFor >= 300000UL) {
      setRecoveryAction("Wi-Fi断线5分钟：保护性重启", RECOVERY_PROTECTIVE_RESTART);
      delay(20);
      ESP.restart();
    }
    return;
  }

  wifiDisconnectedSinceMs = 0;
  wifiSoftRecoveryDone = false;
  wifiHardRecoveryDone = false;
  wifiRadioRecoveryDone = false;
  wifiAuthRecoveryPending = false;
  wifiAuthRecoveryDone = false;
  wifiAuthRecoveryAtMs = 0;

  // A bridge outage is not a Wi-Fi outage: the Mac may be asleep, the app may
  // be restarting, or a single HTTP request may have failed. Keep collecting
  // diagnostics and polling normally, but never reset the radio merely because
  // the bridge is unavailable. Wi-Fi recovery above is driven only by the
  // station's actual connection state.
}

// ---------- backlight brightness ----------
// The panel backlight (TFT_BL, active LOW) is PWM-dimmable — the vendor's own
// firmware does the same. 0 = off, 100 = full. Persisted so it survives reboot.

int brightness = BRIGHTNESS_DEFAULT; // 0-100

void applyBrightness() {
  // analogWriteRange(100) is set in setup(), so the duty value is just the
  // inverted percentage (active LOW: 0 duty = always LOW = full on).
  analogWrite(TFT_BL, 100 - brightness);
}

void loadBrightness() {
  if (!LittleFS.exists(BRIGHTNESS_FILE)) return;
  File f = LittleFS.open(BRIGHTNESS_FILE, "r");
  if (!f) return;
  int v = f.readStringUntil('\n').toInt();
  f.close();
  if (v >= 0 && v <= 100) brightness = v;
}

void saveBrightness() {
  File f = LittleFS.open(BRIGHTNESS_FILE, "w");
  if (!f) return;
  f.println(brightness);
  f.close();
}

bool validAutoCycleSeconds(int seconds) {
  return seconds == 5 || seconds == 10 || seconds == 15 || seconds == 20 ||
         seconds == 30 || seconds == 60 || seconds == 120;
}

const uint16_t VISIBLE_AUTO_MASK =
    (1U << MODE_CODEX) | (1U << MODE_MUSIC) | (1U << MODE_STOCK) |
    (1U << MODE_MARKET) | (1U << MODE_WEATHER);

time_t weatherLocalEpoch() {
  if (weather.serverUnix <= 0) return 0;
  return weather.serverUnix + (time_t)((millis() - weather.syncedAtMs) / 1000UL) +
         (time_t)weather.utcOffsetSeconds;
}

bool weatherIsWeekend() {
  if (weather.serverUnix <= 0) return false;
  time_t localEpoch = weather.serverUnix +
      (time_t)((millis() - weather.syncedAtMs) / 1000UL) +
      (time_t)weather.scheduleUtcOffsetSeconds;
  if (localEpoch <= 0) return false; // before first sync, use weekday schedule
  struct tm localTm;
  gmtime_r(&localEpoch, &localTm);
  return localTm.tm_wday == 0 || localTm.tm_wday == 6;
}

uint16_t activeAutoPageMask() {
  return weatherIsWeekend() ? weekendAutoPageMask : weekdayAutoPageMask;
}

uint16_t activeAutoCycleSeconds() {
  return weatherIsWeekend() ? weekendAutoCycleSeconds : weekdayAutoCycleSeconds;
}

uint8_t selectedAutoPageCount() {
  uint8_t count = 0;
  const uint16_t mask = activeAutoPageMask();
  for (uint8_t mode = MODE_CODEX; mode <= MODE_WEATHER; ++mode) {
    if (mask & (1U << mode)) ++count;
  }
  return count;
}

DisplayMode autoPageAt(uint8_t selectedIndex) {
  uint8_t found = 0;
  const uint16_t mask = activeAutoPageMask();
  for (uint8_t mode = MODE_CODEX; mode <= MODE_WEATHER; ++mode) {
    if (!(mask & (1U << mode))) continue;
    if (found++ == selectedIndex) return (DisplayMode)mode;
  }
  return MODE_CODEX;
}

void loadAutoCycle() {
  if (!LittleFS.exists(AUTO_CYCLE_FILE)) return;
  File f = LittleFS.open(AUTO_CYCLE_FILE, "r");
  if (!f) return;
  const int weekdayMask = f.readStringUntil('\n').toInt();
  const int weekdaySeconds = f.readStringUntil('\n').toInt();
  String weekendMaskLine = f.readStringUntil('\n');
  String weekendSecondsLine = f.readStringUntil('\n');
  f.close();
  uint16_t cleanWeekday = (uint16_t)weekdayMask & VISIBLE_AUTO_MASK;
  if (cleanWeekday == 0) cleanWeekday = 1U << MODE_CODEX;
  weekdayAutoPageMask = cleanWeekday;
  if (validAutoCycleSeconds(weekdaySeconds)) weekdayAutoCycleSeconds = weekdaySeconds;

  // v0.5.5 stored only two lines. Duplicate that schedule for the weekend
  // when upgrading, so existing users keep the same behavior until editing it.
  if (weekendMaskLine.length() == 0) {
    weekendAutoPageMask = weekdayAutoPageMask;
    weekendAutoCycleSeconds = weekdayAutoCycleSeconds;
  } else {
    uint16_t cleanWeekend = (uint16_t)weekendMaskLine.toInt() & VISIBLE_AUTO_MASK;
    if (cleanWeekend == 0) cleanWeekend = 1U << MODE_CODEX;
    weekendAutoPageMask = cleanWeekend;
    const int weekendSeconds = weekendSecondsLine.toInt();
    if (validAutoCycleSeconds(weekendSeconds)) weekendAutoCycleSeconds = weekendSeconds;
  }
}

void saveAutoCycle() {
  File f = LittleFS.open(AUTO_CYCLE_FILE, "w");
  if (!f) return;
  f.println(weekdayAutoPageMask);
  f.println(weekdayAutoCycleSeconds);
  f.println(weekendAutoPageMask);
  f.println(weekendAutoCycleSeconds);
  f.close();
}

void resetAutoCyclePosition() {
  autoPageIndex = 0;
  autoPageStartedMs = millis();
  lastAutoScheduleWeekend = weatherIsWeekend();
}

void advanceAutoCycleIfNeeded(unsigned long nowMs) {
  if (displayMode != MODE_AUTO) return;
  const bool weekend = weatherIsWeekend();
  if (weekend != lastAutoScheduleWeekend) {
    lastAutoScheduleWeekend = weekend;
    autoPageIndex = 0;
    autoPageStartedMs = nowMs;
    lastEffectiveMode = MODE_AUTO;
  }
  const uint8_t count = selectedAutoPageCount();
  if (count == 0) return;
  unsigned long dwellMs = (unsigned long)activeAutoCycleSeconds() * 1000UL;
  if (autoPageAt(autoPageIndex) == MODE_MARKET) {
    // Give the bridge a short grace period to report its favorite count and
    // K-line rotation cadence. Once known, keep this page for one complete
    // favorite round instead of cutting it off at the outer page interval.
    if (!marketAutoDwellKnown && nowMs - autoPageStartedMs < 3000UL) return;
    if (marketAutoDwellKnown && marketAutoDwellMs > dwellMs) dwellMs = marketAutoDwellMs;
  }
  if (nowMs - autoPageStartedMs >= dwellMs) {
    autoPageIndex = (autoPageIndex + 1) % count;
    autoPageStartedMs = nowMs;
  }
}

// ---------- persistence for the bridge host ----------

void loadBridgeHost() {
  if (LittleFS.exists(WIFI_CONFIG_FILE)) {
    File f = LittleFS.open(WIFI_CONFIG_FILE, "r");
    bridgeHost = f.readStringUntil('\n');
    bridgeHost.trim();
    f.close();
  }
}

void saveBridgeHost(const String &host) {
  File f = LittleFS.open(WIFI_CONFIG_FILE, "w");
  f.println(host);
  f.close();
}

// ---------- custom sprite loading ----------

// Checks LittleFS for a previously-uploaded custom sprite and validates its
// size before trusting it (frame count byte + exact expected byte length).
void loadCustomSpriteState() {
  claudeCustom = false;
  if (LittleFS.exists(CLAUDE_SPRITE_FILE)) {
    File f = LittleFS.open(CLAUDE_SPRITE_FILE, "r");
    if (f && f.size() >= 1) {
      uint8_t cnt = f.read();
      size_t expected = 1 + (size_t)cnt * CLAUDE_FRAME_BYTES;
      if (cnt > 0 && cnt <= MAX_CUSTOM_FRAMES && (size_t)f.size() == expected) {
        claudeCustom = true;
        claudeCustomFrames = cnt;
      }
    }
    if (f) f.close();
  }

  codexCustom = false;
  if (LittleFS.exists(CODEX_SPRITE_FILE)) {
    File f = LittleFS.open(CODEX_SPRITE_FILE, "r");
    if (f && f.size() >= 1) {
      uint8_t cnt = f.read();
      size_t expected = 1 + (size_t)cnt * CODEX_FRAME_BYTES;
      if (cnt > 0 && cnt <= MAX_CUSTOM_FRAMES && (size_t)f.size() == expected) {
        codexCustom = true;
        codexCustomFrames = cnt;
      }
    }
    if (f) f.close();
  }

  Serial.printf("[sprite] claude custom=%d frames=%d | codex custom=%d frames=%d\n", claudeCustom,
                claudeCustomFrames, codexCustom, codexCustomFrames);
}

int claudeFrameCount() { return claudeCustom ? claudeCustomFrames : CLAUDE_SPRITE_FRAMES; }
int codexFrameCount() { return codexCustom ? codexCustomFrames : CODEX_SPRITE_FRAMES; }

// Draws one sprite frame centered on screen, one row at a time so we never
// need a full-frame buffer: each row comes either from the custom LittleFS
// file (streamed) or the compiled-in PROGMEM default (copied row-by-row).
void drawSpriteFrame(bool custom, const char *file, const uint16_t *const *progmemFrames, int frameIdx, int w,
                     int h, size_t frameBytes) {
  int x0 = SCREEN_CX - w / 2, y0 = SCREEN_CY - h / 2;
  size_t rowBytes = (size_t)w * 2;
  if (custom) {
    File f = LittleFS.open(file, "r");
    if (!f) return;
    f.seek(1 + (size_t)frameIdx * frameBytes);
    for (int r = 0; r < h; r++) {
      f.read((uint8_t *)rowBuf, rowBytes);
      tft.pushImage(x0, y0 + r, w, 1, rowBuf);
    }
    f.close();
  } else {
    const uint16_t *frame = progmemFrames[frameIdx];
    for (int r = 0; r < h; r++) {
      memcpy_P(rowBuf, frame + (size_t)r * w, rowBytes);
      tft.pushImage(x0, y0 + r, w, 1, rowBuf);
    }
  }
}

// ---------- helpers ----------

String formatTokens(long tokens) {
  if (tokens >= 1000000) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1fM", tokens / 1000000.0);
    return String(buf);
  }
  if (tokens >= 1000) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1fk", tokens / 1000.0);
    return String(buf);
  }
  return String(tokens);
}

// ---------- drawing ----------

void drawStaticChrome() {
  tft.fillScreen(TFT_BLACK);
}

// Bridge unreachable / data stale -> flashing red overrides everything else,
// matches the "urgent, look now" state from the reference signal-light design.
bool bridgeStale() {
  if (!everPolled) return true;
  return (millis() - lastSuccessMs) >=
      2UL * BRIDGE_DATA_PAGE_POLL_INTERVAL_MS + NETWORK_LARGE_TASK_GAP_MS;
}

// True when the app currently on screen is waiting on a permission/approval
// prompt — drives the red "look now, act" border flash.
bool currentAppNeedsInput() {
  return currentApp == APP_CLAUDE ? claudeStatus.needsInput : codexStatus.needsInput;
}

// Working vs idle is now conveyed by the sprite animation itself (moving vs
// still), not by ring color. The ring just stays steady green, except
// bridge-stale which flashes red ("check it now") and overrides everything.
uint16_t currentStatusColor() {
  if (bridgeStale()) return flashOn ? TFT_RED : TFT_BLACK;
  return TFT_GREEN;
}

// The ring is skipped when nothing changed (see drawSquareRing) so the 5s
// poll doesn't visibly blank-and-repaint it. Anything that paints over the
// ring area must invalidate this cache.
float ringLastPct = -1000;
uint16_t ringLastColor = 1;

// Paints the full square border in one color (all four sides), used for the
// attention flash so the whole edge blinks, not just the filled quota arc.
void drawFullBorder(uint16_t color) {
  ringLastPct = -1000; // ring got painted over; next ring draw must repaint
  int x0 = RING_MARGIN, y0 = RING_MARGIN;
  int side = SCREEN_W - 2 * RING_MARGIN;
  tft.fillRect(x0, y0, side, RING_THICKNESS, color);                              // top
  tft.fillRect(x0, SCREEN_H - RING_MARGIN - RING_THICKNESS, side, RING_THICKNESS, color); // bottom
  tft.fillRect(x0, y0, RING_THICKNESS, side, color);                              // left
  tft.fillRect(SCREEN_W - RING_MARGIN - RING_THICKNESS, y0, RING_THICKNESS, side, color); // right
}

// Square progress ring hugging the screen edge. `pct` of the perimeter
// (clockwise from top-left) is drawn in `color`, the rest in dark grey.
void drawSquareRing(float pct, uint16_t color) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  if (pct == ringLastPct && color == ringLastColor) return; // nothing changed
  ringLastPct = pct;
  ringLastColor = color;

  int x0 = RING_MARGIN, y0 = RING_MARGIN;
  int x1 = SCREEN_W - RING_MARGIN, y1 = SCREEN_H - RING_MARGIN;
  int side = x1 - x0;
  float perimeter = side * 4.0;

  // Unfilled track is drawn black (not grey) so it blends into the background
  // and only the active quota portion is visible - still needs to be actively
  // repainted each time though, to erase a previously longer fill if the
  // percentage drops (e.g. a quota window reset).
  tft.fillRect(x0, y0, side, RING_THICKNESS, TFT_BLACK);                  // top
  tft.fillRect(x1 - RING_THICKNESS, y0, RING_THICKNESS, side, TFT_BLACK); // right
  tft.fillRect(x0, y1 - RING_THICKNESS, side, RING_THICKNESS, TFT_BLACK); // bottom
  tft.fillRect(x0, y0, RING_THICKNESS, side, TFT_BLACK);                  // left

  // filled portion, clockwise: top -> right -> bottom -> left
  float remaining = perimeter * (pct / 100.0);
  if (remaining <= 0) return;

  float seg = min(remaining, (float)side);
  tft.fillRect(x0, y0, (int)seg, RING_THICKNESS, color);
  remaining -= side;
  if (remaining <= 0) return;

  seg = min(remaining, (float)side);
  tft.fillRect(x1 - RING_THICKNESS, y0, RING_THICKNESS, (int)seg, color);
  remaining -= side;
  if (remaining <= 0) return;

  seg = min(remaining, (float)side);
  tft.fillRect(x1 - (int)seg, y1 - RING_THICKNESS, (int)seg, RING_THICKNESS, color);
  remaining -= side;
  if (remaining <= 0) return;

  seg = min(remaining, (float)side);
  tft.fillRect(x0, y1 - (int)seg, RING_THICKNESS, (int)seg, color);
}

void drawClaudeSprite(int frameIdx) {
  drawSpriteFrame(claudeCustom, CLAUDE_SPRITE_FILE, claude_sprite_frames, frameIdx, CLAUDE_SPRITE_W,
                  CLAUDE_SPRITE_H, CLAUDE_FRAME_BYTES);
}

void drawCodexSprite(int frameIdx) {
  drawSpriteFrame(codexCustom, CODEX_SPRITE_FILE, codex_sprite_frames, frameIdx, CODEX_SPRITE_W, CODEX_SPRITE_H,
                  CODEX_FRAME_BYTES);
}

String pctText(float pct) {
  return pct >= 0 ? String((int)pct) + "%" : "-";
}

// Quota readout below the sprite: two columns ("5h" / "Wk"), small grey label
// over a big font-4 percentage. Values repaint only when their text changes
// (force = after a full-screen clear), so the 5s poll never flashes them.
const int QUOTA_LABEL_Y = 183, QUOTA_VALUE_Y = 199;
const int QUOTA_COL1_X = 70, QUOTA_COL2_X = 170;
String lastQuota5h, lastQuotaWk;

// pushImage() colors must be pre-byte-swapped (this firmware never enables
// setSwapBytes; see the sprite pipeline). Natural RGB565 -> wire order:
inline uint16_t swap565(uint16_t c) { return (uint16_t)((c << 8) | (c >> 8)); }

// ---- Nothing-phone-style dot-matrix font (NDot look) ----
// Every piece of ASCII text on the Claude/Codex quota pages renders as round
// dots on a fixed grid with visible gaps.
struct DotGlyph {
  char c;
  uint8_t w;
  uint8_t rows[7];
};

const DotGlyph dotGlyphs[] = {
    {'0', 5, {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}},
    {'1', 5, {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'2', 5, {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}},
    {'3', 5, {0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110}},
    {'4', 5, {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}},
    {'5', 5, {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}},
    {'6', 5, {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}},
    {'7', 5, {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}},
    {'8', 5, {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}},
    {'9', 5, {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}},
    {'A', 5, {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'B', 5, {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110}},
    {'C', 5, {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110}},
    {'D', 5, {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110}},
    {'E', 5, {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}},
    {'F', 5, {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'G', 5, {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111}},
    {'H', 5, {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'I', 3, {0b111, 0b010, 0b010, 0b010, 0b010, 0b010, 0b111}},
    {'J', 5, {0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100}},
    {'K', 5, {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}},
    {'L', 5, {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}},
    {'M', 5, {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001}},
    {'N', 5, {0b10001, 0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001}},
    {'O', 5, {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'P', 5, {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'Q', 5, {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101}},
    {'R', 5, {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001}},
    {'S', 5, {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'T', 5, {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'U', 5, {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'V', 5, {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100}},
    {'W', 5, {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010}},
    {'X', 5, {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001}},
    {'Y', 5, {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'Z', 5, {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111}},
    {'d', 5, {0b00001, 0b00001, 0b01101, 0b10011, 0b10001, 0b10011, 0b01101}},
    {'h', 5, {0b10000, 0b10000, 0b10110, 0b11001, 0b10001, 0b10001, 0b10001}},
    {'k', 5, {0b10000, 0b10000, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010}},
    {'s', 5, {0b00000, 0b00000, 0b01111, 0b10000, 0b01110, 0b00001, 0b11110}},
    {'%', 5, {0b11001, 0b11010, 0b00010, 0b00100, 0b01000, 0b01011, 0b10011}},
    {':', 1, {0b0, 0b0, 0b1, 0b0, 0b1, 0b0, 0b0}},
    {'.', 1, {0b0, 0b0, 0b0, 0b0, 0b0, 0b0, 0b1}},
    {',', 1, {0b0, 0b0, 0b0, 0b0, 0b0, 0b1, 0b1}},
    {'\'', 1, {0b1, 0b1, 0b0, 0b0, 0b0, 0b0, 0b0}},
    {'!', 1, {0b1, 0b1, 0b1, 0b1, 0b1, 0b0, 0b1}},
    {'?', 5, {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b00000, 0b00100}},
    {'-', 3, {0b000, 0b000, 0b000, 0b111, 0b000, 0b000, 0b000}},
    {'+', 3, {0b000, 0b010, 0b010, 0b111, 0b010, 0b010, 0b000}},
    {'/', 3, {0b001, 0b001, 0b010, 0b010, 0b010, 0b100, 0b100}},
    {' ', 2, {0b0, 0b0, 0b0, 0b0, 0b0, 0b0, 0b0}},
};

const DotGlyph *dotGlyph(char c) {
  for (const DotGlyph &g : dotGlyphs)
    if (g.c == c) return &g;
  if (c >= 'a' && c <= 'z') return dotGlyph(c - 32);
  return nullptr;
}

int dotCharAdv(char c, int pitch, int r) {
  const DotGlyph *g = dotGlyph(c);
  int w = g ? g->w : 3;
  return (w - 1) * pitch + 2 * r + 1 + pitch;
}

int dotTextWidth(const String &s, int pitch, int r) {
  if (s.length() == 0) return 0;
  int w = 0;
  for (unsigned int i = 0; i < s.length(); i++) w += dotCharAdv(s[i], pitch, r);
  return w - pitch;
}

int dotTextHeight(int pitch, int r) { return 6 * pitch + 2 * r + 1; }

void drawDotChar(char c, int x, int y, int pitch, int r, uint16_t color) {
  const DotGlyph *g = dotGlyph(c);
  if (!g) return;
  for (int ry = 0; ry < 7; ry++)
    for (int cx = 0; cx < g->w; cx++)
      if (g->rows[ry] & (1 << (g->w - 1 - cx))) {
        if (r <= 0) tft.drawPixel(x + r + cx * pitch, y + r + ry * pitch, color);
        else tft.fillCircle(x + r + cx * pitch, y + r + ry * pitch, r, color);
      }
}

void drawDotText(const String &s, int x, int y, int pitch, int r, uint16_t color) {
  for (unsigned int i = 0; i < s.length(); i++) {
    drawDotChar(s[i], x, y, pitch, r, color);
    x += dotCharAdv(s[i], pitch, r);
  }
}

void drawDotTextC(const String &s, int cx, int y, int pitch, int r, uint16_t color) {
  drawDotText(s, cx - dotTextWidth(s, pitch, r) / 2, y, pitch, r, color);
}

int sqCharAdv(char c, int pitch, int d) {
  const DotGlyph *g = dotGlyph(c);
  int w = g ? g->w : 3;
  return (w - 1) * pitch + d + pitch;
}

int sqTextWidth(const String &s, int pitch, int d) {
  if (s.length() == 0) return 0;
  int w = 0;
  for (unsigned int i = 0; i < s.length(); i++) w += sqCharAdv(s[i], pitch, d);
  return w - pitch;
}

void drawSqText(const String &s, int x, int y, int pitch, int d, uint16_t color) {
  for (unsigned int i = 0; i < s.length(); i++) {
    const DotGlyph *g = dotGlyph(s[i]);
    if (g)
      for (int ry = 0; ry < 7; ry++)
        for (int cx = 0; cx < g->w; cx++)
          if (g->rows[ry] & (1 << (g->w - 1 - cx)))
            tft.fillRect(x + cx * pitch, y + ry * pitch, d, d, color);
    x += sqCharAdv(s[i], pitch, d);
  }
}

void drawSqTextC(const String &s, int cx, int y, int pitch, int d, uint16_t color) {
  drawSqText(s, cx - sqTextWidth(s, pitch, d) / 2, y, pitch, d, color);
}

struct TinyGlyph {
  char c;
  uint8_t rows[5];
};

const TinyGlyph tinyGlyphs[] = {
    {'R', {0b110, 0b101, 0b110, 0b101, 0b101}},
    {'E', {0b111, 0b100, 0b110, 0b100, 0b111}},
    {'S', {0b011, 0b100, 0b010, 0b001, 0b110}},
    {'T', {0b111, 0b010, 0b010, 0b010, 0b010}},
};

void drawTinyBoldText(const String &s, int cx, int y, uint16_t color) {
  const int P = 2, D = 2;
  int x = cx - ((int)s.length() * (2 * P + D) + ((int)s.length() - 1) * 2) / 2;
  for (unsigned int i = 0; i < s.length(); i++) {
    for (const TinyGlyph &g : tinyGlyphs)
      if (g.c == s[i]) {
        for (int ry = 0; ry < 5; ry++)
          for (int gx = 0; gx < 3; gx++)
            if (g.rows[ry] & (0b100 >> gx)) tft.fillRect(x + gx * P, y + ry * P, D, D, color);
        break;
      }
    x += 2 * P + D + 2;
  }
}

void drawQuotaText(float hourPct, float weekPct, bool force) {
  // Codex dropped the 5h window (2026-07): the bridge then sends
  // primary_pct=null, so collapse to a single centered "Wk" column.
  bool single = hourPct < 0 && weekPct >= 0;
  static int8_t lastSingle = -1;
  if ((int8_t)single != lastSingle) {
    lastSingle = (int8_t)single;
    force = true;
    tft.fillRect(0, QUOTA_LABEL_Y, 240, QUOTA_VALUE_Y + 22 - QUOTA_LABEL_Y, TFT_BLACK);
  }
  if (single) {
    if (force) drawSqTextC("Wk", 120, QUOTA_LABEL_Y, 2, 2, TFT_LIGHTGREY);
    String v = pctText(weekPct);
    if (force || v != lastQuotaWk) {
      lastQuotaWk = v;
      lastQuota5h = "";
      tft.fillRect(120 - 50, QUOTA_VALUE_Y, 100, 22, TFT_BLACK);
      drawDotTextC(v, 120, QUOTA_VALUE_Y, 3, 1, TFT_WHITE);
    }
    return;
  }
  if (force) {
    drawSqTextC("5h", QUOTA_COL1_X, QUOTA_LABEL_Y, 2, 2, TFT_LIGHTGREY);
    drawSqTextC("Wk", QUOTA_COL2_X, QUOTA_LABEL_Y, 2, 2, TFT_LIGHTGREY);
  }
  String v1 = pctText(hourPct), v2 = pctText(weekPct);
  if (force || v1 != lastQuota5h) {
    lastQuota5h = v1;
    tft.fillRect(QUOTA_COL1_X - 50, QUOTA_VALUE_Y, 100, 22, TFT_BLACK);
    drawDotTextC(v1, QUOTA_COL1_X, QUOTA_VALUE_Y, 3, 1, TFT_WHITE);
  }
  if (force || v2 != lastQuotaWk) {
    lastQuotaWk = v2;
    tft.fillRect(QUOTA_COL2_X - 50, QUOTA_VALUE_Y, 100, 22, TFT_BLACK);
    drawDotTextC(v2, QUOTA_COL2_X, QUOTA_VALUE_Y, 3, 1, TFT_WHITE);
  }
}

// ---------- quota-exhausted countdown ----------
// When the current app's 5h or weekly window is used up, the pet is replaced
// by a countdown to that window's reset (bridge sends minutes-until-reset).
// A spent weekly window blocks usage even after the 5h one resets, so the
// weekly countdown takes priority when both are exhausted.

enum CdType { CD_NONE, CD_5H, CD_WEEK };

float currentHourPct() {
  return currentApp == APP_CLAUDE ? claudeStatus.fiveHourPct : codexStatus.primaryPct;
}

int currentHourResetMin() {
  return currentApp == APP_CLAUDE ? claudeStatus.fiveHourResetMin : codexStatus.primaryResetMin;
}

float currentWeekPct() {
  return currentApp == APP_CLAUDE ? claudeStatus.sevenDayPct : codexStatus.weeklyPct;
}

int currentWeekResetMin() {
  return currentApp == APP_CLAUDE ? claudeStatus.sevenDayResetMin : codexStatus.weeklyResetMin;
}

CdType desiredCountdown() {
  if (currentWeekPct() >= 99.9f && currentWeekResetMin() >= 0) return CD_WEEK;
  if (currentHourPct() >= 99.9f && currentHourResetMin() >= 0) return CD_5H;
  return CD_NONE;
}

CdType showingCd = CD_NONE; // what's on screen now (vs desiredCountdown())
String lastCountdown;

// The bridge only reports whole minutes, so the seconds tick locally against
// a deadline anchored at millis(). Re-anchor only when the bridge disagrees
// by more than ~a minute (new window, big clock drift), otherwise a poll
// landing mid-minute would make the seconds jump around.
unsigned long cdDeadlineMs = 0; // 0 = not anchored
ActiveApp cdApp = APP_CLAUDE;   // which app/window the anchor belongs to
CdType cdAnchorType = CD_NONE;

void syncCountdownDeadline() {
  int m = showingCd == CD_WEEK ? currentWeekResetMin() : currentHourResetMin();
  if (m < 0) {
    cdDeadlineMs = 0;
    return;
  }
  long bridgeSec = (long)m * 60 + 30; // bridge floors to minutes: assume mid-minute
  long ourSec = (long)(cdDeadlineMs - millis()) / 1000;
  if (cdDeadlineMs == 0 || cdApp != currentApp || cdAnchorType != showingCd || ourSec < 0 ||
      labs(ourSec - bridgeSec) > 90) {
    cdDeadlineMs = millis() + (unsigned long)bridgeSec * 1000UL;
    cdApp = currentApp;
    cdAnchorType = showingCd;
  }
}

void drawCountdown(bool force) {
  long remain = cdDeadlineMs ? (long)(cdDeadlineMs - millis()) / 1000
                             : (long)(showingCd == CD_WEEK ? currentWeekResetMin() : currentHourResetMin()) * 60;
  if (remain < 0) remain = 0;
  char buf[16];
  long hours = remain / 3600;
  if (hours >= 100) // weekly can be up to 168h: h:mm:ss wouldn't fit the ring
    snprintf(buf, sizeof(buf), "%ld:%02ld", hours, (remain % 3600) / 60);
  else
    snprintf(buf, sizeof(buf), "%ld:%02ld:%02ld", hours, (remain % 3600) / 60, remain % 60);
  String t(buf);
  if (!force && t == lastCountdown) return;
  // A length change shifts every glyph cell, so clear the whole region.
  if (t.length() != lastCountdown.length()) force = true;
  const int P = 6, R = 2, VAL_Y = 100;
  int x = SCREEN_CX - dotTextWidth(t, P, R) / 2;
  if (force) {
    tft.fillRect(SCREEN_CX - 99, 66, 198, 84, TFT_BLACK);
    drawDotTextC(showingCd == CD_WEEK ? "Wk RESET IN" : "5h RESET IN", SCREEN_CX, 72, 2, 0,
                 TFT_LIGHTGREY);
    drawDotText(t, x, VAL_Y, P, R, TFT_ORANGE);
  } else {
    // Same length keeps identical cell positions, so repaint only digits that
    // changed and avoid a once-per-second full-row flash.
    for (unsigned int i = 0; i < t.length(); i++) {
      int adv = dotCharAdv(t[i], P, R);
      if (t[i] != lastCountdown[i]) {
        tft.fillRect(x, VAL_Y, adv - P, dotTextHeight(P, R), TFT_BLACK);
        drawDotChar(t[i], x, VAL_Y, P, R, TFT_ORANGE);
      }
      x += adv;
    }
  }
  lastCountdown = t;
}

// App logo in the top-left corner (inside the quota ring) so a glance tells
// which app the screen is currently showing. Drawn row-by-row from PROGMEM
// through rowBuf, same as the sprite path.
const int LOGO_X = 14, LOGO_Y = 18;

void drawAppLogo() {
  const uint16_t *logo = (currentApp == APP_CLAUDE) ? claude_logo_0 : codex_logo_0;
  int w = (currentApp == APP_CLAUDE) ? CLAUDE_LOGO_W : CODEX_LOGO_W;
  int h = (currentApp == APP_CLAUDE) ? CLAUDE_LOGO_H : CODEX_LOGO_H;
  for (int r = 0; r < h; r++) {
    memcpy_P(rowBuf, logo + (size_t)r * w, (size_t)w * 2);
    tft.pushImage(LOGO_X, LOGO_Y + r, w, 1, rowBuf);
  }
}

// Days until the weekly quota resets, shown opposite the app logo. Below one
// day the readout changes to hours.
const int RESET_CX = 198, RESET_LABEL_Y = 18, RESET_VALUE_Y = 33;
String lastResetDays;
String lastExactResetTime;

String resetDaysText(int min) {
  if (min < 0) return "";
  if (min < 1440) return String((min + 59) / 60) + "h";
  return String((min + 1439) / 1440) + "d";
}

void drawResetDays(bool force) {
  String t = resetDaysText(currentWeekResetMin());
  if (!force && t == lastResetDays) return;
  lastResetDays = t;
  tft.fillRect(RESET_CX - 32, RESET_LABEL_Y, 60, RESET_VALUE_Y + 27 - RESET_LABEL_Y, TFT_BLACK);
  if (t.length() == 0) return;
  drawTinyBoldText("RESET", RESET_CX, RESET_LABEL_Y, TFT_LIGHTGREY);
  int pitch = t.length() <= 2 ? 4 : 3;
  drawDotTextC(t, RESET_CX, RESET_VALUE_Y, pitch, 1, TFT_WHITE);
}

// Exact Codex weekly reset time in the bridge Mac's local timezone. The
// bridge sends both the absolute Unix instant and the UTC offset that applies
// at that future instant, so this remains correct across daylight-saving
// transitions and does not depend on the weather page having synchronised.
void drawExactResetTime(bool force) {
  if (currentApp != APP_CODEX) return;
  String dateText, timeText;
  if (codexStatus.weeklyResetAt > 0) {
    time_t localReset = (time_t)(codexStatus.weeklyResetAt +
                                 codexStatus.weeklyResetUtcOffsetSec);
    struct tm resetTm;
    gmtime_r(&localReset, &resetTm);
    char dateBuf[32], timeBuf[32];
    snprintf(dateBuf, sizeof(dateBuf), "%02d/%02d", resetTm.tm_mon + 1, resetTm.tm_mday);
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", resetTm.tm_hour, resetTm.tm_min);
    dateText = dateBuf;
    timeText = timeBuf;
  }
  const String key = dateText + "|" + timeText;
  if (!force && key == lastExactResetTime) return;
  lastExactResetTime = key;
  tft.fillRect(75, 18, 81, 31, TFT_BLACK);
  if (dateText.length() == 0) return;
  // The old 1-pixel square glyphs made 08/13 hard to distinguish on the
  // physical 1.54-inch panel. Font 2 is wider, solid and anti-gap at this size.
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(dateText, 116, 15, 2);
  drawDotTextC(timeText, 116, 33, 2, 1, TFT_WHITE);
}

// Codex's ring percentage: the 5h window when it exists, otherwise the
// weekly one (Codex removed the 5h limit in 2026-07).
float codexRingPct() {
  if (codexStatus.primaryPct >= 0) return codexStatus.primaryPct;
  return max(codexStatus.weeklyPct, 0.0f);
}

// Claude's ring percentage: real 5h OAuth quota from the bridge when known,
// otherwise fall back to elapsed session time as a rough stand-in.
float claudeRingPct() {
  if (claudeStatus.fiveHourPct >= 0) return claudeStatus.fiveHourPct;
  return claudeStatus.sessionWindowMin > 0
             ? (100.0 * claudeStatus.sessionMin / claudeStatus.sessionWindowMin)
             : 0;
}

// Redraws whichever app is currently active, full screen: quota ring +
// sprite (or the reset countdown while the 5h window is exhausted).
// Full clear + repaint - only for real transitions (app switch, mode return,
// sprite change); steady-state data updates go through refreshActiveApp().
void drawActiveApp() {
  tft.fillScreen(TFT_BLACK);
  ringLastPct = -1000; // screen was cleared: force the ring repaint
  showingCd = desiredCountdown();
  if (showingCd != CD_NONE) syncCountdownDeadline();
  else cdDeadlineMs = 0;
  if (currentApp == APP_CLAUDE) {
    drawSquareRing(claudeRingPct(), currentStatusColor());
    if (showingCd == CD_NONE) drawClaudeSprite(claudeFrame);
    drawQuotaText(claudeRingPct(), claudeStatus.sevenDayPct, true);
  } else {
    drawSquareRing(codexRingPct(), currentStatusColor());
    if (showingCd == CD_NONE) drawCodexSprite(codexFrame);
    drawQuotaText(codexStatus.primaryPct, codexStatus.weeklyPct, true);
  }
  if (showingCd != CD_NONE) drawCountdown(true);
  drawAppLogo();
  drawExactResetTime(true);
  drawResetDays(true);
}

// In-place refresh after a bridge poll: ring repaint + only the text that
// actually changed. No fillScreen, so the 5s poll doesn't blank the screen.
void refreshActiveApp() {
  if (desiredCountdown() != showingCd) { // pet <-> countdown (or 5h <-> weekly) swap
    drawActiveApp();
    return;
  }
  if (currentApp == APP_CLAUDE) {
    drawSquareRing(claudeRingPct(), currentStatusColor());
    drawQuotaText(claudeRingPct(), claudeStatus.sevenDayPct, false);
  } else {
    drawSquareRing(codexRingPct(), currentStatusColor());
    drawQuotaText(codexStatus.primaryPct, codexStatus.weeklyPct, false);
  }
  drawExactResetTime(false);
  drawResetDays(false);
  if (showingCd != CD_NONE) {
    syncCountdownDeadline();
    drawCountdown(false);
  }
}

// Redraws just the ring (cheap) - used for status color animation ticks
// between full redraws.
void redrawRingOnly() {
  if (currentApp == APP_CLAUDE) {
    drawSquareRing(claudeRingPct(), currentStatusColor());
  } else {
    drawSquareRing(codexRingPct(), currentStatusColor());
  }
}

// Who gets the screen:
//   - display mode pinned (Mac app) -> that app, always
//   - exactly one app working       -> that app, immediately
//   - both working                  -> alternate every SWITCH_BOTH_MS (2s)
//   - neither working               -> alternate slowly (SWITCH_IDLE_MS)
bool updateActiveApp() {
  ActiveApp desired = currentApp;

  if (displayMode == MODE_CLAUDE) {
    desired = APP_CLAUDE;
  } else if (displayMode == MODE_CODEX) {
    desired = APP_CODEX;
  } else if (claudeStatus.needsInput && !codexStatus.needsInput) {
    desired = APP_CLAUDE; // approval prompt wins the screen
  } else if (codexStatus.needsInput && !claudeStatus.needsInput) {
    desired = APP_CODEX;
  } else {
    bool claudeWorking = strcmp(claudeStatus.status, "working") == 0;
    bool codexWorking = strcmp(codexStatus.status, "working") == 0;
    if (claudeWorking && !codexWorking) {
      desired = APP_CLAUDE;
    } else if (codexWorking && !claudeWorking) {
      desired = APP_CODEX;
    } else {
      unsigned long interval = (claudeWorking && codexWorking) ? SWITCH_BOTH_MS : SWITCH_IDLE_MS;
      if (millis() - lastSwitchMs >= interval) {
        lastSwitchMs = millis();
        desired = (currentApp == APP_CLAUDE) ? APP_CODEX : APP_CLAUDE;
      }
    }
  }

  if (desired != currentApp) {
    currentApp = desired;
    lastSwitchMs = millis();
    return true;
  }
  return false;
}

// ---------- net speed screen ----------

String speedText(long bps) {
  char buf[16];
  if (bps >= 1000000) snprintf(buf, sizeof(buf), "%.1fM", bps / 1000000.0);
  else if (bps >= 1000) snprintf(buf, sizeof(buf), "%.0fK", bps / 1000.0);
  else snprintf(buf, sizeof(buf), "%ldB", bps);
  return String(buf);
}

void resetNetChart() {
  memset(netHistRx, 0, sizeof(netHistRx));
  memset(netHistTx, 0, sizeof(netHistTx));
  netScale = 10240;
  netLastDl = "";
  netLastUl = "";
  netLastScaleText = "";
  netLastCpuVal = "";
  netLastMemVal = "";
  netSysLabelsDrawn = false;
  netQHead = 0;
  netQCount = 0;
  netSeq = -1;
}

// Adaptive full scale: the window's peak always lands at ~87% of the chart
// height, so the undulation stays visible no matter the absolute speed.
// (The old 1/2/5 stepped scale could squash everything to under half height.)
long adaptiveNetScale(long maxV) {
  long s = maxV + maxV / 7; // ~1.15x headroom above the peak
  return s > 10240 ? s : 10240;
}

// Static chrome: labels that never change while in net mode.
void drawNetChrome() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(0x7BEF, TFT_BLACK);
  tft.drawString("DOWN", 14, 10, 1);
  tft.drawString("UP", 134, 10, 1);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("MAC NET  -  56s", SCREEN_CX, 226, 1); // below the CPU/MEM row
}

// Mac CPU / memory usage row between the chart and the footer: small grey
// labels at fixed positions, big font-4 values left-aligned at fixed x so a
// width change (5% -> 30%) never shifts the rest of the row around.
// Hidden only if an old bridge doesn't send the fields yet.
const int NET_SYS_Y = 192;                          // row top (26px tall, font 4)
const int NET_CPU_LABEL_X = 28, NET_CPU_VAL_X = 62; // value region 62..126 ("100%" = 63px)
const int NET_MEM_LABEL_X = 130, NET_MEM_VAL_X = 164;

void drawNetSysinfoIfChanged() {
  if (netCpuPct < 0) {
    if (netSysLabelsDrawn) { // bridge stopped sending: erase the whole row
      tft.fillRect(0, NET_SYS_Y, SCREEN_W, 26, TFT_BLACK);
      netSysLabelsDrawn = false;
      netLastCpuVal = "";
      netLastMemVal = "";
    }
    return;
  }
  tft.setTextDatum(TL_DATUM);
  if (!netSysLabelsDrawn) {
    netSysLabelsDrawn = true;
    tft.setTextColor(0x7BEF, TFT_BLACK);
    tft.drawString("CPU", NET_CPU_LABEL_X, NET_SYS_Y + 6, 2);
    tft.drawString("MEM", NET_MEM_LABEL_X, NET_SYS_Y + 6, 2);
  }
  String c = String(netCpuPct) + "%", m = String(netMemPct) + "%";
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  if (c != netLastCpuVal) {
    netLastCpuVal = c;
    tft.fillRect(NET_CPU_VAL_X, NET_SYS_Y, 64, 26, TFT_BLACK);
    tft.drawString(c, NET_CPU_VAL_X, NET_SYS_Y, 4);
  }
  if (m != netLastMemVal) {
    netLastMemVal = m;
    tft.fillRect(NET_MEM_VAL_X, NET_SYS_Y, 64, 26, TFT_BLACK);
    tft.drawString(m, NET_MEM_VAL_X, NET_SYS_Y, 4);
  }
}

// Header readouts (1s-averaged), each repainted only when its text changes.
void drawNetHeaderIfChanged() {
  String dl = speedText(netCurRx) + "/s";
  String ul = speedText(netCurTx) + "/s";
  tft.setTextDatum(TL_DATUM);
  if (dl != netLastDl) {
    netLastDl = dl;
    tft.fillRect(12, 20, 116, 28, TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString(dl, 12, 20, 4);
  }
  if (ul != netLastUl) {
    netLastUl = ul;
    tft.fillRect(132, 20, 108, 28, TFT_BLACK);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString(ul, 132, 20, 4);
  }
}

// Repaints the whole chart region from the sample ring, one row at a time
// through rowBuf (a single pushImage per row = no clear-then-draw flicker).
// Download is a dim-green filled area with a bright top edge; upload is a
// 2px yellow line on top; faint gridlines at 25/50/75%.
void drawNetChart() {
  static const uint16_t COL_GRID = swap565(0x2104);   // very dark grey
  static const uint16_t COL_FILL = swap565(0x02A0);   // dim green
  static const uint16_t COL_EDGE = swap565(TFT_GREEN);
  static const uint16_t COL_UL = swap565(TFT_YELLOW);
  static const uint16_t COL_BLACK = swap565(TFT_BLACK);

  long maxV = 0;
  for (int i = 0; i < NET_CHART_W; i++) {
    if (netHistRx[i] > maxV) maxV = netHistRx[i];
    if (netHistTx[i] > maxV) maxV = netHistTx[i];
  }
  netScale = adaptiveNetScale(maxV);

  // Per-column heights (3-tap smoothed), then per-column line "bands": each
  // band spans from the previous column's height to this one's, so steep
  // rises/falls render as connected vertical strokes instead of detached
  // stair-step dots — that's what makes the undulation read as a continuous
  // line, like the Mac mirror's stroked polyline.
  static uint8_t hRx[NET_CHART_W], hTx[NET_CHART_W];
  static uint8_t dlLo[NET_CHART_W], dlHi[NET_CHART_W]; // DL edge band, incl. 3px weight
  static uint8_t ulLo[NET_CHART_W], ulHi[NET_CHART_W]; // UL line band
  // The panel is physically tiny (2.7cm across), so the stroke must be much
  // thicker than the Mac mirror's to read at the same visual weight.
  const int LINE_T = 10; // stroke thickness in px
  for (int i = 0; i < NET_CHART_W; i++) {
    int lo = i > 0 ? i - 1 : 0, hi = i < NET_CHART_W - 1 ? i + 1 : NET_CHART_W - 1;
    long rx = (netHistRx[lo] + netHistRx[i] + netHistRx[hi]) / 3;
    long tx = (netHistTx[lo] + netHistTx[i] + netHistTx[hi]) / 3;
    int hr = (int)((float)rx / netScale * (NET_CHART_H - 2));
    int ht = (int)((float)tx / netScale * (NET_CHART_H - 2));
    hRx[i] = (uint8_t)constrain(hr, 0, NET_CHART_H - 1);
    hTx[i] = (uint8_t)constrain(ht, 0, NET_CHART_H - 1);
  }
  for (int i = 0; i < NET_CHART_W; i++) {
    int prevR = i > 0 ? hRx[i - 1] : hRx[0];
    int prevT = i > 0 ? hTx[i - 1] : hTx[0];
    dlHi[i] = (uint8_t)max((int)hRx[i], prevR);
    dlLo[i] = (uint8_t)max(0, min((int)hRx[i], prevR) - (LINE_T - 1));
    ulHi[i] = (uint8_t)max((int)hTx[i], prevT);
    ulLo[i] = (uint8_t)max(0, min((int)hTx[i], prevT) - (LINE_T - 1));
  }

  for (int row = 0; row < NET_CHART_H; row++) {
    int yFromBot = NET_CHART_H - 1 - row;
    bool gridRow = (row == NET_CHART_H / 4 || row == NET_CHART_H / 2 || row == 3 * NET_CHART_H / 4);
    for (int i = 0; i < NET_CHART_W; i++) {
      uint16_t c = gridRow ? COL_GRID : COL_BLACK;
      if (yFromBot <= dlHi[i] && yFromBot >= dlLo[i]) c = COL_EDGE;
      else if (yFromBot < dlLo[i]) c = COL_FILL;
      if (ulHi[i] > 0 && yFromBot <= ulHi[i] && yFromBot >= ulLo[i]) c = COL_UL;
      rowBuf[i] = c;
    }
    tft.pushImage(NET_CHART_X, NET_CHART_Y + row, NET_CHART_W, 1, rowBuf);
    if ((row & 31) == 31) yield();
  }

  // axis label (outside the chart, so it never gets repainted over)
  String scaleText = speedText(netScale);
  if (scaleText != netLastScaleText) {
    netLastScaleText = scaleText;
    tft.fillRect(120, 48, 112, 10, TFT_BLACK);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(0x7BEF, TFT_BLACK);
    tft.drawString(scaleText, NET_CHART_X + NET_CHART_W, 48, 1);
    tft.setTextDatum(TL_DATUM);
  }
}

// Chart tick, every NET_DRAW_INTERVAL_MS: shift in queued sample(s), then
// one atomic repaint. If the queue backs up after a slow poll, it works off
// up to three samples per tick until it's back in step.
void netDrawTick() {
  if (!netChromeDrawn) {
    resetNetChart();
    drawNetChrome();
    netChromeDrawn = true;
    netHeaderDirty = true;
  }
  if (netHeaderDirty) {
    drawNetHeaderIfChanged();
    drawNetSysinfoIfChanged();
    netHeaderDirty = false;
  }
  if (netQCount == 0) return;
  int steps = min(netQCount, netQCount > 16 ? 3 : 1);
  while (steps-- > 0 && netQCount > 0) {
    memmove(netHistRx, netHistRx + 1, sizeof(long) * (NET_CHART_W - 1));
    memmove(netHistTx, netHistTx + 1, sizeof(long) * (NET_CHART_W - 1));
    netHistRx[NET_CHART_W - 1] = netQRx[netQHead];
    netHistTx[NET_CHART_W - 1] = netQTx[netQHead];
    netQHead = (netQHead + 1) % NET_QUEUE;
    netQCount--;
  }
  drawNetChart();
}

// Ingests one /net payload (from HTTP polling or a serial #NET frame) into
// the sample queue. The seq field tells us which samples we've already
// queued, so overlapping tails are fine.
bool applyNetJson(JsonDocument &doc) {
  netCurRx = doc["rx_bps"] | 0L;
  netCurTx = doc["tx_bps"] | 0L;
  netCpuPct = doc["cpu_pct"] | -1;
  netMemPct = doc["mem_pct"] | -1;
  netHeaderDirty = true;
  long seq = doc["seq"] | -1L;
  JsonArray rx = doc["rx"], tx = doc["tx"];
  int n = min(rx.size(), tx.size());
  // how many of the tail samples are new to us
  int fresh = (netSeq < 0) ? min(n, 8) : (int)min((long)n, seq - netSeq);
  if (fresh < 0) fresh = 0;
  for (int i = n - fresh; i < n; i++) {
    if (netQCount >= NET_QUEUE) break; // queue full: drop the excess
    int tail = (netQHead + netQCount) % NET_QUEUE;
    netQRx[tail] = rx[i].as<long>();
    netQTx[tail] = tx[i].as<long>();
    netQCount++;
  }
  if (seq >= 0) netSeq = seq;
  return true;
}

bool handleNetPayload(const String &payload) {
  JsonDocument doc;
  return !deserializeJson(doc, payload) && applyNetJson(doc);
}

// Refills the sample queue from the bridge's /net endpoint.
bool pollNet() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0) return false;
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + "/net";
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  markRequestPhase(REQUEST_CONNECTING);
  if (!http.begin(client, url)) { noteRequestResult(-1001); client.stop(); return false; }
  markRequestPhase(REQUEST_SENDING);
  int code = http.GET();
  noteRequestResult(code, http.getSize());
  bool ok = false;
  if (code == HTTP_CODE_OK) {
    markRequestPhase(REQUEST_PARSING);
    JsonDocument doc;
    if (!deserializeJson(doc, *http.getStreamPtr())) ok = applyNetJson(doc);
  }
  closeTrackedHttp(http, client);
  return ok;
}

String timeText(int sec) {
  if (sec < 0) sec = 0;
  char buf[12];
  snprintf(buf, sizeof(buf), "%d:%02d", sec / 60, sec % 60);
  return String(buf);
}

String fitText(String s, int maxPx, int font) {
  if (tft.textWidth(s, font) <= maxPx) return s;
  while (s.length() > 0 && tft.textWidth(s + "...", font) > maxPx) {
    s.remove(s.length() - 1);
  }
  return s + "...";
}

void drawMusicCoverPlaceholder() {
  const int x = (SCREEN_W - MUSIC_COVER_W) / 2;
  const int y = 14;
  tft.fillRect(x, y, MUSIC_COVER_W, MUSIC_COVER_H, TFT_DARKGREY);
  tft.drawRect(x, y, MUSIC_COVER_W, MUSIC_COVER_H, TFT_DARKGREY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_DARKGREY);
  tft.drawString("No Art", SCREEN_CX, y + MUSIC_COVER_H / 2, 2);
}

bool drawMusicCoverFromBridge() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0 || !musicHasArtwork) return false;
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + "/music/cover.raw";
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  markRequestPhase(REQUEST_CONNECTING);
  if (!http.begin(client, url)) { noteRequestResult(-1001); client.stop(); return false; }
  markRequestPhase(REQUEST_SENDING);
  int code = http.GET();
  noteRequestResult(code, http.getSize());
  if (code != HTTP_CODE_OK) {
    closeTrackedHttp(http, client);
    return false;
  }
  markRequestPhase(REQUEST_READING);
  WiFiClient *stream = http.getStreamPtr();
  const int x = (SCREEN_W - MUSIC_COVER_W) / 2;
  const int y = 14;
  const size_t rowBytes = (size_t)MUSIC_COVER_W * 2;
  bool ok = true;
  for (int r = 0; r < MUSIC_COVER_H; r++) {
    int got = stream->readBytes((uint8_t *)rowBuf, rowBytes);
    if (got != (int)rowBytes) {
      ok = false;
      break;
    }
    tft.pushImage(x, y + r, MUSIC_COVER_W, 1, rowBuf);
    yield();
  }
  closeTrackedHttp(http, client);
  return ok;
}

// Streams the Mac-rendered 232x44 title/artist strip and blits it row by
// row — the only way to get CJK on screen without shipping a font.
bool drawMusicTextFromBridge() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0) return false;
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + "/music/text.raw";
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  markRequestPhase(REQUEST_CONNECTING);
  if (!http.begin(client, url)) { noteRequestResult(-1001); client.stop(); return false; }
  markRequestPhase(REQUEST_SENDING);
  int code = http.GET();
  noteRequestResult(code, http.getSize());
  if (code != HTTP_CODE_OK) {
    closeTrackedHttp(http, client);
    return false;
  }
  markRequestPhase(REQUEST_READING);
  WiFiClient *stream = http.getStreamPtr();
  const size_t rowBytes = (size_t)MUSIC_TEXT_W * 2;
  bool ok = true;
  for (int r = 0; r < MUSIC_TEXT_H; r++) {
    int got = stream->readBytes((uint8_t *)rowBuf, rowBytes);
    if (got != (int)rowBytes) {
      ok = false;
      break;
    }
    tft.pushImage(MUSIC_TEXT_X, MUSIC_TEXT_Y + r, MUSIC_TEXT_W, 1, rowBuf);
    yield();
  }
  closeTrackedHttp(http, client);
  return ok;
}

// ASCII-only fallback if the strip fetch fails (CJK will stay blank, but at
// least latin titles show something).
void drawMusicTextFallback() {
  tft.fillRect(MUSIC_TEXT_X, MUSIC_TEXT_Y, MUSIC_TEXT_W, MUSIC_TEXT_H, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  String title = musicTitle[0] ? musicTitle : "No Music";
  tft.drawString(fitText(title, 216, 2), SCREEN_CX, MUSIC_TEXT_Y + 4, 2);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(fitText(musicArtist, 216, 2), SCREEN_CX, MUSIC_TEXT_Y + 24, 2);
}

// Regions repaint independently: cover / text strip only when their rev
// changes, progress bar + time on every poll (partial fill, no flicker
// elsewhere).
void drawMusicScreen(bool coverChanged, bool textChanged) {
  if (!musicChromeDrawn) {
    tft.fillScreen(TFT_BLACK);
    coverChanged = true;
    textChanged = true;
    musicChromeDrawn = true;
  }
  if (coverChanged) {
    musicCoverPending = musicHasArtwork;
    if (!musicHasArtwork) drawMusicCoverPlaceholder();
  }
  if (textChanged) {
    musicTextPending = true;
    drawMusicTextFallback();
  }

  const int bx = 20, by = 204, bw = 200, bh = 8;
  tft.fillRect(0, by - 2, SCREEN_W, SCREEN_H - by + 2, TFT_BLACK);
  tft.fillRect(bx, by, bw, bh, TFT_DARKGREY);
  float progress = musicDuration > 0 ? (float)musicElapsed / (float)musicDuration : 0;
  if (progress < 0) progress = 0;
  if (progress > 1) progress = 1;
  uint16_t color = musicPlaying ? TFT_GREEN : TFT_LIGHTGREY;
  tft.fillRect(bx, by, (int)(bw * progress), bh, color);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(timeText(musicElapsed) + " / " + timeText(musicDuration), SCREEN_CX, 220, 1);
}

bool pollMusic() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0) return false;
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + "/music";
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  markRequestPhase(REQUEST_CONNECTING);
  if (!http.begin(client, url)) { noteRequestResult(-1001); client.stop(); return false; }
  markRequestPhase(REQUEST_SENDING);
  int code = http.GET();
  noteRequestResult(code, http.getSize());
  bool ok = false;
  if (code == HTTP_CODE_OK) {
    markRequestPhase(REQUEST_PARSING);
    JsonDocument doc;
    if (!deserializeJson(doc, *http.getStreamPtr())) {
      strlcpy(musicTitle, doc["title"] | "", sizeof(musicTitle));
      strlcpy(musicArtist, doc["artist"] | "", sizeof(musicArtist));
      musicPlaying = doc["playing"] | false;
      statusMusicPlaying = musicPlaying; // fast stop-detection while music shows
      musicElapsed = doc["elapsed"] | 0;
      musicDuration = doc["duration"] | 0;
      musicHasArtwork = doc["has_artwork"] | false;
      int rev = doc["artwork_rev"] | -1;
      bool coverChanged = rev != musicArtworkRev;
      musicArtworkRev = rev;
      int tRev = doc["text_rev"] | -1;
      bool textChanged = tRev != musicTextRev;
      musicTextRev = tRev;
      drawMusicScreen(coverChanged, textChanged);
      ok = true;
    }
  }
  closeTrackedHttp(http, client);
  return ok;
}

// ---------- stock watchlist screen ----------

bool applyStockJson(JsonDocument &doc) {
  JsonArray arr = doc["stocks"];
  stockCount = 0;
  for (JsonObject s : arr) {
    if (stockCount >= MAX_STOCKS) break;
    strlcpy(stocks[stockCount].code, s["code"] | "", sizeof(stocks[stockCount].code));
    strlcpy(stocks[stockCount].price, s["price"] | "", sizeof(stocks[stockCount].price));
    strlcpy(stocks[stockCount].pct, s["pct"] | "", sizeof(stocks[stockCount].pct));
    stocks[stockCount].up = s["up"] | 0;
    stockCount++;
  }
  const uint32_t incomingNamesRev = doc["names_rev"] | (uint32_t)0;
  stockNamesCacheSupported = doc["names_cacheable"] | false;
  stockNamesRev = incomingNamesRev;
  if (stockCount == 0 || stockNamesRev == 0) {
    stockNamesPending = false;
    stockNamesDrawPending = false;
  } else if (stockNamesCacheSupported && stockNamesCacheValid &&
             stockNamesCachedRev == stockNamesRev &&
             stockNamesCacheCount == stockCount) {
    stockNamesPending = false;
    stockNamesFailures = 0;
    stockNamesRetryAtMs = 0;
    if (diagnosticEffectiveMode == MODE_STOCK &&
        stockNamesDrawnRev != stockNamesRev) {
      stockNamesDrawPending = true;
    }
  } else if (diagnosticEffectiveMode == MODE_STOCK &&
             stockNamesDrawnRev != stockNamesRev) {
    stockNamesPending = true;
    stockNamesRetryAtMs = 0;
  }
  stockEverLoaded = true;
  stockDirty = true;
  return true;
}

bool handleStockPayload(const String &payload) {
  JsonDocument doc;
  return !deserializeJson(doc, payload) && applyStockJson(doc);
}

uint32_t stockHashAppend(uint32_t hash, const char *text) {
  while (*text) {
    hash ^= (uint8_t)*text++;
    hash *= 16777619UL;
  }
  return hash;
}

// Legacy raw endpoint retained for pairing with an older Mac bridge.
bool drawStockNamesRaw() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0) return false;
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + "/stock/names.raw";
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  markRequestPhase(REQUEST_CONNECTING);
  if (!http.begin(client, url)) { noteRequestResult(-1001); client.stop(); return false; }
  markRequestPhase(REQUEST_SENDING);
  int code = http.GET();
  noteRequestResult(code, http.getSize());
  if (code != HTTP_CODE_OK) {
    closeTrackedHttp(http, client);
    return false;
  }
  markRequestPhase(REQUEST_READING);
  WiFiClient *stream = http.getStreamPtr();
  uint8_t cnt = 0;
  if (stream->readBytes(&cnt, 1) != 1) {
    closeTrackedHttp(http, client);
    return false;
  }
  const size_t rowBytes = (size_t)STOCK_NAME_W * 2;
  bool ok = true;
  for (int i = 0; i < cnt && ok; i++) {
    int y0 = 10 + i * 54;
    for (int r = 0; r < STOCK_NAME_H; r++) {
      if (stream->readBytes((uint8_t *)rowBuf, rowBytes) != (int)rowBytes) {
        ok = false;
        break;
      }
      if (i < stockCount) tft.pushImage(70, y0 + r, STOCK_NAME_W, 1, rowBuf);
      yield();
    }
  }
  closeTrackedHttp(http, client);
  return ok;
}

// RLE name strips avoid transferring ~20KB of mostly-black RGB565 pixels each
// time the carousel re-enters the quote page. Format: "SNR1", row count, then
// [run:1][RGB565 big-endian:2]. Decode directly into the existing one-row
// scratch buffer, so compressed and decoded frames never coexist in heap.
bool drawStockNamesRle() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0) return false;
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + "/stock/names.rle";
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  markRequestPhase(REQUEST_CONNECTING);
  if (!http.begin(client, url)) { noteRequestResult(-1001); client.stop(); return false; }
  markRequestPhase(REQUEST_SENDING);
  const int code = http.GET();
  noteRequestResult(code, http.getSize());
  if (code != HTTP_CODE_OK) {
    closeTrackedHttp(http, client);
    return false;
  }
  markRequestPhase(REQUEST_READING);
  WiFiClient *stream = http.getStreamPtr();
  uint8_t header[5] = {};
  if (stream->readBytes(header, sizeof(header)) != (int)sizeof(header) ||
      memcmp(header, "SNR1", 4) != 0 || header[4] > MAX_STOCKS) {
    closeTrackedHttp(http, client);
    return false;
  }
  const uint8_t count = header[4];
  const uint32_t totalPixels = (uint32_t)count * STOCK_NAME_W * STOCK_NAME_H;
  uint32_t emitted = 0;
  int item = 0, row = 0, column = 0;
  bool ok = true;
  while (emitted < totalPixels && ok) {
    uint8_t runData[3] = {};
    if (stream->readBytes(runData, sizeof(runData)) != (int)sizeof(runData) ||
        runData[0] == 0 || emitted + runData[0] > totalPixels) {
      ok = false;
      break;
    }
    // Match the byte order produced when the raw big-endian stream is read
    // directly into the little-endian uint16_t row buffer.
    const uint16_t pixel = (uint16_t)runData[1] | ((uint16_t)runData[2] << 8);
    for (uint16_t n = 0; n < runData[0]; ++n) {
      rowBuf[column++] = pixel;
      emitted++;
      if (column == STOCK_NAME_W) {
        if (item < stockCount) {
          const int y0 = 10 + item * 54;
          tft.pushImage(70, y0 + row, STOCK_NAME_W, 1, rowBuf);
        }
        column = 0;
        if (++row == STOCK_NAME_H) { row = 0; item++; }
        yield();
      }
    }
  }
  ok = ok && emitted == totalPixels && column == 0 && item == count;
  closeTrackedHttp(http, client);
  return ok;
}

bool drawStockNames() {
  if (stockNamesPreferRle) {
    const bool ok = drawStockNamesRle();
    if (!ok) stockNamesPreferRle = false; // next scheduler pass uses old bridge endpoint
    return ok;
  }
  return drawStockNamesRaw();
}

// Local cache format: "SNC2" + stable names_rev (UInt32 BE) + the bridge's
// unchanged SNR1 payload + CRC32 of that payload (UInt32 BE). Keeping the
// network protocol backward-compatible means older firmware can continue to
// consume /stock/names.rle, while v0.5.17+ downloads it only after a revision
// change and redraws future page visits entirely from LittleFS.
const size_t STOCK_NAMES_CACHE_PREFIX_BYTES = 8;
const size_t STOCK_NAMES_CACHE_SUFFIX_BYTES = 4;

uint32_t stockNamesReadU32BE(const uint8_t *data) {
  return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
         ((uint32_t)data[2] << 8) | data[3];
}

void stockNamesWriteU32BE(uint32_t value, uint8_t *data) {
  data[0] = (uint8_t)(value >> 24);
  data[1] = (uint8_t)(value >> 16);
  data[2] = (uint8_t)(value >> 8);
  data[3] = (uint8_t)value;
}

uint32_t stockNamesCrc32Byte(uint32_t crc, uint8_t value) {
  crc ^= value;
  for (int bit = 0; bit < 8; ++bit) {
    crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320UL : 0U);
  }
  return crc;
}

bool validateStockNamesCache(const char *path, uint32_t desiredRev,
                             uint32_t &revisionOut, uint8_t &countOut) {
  File file = LittleFS.open(path, "r");
  const size_t minimumBytes = STOCK_NAMES_CACHE_PREFIX_BYTES + 5 + 3 +
                              STOCK_NAMES_CACHE_SUFFIX_BYTES;
  if (!file || file.size() < minimumBytes ||
      file.size() > STOCK_NAMES_RLE_MAX_BYTES + STOCK_NAMES_CACHE_PREFIX_BYTES +
                    STOCK_NAMES_CACHE_SUFFIX_BYTES) {
    if (file) file.close();
    return false;
  }

  uint8_t prefix[STOCK_NAMES_CACHE_PREFIX_BYTES] = {};
  uint8_t snrHeader[5] = {};
  if (file.read(prefix, sizeof(prefix)) != (int)sizeof(prefix) ||
      memcmp(prefix, "SNC2", 4) != 0 ||
      file.read(snrHeader, sizeof(snrHeader)) != (int)sizeof(snrHeader) ||
      memcmp(snrHeader, "SNR1", 4) != 0 || snrHeader[4] == 0 ||
      snrHeader[4] > MAX_STOCKS) {
    file.close();
    return false;
  }
  const uint32_t revision = stockNamesReadU32BE(prefix + 4);
  if (revision == 0 || (desiredRev != 0 && revision != desiredRev)) {
    file.close();
    return false;
  }

  const size_t payloadEnd = file.size() - STOCK_NAMES_CACHE_SUFFIX_BYTES;
  const uint32_t expectedPixels =
      (uint32_t)snrHeader[4] * STOCK_NAME_W * STOCK_NAME_H;
  uint32_t emitted = 0;
  uint32_t crc = 0xFFFFFFFFUL;
  for (uint8_t b : snrHeader) crc = stockNamesCrc32Byte(crc, b);
  while (file.position() < payloadEnd && emitted < expectedPixels) {
    if (payloadEnd - file.position() < 3) { file.close(); return false; }
    uint8_t record[3] = {};
    if (file.read(record, sizeof(record)) != (int)sizeof(record) ||
        record[0] == 0 || emitted + record[0] > expectedPixels) {
      file.close();
      return false;
    }
    crc = stockNamesCrc32Byte(crc, record[0]);
    crc = stockNamesCrc32Byte(crc, record[1]);
    crc = stockNamesCrc32Byte(crc, record[2]);
    emitted += record[0];
    yield();
  }
  uint8_t storedCrc[4] = {};
  const bool structureOK = emitted == expectedPixels &&
                           file.position() == payloadEnd &&
                           file.read(storedCrc, sizeof(storedCrc)) ==
                               (int)sizeof(storedCrc);
  file.close();
  if (!structureOK || (~crc) != stockNamesReadU32BE(storedCrc)) return false;
  revisionOut = revision;
  countOut = snrHeader[4];
  return true;
}

bool installStockNamesCache(const char *temporaryPath, uint32_t revision) {
  uint32_t validatedRevision = 0;
  uint8_t validatedCount = 0;
  if (!validateStockNamesCache(temporaryPath, revision, validatedRevision,
                               validatedCount)) {
    LittleFS.remove(temporaryPath);
    return false;
  }
  LittleFS.remove(STOCK_NAMES_CACHE_BACKUP_FILE);
  const bool hadCache = LittleFS.exists(STOCK_NAMES_CACHE_FILE);
  if (hadCache && !LittleFS.rename(STOCK_NAMES_CACHE_FILE,
                                   STOCK_NAMES_CACHE_BACKUP_FILE)) {
    LittleFS.remove(temporaryPath);
    return false;
  }
  if (!LittleFS.rename(temporaryPath, STOCK_NAMES_CACHE_FILE)) {
    if (hadCache) LittleFS.rename(STOCK_NAMES_CACHE_BACKUP_FILE,
                                  STOCK_NAMES_CACHE_FILE);
    LittleFS.remove(temporaryPath);
    return false;
  }
  LittleFS.remove(STOCK_NAMES_CACHE_BACKUP_FILE);
  stockNamesCachedRev = validatedRevision;
  stockNamesCacheCount = validatedCount;
  stockNamesCacheValid = true;
  return true;
}

void loadStockNamesCache() {
  if (!LittleFS.exists(STOCK_NAMES_CACHE_FILE) &&
      LittleFS.exists(STOCK_NAMES_CACHE_BACKUP_FILE)) {
    LittleFS.rename(STOCK_NAMES_CACHE_BACKUP_FILE, STOCK_NAMES_CACHE_FILE);
  }
  uint32_t revision = 0;
  uint8_t count = 0;
  stockNamesCacheValid = validateStockNamesCache(
      STOCK_NAMES_CACHE_FILE, 0, revision, count);
  if (!stockNamesCacheValid && LittleFS.exists(STOCK_NAMES_CACHE_BACKUP_FILE)) {
    uint32_t backupRevision = 0;
    uint8_t backupCount = 0;
    if (validateStockNamesCache(STOCK_NAMES_CACHE_BACKUP_FILE, 0,
                                backupRevision, backupCount)) {
      LittleFS.remove(STOCK_NAMES_CACHE_FILE);
      if (LittleFS.rename(STOCK_NAMES_CACHE_BACKUP_FILE,
                          STOCK_NAMES_CACHE_FILE)) {
        stockNamesCacheValid = true;
        revision = backupRevision;
        count = backupCount;
      }
    }
  }
  stockNamesCachedRev = stockNamesCacheValid ? revision : 0;
  stockNamesCacheCount = stockNamesCacheValid ? count : 0;
  LittleFS.remove(STOCK_NAMES_CACHE_TMP_FILE);
  if (stockNamesCacheValid) LittleFS.remove(STOCK_NAMES_CACHE_BACKUP_FILE);
  Serial.printf("[stock] names cache=%s rev=%lu rows=%u\n",
                stockNamesCacheValid ? "valid" : "missing",
                (unsigned long)stockNamesCachedRev, stockNamesCacheCount);
}

bool downloadStockNamesCache() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0 ||
      stockNamesRev == 0) return false;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  markRequestPhase(REQUEST_CONNECTING);
  if (!http.begin(client, "http://" + bridgeHost + "/stock/names.rle")) {
    noteRequestResult(-1001); client.stop(); return false;
  }
  markRequestPhase(REQUEST_SENDING);
  const int code = http.GET();
  const int expectedBytes = http.getSize();
  noteRequestResult(code, expectedBytes);
  if (code != HTTP_CODE_OK || expectedBytes < 8 ||
      expectedBytes > STOCK_NAMES_RLE_MAX_BYTES) {
    closeTrackedHttp(http, client);
    return false;
  }

  File output = LittleFS.open(STOCK_NAMES_CACHE_TMP_FILE, "w");
  if (!output) {
    noteRequestResult(-1003);
    closeTrackedHttp(http, client);
    return false;
  }
  uint8_t prefix[STOCK_NAMES_CACHE_PREFIX_BYTES] = {'S', 'N', 'C', '2'};
  stockNamesWriteU32BE(stockNamesRev, prefix + 4);
  bool ok = output.write(prefix, sizeof(prefix)) == sizeof(prefix);
  markRequestPhase(REQUEST_READING);
  WiFiClient *stream = http.getStreamPtr();
  uint8_t buffer[256];
  size_t received = 0;
  uint32_t crc = 0xFFFFFFFFUL;
  unsigned long deadline = millis() + BRIDGE_HTTP_TIMEOUT_MS;
  while (ok && received < (size_t)expectedBytes) {
    const int available = stream->available();
    if (available > 0) {
      const size_t wanted = min((size_t)available,
                                min(sizeof(buffer), (size_t)expectedBytes - received));
      const int count = stream->read(buffer, wanted);
      if (count > 0) {
        if (output.write(buffer, count) != (size_t)count) { ok = false; break; }
        for (int i = 0; i < count; ++i) crc = stockNamesCrc32Byte(crc, buffer[i]);
        received += count;
        activeRequestBytes = received;
        deadline = millis() + BRIDGE_HTTP_TIMEOUT_MS;
        yield();
        continue;
      }
    }
    if ((long)(millis() - deadline) >= 0) { ok = false; break; }
    delay(1);
  }
  if (ok && received == (size_t)expectedBytes) {
    uint8_t suffix[4];
    stockNamesWriteU32BE(~crc, suffix);
    ok = output.write(suffix, sizeof(suffix)) == sizeof(suffix);
  }
  output.close();
  closeTrackedHttp(http, client);
  if (!ok || received != (size_t)expectedBytes) {
    LittleFS.remove(STOCK_NAMES_CACHE_TMP_FILE);
    return false;
  }
  markRequestPhase(REQUEST_PARSING);
  return installStockNamesCache(STOCK_NAMES_CACHE_TMP_FILE, stockNamesRev);
}

bool drawStockNamesFromCache() {
  if (!stockNamesCacheValid || stockNamesCachedRev != stockNamesRev ||
      stockNamesCacheCount != stockCount) return false;
  File file = LittleFS.open(STOCK_NAMES_CACHE_FILE, "r");
  if (!file) { stockNamesCacheValid = false; return false; }
  uint8_t prefix[STOCK_NAMES_CACHE_PREFIX_BYTES] = {};
  uint8_t header[5] = {};
  if (file.read(prefix, sizeof(prefix)) != (int)sizeof(prefix) ||
      memcmp(prefix, "SNC2", 4) != 0 ||
      stockNamesReadU32BE(prefix + 4) != stockNamesRev ||
      file.read(header, sizeof(header)) != (int)sizeof(header) ||
      memcmp(header, "SNR1", 4) != 0 || header[4] != stockCount) {
    file.close(); stockNamesCacheValid = false; return false;
  }
  const uint32_t totalPixels = (uint32_t)stockCount * STOCK_NAME_W * STOCK_NAME_H;
  uint32_t emitted = 0;
  int item = 0, row = 0, column = 0;
  bool ok = true;
  while (emitted < totalPixels && ok) {
    uint8_t record[3] = {};
    if (file.read(record, sizeof(record)) != (int)sizeof(record) ||
        record[0] == 0 || emitted + record[0] > totalPixels) {
      ok = false;
      break;
    }
    const uint16_t pixel = (uint16_t)record[1] | ((uint16_t)record[2] << 8);
    for (uint16_t n = 0; n < record[0]; ++n) {
      rowBuf[column++] = pixel;
      emitted++;
      if (column == STOCK_NAME_W) {
        const int y0 = 10 + item * 54;
        tft.pushImage(70, y0 + row, STOCK_NAME_W, 1, rowBuf);
        column = 0;
        if (++row == STOCK_NAME_H) { row = 0; item++; }
        yield();
      }
    }
  }
  file.close();
  return ok && emitted == totalPixels && column == 0 && item == stockCount;
}

unsigned long stockNamesRetryDelayMs() {
  if (stockNamesFailures <= 1) return 10000UL;
  if (stockNamesFailures == 2) return 30000UL;
  return 300000UL;
}

bool updateStockNamesAsset() {
  const bool ok = stockNamesCacheSupported ? downloadStockNamesCache()
                                            : drawStockNames();
  if (ok) {
    stockNamesFailures = 0;
    stockNamesRetryAtMs = 0;
    stockNamesPending = false;
    if (stockNamesCacheSupported) stockNamesDrawPending = true;
    else stockNamesDrawnRev = stockNamesRev;
    return true;
  }
  if (stockNamesFailures < 255) stockNamesFailures++;
  stockNamesRetryAtMs = millis() + stockNamesRetryDelayMs();
  return false;
}

bool pollStock() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0) return false;
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + "/stock";
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  markRequestPhase(REQUEST_CONNECTING);
  if (!http.begin(client, url)) { noteRequestResult(-1001); client.stop(); return false; }
  markRequestPhase(REQUEST_SENDING);
  int code = http.GET();
  noteRequestResult(code, http.getSize());
  bool ok = false;
  if (code == HTTP_CODE_OK) {
    markRequestPhase(REQUEST_PARSING);
    JsonDocument doc;
    if (!deserializeJson(doc, *http.getStreamPtr())) ok = applyStockJson(doc);
  }
  closeTrackedHttp(http, client);
  return ok;
}

// 54px per row: small grey code on top, big font-4 price (white) on the left
// and change% on the right - red rising / green falling (CN convention).
// Rows repaint only when their text changes, same trick as everywhere else.
void drawStockScreen() {
  if (!stockChromeDrawn) {
    tft.fillScreen(TFT_BLACK);
    stockChromeDrawn = true;
    for (int i = 0; i < MAX_STOCKS; i++) {
      stockLastCodeHash[i] = UINT32_MAX; // force repaint
      stockLastValHash[i] = UINT32_MAX;
    }
    stockNamesDrawnRev = 0;
    stockNamesDrawPending = false;
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(0x7BEF, TFT_BLACK);
    tft.drawString("STOCKS", SCREEN_CX, 228, 1);
  }
  stockDirty = false;

  if (stockCount == 0) {
    stockNamesPending = false;
    stockNamesDrawPending = false;
    if (stockLastCodeHash[0] != 0) {
      for (int i = 0; i < MAX_STOCKS; i++) {
        stockLastCodeHash[i] = 0;
        stockLastValHash[i] = 0;
      }
      tft.fillRect(0, 0, SCREEN_W, 226, TFT_BLACK);
      tft.setTextDatum(TC_DATUM);
      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      tft.drawString(stockEverLoaded ? "No stocks configured" : "Waiting for bridge...", SCREEN_CX, 100, 2);
      if (stockEverLoaded) tft.drawString("Mac menu: Set watchlist", SCREEN_CX, 124, 2);
    }
    return;
  }

  for (int i = 0; i < MAX_STOCKS; i++) {
    int y0 = 10 + i * 54;
    bool has = i < stockCount;
    // top line (code + name strip) and value line refresh independently, so
    // a price tick never wipes the name bitmap
    const uint32_t codeHash = has ? stockHashAppend(2166136261UL, stocks[i].code) : 0;
    if (codeHash != stockLastCodeHash[i]) {
      stockLastCodeHash[i] = codeHash;
      tft.fillRect(0, y0, SCREEN_W, 17, TFT_BLACK);
      stockNamesDrawnRev = 0; // strip area wiped: redraw names from local cache
      if (has) {
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(0x7BEF, TFT_BLACK);
        tft.drawString(stocks[i].code, 14, y0, 2);
      }
    }
    uint32_t valHash = 0;
    if (has) {
      valHash = stockHashAppend(2166136261UL, stocks[i].price);
      valHash = stockHashAppend(valHash, stocks[i].pct);
      valHash ^= (uint8_t)(stocks[i].up + 1);
      valHash *= 16777619UL;
    }
    if (valHash != stockLastValHash[i]) {
      stockLastValHash[i] = valHash;
      tft.fillRect(0, y0 + 18, SCREEN_W, 36, TFT_BLACK); // value line + inter-row gap
      if (has) {
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString(stocks[i].price, 14, y0 + 18, 4);
        uint16_t pc = stocks[i].up > 0 ? TFT_RED : (stocks[i].up < 0 ? TFT_GREEN : TFT_LIGHTGREY);
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(pc, TFT_BLACK);
        tft.drawString(stocks[i].pct, 226, y0 + 18, 4);
      }
    }
  }

  // Repaint from LittleFS after a page switch. Network is used only if the
  // stable content revision differs from the cached one.
  if (stockNamesRev != 0 && stockNamesDrawnRev != stockNamesRev) {
    if (stockNamesCacheSupported && stockNamesCacheValid &&
        stockNamesCachedRev == stockNamesRev &&
        stockNamesCacheCount == stockCount) {
      stockNamesDrawPending = true;
      stockNamesPending = false;
    } else {
      stockNamesPending = true;
    }
  }
}

// ---------- date + weather screen ----------

bool applyWeatherJson(JsonDocument &doc) {
  const time_t serverUnix = doc["server_unix"] | (time_t)0;
  if (serverUnix > 0) {
    weather.serverUnix = serverUnix;
    weather.syncedAtMs = millis();
    weather.utcOffsetSeconds = doc["utc_offset_seconds"] | (long)(8 * 3600);
    weather.scheduleUtcOffsetSeconds = doc["schedule_utc_offset_seconds"] |
                                       weather.utcOffsetSeconds;
  }
  weather.valid = doc["valid"] | false;
  weather.stale = doc["stale"] | true;
  if (weather.valid) {
    weather.currentTemp = doc["current_temp"] | 0.0;
    weather.currentCode = doc["current_code"] | 0;
    weather.todayHigh = doc["today_high"] | 0.0;
    weather.todayLow = doc["today_low"] | 0.0;
    weather.todayCode = doc["today_code"] | 0;
    weather.tomorrowHigh = doc["tomorrow_high"] | 0.0;
    weather.tomorrowLow = doc["tomorrow_low"] | 0.0;
    weather.tomorrowCode = doc["tomorrow_code"] | 0;
  }
  const uint32_t incomingTextRev = doc["text_rev"] | (uint32_t)0;
  if (incomingTextRev != 0) {
    weatherTextLegacyRaw = false;
    weatherTextRev = incomingTextRev;
    if (!weatherTextCacheValid || weatherTextCachedRev != weatherTextRev) {
      weatherTextPending = true;
      weatherTextRetryAtMs = 0;
    } else {
      weatherTextPending = false;
      weatherTextFailures = 0;
      weatherTextRetryAtMs = 0;
      if (diagnosticEffectiveMode == MODE_WEATHER) weatherTextDrawPending = true;
    }
  } else {
    // New firmware remains usable with a pre-v0.5.16 bridge. The legacy raw
    // strip is requested once per weather-page visit, never from draw code.
    weatherTextLegacyRaw = true;
    weatherTextRev = 0;
    if (diagnosticEffectiveMode == MODE_WEATHER) weatherTextPending = true;
  }
  weatherDirty = true;
  return true;
}

bool handleWeatherPayload(const String &payload) {
  JsonDocument doc;
  return !deserializeJson(doc, payload) && applyWeatherJson(doc);
}

bool pollWeather() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0) return false;
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + "/weather";
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  markRequestPhase(REQUEST_CONNECTING);
  if (!http.begin(client, url)) { noteRequestResult(-1001); client.stop(); return false; }
  markRequestPhase(REQUEST_SENDING);
  const int code = http.GET();
  noteRequestResult(code, http.getSize());
  bool ok = false;
  if (code == HTTP_CODE_OK) {
    markRequestPhase(REQUEST_PARSING);
    JsonDocument doc;
    if (!deserializeJson(doc, *http.getStreamPtr())) ok = applyWeatherJson(doc);
  }
  closeTrackedHttp(http, client);
  return ok;
}

bool weatherCodeHasRain(int code) {
  return (code >= 51 && code <= 67) || (code >= 80 && code <= 82) || code >= 95;
}

bool weatherCodeHasSnow(int code) {
  return (code >= 71 && code <= 77) || (code >= 85 && code <= 86);
}

void drawWeatherIcon(int cx, int cy, int size, int code) {
  const uint16_t yellow = tft.color565(255, 199, 61);
  const uint16_t cloud = tft.color565(184, 201, 214);
  const uint16_t rain = tft.color565(84, 191, 255);
  const bool clear = code == 0;
  const bool hasSun = code <= 2;
  if (hasSun) {
    const int r = clear ? size * 23 / 100 : size * 17 / 100;
    const int sunY = cy - (clear ? 0 : size * 12 / 100);
    tft.fillCircle(cx, sunY, r, yellow);
    if (clear) {
      for (int i = 0; i < 8; ++i) {
        const float a = i * PI / 4.0f;
        tft.drawLine(cx + cos(a) * size * .32f, cy + sin(a) * size * .32f,
                     cx + cos(a) * size * .45f, cy + sin(a) * size * .45f, yellow);
      }
      return;
    }
  }
  const int y = cy + size * 5 / 100;
  tft.fillRoundRect(cx - size * 36 / 100, y - size * 10 / 100,
                    size * 72 / 100, size * 30 / 100, size * 15 / 100, cloud);
  tft.fillCircle(cx, y - size * 10 / 100, size * 22 / 100, cloud);
  if (weatherCodeHasRain(code) || weatherCodeHasSnow(code)) {
    for (int i = -1; i <= 1; ++i) {
      const int x = cx + i * size * 22 / 100;
      const int y0 = cy + size * 28 / 100;
      const int slant = weatherCodeHasSnow(code) ? 0 : size * 6 / 100;
      tft.drawLine(x, y0, x - slant, cy + size * 43 / 100, rain);
    }
  }
}

void drawWeatherClock() {
  time_t localEpoch = weatherLocalEpoch();
  struct tm localTm = {};
  if (localEpoch > 0) gmtime_r(&localEpoch, &localTm);
  char timeText[16];
  if (localEpoch > 0) {
    snprintf(timeText, sizeof(timeText), "%02d:%02d:%02d", localTm.tm_hour,
             localTm.tm_min, localTm.tm_sec);
  } else {
    strcpy(timeText, "--:--:--");
  }
  tft.fillRect(0, 33, SCREEN_W, 43, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(timeText, SCREEN_CX, 39, 4);
}

bool drawWeatherTextStrip(WiFiClient *stream, int x, int y, int w, int h) {
  const size_t bytes = (size_t)w * 2;
  for (int row = 0; row < h; ++row) {
    if (stream->readBytes((uint8_t *)rowBuf, bytes) != bytes) return false;
    tft.pushImage(x, y + row, w, 1, rowBuf);
    yield();
  }
  return true;
}

// Chinese city/date/condition labels are rendered by AppKit on the Mac and
// streamed as five small RGB565 strips, avoiding a multi-hundred-KB CJK font
// in ESP8266 flash.
bool drawWeatherChineseTextRaw() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0) return false;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  markRequestPhase(REQUEST_CONNECTING);
  if (!http.begin(client, "http://" + bridgeHost + "/weather/text.raw")) {
    noteRequestResult(-1001); client.stop(); return false;
  }
  markRequestPhase(REQUEST_SENDING);
  const int code = http.GET();
  noteRequestResult(code, http.getSize());
  if (code != HTTP_CODE_OK) { closeTrackedHttp(http, client); return false; }
  markRequestPhase(REQUEST_READING);
  WiFiClient *stream = http.getStreamPtr();
  bool ok = drawWeatherTextStrip(stream, 0, 0, 108, 32) &&
            drawWeatherTextStrip(stream, 108, 0, 132, 32) &&
            drawWeatherTextStrip(stream, 107, 139, 130, 22) &&
            drawWeatherTextStrip(stream, 42, 177, 78, 28) &&
            drawWeatherTextStrip(stream, 162, 177, 78, 28);
  closeTrackedHttp(http, client);
  return ok;
}

const uint32_t WEATHER_TEXT_EXPECTED_PIXELS =
    108UL * 32UL + 132UL * 32UL + 130UL * 22UL + 78UL * 28UL * 2UL;
const size_t WEATHER_TEXT_HEADER_BYTES = 16;

uint32_t weatherReadU32BE(const uint8_t *data) {
  return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
         ((uint32_t)data[2] << 8) | data[3];
}

uint32_t weatherCrc32Byte(uint32_t crc, uint8_t value) {
  crc ^= value;
  for (int bit = 0; bit < 8; ++bit) {
    crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320UL : 0U);
  }
  return crc;
}

bool validateWeatherTextFile(const char *path, uint32_t desiredRev,
                             uint32_t &revisionOut) {
  File file = LittleFS.open(path, "r");
  if (!file || file.size() < WEATHER_TEXT_HEADER_BYTES + 3 ||
      file.size() > WEATHER_TEXT_RLE_MAX_BYTES) {
    if (file) file.close();
    return false;
  }
  uint8_t header[WEATHER_TEXT_HEADER_BYTES] = {};
  if (file.read(header, sizeof(header)) != (int)sizeof(header) ||
      memcmp(header, "WTR1", 4) != 0) {
    file.close();
    return false;
  }
  const uint32_t revision = weatherReadU32BE(header + 4);
  const uint32_t pixels = weatherReadU32BE(header + 8);
  const uint32_t expectedCrc = weatherReadU32BE(header + 12);
  if (revision == 0 || pixels != WEATHER_TEXT_EXPECTED_PIXELS ||
      (desiredRev != 0 && revision != desiredRev)) {
    file.close();
    return false;
  }

  uint32_t emitted = 0;
  uint32_t crc = 0xFFFFFFFFUL;
  while (file.available()) {
    uint8_t record[3] = {};
    if (file.read(record, sizeof(record)) != (int)sizeof(record) || record[0] == 0 ||
        emitted + record[0] > WEATHER_TEXT_EXPECTED_PIXELS) {
      file.close();
      return false;
    }
    crc = weatherCrc32Byte(crc, record[0]);
    crc = weatherCrc32Byte(crc, record[1]);
    crc = weatherCrc32Byte(crc, record[2]);
    emitted += record[0];
    yield();
  }
  file.close();
  revisionOut = revision;
  return emitted == WEATHER_TEXT_EXPECTED_PIXELS && (~crc) == expectedCrc;
}

bool installWeatherTextCache(const char *temporaryPath, uint32_t revision) {
  uint32_t validatedRevision = 0;
  if (!validateWeatherTextFile(temporaryPath, revision, validatedRevision)) {
    LittleFS.remove(temporaryPath);
    return false;
  }
  LittleFS.remove(WEATHER_TEXT_CACHE_BACKUP_FILE);
  const bool hadCache = LittleFS.exists(WEATHER_TEXT_CACHE_FILE);
  if (hadCache && !LittleFS.rename(WEATHER_TEXT_CACHE_FILE,
                                   WEATHER_TEXT_CACHE_BACKUP_FILE)) {
    LittleFS.remove(temporaryPath);
    return false;
  }
  if (!LittleFS.rename(temporaryPath, WEATHER_TEXT_CACHE_FILE)) {
    if (hadCache) LittleFS.rename(WEATHER_TEXT_CACHE_BACKUP_FILE,
                                  WEATHER_TEXT_CACHE_FILE);
    LittleFS.remove(temporaryPath);
    return false;
  }
  LittleFS.remove(WEATHER_TEXT_CACHE_BACKUP_FILE);
  weatherTextCachedRev = validatedRevision;
  weatherTextCacheValid = true;
  return true;
}

void loadWeatherTextCache() {
  if (!LittleFS.exists(WEATHER_TEXT_CACHE_FILE) &&
      LittleFS.exists(WEATHER_TEXT_CACHE_BACKUP_FILE)) {
    LittleFS.rename(WEATHER_TEXT_CACHE_BACKUP_FILE, WEATHER_TEXT_CACHE_FILE);
  }
  uint32_t revision = 0;
  weatherTextCacheValid = validateWeatherTextFile(
      WEATHER_TEXT_CACHE_FILE, 0, revision);
  if (!weatherTextCacheValid && LittleFS.exists(WEATHER_TEXT_CACHE_BACKUP_FILE)) {
    uint32_t backupRevision = 0;
    if (validateWeatherTextFile(WEATHER_TEXT_CACHE_BACKUP_FILE, 0,
                                backupRevision)) {
      LittleFS.remove(WEATHER_TEXT_CACHE_FILE);
      if (LittleFS.rename(WEATHER_TEXT_CACHE_BACKUP_FILE,
                          WEATHER_TEXT_CACHE_FILE)) {
        weatherTextCacheValid = true;
        revision = backupRevision;
      }
    }
  }
  weatherTextCachedRev = weatherTextCacheValid ? revision : 0;
  LittleFS.remove(WEATHER_TEXT_CACHE_TMP_FILE);
  if (weatherTextCacheValid) LittleFS.remove(WEATHER_TEXT_CACHE_BACKUP_FILE);
  Serial.printf("[weather] cache=%s rev=%lu\n",
                weatherTextCacheValid ? "valid" : "missing",
                (unsigned long)weatherTextCachedRev);
}

bool downloadWeatherTextRLE() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0 ||
      weatherTextRev == 0) return false;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  markRequestPhase(REQUEST_CONNECTING);
  if (!http.begin(client, "http://" + bridgeHost + "/weather/text.rle")) {
    noteRequestResult(-1001); client.stop(); return false;
  }
  markRequestPhase(REQUEST_SENDING);
  const int code = http.GET();
  const int expectedBytes = http.getSize();
  noteRequestResult(code, expectedBytes);
  if (code == HTTP_CODE_NOT_FOUND) weatherTextLegacyRaw = true;
  if (code != HTTP_CODE_OK || expectedBytes < (int)WEATHER_TEXT_HEADER_BYTES + 3 ||
      expectedBytes > WEATHER_TEXT_RLE_MAX_BYTES) {
    closeTrackedHttp(http, client);
    return false;
  }

  File output = LittleFS.open(WEATHER_TEXT_CACHE_TMP_FILE, "w");
  if (!output) {
    noteRequestResult(-1003);
    closeTrackedHttp(http, client);
    return false;
  }
  markRequestPhase(REQUEST_READING);
  WiFiClient *stream = http.getStreamPtr();
  uint8_t buffer[256];
  size_t received = 0;
  unsigned long deadline = millis() + BRIDGE_HTTP_TIMEOUT_MS;
  bool ok = true;
  while (received < (size_t)expectedBytes) {
    const int available = stream->available();
    if (available > 0) {
      const size_t wanted = min((size_t)available,
                                min(sizeof(buffer), (size_t)expectedBytes - received));
      const int count = stream->read(buffer, wanted);
      if (count > 0) {
        if (output.write(buffer, count) != (size_t)count) { ok = false; break; }
        received += count;
        activeRequestBytes = received;
        deadline = millis() + BRIDGE_HTTP_TIMEOUT_MS;
        yield();
        continue;
      }
    }
    if ((long)(millis() - deadline) >= 0) { ok = false; break; }
    delay(1);
  }
  output.close();
  closeTrackedHttp(http, client);
  if (!ok || received != (size_t)expectedBytes) {
    LittleFS.remove(WEATHER_TEXT_CACHE_TMP_FILE);
    return false;
  }
  markRequestPhase(REQUEST_PARSING);
  return installWeatherTextCache(WEATHER_TEXT_CACHE_TMP_FILE, weatherTextRev);
}

bool drawWeatherTextFromCache() {
  if (!weatherTextCacheValid) return false;
  File file = LittleFS.open(WEATHER_TEXT_CACHE_FILE, "r");
  if (!file) { weatherTextCacheValid = false; return false; }
  uint8_t header[WEATHER_TEXT_HEADER_BYTES] = {};
  if (file.read(header, sizeof(header)) != (int)sizeof(header) ||
      memcmp(header, "WTR1", 4) != 0 ||
      weatherReadU32BE(header + 8) != WEATHER_TEXT_EXPECTED_PIXELS) {
    file.close(); weatherTextCacheValid = false; return false;
  }

  const int widths[5] = {108, 132, 130, 78, 78};
  const int heights[5] = {32, 32, 22, 28, 28};
  const int xs[5] = {0, 108, 107, 42, 162};
  const int ys[5] = {0, 0, 139, 177, 177};
  int strip = 0, row = 0, column = 0;
  uint32_t emitted = 0;
  bool ok = true;
  while (file.available() && emitted < WEATHER_TEXT_EXPECTED_PIXELS) {
    uint8_t record[3] = {};
    if (file.read(record, sizeof(record)) != (int)sizeof(record) || record[0] == 0 ||
        emitted + record[0] > WEATHER_TEXT_EXPECTED_PIXELS) {
      ok = false;
      break;
    }
    // Match the byte order produced when the raw big-endian stream is read
    // directly into the little-endian uint16_t row buffer. pushImage() then
    // receives the same pre-swapped value used by the other image pipelines.
    const uint16_t pixel = (uint16_t)record[1] | ((uint16_t)record[2] << 8);
    for (uint16_t n = 0; n < record[0]; ++n) {
      if (strip >= 5) { ok = false; break; }
      rowBuf[column++] = pixel;
      emitted++;
      if (column == widths[strip]) {
        tft.pushImage(xs[strip], ys[strip] + row, widths[strip], 1, rowBuf);
        column = 0;
        if (++row == heights[strip]) { row = 0; strip++; }
        yield();
      }
    }
  }
  file.close();
  return ok && emitted == WEATHER_TEXT_EXPECTED_PIXELS && strip == 5 && column == 0;
}

unsigned long weatherTextRetryDelayMs() {
  if (weatherTextFailures <= 1) return 10000UL;
  if (weatherTextFailures == 2) return 30000UL;
  return 300000UL;
}

bool updateWeatherTextAsset() {
  const bool ok = weatherTextLegacyRaw ? drawWeatherChineseTextRaw()
                                       : downloadWeatherTextRLE();
  if (ok) {
    weatherTextFailures = 0;
    weatherTextRetryAtMs = 0;
    weatherTextPending = false;
    if (!weatherTextLegacyRaw) weatherTextDrawPending = true;
    return true;
  }
  if (weatherTextFailures < 255) weatherTextFailures++;
  weatherTextRetryAtMs = millis() + weatherTextRetryDelayMs();
  return false;
}

void drawWeatherScreen(bool force) {
  if (force || !weatherChromeDrawn) {
    weatherChromeDrawn = true;
    tft.fillScreen(TFT_BLACK);
    const uint16_t divider = tft.color565(41, 70, 95);
    const uint16_t footer = tft.color565(9, 19, 29);
    tft.drawFastHLine(0, 32, SCREEN_W, divider);
    tft.fillRect(0, 168, SCREEN_W, 72, footer);
    tft.drawFastHLine(0, 168, SCREEN_W, divider);
    tft.drawFastVLine(120, 168, 72, divider);
    weatherDirty = true;
  }
  drawWeatherClock();
  if (!weatherDirty) return;
  weatherDirty = false;
  tft.fillRect(0, 77, SCREEN_W, 90, TFT_BLACK);
  drawWeatherIcon(69, 119, 56, weather.currentCode);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  String current = weather.valid ? String((int)round(weather.currentTemp)) : "--";
  tft.drawString(current, 108, 92, 6);
  int tempWidth = tft.textWidth(current, 6);
  tft.drawCircle(112 + tempWidth, 98, 3, TFT_WHITE);
  tft.drawString("C", 119 + tempWidth, 105, 4);

  const uint16_t footer = tft.color565(9, 19, 29);
  const uint16_t warm = tft.color565(255, 180, 95);
  const uint16_t cool = tft.color565(119, 207, 255);
  tft.fillRect(1, 169, 118, 70, footer);
  tft.fillRect(121, 169, 119, 70, footer);
  for (int day = 0; day < 2; ++day) {
    const int x = day * 120;
    const int code = day == 0 ? weather.todayCode : weather.tomorrowCode;
    const float high = day == 0 ? weather.todayHigh : weather.tomorrowHigh;
    const float low = day == 0 ? weather.todayLow : weather.tomorrowLow;
    drawWeatherIcon(x + 27, 203, 31, code);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(warm, footer);
    tft.drawString(weather.valid ? String((int)round(high)) : "--", x + 49, 207, 2);
    tft.setTextColor(TFT_LIGHTGREY, footer);
    tft.drawString("/", x + 76, 207, 2);
    tft.setTextColor(cool, footer);
    tft.drawString(weather.valid ? String((int)round(low)) : "--", x + 87, 207, 2);
  }
}

// ---------- full-screen K-line frame ----------

bool readMarketExact(WiFiClient *stream, uint8_t *dst, size_t length,
                     unsigned long timeoutMs) {
  size_t received = 0;
  unsigned long deadline = millis() + timeoutMs;
  while (received < length) {
    int available = stream->available();
    if (available > 0) {
      size_t want = min((size_t)available, length - received);
      int got = stream->read(dst + received, want);
      if (got > 0) {
        received += (size_t)got;
        continue;
      }
    }
    if ((long)(millis() - deadline) >= 0) return false;
    delay(1);
    yield();
  }
  return true;
}

uint32_t marketCrc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
    }
  }
  return ~crc;
}

uint64_t marketReadU64BE(const uint8_t *data) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value = (value << 8) | data[i];
  return value;
}

uint32_t marketReadU32BE(const uint8_t *data) {
  return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
         ((uint32_t)data[2] << 8) | data[3];
}

// Validate every byte and the decoded pixel count before the TFT is touched.
// A short/corrupt/oversized download therefore leaves the prior page intact.
bool validateMarketFrame(const uint8_t *frame, size_t frameBytes,
                         uint64_t expectedVersion, uint64_t &versionOut) {
  if (frameBytes < MARKET_PACKED_HEADER_BYTES) return false;
  const bool palette4 = memcmp(frame, "MKT2", 4) == 0;
  if (!palette4 && memcmp(frame, "MKT1", 4) != 0) return false;
  const uint16_t width = ((uint16_t)frame[12] << 8) | frame[13];
  const uint16_t height = ((uint16_t)frame[14] << 8) | frame[15];
  if (width != SCREEN_W || height != SCREEN_H) return false;
  const uint64_t version = marketReadU64BE(frame + 4);
  if (version < expectedVersion || version <= lastMarketFrameVersion) return false;
  const uint8_t *payload = frame + MARKET_PACKED_HEADER_BYTES;
  const size_t payloadBytes = frameBytes - MARKET_PACKED_HEADER_BYTES;
  if (marketCrc32(payload, payloadBytes) != marketReadU32BE(frame + 16)) return false;

  size_t cursor = 0, pixels = 0;
  while (cursor < payloadBytes) {
    const uint8_t control = payload[cursor++];
    const size_t count = (control & 0x7F) + 1;
    const bool repeated = control & 0x80;
    const size_t encoded = palette4 ? (repeated ? 1 : (count + 1) / 2)
                                    : (repeated ? 2 : count * 2);
    if (cursor + encoded > payloadBytes || pixels + count > SCREEN_W * SCREEN_H) return false;
    if (palette4 && repeated && payload[cursor] >= 16) return false;
    cursor += encoded;
    pixels += count;
  }
  if (pixels != SCREEN_W * SCREEN_H) return false;
  versionOut = version;
  return true;
}

void drawMarketFrame(const uint8_t *frame, size_t frameBytes) {
  const uint8_t *payload = frame + MARKET_PACKED_HEADER_BYTES;
  const size_t payloadBytes = frameBytes - MARKET_PACKED_HEADER_BYTES;
  uint8_t *rowBytes = reinterpret_cast<uint8_t *>(rowBuf);
  size_t cursor = 0, rowPixels = 0;
  int y = 0;
  const bool palette4 = memcmp(frame, "MKT2", 4) == 0;
  tft.startWrite();
  while (cursor < payloadBytes) {
    const uint8_t control = payload[cursor++];
    size_t count = (control & 0x7F) + 1;
    const bool repeated = control & 0x80;
    uint8_t high = 0, low = 0, paletteIndex = 0, packedIndexes = 0;
    if (repeated) {
      if (palette4) paletteIndex = payload[cursor++];
      else { high = payload[cursor++]; low = payload[cursor++]; }
    }
    size_t item = 0;
    while (count-- > 0) {
      if (palette4) {
        if (!repeated && (item & 1U) == 0) packedIndexes = payload[cursor++];
        const uint8_t index = repeated ? paletteIndex
                                       : ((item & 1U) ? packedIndexes & 0x0F : packedIndexes >> 4);
        const uint16_t color = pgm_read_word(&MARKET_PALETTE_RGB565[index]);
        rowBytes[rowPixels * 2] = (uint8_t)(color >> 8);
        rowBytes[rowPixels * 2 + 1] = (uint8_t)color;
      } else {
        rowBytes[rowPixels * 2] = repeated ? high : payload[cursor++];
        rowBytes[rowPixels * 2 + 1] = repeated ? low : payload[cursor++];
      }
      item++;
      if (++rowPixels == SCREEN_W) {
        tft.pushImage(0, y++, SCREEN_W, 1, rowBuf);
        rowPixels = 0;
        yield();
      }
    }
  }
  tft.endWrite();
}

bool fetchMarketFrame(uint64_t expectedVersion, size_t advertisedBytes, bool palette4) {
  if (advertisedBytes < MARKET_PACKED_HEADER_BYTES ||
      advertisedBytes > MARKET_PACKED_MAX_BYTES) return false;
  // Leave enough contiguous heap for HTTP/TCP and the rest of the firmware.
  if (ESP.getMaxFreeBlockSize() < advertisedBytes + 6144) {
    Serial.printf("[market] insufficient heap frame=%u maxblock=%u\n",
                  (unsigned)advertisedBytes, ESP.getMaxFreeBlockSize());
    return false;
  }
  uint8_t *frame = (uint8_t *)malloc(advertisedBytes);
  if (!frame) return false;

  WiFiClient client;
  client.setTimeout(MARKET_READ_TIMEOUT_MS);
  HTTPClient http;
  String url = "http://" + bridgeHost + (palette4 ? "/market/frame.pal" : "/market/frame.rle");
  http.setTimeout(MARKET_HTTP_TIMEOUT_MS);
  bool ok = false;
  markRequestPhase(REQUEST_CONNECTING);
  if (http.begin(client, url)) {
    markRequestPhase(REQUEST_SENDING);
    const int code = http.GET();
    const int actualBytes = http.getSize();
    noteRequestResult(code, actualBytes);
    if (code == HTTP_CODE_OK && actualBytes == (int)advertisedBytes) {
      markRequestPhase(REQUEST_READING);
      WiFiClient *stream = http.getStreamPtr();
      if (readMarketExact(stream, frame, advertisedBytes, MARKET_TOTAL_TIMEOUT_MS)) {
        markRequestPhase(REQUEST_PARSING);
        uint64_t version = 0;
        if (validateMarketFrame(frame, advertisedBytes, expectedVersion, version)) {
          markRequestPhase(REQUEST_RENDERING);
          drawMarketFrame(frame, advertisedBytes);
          lastMarketFrameVersion = version;
          ok = true;
          Serial.printf("[market] applied version=%llu bytes=%u heap=%u\n",
                        (unsigned long long)version, (unsigned)advertisedBytes,
                        ESP.getFreeHeap());
        }
      }
    }
    closeTrackedHttp(http, client);
  } else { noteRequestResult(-1001); client.stop(); }
  free(frame);
  if (!ok) Serial.println("[market] frame rejected; keeping previous screen");
  return ok;
}

bool pollMarket() {
  if (!networkTrafficReady() || bridgeHost.length() == 0) return false;
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + "/market/version";
  http.setTimeout(MARKET_HTTP_TIMEOUT_MS);
  markRequestPhase(REQUEST_CONNECTING);
  if (!http.begin(client, url)) { noteRequestResult(-1001); client.stop(); return false; }
  markRequestPhase(REQUEST_SENDING);
  const int code = http.GET();
  noteRequestResult(code, http.getSize());
  if (code != HTTP_CODE_OK) { closeTrackedHttp(http, client); return false; }
  markRequestPhase(REQUEST_PARSING);
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, *http.getStreamPtr());
  closeTrackedHttp(http, client);
  if (err) return false;
  const char *session = doc["session"] | "";
  const int favoriteCount = doc["favorite_count"] | 0;
  const int refreshSeconds = doc["refresh_seconds"] | 0;
  if (favoriteCount > 0 && favoriteCount <= 15 &&
      (refreshSeconds == 5 || refreshSeconds == 10 || refreshSeconds == 30 ||
       refreshSeconds == 60 || refreshSeconds == 120)) {
    marketAutoDwellMs = (unsigned long)favoriteCount * (unsigned long)refreshSeconds * 1000UL;
    marketAutoDwellKnown = true;
  }
  if (session[0] != '\0' && strcmp(lastMarketFrameSession, session) != 0) {
    strlcpy(lastMarketFrameSession, session, sizeof(lastMarketFrameSession));
    lastMarketFrameVersion = 0;
  }
  const uint64_t version = doc["version"] | (uint64_t)0;
  const char *paletteCodec = doc["palette_codec"] | "";
  const size_t paletteBytes = doc["palette_bytes"] | (size_t)0;
  const char *legacyCodec = doc["codec"] | "";
  const size_t legacyBytes = doc["packed_bytes"] | (size_t)0;
  if (version > lastMarketFrameVersion) {
    if (strcmp(paletteCodec, "rgb565-palette4-rle-v1") == 0 && paletteBytes > 0) {
      pendingMarketFrameVersion = version;
      pendingMarketFrameBytes = paletteBytes;
      pendingMarketFramePalette4 = true;
    } else if (strcmp(legacyCodec, "rgb565-packbits-v1") == 0) {
      pendingMarketFrameVersion = version;
      pendingMarketFrameBytes = legacyBytes;
      pendingMarketFramePalette4 = false;
    }
  }
  return true;
}

// ---------- WiFi / bridge polling ----------

WiFiManager wifiManager; // global: the config portal now runs non-blocking in loop()

void configModeCallback(WiFiManager *wm) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("WiFi setup needed", 8, 32, 2);
  tft.drawString("Connect phone to AP:", 8, 62, 2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(WIFI_PORTAL_AP_NAME, 8, 87, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("then open 192.168.4.1", 8, 117, 2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("Or: plug into the computer", 8, 155, 2);
  tft.drawString("via USB - no WiFi needed", 8, 178, 2);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Firmware v" FW_VERSION, 8, 215, 2);
}

// Non-blocking: with saved credentials this still waits ~10s for the join,
// but a missing/failed WiFi no longer traps boot in the portal - the portal
// keeps running from loop() while the USB serial link can take over the
// screen (wired mode for APs with client isolation).
void setupWiFi() {
  wifiDisconnectedEventHandler = WiFi.onStationModeDisconnected(
      [](const WiFiEventStationModeDisconnected &event) {
        const unsigned long now = millis();
        pauseNetworkTrafficUntilGotIp();
        const bool recoveryGenerated = pendingDisconnectRecoveryStep != RECOVERY_NONE &&
            now - pendingDisconnectRecoveryAtMs <= 5000UL;
        wifiDisconnectCount++;
        lastWifiDisconnectReason = (uint8_t)event.reason;
        lastDisconnectWasRecovery = recoveryGenerated;
        const bool authTimeout = lastWifiDisconnectReason == 15 ||
                                 lastWifiDisconnectReason == 16 ||
                                 lastWifiDisconnectReason == 204;
        if (authTimeout && !recoveryGenerated) {
          wifiAuthRecoveryPending = true;
          wifiAuthRecoveryDone = false;
          wifiAuthRecoveryAtMs = now + 1500UL;
        }
        if (wifiEverGotIp && !wifiOutageActive) {
          wifiOutageActive = true;
          firstWifiDisconnectReason = lastWifiDisconnectReason;
          firstWifiDisconnectRssi = lastConnectedWifiRssi;
          firstWifiDisconnectChannel = lastConnectedWifiChannel;
          memcpy(firstWifiDisconnectBssid, lastConnectedWifiBssid,
                 sizeof(firstWifiDisconnectBssid));
          firstDisconnectWasRecovery = recoveryGenerated;
          if (!recoveryGenerated) {
            wifiRecoveryMask = 0;
            quickRecoveryAtMs = wifiReinitAtMs = radioResetAtMs = protectiveRestartAtMs = 0;
            authRejoinAtMs = 0;
          }
          lastWifiOutageDurationMs = 0;
        }
        if (wifiDisconnectedSinceMs == 0) wifiDisconnectedSinceMs = max(now, 1UL);
        syncWifiEvidenceToRtc();
        rtcRuntimeDiag.wifiDisconnectCount = wifiDisconnectCount;
        rtcRuntimeDiag.wifiStatus = WL_DISCONNECTED;
        writeRtcRuntimeDiag();
        Serial.printf("[wifi] disconnected reason=%u source=%s count=%lu\n",
                      (unsigned)lastWifiDisconnectReason,
                      recoveryGenerated ? "recovery" : "external/stack",
                      (unsigned long)wifiDisconnectCount);
      });
  wifiGotIpEventHandler = WiFi.onStationModeGotIP(
      [](const WiFiEventStationModeGotIP &event) {
        if (wifiEverGotIp) wifiReconnectCount++;
        wifiEverGotIp = true;
        if (wifiOutageActive && wifiDisconnectedSinceMs != 0) {
          lastWifiOutageDurationMs = millis() - wifiDisconnectedSinceMs;
        }
        wifiOutageActive = false;
        pendingDisconnectRecoveryStep = RECOVERY_NONE;
        pendingDisconnectRecoveryAtMs = 0;
        startNetworkTrafficCooldown();
        wifiDisconnectedSinceMs = 0;
        wifiSoftRecoveryDone = false;
        wifiHardRecoveryDone = false;
        wifiRadioRecoveryDone = false;
        wifiAuthRecoveryPending = false;
        wifiAuthRecoveryDone = false;
        wifiAuthRecoveryAtMs = 0;
        rtcRuntimeDiag.recoveryStep = RECOVERY_NONE;
        syncWifiEvidenceToRtc();
        rtcRuntimeDiag.wifiReconnectCount = wifiReconnectCount;
        rtcRuntimeDiag.wifiStatus = WL_CONNECTED;
        writeRtcRuntimeDiag();
        lastPollMs = 0;
        webServerNeedsRestart = webServerStarted;
        Serial.printf("[wifi] got ip=%s reconnects=%lu; HTTP resumes in %lus\n",
                      event.ip.toString().c_str(), (unsigned long)wifiReconnectCount,
                      WIFI_TRAFFIC_COOLDOWN_MS / 1000UL);
      });
  WiFi.setAutoReconnect(true);
  // This clock is continuously powered. Disabling modem sleep avoids a class
  // of long-idle association/ARP failures seen with some routers and costs
  // only additional power, not display functionality.
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  wifiManager.setAPCallback(configModeCallback);
  wifiManager.setConfigPortalBlocking(false);

  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Connecting WiFi...", 8, 100, 2);

  Serial.println("[wifi] starting WiFiManager autoConnect (non-blocking portal)...");
  bool ok = wifiManager.autoConnect(WIFI_PORTAL_AP_NAME);
  WiFi.setAutoReconnect(true);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  if (WiFi.status() == WL_CONNECTED && networkTrafficResumeAtMs == 0) {
    startNetworkTrafficCooldown();
  }
  Serial.printf("[wifi] autoConnect result=%d ssid=%s ip=%s\n", ok, WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str());
  Serial.printf("[wifi] bridge host = '%s'\n", bridgeHost.c_str());
}

bool applyStatusJson(JsonDocument &doc) {
  strlcpy(bridgeVersion, doc["bridge_version"] | "--", sizeof(bridgeVersion));

  JsonObject c = doc["claude"];
  if (!c.isNull()) {
    strlcpy(claudeStatus.status, c["status"] | "unknown", sizeof(claudeStatus.status));
    claudeStatus.tokensToday = c["tokens_today"] | 0;
    claudeStatus.sessionMin = c["session_min"] | 0;
    claudeStatus.sessionWindowMin = c["session_window_min"] | 300;
    claudeStatus.fiveHourPct = c["five_hour_pct"] | -1.0;
    claudeStatus.fiveHourResetMin = c["five_hour_reset_min"] | -1;
    claudeStatus.sevenDayPct = c["seven_day_pct"] | -1.0;
    claudeStatus.sevenDayResetMin = c["seven_day_reset_min"] | -1;
    claudeStatus.needsInput = c["needs_input"] | false;
  }

  JsonObject x = doc["codex"];
  if (!x.isNull()) {
    strlcpy(codexStatus.status, x["status"] | "unknown", sizeof(codexStatus.status));
    codexStatus.tokensToday = x["tokens_today"] | 0;
    codexStatus.primaryPct = x["primary_pct"] | -1.0;
    codexStatus.primaryResetMin = x["primary_reset_min"] | -1;
    codexStatus.weeklyPct = x["weekly_pct"] | -1.0;
    codexStatus.weeklyResetMin = x["weekly_reset_min"] | -1;
    codexStatus.weeklyResetAt = x["weekly_reset_at"] | (int64_t)-1;
    codexStatus.weeklyResetUtcOffsetSec = x["weekly_reset_utc_offset_sec"] | 0;
    codexStatus.needsInput = x["needs_input"] | false;
  }
  statusMusicPlaying = doc["music_playing"] | false;
  return true;
}

bool parseStatusJson(const String &payload) {
  JsonDocument doc;
  return !deserializeJson(doc, payload) && applyStatusJson(doc);
}

// The mode actually rendered. AUTO selects exactly one configured carousel
// page; the fixed-page modes keep their original behavior.
DisplayMode effectiveMode() {
  if (displayMode == MODE_AUTO) {
    const uint8_t count = selectedAutoPageCount();
    if (count == 0) return MODE_CODEX;
    if (autoPageIndex >= count) autoPageIndex = 0;
    return autoPageAt(autoPageIndex);
  }
  return displayMode;
}

bool selectEffectivePet(DisplayMode eff) {
  if (eff == MODE_CLAUDE) {
    const bool changed = currentApp != APP_CLAUDE;
    currentApp = APP_CLAUDE;
    return changed;
  }
  if (eff == MODE_CODEX) {
    const bool changed = currentApp != APP_CODEX;
    currentApp = APP_CODEX;
    return changed;
  }
  return updateActiveApp();
}

unsigned long bridgeRetryIntervalMs(DisplayMode eff = MODE_AUTO) {
  const unsigned long normalInterval = (eff == MODE_STOCK || eff == MODE_MARKET)
      ? BRIDGE_DATA_PAGE_POLL_INTERVAL_MS : BRIDGE_POLL_INTERVAL_MS;
  if (bridgeConsecutiveFailures < 3) return normalInterval;
  if (bridgeConsecutiveFailures < 6) return 10000UL;
  if (bridgeConsecutiveFailures < 11) return 30000UL;
  return 60000UL;
}

bool pollBridgeHealth() {
  if (!networkTrafficReady() || bridgeHost.length() == 0) return false;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  markRequestPhase(REQUEST_CONNECTING);
  if (!http.begin(client, "http://" + bridgeHost + "/health")) {
    noteRequestResult(-1001); client.stop(); lastBridgeHealthHttpCode = -1001; return false;
  }
  markRequestPhase(REQUEST_SENDING);
  const int code = http.GET();
  lastBridgeHealthHttpCode = code;
  noteRequestResult(code, http.getSize());
  bool ok = false;
  if (code == HTTP_CODE_OK) {
    markRequestPhase(REQUEST_PARSING);
    JsonDocument doc;
    ok = !deserializeJson(doc, *http.getStreamPtr()) && (doc["ok"] | false);
  }
  closeTrackedHttp(http, client);
  lastBridgeHealthHealthy = ok;
  return ok;
}

bool pollBridge() {
  if (!networkTrafficReady() || bridgeHost.length() == 0) {
    Serial.printf("[bridge] skip poll: wifi=%d host='%s'\n", WiFi.status() == WL_CONNECTED, bridgeHost.c_str());
    return false;
  }

  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + BRIDGE_DEFAULT_PATH;
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);

  markRequestPhase(REQUEST_CONNECTING);
  if (!http.begin(client, url)) {
    Serial.println("[bridge] http.begin() failed");
    recordBridgeFailure(-1001);
    noteRequestResult(-1001);
    client.stop();
    return false;
  }
  markRequestPhase(REQUEST_SENDING);
  int code = http.GET();
  noteRequestResult(code, http.getSize());
  lastBridgeHttpCode = code;
  bool ok = false;
  Serial.printf("[bridge] GET %s -> %d\n", url.c_str(), code);
  if (code == HTTP_CODE_OK) {
    markRequestPhase(REQUEST_PARSING);
    JsonDocument doc;
    if (!deserializeJson(doc, *http.getStreamPtr()) && applyStatusJson(doc)) {
      lastSuccessMs = millis();
      everPolled = true;
      recordBridgeSuccess();
      ok = true;
      Serial.printf("[bridge] claude=%s tok=%ld | codex=%s tok=%ld primary=%.0f%%\n",
                    claudeStatus.status, claudeStatus.tokensToday,
                    codexStatus.status, codexStatus.tokensToday, codexStatus.primaryPct);
    } else {
      Serial.println("[bridge] JSON parse failed");
      recordBridgeFailure(-1002);
    }
  } else {
    recordBridgeFailure(code);
    strlcpy(claudeStatus.status, "offline", sizeof(claudeStatus.status));
    strlcpy(codexStatus.status, "offline", sizeof(codexStatus.status));
  }
  closeTrackedHttp(http, client);
  DisplayMode eff = effectiveMode();
  if (eff != MODE_NET && eff != MODE_MUSIC && eff != MODE_STOCK &&
      eff != MODE_MARKET && eff != MODE_WEATHER) {
    // Only a real app switch clears the screen; a plain data refresh paints
    // in place so the poll doesn't flash the whole display.
    if (selectEffectivePet(eff)) drawActiveApp();
    else refreshActiveApp();
  }
  return ok;
}

bool sendBootDiagnosticReport() {
  if (!networkTrafficReady() || bridgeHost.length() == 0) return false;
  JsonDocument doc;
  doc["firmware"] = FW_VERSION;
  doc["core_version"] = ESP.getCoreVersion();
  doc["build_variant"] = FW_CORE_AB_LABEL;
  doc["boot_count"] = bootCount;
  doc["reset_reason"] = lastResetReason;
  doc["reset_info"] = lastResetInfo;
  doc["previous_stage"] = runtimeStageName(previousRuntimeStage);
  doc["previous_mode"] = runtimeModeName(previousRuntimeMode);
  doc["previous_network_task"] = networkTaskName(previousNetworkTask);
  doc["previous_request_phase"] = requestPhaseName(previousRequestPhase);
  doc["previous_request_code"] = previousRequestCode;
  doc["previous_request_duration_ms"] = previousRequestDurationMs;
  doc["previous_request_bytes"] = previousRequestBytes;
  doc["previous_request_free_heap"] = previousRequestFreeHeap;
  doc["previous_request_max_block"] = previousRequestMaxBlock;
  doc["previous_first_disconnect_reason"] = previousFirstWifiDisconnectReason;
  doc["previous_last_disconnect_reason"] = previousLastWifiDisconnectReason;
  doc["previous_outage_ms"] = previousOutageDurationMs;
  doc["device_ip"] = WiFi.localIP().toString();
  doc["rssi"] = WiFi.RSSI();
  String body;
  serializeJson(doc, body);

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  markRequestPhase(REQUEST_CONNECTING);
  if (!http.begin(client, "http://" + bridgeHost + "/device/boot-report")) {
    noteRequestResult(-1001); client.stop(); return false;
  }
  http.addHeader("Content-Type", "application/json");
  markRequestPhase(REQUEST_SENDING);
  const int code = http.POST((uint8_t *)body.c_str(), body.length());
  noteRequestResult(code, http.getSize());
  const bool ok = code == HTTP_CODE_OK;
  closeTrackedHttp(http, client);
  return ok;
}

// ---------- wired (USB serial) bridge link ----------
// Fallback for WiFi networks with client isolation (device can't reach the
// bridge over LAN) - or for skipping WiFi setup entirely: when the clock is
// plugged into the computer over USB, the bridge pushes the same /status and
// /net payloads down the CH340 serial line as newline-terminated frames:
//   bridge -> device:  #HELLO   #STATUS {json}   #NET {json}   #CMD {json}
//   device -> bridge:  #DEVICE {"name":"aiclock","fw":"x.y.z"}
// Everything else the device prints (logs) is ignored by the bridge.
unsigned long lastSerialFrameMs = 0;
bool wiredEverLinked = false;
char serialLine[1600]; // biggest frame is #STATUS at ~600 bytes
size_t serialLineLen = 0;

bool wiredActive() { return wiredEverLinked && (millis() - lastSerialFrameMs) < 15000UL; }

// Runs at most one outbound HTTP transaction per Arduino loop iteration. The
// old implementation could perform status + weather + page metadata + a large
// binary asset back-to-back after one slow request returned, which starved the
// ESP8266 SDK and correlated with Software Watchdog resets. Metadata and binary
// assets are now separate queue entries and every transaction gets a short
// scheduler gap plus a persistent request breadcrumb.
bool runOneNetworkTask(unsigned long nowMs, DisplayMode eff) {
  if (wiredActive() || !networkTrafficReady() || bridgeHost.length() == 0) return false;
  if (activeNetworkTask != NET_TASK_NONE) return false;
  if (nextNetworkTaskAtMs != 0 && (long)(nowMs - nextNetworkTaskAtMs) < 0) return false;

  NetworkTask task = NET_TASK_NONE;
  if (eff == MODE_MARKET && pendingMarketFrameVersion > lastMarketFrameVersion &&
      nowMs - lastMarketFrameAttemptMs >= 5000UL) {
    task = NET_TASK_MARKET_FRAME;
  } else if (eff == MODE_WEATHER &&
             (lastWeatherPollMs == 0 ||
              nowMs - lastWeatherPollMs >= WEATHER_POLL_INTERVAL_MS)) {
    // Metadata owns text_rev, so it must run before deciding whether a new
    // compressed text asset is needed.
    task = NET_TASK_WEATHER_META;
  } else if (eff == MODE_MUSIC && musicCoverPending && musicHasArtwork &&
             nowMs - lastMusicCoverAttemptMs >= 5000UL) {
    task = NET_TASK_MUSIC_COVER;
  } else if (eff == MODE_MUSIC && musicTextPending &&
             nowMs - lastMusicTextAttemptMs >= 5000UL) {
    task = NET_TASK_MUSIC_TEXT;
  } else if (eff == MODE_STOCK && stockNamesPending &&
             (stockNamesRetryAtMs == 0 ||
              (long)(nowMs - stockNamesRetryAtMs) >= 0)) {
    task = NET_TASK_STOCK_NAMES;
  } else if (eff == MODE_WEATHER && weatherTextPending &&
             (weatherTextRetryAtMs == 0 ||
              (long)(nowMs - weatherTextRetryAtMs) >= 0)) {
    task = NET_TASK_WEATHER_TEXT;
  } else if (eff == MODE_NET && nowMs - lastNetPollMs >= NET_POLL_INTERVAL_MS) {
    task = NET_TASK_NET;
  } else if (eff == MODE_MUSIC && nowMs - lastMusicPollMs >= MUSIC_POLL_INTERVAL_MS) {
    task = NET_TASK_MUSIC_META;
  } else if (eff == MODE_STOCK && nowMs - lastStockPollMs >= STOCK_POLL_INTERVAL_MS) {
    task = NET_TASK_STOCK_META;
  } else if (eff == MODE_MARKET && nowMs - lastMarketPollMs >= MARKET_POLL_INTERVAL_MS) {
    task = NET_TASK_MARKET_META;
  } else if (bridgeConsecutiveFailures >= 3 &&
             nowMs - lastBridgeHealthPollMs >= 30000UL) {
    task = NET_TASK_HEALTH;
  } else if (everPolled && bootReportPending &&
             (lastBootReportAttemptMs == 0 || nowMs - lastBootReportAttemptMs >= 60000UL)) {
    task = NET_TASK_BOOT_REPORT;
  } else if (nowMs - lastPollMs >= bridgeRetryIntervalMs(eff)) {
    task = NET_TASK_BRIDGE;
  } else if (lastWeatherPollMs == 0 ||
             nowMs - lastWeatherPollMs >= WEATHER_POLL_INTERVAL_MS) {
    task = NET_TASK_WEATHER_META;
  }
  if (task == NET_TASK_NONE) return false;

  beginNetworkRequest(task);
  bool ok = false;
  switch (task) {
    case NET_TASK_BRIDGE:
      lastPollMs = nowMs;
      ok = pollBridge();
      break;
    case NET_TASK_HEALTH:
      lastBridgeHealthPollMs = nowMs;
      ok = pollBridgeHealth();
      if (ok) lastPollMs = 0; // server is alive: retry the real status promptly
      break;
    case NET_TASK_BOOT_REPORT:
      lastBootReportAttemptMs = nowMs;
      ok = sendBootDiagnosticReport();
      if (ok) bootReportPending = false;
      break;
    case NET_TASK_NET:
      lastNetPollMs = nowMs;
      ok = pollNet();
      break;
    case NET_TASK_MUSIC_META:
      lastMusicPollMs = nowMs;
      ok = pollMusic();
      break;
    case NET_TASK_MUSIC_COVER:
      lastMusicCoverAttemptMs = nowMs;
      ok = drawMusicCoverFromBridge();
      if (ok) musicCoverPending = false;
      break;
    case NET_TASK_MUSIC_TEXT:
      lastMusicTextAttemptMs = nowMs;
      ok = drawMusicTextFromBridge();
      if (ok) musicTextPending = false;
      break;
    case NET_TASK_STOCK_META:
      lastStockPollMs = nowMs;
      ok = pollStock();
      break;
    case NET_TASK_STOCK_NAMES:
      lastStockNamesAttemptMs = nowMs;
      ok = updateStockNamesAsset();
      break;
    case NET_TASK_WEATHER_META:
      lastWeatherPollMs = nowMs;
      ok = pollWeather();
      break;
    case NET_TASK_WEATHER_TEXT:
      lastWeatherTextAttemptMs = nowMs;
      ok = updateWeatherTextAsset();
      break;
    case NET_TASK_MARKET_META:
      lastMarketPollMs = nowMs;
      ok = pollMarket();
      break;
    case NET_TASK_MARKET_FRAME:
      lastMarketFrameAttemptMs = nowMs;
      ok = fetchMarketFrame(pendingMarketFrameVersion, pendingMarketFrameBytes,
                            pendingMarketFramePalette4);
      if (ok) {
        pendingMarketFrameVersion = 0;
        pendingMarketFrameBytes = 0;
      }
      break;
    default:
      break;
  }
  finishNetworkRequest(ok);
  return true;
}

// First data over either transport replaces the boot/portal screen.
void showMainUiIfNeeded() {
  if (mainUiShown) return;
  mainUiShown = true;
  if (displayMode == MODE_AUTO) resetAutoCyclePosition();
  drawStaticChrome();
  updateActiveApp();
  drawActiveApp();
}

void handleSerialFrame(char *line) {
  lastSerialFrameMs = millis();
  wiredEverLinked = true;
  if (!strncmp(line, "#HELLO", 6)) {
    Serial.printf("#DEVICE {\"name\":\"aiclock\",\"fw\":\"%s\"}\n", FW_VERSION);
    return;
  }
  if (!strncmp(line, "#STATUS ", 8)) {
    if (parseStatusJson(String(line + 8))) {
      lastSuccessMs = millis();
      everPolled = true;
      showMainUiIfNeeded();
      DisplayMode eff = effectiveMode();
      if (eff != MODE_NET && eff != MODE_MUSIC && eff != MODE_STOCK &&
          eff != MODE_MARKET && eff != MODE_WEATHER) {
        if (selectEffectivePet(eff)) drawActiveApp();
        else refreshActiveApp();
      }
    }
    return;
  }
  if (!strncmp(line, "#NET ", 5)) {
    handleNetPayload(String(line + 5));
    return;
  }
  if (!strncmp(line, "#STOCK ", 7)) {
    handleStockPayload(String(line + 7));
    return;
  }
  if (!strncmp(line, "#WEATHER ", 9)) {
    handleWeatherPayload(String(line + 9));
    return;
  }
  if (!strncmp(line, "#CMD ", 5)) {
    JsonDocument doc;
    if (deserializeJson(doc, line + 5)) return;
    if (doc["brightness"].is<int>()) {
      brightness = constrain(doc["brightness"].as<int>(), 0, 100);
      applyBrightness();
      saveBrightness();
    }
    const char *mode = doc["display"] | (const char *)nullptr;
    if (mode) {
      String m(mode);
      if (m == "auto") { displayMode = MODE_AUTO; resetAutoCyclePosition(); lastEffectiveMode = MODE_AUTO; }
      else if (m == "claude") displayMode = MODE_CLAUDE;
      else if (m == "codex") displayMode = MODE_CODEX;
      else if (m == "net") displayMode = MODE_NET;
      else if (m == "music") displayMode = MODE_MUSIC;
      else if (m == "stock") displayMode = MODE_STOCK;
      else if (m == "market") displayMode = MODE_MARKET;
      else if (m == "weather") displayMode = MODE_WEATHER;
      // the effectiveMode transition handler in loop() repaints the chrome
    }
    return;
  }
}

// Drains the UART, splitting on newlines; frames start with '#', everything
// else (line noise, echoes) is dropped.
void pumpSerial() {
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (serialLineLen > 0 && serialLine[0] == '#') {
        serialLine[serialLineLen] = 0;
        handleSerialFrame(serialLine);
      }
      serialLineLen = 0;
    } else if (serialLineLen < sizeof(serialLine) - 1) {
      serialLine[serialLineLen++] = ch;
    } else {
      serialLineLen = 0; // oversized line: drop it
    }
  }
}

// ---------- web admin ----------

String htmlEscape(const String &s) {
  String out = s;
  out.replace("&", "&amp;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  out.replace("\"", "&quot;");
  return out;
}

void handleRoot() {
  saveRuntimeStage(STAGE_WEB);
  // Chunk the page directly from flash instead of building one multi-KB
  // String on the ESP8266 heap.
  webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webServer.send(200, "text/html; charset=utf-8", "");
  webServer.sendContent_P(PSTR(
      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>AI Clock 设置</title><style>"
      "body{font-family:-apple-system,sans-serif;max-width:480px;margin:24px auto;padding:0 16px;color:#222}"
      "h1{font-size:20px}label{display:block;margin-top:16px;font-weight:600}"
      "input{width:100%;box-sizing:border-box;padding:8px;font-size:16px;margin-top:4px}"
      "button{margin-top:16px;padding:10px 20px;font-size:16px;background:#2563eb;color:#fff;border:0;border-radius:6px}"
      "table{margin-top:20px;border-collapse:collapse;width:100%}"
      "td{padding:4px 8px;border-bottom:1px solid #eee;font-size:14px}"
      "</style></head><body><h1>AI Clock 设置</h1>"
      "<form method='POST' action='/save'><label>Bridge host (ip:port)</label>"
      "<input name='bridge' value='"));
  webServer.sendContent(htmlEscape(bridgeHost));
  webServer.sendContent_P(PSTR("' placeholder='192.168.1.181:8765'><button type='submit'>保存</button></form>"
      "<h2 style='font-size:16px;margin-top:28px'>屏幕亮度</h2>"
      "<input type='range' min='0' max='100' value='"));
  webServer.sendContent(String(brightness));
  webServer.sendContent_P(PSTR("' id='bri' oninput=\"document.getElementById('briv').textContent=this.value+'%'\" "
      "onchange=\"fetch('/api/brightness',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'level='+this.value})\">"
      "<div style='font-size:13px;color:#555'>当前：<span id='briv'>"));
  webServer.sendContent(String(brightness));
  webServer.sendContent_P(PSTR("%</span>（0 = 熄屏，设置立即生效并记住）</div>"
      "<h2 style='font-size:16px;margin-top:28px'>桌宠动画（上传 GIF）</h2>"
      "<p style='font-size:13px;color:#555'>上传一个 .gif，设备会在板上解码并缩放到对应角色的尺寸，"
      "立刻替换动画，无需重新编译或烧录。GIF 太大可能因内存不足解码失败，换小一点的即可。</p>"
      "<form id='gifForm' method='POST' enctype='multipart/form-data' onsubmit='return setGifAction()'>"
      "<label>角色</label><select id='gifTarget'><option value='claude'>Claude</option>"
      "<option value='codex'>Codex</option></select><label>GIF 文件</label>"
      "<input type='file' name='file' accept='.gif' required><button type='submit'>上传并应用</button></form>"
      "<script>function setGifAction(){document.getElementById('gifForm').action='/sprite/'+"
      "document.getElementById('gifTarget').value;return true}</script>"
      "<h2 style='font-size:16px;margin-top:28px'>设备诊断</h2><table>"
      "<tr><td>Wi-Fi 名称</td><td>"));
  webServer.sendContent(htmlEscape(WiFi.SSID()));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>Wi-Fi 信号</td><td>"));
  webServer.sendContent(String(WiFi.RSSI()) + " dBm");
  webServer.sendContent_P(PSTR("</td></tr><tr><td>设备 IP</td><td>"));
  webServer.sendContent(WiFi.localIP().toString());
  webServer.sendContent_P(PSTR("</td></tr><tr><td>桥接状态</td><td>"));
  webServer.sendContent(wiredActive() ? "在线（USB）" :
      (everPolled && millis() - lastSuccessMs < 30000UL ? "在线（局域网）" : "离线"));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>上次数据更新</td><td>"));
  webServer.sendContent(everPolled ? String((millis() - lastSuccessMs) / 1000) + " 秒前" : "从未");
  webServer.sendContent_P(PSTR("</td></tr><tr><td>固件版本</td><td>" FW_VERSION
      "</td></tr><tr><td>核心版本</td><td>" FW_CORE_AB_LABEL
      "</td></tr><tr><td>桥接版本</td><td>"));
  webServer.sendContent(htmlEscape(bridgeVersion));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>本次启动原因</td><td>"));
  webServer.sendContent(htmlEscape(lastResetReason));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>累计启动次数</td><td>"));
  webServer.sendContent(String(bootCount));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>重启前运行位置</td><td>"));
  webServer.sendContent(String(runtimeStageName(previousRuntimeStage)) + " / " +
                        runtimeModeName(previousRuntimeMode));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>重启前恢复步骤</td><td>"));
  webServer.sendContent(recoveryStepName(previousRecoveryStep));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>重启前首次断线</td><td>"));
  webServer.sendContent(disconnectEventText(previousFirstWifiDisconnectReason,
                                            previousFirstDisconnectWasRecovery));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>重启前最后断线</td><td>"));
  webServer.sendContent(disconnectEventText(previousLastWifiDisconnectReason,
                                            previousLastDisconnectWasRecovery));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>首次断线前无线环境</td><td>"));
  webServer.sendContent(previousFirstWifiDisconnectReason == 0 ? "无记录" :
                        String(previousFirstWifiRssi) + " dBm / 信道 " +
                        String(previousFirstWifiChannel) + " / " +
                        wifiBssidText(previousFirstWifiBssid));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>重启前断线时长</td><td>"));
  webServer.sendContent(String(previousOutageDurationMs / 1000UL) + " 秒");
  webServer.sendContent_P(PSTR("</td></tr><tr><td>重启前恢复时间线</td><td>"));
  webServer.sendContent(recoveryTimelineText(previousQuickRecoveryAtMs,
                        previousWifiReinitAtMs, previousRadioResetAtMs,
                        previousProtectiveRestartAtMs, previousAuthRejoinAtMs));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>重启前断线 / 重连</td><td>"));
  webServer.sendContent(String(previousWifiDisconnectCount) + " / " +
                        String(previousWifiReconnectCount));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>重启前桥接失败 / HTTP</td><td>"));
  webServer.sendContent(String(previousBridgeFailures) + " / " +
                        String(previousBridgeHttpCode));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>重启前网络任务</td><td>"));
  webServer.sendContent(String(networkTaskName(previousNetworkTask)) + " / " +
                        requestPhaseName(previousRequestPhase));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>重启前请求结果</td><td>"));
  webServer.sendContent(String(previousRequestCode) + " / " +
                        String(previousRequestDurationMs) + " ms / " +
                        String(previousRequestBytes) + " B");
  webServer.sendContent_P(PSTR("</td></tr><tr><td>重启前请求内存</td><td>"));
  webServer.sendContent(String(previousRequestFreeHeap / 1024UL) + " KB / 最大块 " +
                        String(previousRequestMaxBlock / 1024UL) + " KB");
  webServer.sendContent_P(PSTR("</td></tr><tr><td>Wi-Fi 内部状态</td><td>"));
  webServer.sendContent(String(wifiStatusName((uint8_t)WiFi.status())) + " (" +
                        String((int)WiFi.status()) + ")");
  webServer.sendContent_P(PSTR("</td></tr><tr><td>本次首次 / 最后断线</td><td>"));
  webServer.sendContent(disconnectEventText(firstWifiDisconnectReason,
                        firstDisconnectWasRecovery) + " / " +
                        disconnectEventText(lastWifiDisconnectReason,
                        lastDisconnectWasRecovery));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>首次断线前无线环境</td><td>"));
  webServer.sendContent(firstWifiDisconnectReason == 0 ? "无记录" :
                        String(firstWifiDisconnectRssi) + " dBm / 信道 " +
                        String(firstWifiDisconnectChannel) + " / " +
                        wifiBssidText(firstWifiDisconnectBssid));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>最近断线时长</td><td>"));
  webServer.sendContent(String(currentWifiOutageDurationMs() / 1000UL) + " 秒");
  webServer.sendContent_P(PSTR("</td></tr><tr><td>本次恢复时间线</td><td>"));
  webServer.sendContent(recoveryTimelineText(quickRecoveryAtMs, wifiReinitAtMs,
                        radioResetAtMs, protectiveRestartAtMs, authRejoinAtMs));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>断线 / 重连次数</td><td>"));
  webServer.sendContent(String(wifiDisconnectCount) + " / " + String(wifiReconnectCount));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>桥接连续 / 累计失败</td><td>"));
  webServer.sendContent(String(bridgeConsecutiveFailures) + " / " + String(bridgeTotalFailures));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>最近桥接 HTTP 结果</td><td>"));
  webServer.sendContent(String(lastBridgeHttpCode));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>桥接健康检查</td><td>"));
  webServer.sendContent(String(lastBridgeHealthHealthy ? "正常" : "未知/失败") +
                        " / HTTP " + String(lastBridgeHealthHttpCode));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>网络请求成功 / 失败</td><td>"));
  webServer.sendContent(String(networkRequestCount - networkRequestFailures) + " / " +
                        String(networkRequestFailures));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>最近网络任务 / 阶段</td><td>"));
  webServer.sendContent(String(networkTaskName(rtcRuntimeDiag.networkTask)) + " / " +
                        requestPhaseName(rtcRuntimeDiag.requestPhase));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>最近请求结果</td><td>"));
  webServer.sendContent(String(rtcRuntimeDiag.requestCode) + " / " +
                        String(rtcRuntimeDiag.requestDurationMs) + " ms / " +
                        String(rtcRuntimeDiag.requestBytes) + " B");
  webServer.sendContent_P(PSTR("</td></tr><tr><td>最近恢复动作</td><td>"));
  webServer.sendContent(htmlEscape(lastRecoveryAction));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>四行报价名称缓存</td><td>"));
  webServer.sendContent(String(stockNamesCacheValid ? "有效" : "无") +
                        " / 本地 " + String(stockNamesCachedRev) +
                        " / 桥接 " + String(stockNamesRev) +
                        " / 行数 " + String(stockNamesCacheCount) +
                        " / 失败 " + String(stockNamesFailures));
  webServer.sendContent_P(PSTR("</td></tr><tr><td>天气文字缓存</td><td>"));
  webServer.sendContent(String(weatherTextCacheValid ? "有效" : "无") +
                        " / 本地 " + String(weatherTextCachedRev) +
                        " / 桥接 " + String(weatherTextRev) +
                        " / 失败 " + String(weatherTextFailures));
  for (uint8_t i = 0; i < restartHistory.count; ++i) {
    const RestartHistoryEntry &entry = restartHistory.entries[i];
    webServer.sendContent_P(PSTR("</td></tr><tr><td>历史重启 #"));
    webServer.sendContent(String(entry.bootCount));
    webServer.sendContent_P(PSTR("</td><td>"));
    webServer.sendContent(String(entry.resetReason) + " · " + runtimeStageName(entry.stage) +
                          "/" + runtimeModeName(entry.mode) + " · 首次原因 " +
                          String((unsigned)entry.firstReason) + "（" +
                          disconnectOriginName(entry.firstWasRecovery != 0) + "）· " +
                          String(entry.outageDurationMs / 1000UL) + "秒 · " +
                          networkTaskName(entry.networkTask) + "/" +
                          requestPhaseName(entry.requestPhase) + " HTTP " +
                          String(entry.requestCode));
  }
  webServer.sendContent_P(PSTR(
      "</td></tr><tr><td>空闲可用内存</td><td id='heap'>--</td></tr>"
      "<tr><td>最低空闲内存</td><td id='heapMin'>--</td></tr>"
      "<tr><td>最大连续内存块</td><td id='heapBlock'>--</td></tr>"
      "<tr><td>内存碎片率</td><td id='heapFrag'>--</td></tr>"));
  FSInfo webFsInfo;
  if (LittleFS.info(webFsInfo)) {
    webServer.sendContent_P(PSTR("<tr><td>文件存储</td><td>"));
    webServer.sendContent(String(webFsInfo.usedBytes / 1024UL) + " / " +
                          String(webFsInfo.totalBytes / 1024UL) + " KB");
    webServer.sendContent_P(PSTR("</td></tr>"));
  }
  const unsigned long upMinutes = millis() / 60000UL;
  webServer.sendContent_P(PSTR("<tr><td>运行时间</td><td>"));
  webServer.sendContent(String(upMinutes / 1440UL) + " 天 " +
      String((upMinutes / 60UL) % 24UL) + " 小时 " + String(upMinutes % 60UL) + " 分");
  webServer.sendContent_P(PSTR("</td></tr><tr><td>OTA 可用空间</td><td>"));
  webServer.sendContent(String(ESP.getFreeSketchSpace() / 1024UL) + " KB");
  webServer.sendContent_P(PSTR("</td></tr><tr><td>Claude</td><td>"));
  webServer.sendContent(htmlEscape(claudeStatus.status) + ", " +
                        formatTokens(claudeStatus.tokensToday) + " tok");
  webServer.sendContent_P(PSTR("</td></tr><tr><td>Codex</td><td>"));
  webServer.sendContent(htmlEscape(codexStatus.status) + ", " +
      formatTokens(codexStatus.tokensToday) + " tok, " +
      (codexStatus.primaryPct >= 0 ? "5h " + String(codexStatus.primaryPct, 0) + "%" :
       codexStatus.weeklyPct >= 0 ? "Wk " + String(codexStatus.weeklyPct, 0) + "%" : "5h ?"));
  webServer.sendContent_P(PSTR(
      "</td></tr></table>"
      "<form method='POST' action='/reconnect' onsubmit=\"return confirm('立即重新初始化 Wi-Fi？设备设置不会丢失。')\">"
      "<button type='submit' style='background:#d97706'>立即重连 Wi-Fi</button></form>"
      "<form method='POST' action='/reset-wifi' onsubmit=\"return confirm('清除 WiFi 设置并重启？设备会开启配网热点。')\">"
      "<button type='submit' style='background:#dc2626'>重置 WiFi</button></form>"
      "<h2 style='font-size:16px;margin-top:32px'>网络 OTA 固件升级</h2>"
      "<p style='font-size:13px;color:#555'>当前版本：<b>" FW_VERSION "</b>。请选择本项目生成的 ESP8266 firmware.bin。"
      "升级不会清除 Wi-Fi、轮播设置或桌宠文件。上传期间请勿断电。</p>"
      "<form method='POST' action='/update' enctype='multipart/form-data' "
      "onsubmit=\"return confirm('确认升级固件？上传和重启期间请勿断电。')\">"
      "<input type='file' name='firmware' accept='.bin,application/octet-stream' required>"
      "<button type='submit' style='background:#059669'>上传并升级</button></form>"
      "<script>function put(id,v){document.getElementById(id).textContent=v}"
      "function loadDiag(){fetch('/api/diagnostics',{cache:'no-store'}).then(r=>r.json()).then(d=>{"
      "put('heap',Math.round(d.free_heap/1024)+' KB');put('heapMin',Math.round(d.min_free_heap/1024)+' KB');"
      "put('heapBlock',Math.round(d.max_free_block/1024)+' KB');put('heapFrag',d.fragmentation+'%');"
      "}).catch(()=>{})}loadDiag();setInterval(loadDiag,5000)</script></body></html>"));
  webServer.sendContent(""); // terminating chunk for the HTTP/1.1 response
  saveRuntimeStage(STAGE_IDLE);
}

void handleApiDiagnostics() {
  JsonDocument doc;
  doc["free_heap"] = idleFreeHeap;
  doc["core_version"] = ESP.getCoreVersion();
  doc["build_variant"] = FW_CORE_AB_LABEL;
  doc["min_free_heap"] = minimumIdleFreeHeap;
  doc["max_free_block"] = idleMaxFreeBlock;
  doc["fragmentation"] = idleHeapFragmentation;
  doc["reset_reason"] = lastResetReason;
  doc["reset_info"] = lastResetInfo;
  doc["boot_count"] = bootCount;
  doc["previous_stage"] = runtimeStageName(previousRuntimeStage);
  doc["previous_mode"] = runtimeModeName(previousRuntimeMode);
  doc["previous_recovery_step"] = recoveryStepName(previousRecoveryStep);
  doc["previous_first_disconnect_reason"] = previousFirstWifiDisconnectReason;
  doc["previous_last_disconnect_reason"] = previousLastWifiDisconnectReason;
  doc["previous_first_disconnect_origin"] = disconnectOriginName(previousFirstDisconnectWasRecovery);
  doc["previous_last_disconnect_origin"] = disconnectOriginName(previousLastDisconnectWasRecovery);
  doc["previous_first_disconnect_rssi"] = previousFirstWifiRssi;
  doc["previous_first_disconnect_channel"] = previousFirstWifiChannel;
  doc["previous_first_disconnect_bssid"] = wifiBssidText(previousFirstWifiBssid);
  doc["previous_outage_ms"] = previousOutageDurationMs;
  doc["previous_recovery_timeline"] = recoveryTimelineText(previousQuickRecoveryAtMs,
      previousWifiReinitAtMs, previousRadioResetAtMs, previousProtectiveRestartAtMs,
      previousAuthRejoinAtMs);
  doc["previous_wifi_disconnects"] = previousWifiDisconnectCount;
  doc["previous_wifi_reconnects"] = previousWifiReconnectCount;
  doc["previous_bridge_failures"] = previousBridgeFailures;
  doc["previous_bridge_http_code"] = previousBridgeHttpCode;
  doc["previous_network_task"] = networkTaskName(previousNetworkTask);
  doc["previous_request_phase"] = requestPhaseName(previousRequestPhase);
  doc["previous_request_succeeded"] = previousRequestSucceeded;
  doc["previous_request_code"] = previousRequestCode;
  doc["previous_request_duration_ms"] = previousRequestDurationMs;
  doc["previous_request_bytes"] = previousRequestBytes;
  doc["previous_request_free_heap"] = previousRequestFreeHeap;
  doc["previous_request_max_block"] = previousRequestMaxBlock;
  doc["wifi_status"] = (int)WiFi.status();
  doc["first_disconnect_reason"] = firstWifiDisconnectReason;
  doc["last_disconnect_reason"] = lastWifiDisconnectReason;
  doc["first_disconnect_origin"] = disconnectOriginName(firstDisconnectWasRecovery);
  doc["last_disconnect_origin"] = disconnectOriginName(lastDisconnectWasRecovery);
  doc["first_disconnect_rssi"] = firstWifiDisconnectRssi;
  doc["first_disconnect_channel"] = firstWifiDisconnectChannel;
  doc["first_disconnect_bssid"] = wifiBssidText(firstWifiDisconnectBssid);
  doc["outage_ms"] = currentWifiOutageDurationMs();
  doc["recovery_timeline"] = recoveryTimelineText(quickRecoveryAtMs, wifiReinitAtMs,
                                                   radioResetAtMs, protectiveRestartAtMs,
                                                   authRejoinAtMs);
  doc["wifi_disconnects"] = wifiDisconnectCount;
  doc["wifi_reconnects"] = wifiReconnectCount;
  doc["bridge_consecutive_failures"] = bridgeConsecutiveFailures;
  doc["bridge_total_failures"] = bridgeTotalFailures;
  doc["last_bridge_http_code"] = lastBridgeHttpCode;
  doc["bridge_retry_ms"] = bridgeRetryIntervalMs(effectiveMode());
  doc["bridge_health_ok"] = lastBridgeHealthHealthy;
  doc["bridge_health_http_code"] = lastBridgeHealthHttpCode;
  doc["network_request_count"] = networkRequestCount;
  doc["network_request_failures"] = networkRequestFailures;
  doc["active_network_task"] = networkTaskName((uint8_t)activeNetworkTask);
  doc["active_request_phase"] = requestPhaseName((uint8_t)activeRequestPhase);
  doc["last_network_task"] = networkTaskName(rtcRuntimeDiag.networkTask);
  doc["last_request_phase"] = requestPhaseName(rtcRuntimeDiag.requestPhase);
  doc["last_request_code"] = rtcRuntimeDiag.requestCode;
  doc["last_request_duration_ms"] = rtcRuntimeDiag.requestDurationMs;
  doc["last_request_bytes"] = rtcRuntimeDiag.requestBytes;
  doc["last_recovery_action"] = lastRecoveryAction;
  doc["stock_names_cache_supported"] = stockNamesCacheSupported;
  doc["stock_names_cache_valid"] = stockNamesCacheValid;
  doc["stock_names_cached_rev"] = stockNamesCachedRev;
  doc["stock_names_bridge_rev"] = stockNamesRev;
  doc["stock_names_cache_count"] = stockNamesCacheCount;
  doc["stock_names_failures"] = stockNamesFailures;
  doc["weather_text_cache_valid"] = weatherTextCacheValid;
  doc["weather_text_cached_rev"] = weatherTextCachedRev;
  doc["weather_text_bridge_rev"] = weatherTextRev;
  doc["weather_text_failures"] = weatherTextFailures;
  doc["weather_text_legacy_raw"] = weatherTextLegacyRaw;
  JsonArray history = doc["restart_history"].to<JsonArray>();
  for (uint8_t i = 0; i < restartHistory.count; ++i) {
    const RestartHistoryEntry &entry = restartHistory.entries[i];
    JsonObject item = history.add<JsonObject>();
    item["boot_count"] = entry.bootCount;
    item["reset_reason"] = entry.resetReason;
    item["stage"] = runtimeStageName(entry.stage);
    item["mode"] = runtimeModeName(entry.mode);
    item["recovery_step"] = recoveryStepName(entry.recoveryStep);
    item["first_reason"] = entry.firstReason;
    item["last_reason"] = entry.lastReason;
    item["first_origin"] = disconnectOriginName(entry.firstWasRecovery != 0);
    item["last_origin"] = disconnectOriginName(entry.lastWasRecovery != 0);
    item["first_rssi"] = entry.firstRssi;
    item["first_channel"] = entry.firstChannel;
    item["outage_ms"] = entry.outageDurationMs;
    item["network_task"] = networkTaskName(entry.networkTask);
    item["request_phase"] = requestPhaseName(entry.requestPhase);
    item["request_succeeded"] = entry.requestSucceeded != 0;
    item["request_code"] = entry.requestCode;
    item["request_duration_ms"] = entry.requestDurationMs;
    item["request_bytes"] = entry.requestBytes;
    item["request_free_heap"] = entry.requestFreeHeap;
    item["request_max_block"] = entry.requestMaxBlock;
    item["recovery_timeline"] = recoveryTimelineText(entry.quickRecoveryAtMs,
        entry.wifiReinitAtMs, entry.radioResetAtMs, entry.protectiveRestartAtMs,
        entry.authRejoinAtMs);
    item["reset_info"] = entry.resetInfo;
  }
  String body;
  serializeJson(doc, body);
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(200, "application/json", body);
}

void handleManualReconnect() {
  webServer.send(200, "text/html; charset=utf-8",
                 "<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width'>"
                 "<h2>正在重新连接 Wi-Fi</h2><p>设备会保留全部设置，请等待约 20 秒后返回诊断页。</p>"
                 "<script>setTimeout(()=>location.href='/',20000)</script>");
  manualReconnectAtMs = millis() + 500UL;
}

void handleSave() {
  String newHost = webServer.arg("bridge");
  newHost.trim();
  bridgeHost = newHost;
  saveBridgeHost(bridgeHost);
  Serial.printf("[web] bridge host updated to '%s'\n", bridgeHost.c_str());
  webServer.sendHeader("Location", "/");
  webServer.send(303);
}

// ---------- JSON API for the Mac app ----------

const char *displayModeName(DisplayMode m) {
  if (m == MODE_CLAUDE) return "claude";
  if (m == MODE_CODEX) return "codex";
  if (m == MODE_NET) return "net";
  if (m == MODE_MUSIC) return "music";
  if (m == MODE_STOCK) return "stock";
  if (m == MODE_MARKET) return "market";
  if (m == MODE_WEATHER) return "weather";
  return "auto";
}

void handleApiInfo() {
  JsonDocument doc;
  doc["ip"] = WiFi.localIP().toString();
  doc["ssid"] = WiFi.SSID();
  doc["bridge"] = bridgeHost;
  doc["mode"] = displayModeName(displayMode);           // configured mode
  doc["effective"] = displayModeName(effectiveMode());   // what's on screen now
  doc["music_playing"] = statusMusicPlaying;
  doc["showing"] = (currentApp == APP_CLAUDE) ? "claude" : "codex";
  doc["last_update_s"] = everPolled ? (long)((millis() - lastSuccessMs) / 1000) : -1;
  doc["sprite_rev"] = spriteRev;
  doc["brightness"] = brightness;
  doc["wired"] = wiredActive(); // true = data currently arrives over USB serial
  doc["fw"] = FW_VERSION;
  doc["core_version"] = ESP.getCoreVersion();
  doc["build_variant"] = FW_CORE_AB_LABEL;
  doc["bridge_version"] = bridgeVersion;
  doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -127;
  doc["free_heap"] = idleFreeHeap;
  doc["min_free_heap"] = minimumIdleFreeHeap;
  doc["max_free_block"] = idleMaxFreeBlock;
  doc["heap_fragmentation"] = idleHeapFragmentation;
  doc["reset_reason"] = lastResetReason;
  doc["boot_count"] = bootCount;
  doc["previous_stage"] = runtimeStageName(previousRuntimeStage);
  doc["previous_mode"] = runtimeModeName(previousRuntimeMode);
  doc["wifi_status"] = (int)WiFi.status();
  doc["first_wifi_disconnect_reason"] = firstWifiDisconnectReason;
  doc["last_wifi_disconnect_reason"] = lastWifiDisconnectReason;
  doc["wifi_disconnect_reason"] = lastWifiDisconnectReason; // compatibility
  doc["first_wifi_disconnect_origin"] = disconnectOriginName(firstDisconnectWasRecovery);
  doc["last_wifi_disconnect_origin"] = disconnectOriginName(lastDisconnectWasRecovery);
  doc["first_wifi_disconnect_rssi"] = firstWifiDisconnectRssi;
  doc["first_wifi_disconnect_channel"] = firstWifiDisconnectChannel;
  doc["first_wifi_disconnect_bssid"] = wifiBssidText(firstWifiDisconnectBssid);
  doc["wifi_outage_ms"] = currentWifiOutageDurationMs();
  doc["wifi_disconnects"] = wifiDisconnectCount;
  doc["wifi_reconnects"] = wifiReconnectCount;
  doc["bridge_consecutive_failures"] = bridgeConsecutiveFailures;
  doc["bridge_total_failures"] = bridgeTotalFailures;
  doc["last_bridge_http_code"] = lastBridgeHttpCode;
  doc["bridge_retry_ms"] = bridgeRetryIntervalMs(effectiveMode());
  doc["bridge_health_ok"] = lastBridgeHealthHealthy;
  doc["bridge_health_http_code"] = lastBridgeHealthHttpCode;
  doc["network_request_count"] = networkRequestCount;
  doc["network_request_failures"] = networkRequestFailures;
  doc["active_network_task"] = networkTaskName((uint8_t)activeNetworkTask);
  doc["active_request_phase"] = requestPhaseName((uint8_t)activeRequestPhase);
  doc["last_network_task"] = networkTaskName(rtcRuntimeDiag.networkTask);
  doc["last_request_phase"] = requestPhaseName(rtcRuntimeDiag.requestPhase);
  doc["last_request_code"] = rtcRuntimeDiag.requestCode;
  doc["last_request_duration_ms"] = rtcRuntimeDiag.requestDurationMs;
  doc["last_request_bytes"] = rtcRuntimeDiag.requestBytes;
  doc["last_recovery_action"] = lastRecoveryAction;
  doc["stock_names_cache_supported"] = stockNamesCacheSupported;
  doc["stock_names_cache_valid"] = stockNamesCacheValid;
  doc["stock_names_cached_rev"] = stockNamesCachedRev;
  doc["stock_names_bridge_rev"] = stockNamesRev;
  doc["stock_names_cache_count"] = stockNamesCacheCount;
  doc["stock_names_failures"] = stockNamesFailures;
  doc["weather_text_cache_valid"] = weatherTextCacheValid;
  doc["weather_text_cached_rev"] = weatherTextCachedRev;
  doc["weather_text_bridge_rev"] = weatherTextRev;
  doc["weather_text_failures"] = weatherTextFailures;
  doc["uptime_s"] = millis() / 1000UL;
  doc["ota_space"] = ESP.getFreeSketchSpace();
  FSInfo fsInfo;
  if (LittleFS.info(fsInfo)) {
    doc["fs_used"] = fsInfo.usedBytes;
    doc["fs_total"] = fsInfo.totalBytes;
  }
  doc["auto_seconds"] = activeAutoCycleSeconds();
  doc["auto_schedule"] = weatherIsWeekend() ? "weekend" : "weekday";
  JsonArray autoPages = doc["auto_pages"].to<JsonArray>();
  for (uint8_t mode = MODE_CODEX; mode <= MODE_WEATHER; ++mode) {
    if (activeAutoPageMask() & (1U << mode)) autoPages.add(displayModeName((DisplayMode)mode));
  }
  doc["weekday_auto_seconds"] = weekdayAutoCycleSeconds;
  JsonArray weekdayPages = doc["weekday_auto_pages"].to<JsonArray>();
  doc["weekend_auto_seconds"] = weekendAutoCycleSeconds;
  JsonArray weekendPages = doc["weekend_auto_pages"].to<JsonArray>();
  for (uint8_t mode = MODE_CODEX; mode <= MODE_WEATHER; ++mode) {
    if (weekdayAutoPageMask & (1U << mode)) weekdayPages.add(displayModeName((DisplayMode)mode));
    if (weekendAutoPageMask & (1U << mode)) weekendPages.add(displayModeName((DisplayMode)mode));
  }
  JsonObject c = doc["claude"].to<JsonObject>();
  c["status"] = claudeStatus.status;
  c["custom_sprite"] = claudeCustom;
  c["w"] = CLAUDE_SPRITE_W;
  c["h"] = CLAUDE_SPRITE_H;
  JsonObject x = doc["codex"].to<JsonObject>();
  x["status"] = codexStatus.status;
  x["custom_sprite"] = codexCustom;
  x["w"] = CODEX_SPRITE_W;
  x["h"] = CODEX_SPRITE_H;
  String out;
  serializeJson(doc, out);
  webServer.send(200, "application/json", out);
}

void handleApiDisplay() {
  String mode = webServer.arg("mode");
  if (mode == "auto") {
    displayMode = MODE_AUTO;
    resetAutoCyclePosition();
    lastEffectiveMode = MODE_AUTO; // effective carousel pages never equal AUTO
  }
  else if (mode == "claude") displayMode = MODE_CLAUDE;
  else if (mode == "codex") displayMode = MODE_CODEX;
  else if (mode == "net") displayMode = MODE_NET;
  else if (mode == "music") displayMode = MODE_MUSIC;
  else if (mode == "stock") displayMode = MODE_STOCK;
  else if (mode == "market") displayMode = MODE_MARKET;
  else if (mode == "weather") displayMode = MODE_WEATHER;
  else {
    webServer.send(400, "text/plain", "mode must be auto|claude|codex|net|music|stock|market|weather");
    return;
  }
  Serial.printf("[api] display mode = %s\n", mode.c_str());
  if (displayMode == MODE_AUTO) {
    // loop() performs the first selected page's normal transition atomically.
  } else if (displayMode == MODE_NET) {
    netChromeDrawn = false;
    lastNetPollMs = 0; // poll + draw on the next loop tick
  } else if (displayMode == MODE_MUSIC) {
    musicChromeDrawn = false;
    lastMusicPollMs = 0; // poll + draw on the next loop tick
  } else if (displayMode == MODE_STOCK) {
    stockChromeDrawn = false;
    lastStockPollMs = 0; // poll + draw on the next loop tick
    stockNamesDrawPending = false;
  } else if (displayMode == MODE_MARKET) {
    lastMarketPollMs = 0;
    lastMarketFrameVersion = 0; // force a redraw when returning from another page
  } else if (displayMode == MODE_WEATHER) {
    weatherChromeDrawn = false;
    weatherDirty = true;
    weatherTextDrawPending = weatherTextCacheValid;
    if (weatherTextLegacyRaw ||
        (weatherTextRev != 0 &&
         (!weatherTextCacheValid || weatherTextCachedRev != weatherTextRev))) {
      weatherTextPending = true;
    }
  } else {
    updateActiveApp();
    drawActiveApp(); // unconditional: also repaints over a previous net chart
  }
  webServer.send(200, "text/plain", "ok");
}

bool parseAutoPageList(String pages, uint16_t &mask) {
  mask = 0;
  while (pages.length() > 0) {
    int comma = pages.indexOf(',');
    String page = comma >= 0 ? pages.substring(0, comma) : pages;
    pages = comma >= 0 ? pages.substring(comma + 1) : "";
    page.trim();
    if (page == "codex") mask |= 1U << MODE_CODEX;
    else if (page == "music") mask |= 1U << MODE_MUSIC;
    else if (page == "stock") mask |= 1U << MODE_STOCK;
    else if (page == "market") mask |= 1U << MODE_MARKET;
    else if (page == "weather") mask |= 1U << MODE_WEATHER;
    else if (page.length() > 0) return false;
  }
  return mask != 0;
}

void handleApiAutoCycle() {
  const bool splitSchedule = webServer.hasArg("weekday_pages") || webServer.hasArg("weekend_pages");
  String weekdayPages = splitSchedule ? webServer.arg("weekday_pages") : webServer.arg("pages");
  String weekendPages = splitSchedule ? webServer.arg("weekend_pages") : webServer.arg("pages");
  const int weekdaySeconds = (splitSchedule ? webServer.arg("weekday_seconds")
                                             : webServer.arg("seconds")).toInt();
  const int weekendSeconds = (splitSchedule ? webServer.arg("weekend_seconds")
                                             : webServer.arg("seconds")).toInt();
  uint16_t weekdayMask = 0, weekendMask = 0;
  if (!parseAutoPageList(weekdayPages, weekdayMask) ||
      !parseAutoPageList(weekendPages, weekendMask)) {
    webServer.send(400, "text/plain", "each schedule needs codex|music|stock|market|weather");
    return;
  }
  if (!validAutoCycleSeconds(weekdaySeconds) || !validAutoCycleSeconds(weekendSeconds)) {
    webServer.send(400, "text/plain", "seconds must be 5|10|15|20|30|60|120");
    return;
  }
  weekdayAutoPageMask = weekdayMask;
  weekdayAutoCycleSeconds = weekdaySeconds;
  weekendAutoPageMask = weekendMask;
  weekendAutoCycleSeconds = weekendSeconds;
  resetAutoCyclePosition();
  saveAutoCycle();
  if (displayMode == MODE_AUTO) lastEffectiveMode = MODE_AUTO;
  Serial.printf("[api] auto weekday=%u/%u weekend=%u/%u\n", weekdayAutoPageMask,
                weekdayAutoCycleSeconds, weekendAutoPageMask, weekendAutoCycleSeconds);
  webServer.send(200, "text/plain", "ok");
}

void handleApiBrightness() {
  String levelArg = webServer.arg("level");
  if (levelArg.length() == 0) {
    webServer.send(400, "text/plain", "missing level (0-100)");
    return;
  }
  int level = levelArg.toInt();
  if (level < 0) level = 0;
  if (level > 100) level = 100;
  brightness = level;
  applyBrightness();
  saveBrightness();
  Serial.printf("[api] brightness = %d\n", brightness);
  webServer.send(200, "text/plain", "ok");
}

void handleApiBridge() {
  String newHost = webServer.arg("host");
  newHost.trim();
  if (newHost.length() == 0) {
    webServer.send(400, "text/plain", "missing host");
    return;
  }
  bridgeHost = newHost;
  saveBridgeHost(bridgeHost);
  Serial.printf("[api] bridge host = '%s'\n", bridgeHost.c_str());
  webServer.send(200, "text/plain", "ok");
  lastPollMs = 0; // poll the new bridge on the next loop tick
}

void appendAutoPages(JsonArray pages, uint16_t mask) {
  for (uint8_t mode = MODE_CODEX; mode <= MODE_WEATHER; ++mode) {
    if (mask & (1U << mode)) pages.add(displayModeName((DisplayMode)mode));
  }
}

void handleApiSettingsGet() {
  JsonDocument doc;
  doc["schema"] = 1;
  doc["firmware"] = FW_VERSION;
  doc["bridge"] = bridgeHost;
  doc["mode"] = displayModeName(displayMode);
  doc["brightness"] = brightness;
  doc["weekday_seconds"] = weekdayAutoCycleSeconds;
  appendAutoPages(doc["weekday_pages"].to<JsonArray>(), weekdayAutoPageMask);
  doc["weekend_seconds"] = weekendAutoCycleSeconds;
  appendAutoPages(doc["weekend_pages"].to<JsonArray>(), weekendAutoPageMask);
  String out;
  serializeJson(doc, out);
  webServer.send(200, "application/json", out);
}

bool settingsMask(JsonVariantConst value, uint16_t &mask) {
  if (!value.is<JsonArrayConst>()) return false;
  mask = 0;
  for (JsonVariantConst item : value.as<JsonArrayConst>()) {
    if (!item.is<const char *>()) return false;
    String page = item.as<const char *>();
    if (page == "diag") continue; // migrate short-lived v0.5.9-test backups
    uint16_t bit = 0;
    if (!parseAutoPageList(page, bit)) return false;
    mask |= bit;
  }
  if (mask == 0) mask = 1U << MODE_CODEX;
  return true;
}

void handleApiSettingsRestore() {
  const String body = webServer.arg("plain");
  if (body.length() == 0 || body.length() > 4096) {
    webServer.send(400, "text/plain", "invalid settings body");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body) || (doc["schema"] | 0) != 1) {
    webServer.send(400, "text/plain", "unsupported settings file");
    return;
  }
  uint16_t weekdayMask = 0, weekendMask = 0;
  const int weekdaySeconds = doc["weekday_seconds"] | 0;
  const int weekendSeconds = doc["weekend_seconds"] | 0;
  if (!settingsMask(doc["weekday_pages"], weekdayMask) ||
      !settingsMask(doc["weekend_pages"], weekendMask) ||
      !validAutoCycleSeconds(weekdaySeconds) || !validAutoCycleSeconds(weekendSeconds)) {
    webServer.send(400, "text/plain", "invalid auto schedule");
    return;
  }
  const char *modeValue = doc["mode"] | "auto";
  String mode(modeValue);
  DisplayMode restoredMode = MODE_AUTO;
  if (mode == "codex") restoredMode = MODE_CODEX;
  else if (mode == "music") restoredMode = MODE_MUSIC;
  else if (mode == "stock") restoredMode = MODE_STOCK;
  else if (mode == "market") restoredMode = MODE_MARKET;
  else if (mode == "weather") restoredMode = MODE_WEATHER;
  else if (mode == "diag") restoredMode = MODE_CODEX; // no longer a screen page
  else if (mode != "auto") {
    webServer.send(400, "text/plain", "invalid display mode");
    return;
  }

  String restoredBridge = doc["bridge"] | "";
  restoredBridge.trim();
  bridgeHost = restoredBridge;
  saveBridgeHost(bridgeHost);
  brightness = constrain(doc["brightness"] | BRIGHTNESS_DEFAULT, 0, 100);
  applyBrightness();
  saveBrightness();
  weekdayAutoPageMask = weekdayMask;
  weekdayAutoCycleSeconds = weekdaySeconds;
  weekendAutoPageMask = weekendMask;
  weekendAutoCycleSeconds = weekendSeconds;
  saveAutoCycle();
  displayMode = restoredMode;
  resetAutoCyclePosition();
  lastEffectiveMode = MODE_AUTO;
  webServer.send(200, "application/json", "{\"ok\":true}");
}

bool otaUploadOK = false;
bool otaUploadStarted = false;
bool otaHeaderChecked = false;
size_t otaBytesWritten = 0;
size_t otaMaxBytes = 0;
String otaUploadError;
unsigned long otaRestartAtMs = 0;

void handleOtaUploadChunk() {
  HTTPUpload &upload = webServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    saveRuntimeStage(STAGE_OTA);
    otaUploadOK = false;
    otaUploadStarted = true;
    otaHeaderChecked = false;
    otaBytesWritten = 0;
    otaMaxBytes = 0;
    otaUploadError = "";
    String filename = upload.filename;
    filename.toLowerCase();
    if (!filename.endsWith(".bin")) {
      otaUploadError = "请选择 ESP8266 firmware.bin 文件";
      return;
    }
    const size_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    otaMaxBytes = maxSketchSpace;
    if (!Update.begin(maxSketchSpace, U_FLASH)) {
      otaUploadError = "设备没有足够的 OTA 空间";
      return;
    }
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("FIRMWARE UPDATE", SCREEN_CX, 75, 2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Uploading...", SCREEN_CX, 110, 2);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (otaUploadError.length() > 0) return;
    if (!otaHeaderChecked) {
      otaHeaderChecked = true;
      if (upload.currentSize < 4 || upload.buf[0] != 0xE9) {
        otaUploadError = "文件不是有效的ESP8266固件";
        Update.end(false);
        return;
      }
    }
    if (otaBytesWritten + upload.currentSize > otaMaxBytes) {
      otaUploadError = "固件超过设备可用OTA空间";
      Update.end(false);
      return;
    }
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      otaUploadError = "写入固件失败";
      Update.end(false);
    } else {
      otaBytesWritten += upload.currentSize;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (otaUploadError.length() == 0 && otaBytesWritten < 100 * 1024UL) {
      otaUploadError = "固件文件异常小，已拒绝升级";
      Update.end(false);
    } else if (otaUploadError.length() == 0 && Update.end(true)) {
      otaUploadOK = true;
    } else if (otaUploadError.length() == 0) {
      otaUploadError = "固件校验失败，设备仍保留旧版本";
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.end(false);
    otaUploadError = "上传已中止";
  }
}

void handleOtaUploadDone() {
  if (!otaUploadStarted || !otaUploadOK) {
    const String message = otaUploadError.length() ? otaUploadError : "没有收到固件文件";
    webServer.send(400, "text/plain; charset=utf-8", message);
    otaUploadStarted = false;
    return;
  }
  webServer.send(200, "text/html; charset=utf-8",
                 "<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width'>"
                 "<h2>升级成功</h2><p>设备正在重启。Wi-Fi、轮播设置和桌宠文件会保留。</p>"
                 "<script>setTimeout(()=>location.href='/',12000)</script>");
  otaUploadStarted = false;
  otaRestartAtMs = millis() + 1200UL;
}

// Streams the animation currently in use for a slot, in the same wire format
// as the custom .bin: [1 byte frame count][RGB565 frames...]. Lets the Mac
// app mirror exactly what the device is showing (custom upload or built-in).
void handleSpriteRaw(ActiveApp slot) {
  bool custom = (slot == APP_CLAUDE) ? claudeCustom : codexCustom;
  const char *binPath = (slot == APP_CLAUDE) ? CLAUDE_SPRITE_FILE : CODEX_SPRITE_FILE;
  if (custom) {
    File f = LittleFS.open(binPath, "r");
    if (f) {
      webServer.streamFile(f, "application/octet-stream");
      f.close();
      return;
    }
  }
  int frames = (slot == APP_CLAUDE) ? CLAUDE_SPRITE_FRAMES : CODEX_SPRITE_FRAMES;
  int w = (slot == APP_CLAUDE) ? CLAUDE_SPRITE_W : CODEX_SPRITE_W;
  int h = (slot == APP_CLAUDE) ? CLAUDE_SPRITE_H : CODEX_SPRITE_H;
  const uint16_t *const *arr = (slot == APP_CLAUDE) ? claude_sprite_frames : codex_sprite_frames;
  size_t frameBytes = (size_t)w * h * 2;
  webServer.setContentLength(1 + (size_t)frames * frameBytes);
  webServer.send(200, "application/octet-stream", "");
  uint8_t cnt = (uint8_t)frames;
  webServer.sendContent((const char *)&cnt, 1);
  for (int i = 0; i < frames; i++) {
    webServer.sendContent_P((PGM_P)arr[i], frameBytes);
    yield();
  }
}

// Removes a custom sprite so the compiled-in default animation comes back.
void handleSpriteReset(ActiveApp slot) {
  const char *binPath = (slot == APP_CLAUDE) ? CLAUDE_SPRITE_FILE : CODEX_SPRITE_FILE;
  LittleFS.remove(binPath);
  spriteRev++;
  loadCustomSpriteState();
  if (slot == APP_CLAUDE) claudeFrame = 0;
  else codexFrame = 0;
  if (currentApp == slot) drawActiveApp();
  webServer.send(200, "text/plain", "ok");
}

void handleResetWifi() {
  saveRuntimeStage(STAGE_WIFI_RECOVERY);
  webServer.send(200, "text/html", "<html><body>Resetting WiFi, device will restart...</body></html>");
  delay(200);
  WiFiManager wm;
  wm.resetSettings();
  ESP.restart();
}

// ---------- on-device GIF decode (AnimatedGIF) ----------
// AnimatedGIF hands us the image one horizontal line at a time (via the draw
// callback) at the GIF's native resolution, so we never need a full-canvas
// buffer. We nearest-neighbour rescale into the target slot size and stream the
// result straight to the .bin one target row at a time. Because the .bin can't
// hold a whole frame in RAM to composite against, GIFs that only re-encode a
// changed sub-rectangle (the common optimizer output, disposal method 1) are
// composited by reading the *previous frame's* rows back out of the .bin we're
// writing. (Disposal method 2 "restore to background" isn't distinguished -
// uncovered pixels keep the previous frame instead of clearing; fine for the
// looping character animations this is for.)

struct GifDecodeCtx {
  int canvasW, canvasH; // GIF native size
  int targetW, targetH; // slot size we're rescaling down to
  size_t rowBytes;      // targetW * 2
  File out;             // output .bin, written sequentially
  File prevFile;        // previous frame in the .bin, read sequentially for compositing
  bool hasPrev;         // false for frame 0 (nothing to composite over -> black)
  int producedRow;      // next target row still owed for the current frame
};

static File gifReadFile; // one decode runs at a time, so a single handle is fine

void *gifOpenCB(const char *fname, int32_t *pSize) {
  gifReadFile = LittleFS.open(fname, "r");
  if (!gifReadFile) return nullptr;
  *pSize = (int32_t)gifReadFile.size();
  return (void *)&gifReadFile;
}

void gifCloseCB(void *) {
  if (gifReadFile) gifReadFile.close();
}

int32_t gifReadCB(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen) {
  File *f = (File *)pFile->fHandle;
  // AnimatedGIF's own SD example keeps this one-byte-short guard near EOF.
  if ((pFile->iSize - pFile->iPos) < iLen) iLen = pFile->iSize - pFile->iPos - 1;
  if (iLen <= 0) return 0;
  int32_t n = (int32_t)f->read(pBuf, iLen);
  pFile->iPos = (int32_t)f->position();
  return n;
}

int32_t gifSeekCB(GIFFILE *pFile, int32_t iPosition) {
  File *f = (File *)pFile->fHandle;
  f->seek(iPosition);
  pFile->iPos = iPosition;
  return iPosition;
}

// Loads the next previous-frame row into prevRowBuf (black if there's no
// previous frame). Reads are sequential and stay aligned with producedRow.
static void readPrevRow(GifDecodeCtx *ctx) {
  if (ctx->hasPrev)
    ctx->prevFile.read((uint8_t *)prevRowBuf, ctx->rowBytes);
  else
    memset(prevRowBuf, 0, ctx->rowBytes);
}

// Appends the current rowBuf as the next output row.
static void emitRow(GifDecodeCtx *ctx) {
  ctx->out.write((const uint8_t *)rowBuf, ctx->rowBytes);
  ctx->producedRow++;
}

// Emits a row that this frame doesn't touch: a straight copy of the previous
// frame (top/bottom gaps of a partial frame).
static void emitPrevRow(GifDecodeCtx *ctx) {
  readPrevRow(ctx);
  memcpy(rowBuf, prevRowBuf, ctx->rowBytes);
  emitRow(ctx);
}

// Rescales one decoded native line into target rows, compositing over the
// previous frame, and streams every target row it can now finalize.
void gifDrawCB(GIFDRAW *pDraw) {
  GifDecodeCtx *ctx = (GifDecodeCtx *)pDraw->pUser;
  int sy = pDraw->iY + pDraw->y; // absolute source line on the GIF canvas
  if (sy < 0 || sy >= ctx->canvasH) return;

  const uint8_t *pal = pDraw->pPalette24; // RGB888, 256 entries
  const uint8_t *src = pDraw->pPixels;    // palette indices, one per pixel of this line
  bool hasTrans = pDraw->ucHasTransparency;
  uint8_t transIdx = pDraw->ucTransparent;

  // Emit every target row whose nearest source line is <= sy and isn't done yet.
  while (ctx->producedRow < ctx->targetH) {
    int ty = ctx->producedRow;
    int srcRow = (int)((long)ty * ctx->canvasH / ctx->targetH);
    if (srcRow > sy) break;                       // needs a later source line
    if (srcRow < sy) { emitPrevRow(ctx); continue; } // source line was skipped -> previous frame

    // srcRow == sy: composite this source line over the previous frame's row.
    readPrevRow(ctx);
    memcpy(rowBuf, prevRowBuf, ctx->rowBytes);
    for (int tx = 0; tx < ctx->targetW; tx++) {
      int sx = (int)((long)tx * ctx->canvasW / ctx->targetW);
      int rel = sx - pDraw->iX;
      if (rel < 0 || rel >= pDraw->iWidth) continue; // outside this frame's rect: keep previous pixel
      uint8_t idx = src[rel];
      if (hasTrans && idx == transIdx) continue;     // transparent: keep previous pixel
      uint8_t r = pal[idx * 3 + 0], g = pal[idx * 3 + 1], b = pal[idx * 3 + 2];
      uint16_t val = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
      rowBuf[tx] = (uint16_t)(((val & 0xFF) << 8) | (val >> 8)); // byte-swap to match convert_sprites.py
    }
    emitRow(ctx);
  }
}

// Decodes gifPath into binPath in the [count][frames...] wire format the
// display path reads. Returns false on open/decode failure.
bool decodeGifToBin(const char *gifPath, const char *binPath, int targetW, int targetH) {
  // AnimatedGIF's internal state (~24KB of LZW/line/palette buffers) is big, so
  // allocate it on the heap only for the duration of a decode rather than
  // paying for it in .bss for the whole uptime.
  AnimatedGIF *gif = new AnimatedGIF();
  if (!gif) return false;
  gif->begin(GIF_PALETTE_RGB888);
  if (!gif->open(gifPath, gifOpenCB, gifCloseCB, gifReadCB, gifSeekCB, gifDrawCB)) {
    Serial.printf("[gif] open failed err=%d\n", gif->getLastError());
    delete gif;
    return false;
  }

  GifDecodeCtx ctx;
  ctx.canvasW = gif->getCanvasWidth();
  ctx.canvasH = gif->getCanvasHeight();
  ctx.targetW = targetW;
  ctx.targetH = targetH;
  ctx.rowBytes = (size_t)targetW * 2;
  ctx.hasPrev = false;
  size_t frameBytes = (size_t)targetW * targetH * 2;

  ctx.out = LittleFS.open(binPath, "w");
  if (!ctx.out) {
    gif->close();
    delete gif;
    return false;
  }
  ctx.out.write((uint8_t)0); // placeholder frame count, patched once we know the total

  uint8_t count = 0;
  int delayMs = 0, more = 1;
  while (count < MAX_CUSTOM_FRAMES) {
    ctx.producedRow = 0;
    ctx.hasPrev = false;
    if (count > 0) {
      ctx.out.flush(); // make the just-written previous frame visible to the read handle
      ctx.prevFile = LittleFS.open(binPath, "r");
      ctx.hasPrev = (bool)ctx.prevFile;
      if (ctx.hasPrev) ctx.prevFile.seek(1 + (size_t)(count - 1) * frameBytes);
    }

    more = gif->playFrame(false, &delayMs, &ctx);

    if (more >= 0) {
      // finalize any bottom rows this frame never touched
      while (ctx.producedRow < ctx.targetH) emitPrevRow(&ctx);
      count++;
    }
    if (ctx.prevFile) ctx.prevFile.close();
    if (more <= 0) break; // 0 = last frame, <0 = decode error
    yield();              // feed the WDT between frames
  }
  gif->close();
  delete gif;
  ctx.out.close();

  if (count == 0) {
    LittleFS.remove(binPath);
    return false;
  }
  File patch = LittleFS.open(binPath, "r+");
  if (patch) {
    patch.seek(0);
    patch.write(count);
    patch.close();
  }
  Serial.printf("[gif] decoded %d frame(s) %dx%d -> %dx%d\n", count, ctx.canvasW, ctx.canvasH, targetW, targetH);
  return true;
}

// ---------- sprite upload (raw .gif -> on-device decode) ----------
// ESP8266WebServer fully buffers a plain POST body into a heap String before
// the handler runs, which a whole GIF would blow RAM on - so we take the
// upload over its streaming multipart/HTTPUpload path, writing the raw .gif to
// LittleFS in small chunks, then decode it on the done callback.
File uploadFile;

void handleSpriteUploadChunk(const char *gifPath) {
  HTTPUpload &upload = webServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    uploadFile = LittleFS.open(gifPath, "w");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END || upload.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) uploadFile.close();
  }
}

void handleSpriteUploadDone(ActiveApp slot) {
  const char *gifPath = (slot == APP_CLAUDE) ? CLAUDE_GIF_FILE : CODEX_GIF_FILE;
  const char *binPath = (slot == APP_CLAUDE) ? CLAUDE_SPRITE_FILE : CODEX_SPRITE_FILE;
  int tw = (slot == APP_CLAUDE) ? CLAUDE_SPRITE_W : CODEX_SPRITE_W;
  int th = (slot == APP_CLAUDE) ? CLAUDE_SPRITE_H : CODEX_SPRITE_H;

  bool ok = decodeGifToBin(gifPath, binPath, tw, th);
  LittleFS.remove(gifPath); // temp raw gif no longer needed once decoded

  spriteRev++;
  loadCustomSpriteState();
  if (slot == APP_CLAUDE) claudeFrame = 0;
  else codexFrame = 0;
  if (currentApp == slot) drawActiveApp();

  if (ok) {
    webServer.send(200, "text/plain", "ok");
    Serial.println("[sprite] gif decoded & applied");
  } else {
    webServer.send(500, "text/plain", "gif decode failed (too large or unsupported?)");
    Serial.println("[sprite] gif decode FAILED");
  }
}

void setupWebServer() {
  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/save", HTTP_POST, handleSave);
  webServer.on("/reconnect", HTTP_POST, handleManualReconnect);
  webServer.on("/reset-wifi", HTTP_POST, handleResetWifi);
  webServer.on("/api/info", HTTP_GET, handleApiInfo);
  webServer.on("/api/diagnostics", HTTP_GET, handleApiDiagnostics);
  webServer.on("/api/display", HTTP_POST, handleApiDisplay);
  webServer.on("/api/auto", HTTP_POST, handleApiAutoCycle);
  webServer.on("/api/bridge", HTTP_POST, handleApiBridge);
  webServer.on("/api/brightness", HTTP_POST, handleApiBrightness);
  webServer.on("/api/settings", HTTP_GET, handleApiSettingsGet);
  webServer.on("/api/settings", HTTP_POST, handleApiSettingsRestore);
  webServer.on("/update", HTTP_POST, handleOtaUploadDone, handleOtaUploadChunk);
  webServer.on("/sprite/claude/reset", HTTP_POST, []() { handleSpriteReset(APP_CLAUDE); });
  webServer.on("/sprite/codex/reset", HTTP_POST, []() { handleSpriteReset(APP_CODEX); });
  webServer.on("/sprite/claude/raw", HTTP_GET, []() { handleSpriteRaw(APP_CLAUDE); });
  webServer.on("/sprite/codex/raw", HTTP_GET, []() { handleSpriteRaw(APP_CODEX); });
  webServer.on(
      "/sprite/claude", HTTP_POST, []() { handleSpriteUploadDone(APP_CLAUDE); },
      []() { handleSpriteUploadChunk(CLAUDE_GIF_FILE); });
  webServer.on(
      "/sprite/codex", HTTP_POST, []() { handleSpriteUploadDone(APP_CODEX); },
      []() { handleSpriteUploadChunk(CODEX_GIF_FILE); });
  webServer.begin();
  Serial.printf("[web] admin server listening on http://%s/\n", WiFi.localIP().toString().c_str());
}

// ---------- Arduino entry points ----------

void setup() {
  Serial.setRxBufferSize(2048); // a serial #STATUS frame (~600B) must survive a slow draw
  Serial.begin(115200);
  LittleFS.begin();
  loadStockNamesCache();
  loadWeatherTextCache();
  loadBootDiagnostics();
  loadBridgeHost();
  loadBrightness();
  loadAutoCycle();
  loadCustomSpriteState();

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  analogWriteFreq(BRIGHTNESS_PWM_FREQ);
  analogWriteRange(100); // duty maps 1:1 to a 0-100 percentage
  applyBrightness();

  setupWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    setupWebServer();
    webServerStarted = true;

    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("WiFi connected", 8, 70, 2);
    tft.drawString("Admin page:", 8, 100, 2);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("http://" + WiFi.localIP().toString(), 8, 125, 2);
    delay(3000);

    showMainUiIfNeeded();
    lastPollMs = 0; // scheduler starts only after the 30-second Wi-Fi grace period
  }
  sampleHeapHealth(true);
  saveRuntimeStage(STAGE_IDLE);
  // else: the config-portal screen stays up; either the user configures WiFi
  // (handled in loop) or serial #STATUS frames arrive and take the screen over
}

void loop() {
  wifiManager.process(); // keeps the config portal alive until WiFi is set up
  pumpSerial();          // wired (USB) bridge frames

  if (!webServerStarted && WiFi.status() == WL_CONNECTED) {
    // WiFi came up after boot (portal or slow AP); the portal has released
    // port 80 by now, so the admin server can bind it
    setupWebServer();
    webServerStarted = true;
    showMainUiIfNeeded();
    lastPollMs = 0; // poll the bridge right away
  }
  if (webServerStarted) webServer.handleClient();
  if (otaRestartAtMs != 0) {
    if ((long)(millis() - otaRestartAtMs) >= 0) ESP.restart();
    return; // preserve eboot's pending OTA command until the reboot
  }
  if (manualReconnectAtMs != 0 && (long)(millis() - manualReconnectAtMs) >= 0) {
    manualReconnectAtMs = 0;
    hardReconnectWiFi("用户从诊断页手动重新连接", RECOVERY_MANUAL);
  }
  maintainConnectivity(millis());
  if (webServerNeedsRestart && WiFi.status() == WL_CONNECTED) {
    webServer.stop();
    webServer.begin();
    webServerNeedsRestart = false;
    Serial.printf("[web] server rebound after Wi-Fi recovery on %s\n", WiFi.localIP().toString().c_str());
  }
  sampleHeapHealth();
  // Reconnect APIs can report WL_CONNECTED briefly while the SDK is still
  // leaving/rejoining the AP. Do not start HTTP work in that transition. Once
  // GotIP fires, wait another five seconds before any bridge or market request.
  if (!networkTrafficReady()) {
    delay(0);
    return;
  }
  if (!mainUiShown) return; // config-portal screen is up, nothing to animate

  unsigned long nowMs = millis();
  advanceAutoCycleIfNeeded(nowMs);

  // Effective mode may differ from the configured one (AUTO -> music while
  // audio plays). On a transition, reset the incoming mode's chrome so it
  // repaints cleanly, and repaint the pet immediately when returning to it.
  DisplayMode eff = effectiveMode();
  diagnosticEffectiveMode = (uint8_t)eff;
  if (eff != lastEffectiveMode) {
    lastEffectiveMode = eff;
    if (eff == MODE_NET) {
      netChromeDrawn = false;
      lastNetPollMs = 0;
    } else if (eff == MODE_MUSIC) {
      musicChromeDrawn = false;
      lastMusicPollMs = 0;
      musicCoverPending = false;
      musicTextPending = false;
    } else if (eff == MODE_STOCK) {
      stockChromeDrawn = false;
      lastStockPollMs = 0;
      stockNamesPending = false;
      stockNamesDrawPending = false;
    } else if (eff == MODE_MARKET) {
      lastMarketPollMs = 0;
      lastMarketFrameVersion = 0;
      marketAutoDwellKnown = false;
      marketAutoDwellMs = 0;
    } else if (eff == MODE_WEATHER) {
      weatherChromeDrawn = false;
      weatherDirty = true;
      weatherTextDrawPending = weatherTextCacheValid;
      if (weatherTextLegacyRaw ||
          (weatherTextRev != 0 &&
           (!weatherTextCacheValid || weatherTextCachedRev != weatherTextRev))) {
        weatherTextPending = true;
      }
      lastWeatherClockMs = 0;
    } else {
      selectEffectivePet(eff);
      drawActiveApp();
    }
  }

  if (eff == MODE_NET) {
    // net-speed mode: rendering (constant-rate sweep) is independent of the
    // bridge polls that refill its sample queue
    if (nowMs - lastNetDrawMs >= NET_DRAW_INTERVAL_MS) {
      lastNetDrawMs = nowMs;
      netDrawTick();
    }
  } else if (eff == MODE_MUSIC) {
    // Metadata and binary assets are fetched by the single-request scheduler.
  } else if (eff == MODE_STOCK) {
    if (!stockChromeDrawn || stockDirty) drawStockScreen();
    if (stockNamesDrawPending && stockNamesCacheValid) {
      stockNamesDrawPending = false;
      if (drawStockNamesFromCache()) {
        stockNamesDrawnRev = stockNamesCachedRev;
      } else {
        stockNamesCacheValid = false;
        stockNamesCachedRev = 0;
        stockNamesCacheCount = 0;
        if (stockNamesRev != 0) stockNamesPending = true;
      }
    }
  } else if (eff == MODE_MARKET) {
    // Metadata and the large frame are deliberately two separate queue tasks.
  } else if (eff == MODE_WEATHER) {
    if (!weatherChromeDrawn) drawWeatherScreen(true);
    if (weatherTextDrawPending && weatherTextCacheValid) {
      weatherTextDrawPending = false;
      if (!drawWeatherTextFromCache()) {
        weatherTextCacheValid = false;
        if (weatherTextRev != 0) weatherTextPending = true;
      }
    }
    if (nowMs - lastWeatherClockMs >= 1000UL) {
      lastWeatherClockMs = nowMs;
      drawWeatherScreen(false);
    }
  } else {
    // sprite walk-cycle animation (only advances while that app is showing)
    if (nowMs - lastAnimMs >= ANIM_INTERVAL_MS) {
      lastAnimMs = nowMs;
      bool claudeWorking = strcmp(claudeStatus.status, "working") == 0;
      bool codexWorking = strcmp(codexStatus.status, "working") == 0;
      if (showingCd != CD_NONE) {
        // countdown owns the center area: no sprite frames over it
      } else if (currentApp == APP_CLAUDE && claudeWorking) {
        claudeFrame = (claudeFrame + 1) % claudeFrameCount();
        drawClaudeSprite(claudeFrame);
      } else if (currentApp == APP_CODEX && codexWorking) {
        codexFrame = (codexFrame + 1) % codexFrameCount();
        drawCodexSprite(codexFrame);
      }
    }

    // countdown seconds tick locally between bridge polls
    static unsigned long lastCdTickMs = 0;
    if (showingCd != CD_NONE && nowMs - lastCdTickMs >= 1000) {
      lastCdTickMs = nowMs;
      drawCountdown(false);
    }

    // "urgent" flash toggle (independent, faster cadence)
    if (nowMs - lastFlashMs >= FLASH_INTERVAL_MS) {
      lastFlashMs = nowMs;
      flashOn = !flashOn;
      if (bridgeStale()) {
        redrawRingOnly();
      } else if (currentAppNeedsInput()) {
        // approval needed: blink the whole border red, restore the quota ring
        // on the off-phase so it doesn't erase the normal chrome permanently
        if (flashOn) drawFullBorder(TFT_RED);
        else redrawRingOnly();
      }
    }

    // alternate which app is shown when neither/both are uniquely working
    if (displayMode != MODE_AUTO && updateActiveApp()) {
      drawActiveApp();
    }
  }

  // Status, page metadata, text strips and market frames all share one queue.
  // This call performs zero or one outbound HTTP transaction per loop.
  runOneNetworkTask(nowMs, eff);
  delay(0);
}
