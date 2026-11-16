#!/bin/bash

# Read-only Unraid host preflight. It does not touch EC ports, USB endpoints,
# array state, or plugin files.

set -u

BIN="${1:-/usr/local/libexec/harboros-subscreen/harboros-subscreen}"
REQUIRE_PANEL=0
if [[ "${2:-}" == "--require-panel" ]]; then
    REQUIRE_PANEL=1
fi

errors=0
warn() { printf 'WARN: %s\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*"; errors=$((errors + 1)); }
ok() { printf 'OK: %s\n' "$*"; }

arch="$(uname -m 2>/dev/null || true)"
if [[ "$arch" == "x86_64" ]]; then
    ok "host architecture is x86_64"
else
    fail "host architecture is '$arch'; this build targets x86_64"
fi

if [[ "$(id -u 2>/dev/null || echo 1)" == 0 ]]; then
    ok "running as root"
else
    fail "root is required for USB access and EC ioperm"
fi

if [[ -r /etc/unraid-version ]] && grep -Fqx 'version="7.4.0"' /etc/unraid-version; then
    ok "validated Unraid version is 7.4.0"
else
    fail "this test release requires Unraid 7.4.0"
fi

vendor="$(cat /sys/class/dmi/id/sys_vendor 2>/dev/null || true)"
product="$(cat /sys/class/dmi/id/product_name 2>/dev/null || true)"
if [[ "$vendor" == 'Harbor Innovations' && "$product" == NAS-N1 ]]; then
    ok "validated Harbor Nexus / NAS-N1 hardware"
else
    fail "this EC-aware test build requires Harbor Nexus / NAS-N1"
fi

if [[ -x "$BIN" ]]; then
    ok "binary exists: $BIN"
else
    fail "binary is missing or not executable: $BIN"
fi

if command -v ldd >/dev/null 2>&1 && [[ -x "$BIN" ]]; then
    dependencies="$(ldd "$BIN" 2>&1)"
    ldd_status=$?
    missing="$(printf '%s\n' "$dependencies" | grep 'not found' || true)"
    if [[ -n "$missing" ]]; then
        fail "dynamic library missing"
        printf '%s\n' "$missing"
    elif [[ "$ldd_status" != 0 ]]; then
        fail "ldd could not inspect the binary"
        printf '%s\n' "$dependencies"
    else
        ok "dynamic libraries resolve"
    fi
else
    warn "ldd unavailable; dynamic library resolution not checked"
fi

panel_found=0
if command -v lsusb >/dev/null 2>&1; then
    if lsusb -d 5448:0002 2>/dev/null | grep -q .; then
        panel_found=1
        ok "USB panel 5448:0002 is present"
    else
        warn "USB panel 5448:0002 is not currently present"
    fi
else
    warn "lsusb unavailable; USB panel presence not checked"
fi
if [[ "$REQUIRE_PANEL" == 1 && "$panel_found" != 1 ]]; then
    fail "--require-panel requested, but panel is absent"
fi

if [[ -d /sys/block ]]; then
    disk_count="$(find /sys/block -mindepth 1 -maxdepth 1 -type l 2>/dev/null | wc -l)"
    ok "/sys/block is readable ($disk_count block devices)"
else
    fail "/sys/block is unavailable; disk temperature fallback cannot run"
fi

fan_inputs="$(for input in /sys/class/hwmon/hwmon*/fan*_input; do
    [[ -f "$input" ]] && printf '%s\n' "$input"
done | sort)"
if [[ -n "$fan_inputs" ]]; then
    ok "hwmon fan inputs found"
    printf '%s\n' "$fan_inputs"
else
    warn "no hwmon fan inputs; fan control likely remains EC/BIOS-specific"
fi

printf 'Requested policy: EC_WRITE=%s OTA=%s SUSPEND=%s STORAGE=%s\n' \
    "${HARBOR_SUBSCREEN_ALLOW_EC_WRITE:-0}" "${HARBOR_SUBSCREEN_ALLOW_OTA:-0}" \
    "${HARBOR_SUBSCREEN_ALLOW_SUSPEND:-0}" "${HARBOR_SUBSCREEN_ENABLE_STORAGE:-0}"
if [[ "$errors" != 0 ]]; then
    printf 'Preflight failed with %d error(s).\n' "$errors"
    exit 1
fi
printf 'Preflight passed.\n'
