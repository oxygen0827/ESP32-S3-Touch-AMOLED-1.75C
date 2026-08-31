#!/usr/bin/env bash
set -euo pipefail

source_file="${1:-/Users/hsh/vibecoding/confer-sum/vocat/products/ws_meeting_demo/sdkconfig.local}"
target_file="${2:-$(cd "$(dirname "$0")/.." && pwd)/sdkconfig.local}"

if [[ ! -f "$source_file" ]]; then
    printf 'private source config not found: %s\n' "$source_file" >&2
    exit 1
fi

read_value() {
    local key="$1"
    awk -F= -v key="$key" '$1 == key {sub(/^[^=]*=/, ""); print; exit}' "$source_file"
}

wifi_ssid="$(read_value CONFIG_MEETING_WIFI_SSID)"
wifi_password="$(read_value CONFIG_MEETING_WIFI_PASSWORD)"
api_base="$(read_value CONFIG_WS_MEETING_API_BASE_URL)"
topic="$(read_value CONFIG_WS_MEETING_TOPIC)"
if [[ -z "$topic" ]]; then
    topic='"Meeting"'
fi

for value_name in wifi_ssid wifi_password api_base; do
    if [[ -z "${!value_name}" ]]; then
        printf 'private source config is missing a required value\n' >&2
        exit 1
    fi
done

mkdir -p "$(dirname "$target_file")"
umask 077
{
    printf 'CONFIG_CLARE_WIFI_SSID=%s\n' "$wifi_ssid"
    printf 'CONFIG_CLARE_WIFI_PASSWORD=%s\n' "$wifi_password"
    printf 'CONFIG_CLARE_API_BASE_URL=%s\n' "$api_base"
    printf 'CONFIG_CLARE_TOPIC=%s\n' "$topic"
} > "$target_file"
chmod 600 "$target_file"
printf 'imported private Clare build configuration (values were not printed)\n'
