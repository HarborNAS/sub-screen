#!/usr/bin/env python3
"""Exercise installer transactions with fake hosts/assets; never use real /boot."""
import hashlib
import io
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tarfile
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1] / "install.sh"
BASH = os.environ.get("TEST_BASH", "bash")
ARCHIVE = "harboros-subscreen-unraid-v0.1.0-test.1-x86_64.tgz"
POLICY = """HARBOR_SUBSCREEN_ALLOW_EC_WRITE=0
HARBOR_SUBSCREEN_ALLOW_OTA=0
HARBOR_SUBSCREEN_ALLOW_SUSPEND=0
HARBOR_SUBSCREEN_ENABLE_STORAGE=0
"""
LAUNCHER = r'''#!/usr/bin/env bash
set -eu
dir=$(cd "$(dirname "$0")" && pwd)
case "$1" in
  start)
    grep -q BROKEN "$dir/harboros-subscreen" && exit 9
    mkdir -p "$MOCK_HOST/runtime"
    cp "$dir/harboros-subscreen" "$MOCK_HOST/runtime/harboros-subscreen"
    touch "$MOCK_HOST/running"
    ;;
  stop)
    [[ ${MOCK_STOP_FAIL:-0} == 0 ]] || exit 8
    rm -f "$MOCK_HOST/running"
    ;;
  status) [[ -f "$MOCK_HOST/running" ]] ;;
  *) exit 2 ;;
esac
'''
PREFLIGHT = r'''#!/usr/bin/env bash
set -eu
[[ -x "$1" && "$2" == --require-panel ]]
[[ ${MOCK_PANEL:-1} == 1 ]] || { echo 'FAIL: panel absent'; exit 1; }
[[ $HARBOR_SUBSCREEN_ALLOW_EC_WRITE == 0 && $HARBOR_SUBSCREEN_ALLOW_OTA == 0 &&
   $HARBOR_SUBSCREEN_ALLOW_SUSPEND == 0 && $HARBOR_SUBSCREEN_ENABLE_STORAGE == 0 ]]
echo checked >> "$MOCK_HOST/preflight-calls"
'''


def write(path, text, mode=0o644):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.encode())
    path.chmod(mode)


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="harbor-installer-tests-")
        self.root = Path(self.temporary.name).resolve()
        self.host = self.root / "host"
        self.assets = self.root / "assets"
        self.bin = self.root / "mock-bin"
        self.dest = self.host / "boot/config/plugins/harboros-subscreen"
        self.backups = self.host / "boot/config/harboros-subscreen-backups"
        self.boot_script = "#!/bin/bash\n# existing operator startup configuration\n"
        write(self.host / "boot/config/go", self.boot_script)
        write(self.host / "etc/unraid-version", 'version="7.4.0"\n')
        write(self.host / "dmi/sys_vendor", "Harbor Innovations\n")
        write(self.host / "dmi/product_name", "NAS-N1\n")
        self.assets.mkdir()
        self.bin.mkdir()
        self.env = dict(os.environ, MOCK_HOST=self.host.as_posix(), MOCK_ASSETS=self.assets.as_posix())
        mock_path = self.bin.as_posix()
        if os.name == "nt":
            mock_path = "/" + mock_path[0].lower() + mock_path[2:]
        self.env["TEST_MOCK_BIN"] = mock_path
        for command, body in {
            "id": 'printf "%s\\n" "${MOCK_UID:-0}"\n',
            "uname": 'printf "%s\\n" "${MOCK_ARCH:-x86_64}"\n',
            "mountpoint": '[[ ${MOCK_MOUNTED:-1} == 1 ]]\n',
            "sync": 'exit 0\n',
            "curl": r'''[[ ${MOCK_FETCH_FAIL:-0} == 0 ]] || exit 22
out= url=
while (( $# )); do
  case "$1" in
    --output) out=$2; shift 2 ;;
    https://*) url=$1; shift ;;
    *) shift ;;
  esac
done
[[ "$url" == https://github.com/HarborNAS/sub-screen/releases/download/unraid-v0.1.0-test.1/* ]]
printf '%s\n' "$url" >> "$MOCK_HOST/downloads"
cp "$MOCK_ASSETS/${url##*/}" "$out"
''',
        }.items():
            write(self.bin / command, "#!/usr/bin/env bash\nset -eu\n" + body, 0o755)
        script = SOURCE.read_text()
        substitutions = {
            'BOOT_DIR="/boot"': self.host / "boot",
            'RUNTIME_DIR="/usr/local/libexec/harboros-subscreen"': self.host / "runtime",
            'STATE_DIR="/var/run"': self.host / "state",
            'LOG_FILE="/var/log/harboros-subscreen.log"': self.host / "log/display.log",
            'UNRAID_VERSION_FILE="/etc/unraid-version"': self.host / "etc/unraid-version",
            'SYS_VENDOR_FILE="/sys/class/dmi/id/sys_vendor"': self.host / "dmi/sys_vendor",
            'PRODUCT_NAME_FILE="/sys/class/dmi/id/product_name"': self.host / "dmi/product_name",
        }
        for original, path in substitutions.items():
            self.assertIn(original, script)
            script = script.replace(original, original.split("=", 1)[0] + "=" + shlex.quote(path.as_posix()), 1)
        self.installer = self.root / "installer-under-test.sh"
        write(self.installer, script, 0o755)
        self.make_archive()

    def tearDown(self):
        # The fixture owns this freshly generated directory only.
        self.assertEqual(self.root.parent, Path(tempfile.gettempdir()).resolve())
        self.assertTrue(self.root.name.startswith("harbor-installer-tests-"))
        self.temporary.cleanup()

    def make_archive(self, binary="GOOD NEW", extra=None, symlink=False, duplicate=False):
        files = {
            "harboros-subscreen": "#!/bin/bash\n# " + binary + "\n",
            "preflight.sh": PREFLIGHT,
            "rc.harboros-subscreen": LAUNCHER,
            "runtime.env.example": POLICY,
            "README.md": "Experimental test build.\n",
        }
        with tarfile.open(self.assets / ARCHIVE, "w:gz") as tar:
            root = tarfile.TarInfo("harboros-subscreen/")
            root.type = tarfile.DIRTYPE
            root.mode = 0o755
            tar.addfile(root)
            for name, text in files.items():
                info = tarfile.TarInfo("harboros-subscreen/" + name)
                info.mode = 0o755 if name in ("harboros-subscreen", "preflight.sh", "rc.harboros-subscreen") else 0o644
                if name == "harboros-subscreen" and symlink:
                    info.type = tarfile.SYMTYPE
                    info.linkname = "/etc/shadow"
                    tar.addfile(info)
                else:
                    data = text.encode()
                    info.size = len(data)
                    tar.addfile(info, io.BytesIO(data))
                    if duplicate and name == "README.md":
                        tar.addfile(info, io.BytesIO(data))
            if extra:
                info = tarfile.TarInfo(extra)
                info.size = 4
                tar.addfile(info, io.BytesIO(b"evil"))
        digest = hashlib.sha256((self.assets / ARCHIVE).read_bytes()).hexdigest()
        write(self.assets / (ARCHIVE + ".sha256"), digest + "  " + ARCHIVE + "\n")

    def run_installer(self, action="install", succeeds=True, **env):
        result = subprocess.run([BASH, "--noprofile", "--norc", "-c",
                                 'export PATH="$TEST_MOCK_BIN:$PATH"; exec bash "$@"',
                                 "installer-test", self.installer.as_posix(), action], env=dict(self.env, **env),
                                text=True, capture_output=True, timeout=30)
        if succeeds:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.host / "boot/config/go").read_text(), self.boot_script)
        self.assertFalse((self.host / "state/harboros-subscreen-installer.lock").exists())
        return result.stdout + result.stderr

    def test_install_start_stop_and_uninstall(self):
        self.run_installer(HARBOR_SUBSCREEN_ALLOW_EC_WRITE="1")
        self.assertTrue((self.host / "running").exists())
        self.assertEqual((self.dest / "runtime.env").read_text(), POLICY)
        self.run_installer("status")
        self.run_installer("stop")
        self.run_installer("status", succeeds=False)
        self.run_installer("start")
        write(self.host / "runtime/operator-note", "keep\n")
        self.run_installer("uninstall")
        self.assertFalse(self.dest.exists())
        self.assertFalse((self.host / "running").exists())
        self.assertTrue((self.host / "runtime/operator-note").exists())

    def test_repeated_install_preserves_config_custom_files_and_backups(self):
        self.run_installer()
        custom = POLICY + "# operator comment retained\nHARBOR_SUBSCREEN_FIRMWARE='/boot/custom firmware.bin'\n"
        write(self.dest / "runtime.env", custom)
        write(self.dest / "operator-note", "keep\n")
        for version in ("GOOD SECOND", "GOOD THIRD"):
            self.make_archive(binary=version)
            self.run_installer()
            self.assertEqual((self.dest / "runtime.env").read_text(), custom)
            self.assertEqual((self.dest / "operator-note").read_text(), "keep\n")
        self.assertEqual(len(list(self.backups.glob("*/previous/runtime.env"))), 2)
        self.run_installer("uninstall")
        self.assertEqual(len(list(self.backups.glob("*/previous/runtime.env"))), 2)

    def test_checksum_mismatch_and_wrong_filename_are_rejected(self):
        write(self.assets / (ARCHIVE + ".sha256"), "0" * 64 + "  " + ARCHIVE + "\n")
        self.assertIn("SHA-256 mismatch", self.run_installer(succeeds=False))
        self.make_archive()
        digest = hashlib.sha256((self.assets / ARCHIVE).read_bytes()).hexdigest()
        write(self.assets / (ARCHIVE + ".sha256"), digest + "  ../../etc/shadow\n")
        self.assertIn("unexpected file", self.run_installer(succeeds=False))
        self.assertFalse(self.dest.exists())

    def test_download_failure_does_not_change_host(self):
        self.run_installer(succeeds=False, MOCK_FETCH_FAIL="1")
        self.assertFalse(self.dest.exists())

    def test_unsupported_host_is_rejected_before_download(self):
        for env in ({"MOCK_UID": "1000"}, {"MOCK_ARCH": "aarch64"}, {"MOCK_MOUNTED": "0"}):
            self.run_installer(succeeds=False, **env)
        write(self.host / "etc/unraid-version", 'version="6.12.0"\n')
        self.run_installer(succeeds=False)
        write(self.host / "etc/unraid-version", 'version="7.4.0"\n')
        write(self.host / "dmi/product_name", "OTHER-NAS\n")
        self.run_installer(succeeds=False)
        self.assertFalse((self.host / "downloads").exists())

    def test_missing_panel_fails_before_installation(self):
        self.assertIn("panel absent", self.run_installer(succeeds=False, MOCK_PANEL="0"))
        self.assertFalse(self.dest.exists())

    def test_path_traversal_absolute_path_link_and_duplicates_are_rejected(self):
        for extra in ("harboros-subscreen/../escape", "/tmp/harbor-installer-escape"):
            self.make_archive(extra=extra)
            self.assertIn("unexpected path", self.run_installer(succeeds=False))
        self.make_archive(symlink=True)
        self.assertIn("links or special files", self.run_installer(succeeds=False))
        self.make_archive(duplicate=True)
        self.assertIn("duplicate entries", self.run_installer(succeeds=False))
        self.assertFalse(self.dest.exists())

    def test_unsafe_or_executable_config_is_preserved_but_not_started(self):
        self.run_installer()
        for custom in (POLICY.replace("ALLOW_OTA=0", "ALLOW_OTA=1"),
                       POLICY + "UNSAFE=$(touch " + self.host.as_posix() + "/executed)\n",
                       POLICY + "PIDFILE=/boot/config/shadow\n",
                       POLICY + "LOG=/boot/config/go\n",
                       POLICY + "export BASH_ENV=/boot/operator-script.sh\n"):
            write(self.dest / "runtime.env", custom)
            self.run_installer(succeeds=False)
            self.assertEqual((self.dest / "runtime.env").read_text(), custom)
            self.assertTrue((self.host / "running").exists())
            self.assertFalse((self.host / "executed").exists())

    def test_start_failure_restores_previous_running_installation(self):
        self.make_archive(binary="GOOD OLD")
        self.run_installer()
        previous = (self.dest / "harboros-subscreen").read_bytes()
        custom = POLICY + "# preserve this comment on rollback\n"
        write(self.dest / "runtime.env", custom)
        self.make_archive(binary="BROKEN CANDIDATE")
        output = self.run_installer(succeeds=False)
        self.assertIn("restoring the previous", output)
        self.assertEqual((self.dest / "harboros-subscreen").read_bytes(), previous)
        self.assertEqual((self.dest / "runtime.env").read_text(), custom)
        self.assertTrue((self.host / "running").exists())

    def test_first_install_start_failure_removes_candidate(self):
        self.make_archive(binary="BROKEN CANDIDATE")
        self.run_installer(succeeds=False)
        self.assertFalse(self.dest.exists())
        self.assertFalse((self.host / "running").exists())

    def test_stop_failure_does_not_replace_existing_install(self):
        self.run_installer()
        previous = (self.dest / "harboros-subscreen").read_bytes()
        self.make_archive(binary="GOOD REPLACEMENT")
        self.run_installer(succeeds=False, MOCK_STOP_FAIL="1")
        self.assertEqual((self.dest / "harboros-subscreen").read_bytes(), previous)
        self.assertTrue((self.host / "running").exists())

    def test_uninstall_remains_available_after_unraid_upgrade(self):
        self.run_installer()
        write(self.host / "etc/unraid-version", 'version="7.4.1"\n')
        self.run_installer("start", succeeds=False)
        self.run_installer("uninstall")
        self.assertFalse(self.dest.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
