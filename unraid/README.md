# Nexus status display on Unraid

**Experimental release: `unraid-v0.1.0-test.1`.** This host application provides basic output for the built-in Nexus status display. No kernel compilation is required on the NAS.

The supported test configuration is **Harbor Nexus / NAS-N1, Unraid 7.4.0 x86_64, and USB display `5448:0002`**. Installation and startup require the tested operating-system version and DMI identity: vendor `Harbor Innovations`, product `NAS-N1`. The installer also checks the architecture, display, and runtime dependencies. Other versions and hardware need separate validation; cleanup commands remain available after an OS upgrade.

Basic display output, USB communication, and touch page switching have passed a physical-hardware test. Fan control, temperature accuracy, disk pages, firmware updates, host suspend, and unattended long-term operation have not been validated. See the [validation record](https://github.com/HarborNAS/sub-screen/blob/unraid-v0.1.0-test.1/unraid/VALIDATION-20261116.md) for the tested scope.

## Install

Open a **root** terminal in the Unraid WebGUI or connect using SSH. If Unraid boots from a USB flash drive, keep that drive connected while it is running. Stop any other status-display application before installing.

Download the version-pinned installer, then run it:

```bash
curl -fsSL https://raw.githubusercontent.com/HarborNAS/sub-screen/unraid-v0.1.0-test.1/unraid/install.sh \
  -o /tmp/harboros-subscreen-install.sh &&
bash /tmp/harboros-subscreen-install.sh install
```

The installer downloads this release's archive and checksum, verifies the download and archive contents, checks the host, installs the files, and starts the service. It uses the Linux binary in the Unraid package; the HarborOS `.deb` and Windows `.exe` are separate builds.

This release uses manual startup. Installation does **not** add an automatic startup command or modify `/boot/config/go`. It is not a Community Applications or `.plg` plugin.

On reinstallation, the installer preserves a supported safe `runtime.env` and backs up the previous installation under `/boot/config/harboros-subscreen-backups/`. It refuses unsafe or unsupported configuration syntax rather than enabling unvalidated features. If the new installation cannot start, it attempts to restore the previous installation.

## Verify the display

```bash
bash /boot/config/plugins/harboros-subscreen/rc.harboros-subscreen status
tail -n 60 /var/log/harboros-subscreen.log
```

The status command should report `running pid=...`. Startup logs should include:

```text
Runtime policy: ec-write=off ota=off suspend=off storage=off
USB device opened successfully
Interface claimed successfully
Firmware OTA disabled by runtime policy
```

Device replies should follow. Confirm that the physical display shows content and that touch page switching works.

The preflight warning `no hwmon fan inputs` does not mean display installation failed. Missing libraries, an absent USB display, or USB communication errors require investigation. Keep the preflight output and relevant log messages for your support contact.

## Stop, restart, or start after a reboot

```bash
# Stop after your evaluation.
bash /boot/config/plugins/harboros-subscreen/rc.harboros-subscreen stop

# Start manually, including after Unraid reboots.
bash /boot/config/plugins/harboros-subscreen/rc.harboros-subscreen start

# Restart a running service.
bash /boot/config/plugins/harboros-subscreen/rc.harboros-subscreen restart
```

The service survives closing the SSH session. Its files persist under `/boot/config/plugins/harboros-subscreen/`; the launcher copies the executable to `/usr/local/libexec/harboros-subscreen/` in RAM before starting it.

USB disconnection stops the application. Reconnect the display and restart the service to resume. If the **Unraid boot drive** is disconnected, recover the boot drive before continuing; restarting this application alone does not repair a missing `/boot` filesystem.

## Runtime policy and limitations

Keep these settings in `/boot/config/plugins/harboros-subscreen/runtime.env` at `0`:

```text
HARBOR_SUBSCREEN_ALLOW_EC_WRITE=0
HARBOR_SUBSCREEN_ALLOW_OTA=0
HARBOR_SUBSCREEN_ALLOW_SUSPEND=0
HARBOR_SUBSCREEN_ENABLE_STORAGE=0
```

EC telemetry reads and normal USB display commands remain active. The program therefore still accesses the embedded controller and updates the display; it does not enable EC register writes or firmware updates under this policy. If EC I/O permissions are unavailable, the program reports unknown telemetry instead of attempting raw port access.

This build is **not a fix for low system-fan speeds**. Do not enable EC writes or firmware updates as a troubleshooting step. The two system-fan register mappings remain unverified.

Debug logging is verbose and uses the RAM-backed `/var/log` directory. Stop the service when your evaluation is complete. Long-term operation, log rotation, and automatic startup are outside this release's validated scope.

## Uninstall

Use the downloaded installer:

```bash
bash /tmp/harboros-subscreen-install.sh uninstall
```

If `/tmp` was cleared by a reboot, download the same version-pinned script again with the `curl` command above, then run `uninstall` instead of `install`.

Uninstallation stops the service and removes its managed runtime and installation files. Previous-installation backups under `/boot/config/harboros-subscreen-backups/` are retained.

## Manual download

Release assets are available on the [Unraid test release page](https://github.com/HarborNAS/sub-screen/releases/tag/unraid-v0.1.0-test.1):

- `harboros-subscreen-unraid-v0.1.0-test.1-x86_64.tgz`
- `harboros-subscreen-unraid-v0.1.0-test.1-x86_64.tgz.sha256`

The archive contains the executable, preflight script, launcher, safe environment template, and this guide. It contains no panel firmware. The installer is maintained separately at `unraid/install.sh` in the release tag.

## Build and test from source

The Unraid variant is in `unraid/src/panel.c` and shares the root `protocol.h`. The root `panel.c` and Debian installer remain unchanged.

On an x86_64 Linux build machine with a C compiler, GNU make, and `libusb-1.0` development headers:

```bash
make -f unraid/Makefile all test
bash unraid/tests/installer_test.sh
VERSION=unraid-v0.1.0-test.1 bash unraid/package.sh
```

Build outputs are under `.build/unraid/`, and archives are under `dist/`. The EC unit test mocks port I/O; it does not access hardware. Installer tests use isolated fixtures and do not install the display service on the build machine.

`package.sh` normalizes ownership, permissions, timestamps, file order, and gzip metadata. With the same inputs and `SOURCE_DATE_EPOCH`, packaging produces the same archive. Compiler and dependency differences can still change the binary. To package an explicitly selected prebuilt binary, set `BINARY=/absolute/path/to/harboros-subscreen`; debug information is stripped from the staged copy.

The Linux CI workflow builds the isolated Unraid source, runs the EC and installer tests, checks repeatable packaging, and uploads the archive and checksum as workflow artifacts. Hardware tests remain a separate release check.

References: [upstream project](https://github.com/HarborNAS/sub-screen), [Unraid boot sequence and `/boot`](https://docs.unraid.net/unraid-os/troubleshooting/common-issues/boot-and-startup-failures/).
