# Unraid status-display validation

Date: 2026-11-16

## Hardware configuration

- Harbor Nexus / NAS-N1
- Unraid 7.4.0, kernel 6.18.60-Unraid, x86_64
- Built-in status display: USB `5448:0002`
- Panel firmware reported by the device: `0.3.1.0`

## Initial hardware probe

The initial Linux x86_64 candidate was cross-compiled with Zig 0.15.2. Its ELF SHA-256 was:

```text
e6b8dde4a29fecf86f4fb646ce4de9942074b91a3c9384e3da99e2c040812a9b
```

This hash identifies the historical probe binary, not a guarantee that a later compiler produces identical bytes.

Observed results on the physical Nexus:

- `ldd` resolved all runtime dependencies.
- The EC permission regression test passed. It mocks port operations and verifies failure handling, not EC register meanings.
- A 60-second run used explicit `0` values for EC writes, OTA, suspend, and storage scanning.
- The application claimed the USB interface and received 51 reply packets.
- It obtained the panel firmware version and sent the InfoPage text `Unraid OS 7.4 x86_64`.
- Three EC RAM reads completed. Value meanings and measurement accuracy were not independently validated.
- No failure, segmentation-fault, or libusb-error log lines were observed.
- `timeout` returned the expected code `124`. Both worker threads exited, followed by `USB device closed`.
- The operator confirmed that the physical display showed content and touch page switching worked normally.

## Launcher checks

The corrected launcher was tested on Unraid in isolated temporary paths. Startup, survival after closing SSH, status reporting after reconnecting, and a clean stop all passed. This also exercised the bounded hwmon preflight scan.

These checks did not enable automatic startup or create a persistent service installation on the test machine.

## Release checks

The [native Linux CI run](https://github.com/HarborNAS/sub-screen/actions/runs/43820158321) passed for source commit `9e6f2e1e5f660c99fbddc1729df1c78cfc2f896a`. It built the isolated `unraid/src/panel.c` variant with GCC, passed the EC mock and 12 installer fixture scenarios, and verified repeatable packaging.

The native executable from that CI artifact was installed and tested on the same physical Nexus. Its ELF SHA-256 is:

```text
97329342f149c17f973505e388489e07aef4f2a9caabe407ccb4f2b8564533ad
```

The prepublication test used the unchanged installer with a narrowly scoped local download fixture serving the CI archive and checksum. All subsequent installer steps ran against the real Unraid paths. This separated hardware and installation validation from the public GitHub download check.

Observed results:

- The archive checksum and expected contents passed validation.
- The native executable's dependencies resolved, and the host, OS, and USB panel checks passed.
- Installation persisted files under `/boot/config/plugins/harboros-subscreen/` and launched the identical executable from `/usr/local/libexec/harboros-subscreen/` in RAM.
- The running process reported all four runtime policy flags off, opened and claimed the USB interface, and received device replies.
- The service remained running after the installing SSH connection closed; status passed on a new connection.
- Stop completed cleanly, including `USB device closed`; status then reported `stopped`.
- No failure, segmentation-fault, or libusb-error log lines were observed.
- Uninstall removed the managed installation, runtime binary, PID, and installer lock.
- `/boot/config/go` retained its original checksum. The Unraid array remained stopped throughout.

The physical screen and touch confirmation above applies to the initial probe; the native release test additionally verified the logged device exchange and installation lifecycle. It does not constitute a second operator confirmation of every screen.

The final release archive checksum is published in its `.sha256` asset and release notes. Packaging timestamps and documentation revisions can change the archive checksum without changing the executable. The final public download and installation result is recorded in the [release notes](https://github.com/HarborNAS/sub-screen/releases/tag/unraid-v0.1.0-test.1).

## Boundaries

This is basic display and transport validation. It does not establish:

- Correctness of every displayed value or every button action.
- Fan-speed control or the mappings of the two system-fan registers.
- Temperature accuracy or Unraid array/cache disk enumeration.
- EC configuration writes, firmware updates, or host suspend.
- Long-running stability, log rotation, or automatic startup.

Keep the four runtime policy flags disabled for this test release.
