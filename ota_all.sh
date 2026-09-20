#!/usr/bin/env bash
#
# Build the firmware for each hardware group and push it over OTA (espota) to
# every device on the local network. Device lists mirror the comment block next
# to [env:xiao_sense_ota] in platformio.ini (currently around line 702) --
# update both places together.
#
# Usage:
#   ./ota_all.sh            build + flash every device in every group
#   ./ota_all.sh --retry    build + flash only the devices that FAILED on the
#                           most recent run, per ota_results.log
#
# After each successful upload the device is polled at /statistics/static
# (see CWebServer::GetStatistics in src/webserver.cpp) until it comes back up,
# and its BUILD_TIMESTAMP (the running firmware's __DATE__ __TIME__) is
# recorded -- confirming not just that the upload succeeded but which build
# is actually running. Every attempt, successful or not, is appended to
# ota_results.log as one run block.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

LOG_FILE="$SCRIPT_DIR/ota_results.log"

RETRY=0
for arg in "$@"; do
    case "$arg" in
        --retry) RETRY=1 ;;
        *)
            echo "usage: $0 [--retry]" >&2
            exit 1
            ;;
    esac
done

for tool in jq curl python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: '$tool' is required but not found on PATH" >&2
        exit 1
    fi
done

# group: "pio-environment name:ip name:ip ..."
# Add new Waveshare panels to the second group as they join the network.
# NOTE: don't call this array GROUPS -- bash reserves that name for the
# process's numeric group ID list and silently clobbers it.
OTA_GROUPS=(
  "xiao_sense|tester1:192.168.86.21 deck1:192.168.86.69 deck2:192.168.86.72 deck3:192.168.86.68 matrix-16x16:192.168.86.74"
  "waveshare_esp32s3_rgb_matrix|matrix-192x64:192.168.86.37"
)

ESPOTA="$(find "$HOME/.platformio/packages/framework-arduinoespressif32/tools" -maxdepth 1 -name espota.py 2>/dev/null | head -1)"
if [[ -z "$ESPOTA" ]]; then
    echo "error: could not find espota.py under ~/.platformio/packages/framework-arduinoespressif32/tools" >&2
    exit 1
fi

# On --retry, narrow OTA_GROUPS down to just the devices that FAILED in the
# most recent run block of the log (fields are name, ip, env, status, detail).
if [[ "$RETRY" -eq 1 ]]; then
    if [[ ! -f "$LOG_FILE" ]]; then
        echo "error: --retry needs a previous run in $LOG_FILE, none found" >&2
        exit 1
    fi

    last_run="$(awk '/^=== OTA run /{block=""} {block = block $0 ORS} END{printf "%s", block}' "$LOG_FILE")"
    mapfile -t failed_names < <(printf '%s' "$last_run" | awk -F'\t' '$4 == "FAILED" {print $1}')

    if [[ ${#failed_names[@]} -eq 0 ]]; then
        echo "Nothing to retry -- the most recent run in $LOG_FILE had no failures."
        exit 0
    fi
    echo "Retrying devices that failed last run: ${failed_names[*]}"

    new_groups=()
    for group in "${OTA_GROUPS[@]}"; do
        pio_env="${group%%|*}"
        devices="${group#*|}"
        keep=""
        for entry in $devices; do
            name="${entry%%:*}"
            for f in "${failed_names[@]}"; do
                [[ "$name" == "$f" ]] && keep="$keep $entry"
            done
        done
        [[ -n "$keep" ]] && new_groups+=("$pio_env|${keep# }")
    done
    OTA_GROUPS=("${new_groups[@]}")
fi

log_result() {
    # name, ip, pio_env, status (OK|FAILED), detail -- tab-separated for awk.
    printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$5" >> "$LOG_FILE"
}

# Polls the freshly-flashed device until it reports back up, then returns its
# BUILD_TIMESTAMP. ESP32s take a few seconds to reboot and rejoin WiFi after
# an OTA write, so this allows up to ~60s before giving up.
wait_for_build_timestamp() {
    local ip="$1" json ts
    for _ in $(seq 1 20); do
        sleep 3
        json="$(curl -s -m 3 "http://$ip/statistics/static" 2>/dev/null)" || continue
        ts="$(printf '%s' "$json" | jq -r '.BUILD_TIMESTAMP // empty' 2>/dev/null)"
        [[ -n "$ts" ]] && { printf '%s' "$ts"; return 0; }
    done
    return 1
}

echo "=== OTA run $(date -Iseconds) ===" >> "$LOG_FILE"

declare -a FAILED=()
TOTAL=0

for group in "${OTA_GROUPS[@]}"; do
    pio_env="${group%%|*}"
    devices="${group#*|}"

    echo "==> Building environment '$pio_env'"
    if ! pio run -e "$pio_env"; then
        echo "error: build failed for '$pio_env', skipping its devices" >&2
        for entry in $devices; do
            name="${entry%%:*}"
            ip="${entry#*:}"
            FAILED+=("$name (build failed)")
            log_result "$name" "$ip" "$pio_env" "FAILED" "build failed"
        done
        continue
    fi

    firmware=".pio/build/$pio_env/firmware.bin"
    if [[ ! -f "$firmware" ]]; then
        echo "error: expected firmware image not found at $firmware" >&2
        for entry in $devices; do
            name="${entry%%:*}"
            ip="${entry#*:}"
            FAILED+=("$name (no firmware image)")
            log_result "$name" "$ip" "$pio_env" "FAILED" "no firmware image"
        done
        continue
    fi

    for entry in $devices; do
        name="${entry%%:*}"
        ip="${entry#*:}"
        TOTAL=$((TOTAL + 1))
        echo
        echo "==> OTA upload to $name ($ip) [$pio_env]"
        if python3 "$ESPOTA" -i "$ip" -f "$firmware"; then
            echo "==> $name upload OK, waiting for it to reboot..."
            if compiled_on="$(wait_for_build_timestamp "$ip")"; then
                echo "==> $name OK (compiled on $compiled_on)"
                log_result "$name" "$ip" "$pio_env" "OK" "$compiled_on"
            else
                echo "==> $name uploaded but never came back online to confirm" >&2
                FAILED+=("$name (no confirmation)")
                log_result "$name" "$ip" "$pio_env" "FAILED" "uploaded but did not come back online"
            fi
        else
            echo "==> $name FAILED" >&2
            FAILED+=("$name")
            log_result "$name" "$ip" "$pio_env" "FAILED" "espota upload failed"
        fi
    done
done

echo
if [[ ${#FAILED[@]} -eq 0 ]]; then
    echo "All $TOTAL devices updated successfully. See $LOG_FILE for build timestamps."
else
    echo "Completed with failures: ${FAILED[*]}" >&2
    echo "See $LOG_FILE for details. Run '$0 --retry' to retry just these." >&2
    exit 1
fi
