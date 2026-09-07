#!/bin/bash

# Continuously samples the ESP8266 diagnostic API and the local Mac bridge.
# Usage: ./tools/monitor_stability.sh 172.28.29.114 [seconds] [bridge-url]

set -u

device_ip="${1:-}"
interval_s="${2:-10}"
bridge_url="${3:-http://127.0.0.1:8765}"

if [[ -z "$device_ip" ]]; then
  echo "用法: $0 设备IP [间隔秒数] [桥接地址]"
  exit 2
fi

echo "开始监测设备 $device_ip；间隔 ${interval_s}s。按 Control-C 停止。"
while true; do
  echo
  date '+%Y-%m-%d %H:%M:%S'
  echo "[设备]"
  curl --silent --show-error --max-time 5 "http://${device_ip}/api/diagnostics" || echo "设备请求失败"
  echo
  echo "[桥接]"
  curl --silent --show-error --max-time 5 "${bridge_url}/health" || echo "桥接请求失败"
  echo
  sleep "$interval_s"
done
