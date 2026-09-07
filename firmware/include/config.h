#pragma once

// ---- Firmware version (shown on the first-time WiFi setup screen & /api/info) ----
#define FW_VERSION "0.5.17-test"
#ifndef FW_CORE_AB_LABEL
#define FW_CORE_AB_LABEL "core-3.1.2"
#endif

// ---- Bridge polling ----
#define BRIDGE_DEFAULT_PORT 8765
#define BRIDGE_DEFAULT_PATH "/status"
#define BRIDGE_POLL_INTERVAL_MS 5000
#define BRIDGE_DATA_PAGE_POLL_INTERVAL_MS 10000
#define BRIDGE_HTTP_TIMEOUT_MS 3000
#define NETWORK_STARTUP_GRACE_MS 30000
#define NETWORK_TASK_GAP_MS 300
#define NETWORK_LARGE_TASK_GAP_MS 1500

// ---- WiFiManager ----
#define WIFI_PORTAL_AP_NAME "AI-Clock-Setup"
#define WIFI_CONFIG_FILE "/bridge_host.txt"

// ---- Backlight ----
#define BRIGHTNESS_FILE "/brightness.txt"
#define BRIGHTNESS_DEFAULT 100
#define BRIGHTNESS_PWM_FREQ 2000 // Hz; high enough to avoid visible flicker when dim

// ---- Configurable automatic page carousel ----
#define AUTO_CYCLE_FILE "/auto_cycle.txt"
#define AUTO_CYCLE_DEFAULT_SECONDS 10

// ---- Four-row quote name cache ----
#define STOCK_NAMES_CACHE_FILE "/stock-names.snc"
#define STOCK_NAMES_CACHE_TMP_FILE "/stock-names.tmp"
#define STOCK_NAMES_CACHE_BACKUP_FILE "/stock-names.bak"
#define STOCK_NAMES_RLE_MAX_BYTES 32768

// ---- Date/weather page ----
#define WEATHER_POLL_INTERVAL_MS 300000
#define WEATHER_TEXT_CACHE_FILE "/weather-text.wtr"
#define WEATHER_TEXT_CACHE_TMP_FILE "/weather-text.tmp"
#define WEATHER_TEXT_CACHE_BACKUP_FILE "/weather-text.bak"
#define WEATHER_TEXT_RLE_MAX_BYTES 49152

// ---- Display layout (240x240 ST7789) ----
#define SCREEN_W 240
#define SCREEN_H 240
