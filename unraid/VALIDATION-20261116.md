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

The release workflow builds the isolated `unraid/src/panel.c` variant with native GCC, runs the EC mock and installer fixture tests, and verifies deterministic packaging. The GitHub Actions result records whether those checks passed for a given commit.

The native release artifact and complete persistent installer procedure require a separate physical-hardware check before release. Update this record with that artifact's checksum and result when completed; the historical probe result alone does not establish that a newly built artifact was hardware-tested.

## Boundaries

This is basic display and transport validation. It does not establish:

- Correctness of every displayed value or every button action.
- Fan-speed control or the mappings of the two system-fan registers.
- Temperature accuracy or Unraid array/cache disk enumeration.
- EC configuration writes, firmware updates, or host suspend.
- Long-running stability, log rotation, or automatic startup.

Keep the four runtime policy flags disabled for this test release.
