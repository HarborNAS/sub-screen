#!/usr/bin/env bash

# Build a deterministic Unraid test archive from a fixed binary and source tree.
# This does not install or start anything on the target host.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${OUT_DIR:-${ROOT_DIR}/dist}"
VERSION="${VERSION:-unraid-v0.1.0-test.1}"
STRIP="${STRIP:-strip}"
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "${ROOT_DIR}" log -1 --format=%ct 2>/dev/null || printf '0')}"

[[ "${VERSION}" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || { echo 'invalid VERSION' >&2; exit 1; }
[[ "${SOURCE_DATE_EPOCH}" =~ ^[0-9]+$ ]] || { echo 'invalid SOURCE_DATE_EPOCH' >&2; exit 1; }
export LC_ALL=C TZ=UTC

if [[ -z "${BINARY:-}" ]]; then
    make -C "${ROOT_DIR}" -f unraid/Makefile
    BINARY="${ROOT_DIR}/.build/unraid/harboros-subscreen"
fi
[[ -f "${BINARY}" ]] || { echo "missing binary: ${BINARY}" >&2; exit 1; }

stage="$(mktemp -d -t harboros-subscreen-package.XXXXXX)"
trap 'rm -rf "${stage}"' EXIT
bundle_dir="${stage}/harboros-subscreen"
install -d -m 0755 "${bundle_dir}"

install -m 0755 "${BINARY}" "${bundle_dir}/harboros-subscreen"
"${STRIP}" --strip-debug "${bundle_dir}/harboros-subscreen"
install -m 0755 "${ROOT_DIR}/unraid/preflight.sh" "${bundle_dir}/preflight.sh"
install -m 0755 "${ROOT_DIR}/unraid/rc.harboros-subscreen.example" "${bundle_dir}/rc.harboros-subscreen"
install -m 0644 "${ROOT_DIR}/unraid/runtime.env.example" "${bundle_dir}/runtime.env.example"
install -m 0644 "${ROOT_DIR}/unraid/README.md" "${bundle_dir}/README.md"

mkdir -p "${OUT_DIR}"
archive="${OUT_DIR}/harboros-subscreen-${VERSION}-x86_64.tgz"
tar --sort=name --mtime="@${SOURCE_DATE_EPOCH}" --owner=0 --group=0 \
    --numeric-owner --format=gnu -C "${stage}" -cf - harboros-subscreen |
    gzip -n > "${archive}"
(
    cd "${OUT_DIR}"
    sha256sum "$(basename "${archive}")" > "$(basename "${archive}").sha256"
)
printf 'Bundle: %s\nChecksum: %s\n' "${archive}" "${archive}.sha256"
