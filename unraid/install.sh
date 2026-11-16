#!/usr/bin/env bash
# Installer for the explicitly versioned, experimental Unraid build.
set -Eeuo pipefail
umask 077

RELEASE_TAG="unraid-v0.1.0-test.1"
REPOSITORY="HarborNAS/sub-screen"
BOOT_DIR="/boot"
RUNTIME_DIR="/usr/local/libexec/harboros-subscreen"
STATE_DIR="/var/run"
LOG_FILE="/var/log/harboros-subscreen.log"
UNRAID_VERSION_FILE="/etc/unraid-version"
SYS_VENDOR_FILE="/sys/class/dmi/id/sys_vendor"
PRODUCT_NAME_FILE="/sys/class/dmi/id/product_name"

PLUGIN_PARENT="${BOOT_DIR}/config/plugins"
PLUGIN_DIR="${PLUGIN_PARENT}/harboros-subscreen"
BACKUP_ROOT="${BOOT_DIR}/config/harboros-subscreen-backups"
RC="${PLUGIN_DIR}/rc.harboros-subscreen"
LOCK="${STATE_DIR}/harboros-subscreen-installer.lock"
ARCHIVE="harboros-subscreen-${RELEASE_TAG}-x86_64.tgz"
BASE_URL="https://github.com/${REPOSITORY}/releases/download/${RELEASE_TAG}"
work="" new_dir="" backup_dir=""
lock_owned=0 transaction=0 old_running=0 old_moved=0 new_installed=0

say() { printf '%s\n' "$*"; }
fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
safe_run() {
    env HARBOR_SUBSCREEN_ALLOW_EC_WRITE=0 HARBOR_SUBSCREEN_ALLOW_OTA=0 \
        HARBOR_SUBSCREEN_ALLOW_SUSPEND=0 HARBOR_SUBSCREEN_ENABLE_STORAGE=0 "$@"
}

rollback() {
    say "Installation failed; restoring the previous service state." >&2
    if (( new_installed )); then
        if ! safe_run bash "$RC" stop; then
            say "Rollback stopped: the candidate could not stop. Previous files remain at ${backup_dir:-none}." >&2
            return 1
        fi
        rm -rf -- "$PLUGIN_DIR" || return 1
        rm -f -- "${RUNTIME_DIR}/harboros-subscreen" || return 1
        rmdir -- "$RUNTIME_DIR" 2>/dev/null || true
    fi
    if (( old_moved )); then
        mv -- "${backup_dir}/previous" "$PLUGIN_DIR" || return 1
        rmdir -- "$backup_dir" 2>/dev/null || true
    fi
    if (( old_running )); then
        safe_run bash "$RC" start || {
            say "Previous files were restored, but restart failed; check ${LOG_FILE}." >&2
            return 1
        }
    fi
    transaction=0
}

finish() {
    local code=$?
    trap - EXIT INT TERM HUP
    set +e
    if (( code != 0 && transaction )); then rollback || code=1; fi
    if [[ -n "$new_dir" && "$new_dir" == "${PLUGIN_PARENT}/.harboros-subscreen.new."* ]]; then
        rm -rf -- "$new_dir"
    fi
    if [[ -n "$work" && "$work" == /tmp/harboros-subscreen-installer.* ]]; then
        rm -rf -- "$work"
    fi
    if [[ -n "$backup_dir" && ! -e "${backup_dir}/previous" ]]; then rmdir -- "$backup_dir" 2>/dev/null || true; fi
    if (( lock_owned )); then
        rm -f -- "${LOCK}/pid"
        rmdir -- "$LOCK" 2>/dev/null || true
    fi
    exit "$code"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

require_host() {
    [[ "$(id -u)" == 0 ]] || fail "Run this installer as root on Unraid."
    [[ "$(uname -m)" == x86_64 ]] || fail "This release requires x86_64."
    [[ -r "$UNRAID_VERSION_FILE" ]] || fail "This installer must run on Unraid."
    mountpoint -q "$BOOT_DIR" || fail "${BOOT_DIR} is not mounted; keep the boot device connected."
    local path
    for path in "${BOOT_DIR}/config" "$PLUGIN_PARENT" "$PLUGIN_DIR" "$BACKUP_ROOT" "$RUNTIME_DIR"; do
        [[ ! -L "$path" ]] || fail "Refusing an unexpected symbolic link: ${path}"
    done
}

require_hardware() {
    grep -Fqx 'version="7.4.0"' "$UNRAID_VERSION_FILE" ||
        fail "This test release supports the validated Unraid 7.4.0 only."
    [[ -r "$SYS_VENDOR_FILE" && -r "$PRODUCT_NAME_FILE" ]] || fail "Cannot verify Nexus hardware identity."
    [[ "$(cat "$SYS_VENDOR_FILE")" == 'Harbor Innovations' && "$(cat "$PRODUCT_NAME_FILE")" == NAS-N1 ]] ||
        fail "This EC-aware test build supports Harbor Nexus / NAS-N1 hardware only."
}

require_installation() {
    [[ -d "$PLUGIN_DIR" && -f "$RC" && ! -L "$RC" ]] || fail "No recognized installation at ${PLUGIN_DIR}."
}

# Accept only literal shell assignments, never evaluate an existing config while
# deciding whether it is safe to retain. Only the documented keys are accepted.
validate_env() {
    local file=$1 line key value bare seen=" "
    [[ -f "$file" && ! -L "$file" ]] || fail "Missing or symlinked runtime.env: ${file}"
    while IFS= read -r line || [[ -n "$line" ]]; do
        line=${line%$'\r'}
        [[ "$line" =~ ^[[:space:]]*$ || "$line" =~ ^[[:space:]]*# ]] && continue
        [[ "$line" == 'export '* ]] && line=${line#export }
        [[ "$line" =~ ^([A-Za-z_][A-Za-z0-9_]*)=(.*)$ ]] ||
            fail "runtime.env must contain comments or simple literal assignments."
        key=${BASH_REMATCH[1]} value=${BASH_REMATCH[2]}
        if [[ "$value" =~ ^[A-Za-z0-9_./:@+-]*$ ]]; then
            bare=$value
        elif [[ "$value" == \"*\" && ${#value} -ge 2 ]]; then
            bare=${value:1:${#value}-2}
            [[ "$bare" != *\"* && "$bare" != *\$* && "$bare" != *\`* && "$bare" != *\\* ]] ||
                fail "runtime.env double-quoted values must be literal (no expansion or escapes)."
        elif [[ "$value" == \'*\' && ${#value} -ge 2 ]]; then
            bare=${value:1:${#value}-2}
            [[ "$bare" != *\'* ]] || fail "runtime.env has an invalid quoted value."
        else
            fail "runtime.env must use simple literal values; preserve custom shell logic separately."
        fi
        case "$key" in
            HARBOR_SUBSCREEN_ALLOW_EC_WRITE|HARBOR_SUBSCREEN_ALLOW_OTA|HARBOR_SUBSCREEN_ALLOW_SUSPEND|HARBOR_SUBSCREEN_ENABLE_STORAGE)
                [[ "$bare" == 0 ]] || fail "This test release requires ${key}=0. Existing config was not changed."
                seen+="${key} " ;;
            HARBOR_SUBSCREEN_FIRMWARE) ;;
            *) fail "Unsupported runtime.env key: ${key}. Existing config was not changed." ;;
        esac
    done < "$file"
    for key in HARBOR_SUBSCREEN_ALLOW_EC_WRITE HARBOR_SUBSCREEN_ALLOW_OTA HARBOR_SUBSCREEN_ALLOW_SUSPEND HARBOR_SUBSCREEN_ENABLE_STORAGE; do
        [[ "$seen" == *" ${key} "* ]] || fail "runtime.env must explicitly contain ${key}=0."
    done
}

download() {
    curl --fail --location --silent --show-error --retry 2 --connect-timeout 15 --max-time 180 \
        --proto '=https' --proto-redir '=https' --tlsv1.2 --output "$2" "$1"
}

verify_archive() {
    local checksum_line expected file entry type count=0
    [[ "$(wc -l < "${work}/${ARCHIVE}.sha256")" -eq 1 ]] || fail "Unexpected checksum file format."
    IFS= read -r checksum_line < "${work}/${ARCHIVE}.sha256"
    checksum_line=${checksum_line%$'\r'}
    [[ "$checksum_line" =~ ^([[:xdigit:]]{64})[[:space:]][[:space:]]?\*?([^[:space:]]+)$ ]] || fail "Invalid checksum file."
    expected=${BASH_REMATCH[1],,} file=${BASH_REMATCH[2]}
    [[ "$file" == "$ARCHIVE" ]] || fail "Checksum names an unexpected file."
    [[ "$(sha256sum "${work}/${ARCHIVE}" | cut -d ' ' -f 1)" == "$expected" ]] || fail "Archive SHA-256 mismatch."
    tar --absolute-names -tzf "${work}/${ARCHIVE}" > "${work}/members"
    tar --absolute-names -tvzf "${work}/${ARCHIVE}" > "${work}/types"
    [[ -z "$(sort "${work}/members" | uniq -d)" ]] || fail "Archive contains duplicate entries."
    while IFS= read -r entry; do
        case "$entry" in
            harboros-subscreen|harboros-subscreen/|harboros-subscreen/harboros-subscreen|harboros-subscreen/preflight.sh|harboros-subscreen/rc.harboros-subscreen|harboros-subscreen/runtime.env.example|harboros-subscreen/README.md) ;;
            *) fail "Archive contains an unexpected path: ${entry}" ;;
        esac
        count=$((count + 1))
    done < "${work}/members"
    (( count >= 5 && count <= 6 )) || fail "Archive has an unexpected number of entries."
    while IFS= read -r entry; do
        type=${entry:0:1}
        [[ "$type" == '-' || "$type" == d ]] || fail "Archive links or special files are not allowed."
    done < "${work}/types"
    mkdir "${work}/unpacked"
    tar --no-same-owner --no-same-permissions -xzf "${work}/${ARCHIVE}" -C "${work}/unpacked"
    for file in harboros-subscreen preflight.sh rc.harboros-subscreen runtime.env.example README.md; do
        [[ -f "${work}/unpacked/harboros-subscreen/${file}" && ! -L "${work}/unpacked/harboros-subscreen/${file}" ]] ||
            fail "Archive is missing a regular required file: ${file}"
    done
    chmod 0755 "${work}/unpacked/harboros-subscreen/harboros-subscreen"
}

install_release() {
    require_hardware
    [[ -w "$BOOT_DIR" ]] || fail "${BOOT_DIR} is not writable."
    local file payload existing=0
    for file in curl tar sha256sum mktemp sort uniq cut; do command -v "$file" >/dev/null || fail "Missing required command: ${file}"; done
    if [[ -e "$PLUGIN_DIR" ]]; then
        require_installation
        validate_env "${PLUGIN_DIR}/runtime.env"
        existing=1
    fi
    work=$(mktemp -d /tmp/harboros-subscreen-installer.XXXXXX)
    say "Downloading experimental ${RELEASE_TAG}..."
    download "${BASE_URL}/${ARCHIVE}" "${work}/${ARCHIVE}"
    download "${BASE_URL}/${ARCHIVE}.sha256" "${work}/${ARCHIVE}.sha256"
    verify_archive
    payload="${work}/unpacked/harboros-subscreen"
    validate_env "${payload}/runtime.env.example"
    safe_run bash "${payload}/preflight.sh" "${payload}/harboros-subscreen" --require-panel

    mkdir -p "$PLUGIN_PARENT"
    new_dir=$(mktemp -d "${PLUGIN_PARENT}/.harboros-subscreen.new.XXXXXX")
    if (( existing )); then cp -R -- "${PLUGIN_DIR}/." "${new_dir}/"; fi
    for file in harboros-subscreen preflight.sh rc.harboros-subscreen runtime.env.example README.md; do
        rm -f -- "${new_dir}/${file}"
        cp -- "${payload}/${file}" "${new_dir}/${file}"
    done
    if (( ! existing )); then cp -- "${payload}/runtime.env.example" "${new_dir}/runtime.env"; fi
    rm -f -- "${new_dir}/installed-version"
    printf '%s\n' "$RELEASE_TAG" > "${new_dir}/installed-version"
    if (( existing )); then
        mkdir -p "$BACKUP_ROOT"
        backup_dir=$(mktemp -d "${BACKUP_ROOT}/$(date -u +%Y%m%dT%H%M%SZ)-${RELEASE_TAG}-XXXXXX")
        if safe_run bash "$RC" status >/dev/null 2>&1; then old_running=1; fi
    fi
    transaction=1
    if (( existing )); then
        safe_run bash "$RC" stop
        mv -- "$PLUGIN_DIR" "${backup_dir}/previous"
        old_moved=1
    fi
    mv -- "$new_dir" "$PLUGIN_DIR"
    new_dir=""
    new_installed=1
    safe_run bash "$RC" start
    safe_run bash "$RC" status
    sync
    transaction=0
    say "Installed ${RELEASE_TAG}. Automatic startup is not enabled."
    say "Log: ${LOG_FILE}. Stop the service when your evaluation is complete."
    [[ -z "$backup_dir" ]] || say "Previous installation: ${backup_dir}/previous"
}

uninstall_release() {
    require_installation
    [[ -w "$BOOT_DIR" ]] || fail "${BOOT_DIR} is not writable."
    safe_run bash "$RC" stop
    rm -rf -- "$PLUGIN_DIR"
    rm -f -- "${RUNTIME_DIR}/harboros-subscreen" "${STATE_DIR}/harboros-subscreen.pid" "$LOG_FILE"
    rmdir -- "$RUNTIME_DIR" 2>/dev/null || true
    sync
    say "Uninstalled. Existing backups at ${BACKUP_ROOT} were retained."
}

main() {
    local action=${1:-install}
    [[ $# -le 1 ]] || fail "Usage: bash install.sh [install|start|stop|status|uninstall]"
    case "$action" in install|start|stop|status|uninstall) ;; *) fail "Unknown action: ${action}" ;; esac
    require_host
    mkdir -p "$STATE_DIR"
    mkdir "$LOCK" 2>/dev/null || fail "Another installer is running, or a stale lock exists at ${LOCK}."
    lock_owned=1
    printf '%s\n' "$$" > "${LOCK}/pid"
    case "$action" in
        install) install_release ;;
        uninstall) uninstall_release ;;
        start)
            require_hardware
            require_installation
            validate_env "${PLUGIN_DIR}/runtime.env"
            safe_run bash "$RC" start
            safe_run bash "$RC" status ;;
        stop|status)
            require_installation
            safe_run bash "$RC" "$action" ;;
    esac
}

main "$@"
