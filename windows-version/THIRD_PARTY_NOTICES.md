# Third Party Notices

The Windows one-click installer bundles the following third-party sensor access
components so NAS-N1 systems can read the same EC telemetry used by the Linux
implementation.

## PawnIO

- File: `resources/PawnIo/PawnIO_setup.exe`
- Version tested: `2.2.0.0`
- Source: <https://github.com/namazso/PawnIO.Setup/releases/tag/2.2.0>
- SHA256: `1F519A22E47187F70A1379A48CA604981C4FCF694F4E65B734AAA74A9FBA3032`
- Purpose: installs the official signed PawnIO kernel driver used for EC access.
- License note: PawnIO is distributed under the GNU GPL with the upstream
  DeviceIoControl interface exception. Review the upstream license before any
  broader commercial redistribution.

## LibreHardwareMonitor PawnIO Module

- File: `resources/PawnIo/LpcACPIEC.bin`
- Upstream release: LibreHardwareMonitor `v0.9.6`
- Source: <https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases/tag/v0.9.6>
- SHA256: `C38FD116E7AFF4D1FDB0A494E296BE0A6708E5A22FC72F14587442FB7F8F7906`
- Purpose: PawnIO module used to access ACPI EC I/O ports `0x62/0x66`.
- License note: LibreHardwareMonitor is MPL 2.0 and includes separate
  third-party notices in the upstream repository.
