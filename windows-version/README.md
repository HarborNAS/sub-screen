# HarborOS Subscreen - Windows WinUSB Service

This Windows version drives the HarborOS subscreen hardware as a user-mode WinUSB service. It does not register the device as a Windows display. The service sends the same packed USB protocol used by the Linux implementation to the USB device `VID_5448&PID_0002`.

## Current Scope

- USB transport: WinUSB bulk pipes, expected `OUT 0x02` and `IN 0x81`
- Device interface GUID: `{63730607-e5e5-48c9-b9de-e808f11c6a35}`
- Runtime modes: Windows service, real console mode, mock USB console mode
- Diagnostics: `probe`, `setup-check`, `selftest`
- Service features: Time/Home, System, Disk, WLAN, Mode, Info pages, ACK/page read thread, reconnect handling
- Manual firmware tools: `fw-version`, `fw-update <firmware.bin>`
- Not implemented as Windows drivers: EC register writes, Windows virtual display / IddCx, custom kernel/UMDF sensor drivers

## Build

Install Visual Studio Build Tools with the C++ workload and Windows SDK, then run:

```cmd
build.bat Debug
build.bat Release
```

The output is written to:

```text
build\Debug\subscreen.exe
build\Release\subscreen.exe
build\Release\HarborSubscreenSetup.exe
```

## Signing

Customer releases should sign both executables with Microsoft Artifact Signing:

```cmd
sign-release.bat
```

The signing script builds Release, signs `build\Release\subscreen.exe`, rebuilds only the installer so it embeds that signed binary, signs `build\Release\HarborSubscreenSetup.exe`, verifies both signatures, then writes release assets and SHA256 hashes to `dist\`.

Copy `signing\metadata.sample.json` to `signing\metadata.json` and fill it with Harbor's Artifact Signing endpoint, code signing account name, and certificate profile name. Do not commit the real `metadata.json`.

See [SIGNING.md](SIGNING.md) for Azure setup, local dependency checks, manual SignTool commands, and the release checklist.

## Customer One-Click Installer

Release builds produce a single customer installer:

```cmd
build\Release\HarborSubscreenSetup.exe
```

Run it elevated or double-click it and accept UAC. The installer:

1. Finds exactly one present `USB\VID_5448&PID_0002` device on the fixed internal USB port.
2. Selects Microsoft's inbox `WinUsb Device` node from `C:\Windows\INF\winusb.inf`.
3. Registers `DeviceInterfaceGUIDs={63730607-e5e5-48c9-b9de-e808f11c6a35}` on that device instance.
4. Restarts the device instance, extracts `subscreen.exe` to `%ProgramFiles%\Harbor\Subscreen`, installs `HarborOSSubscreenService`, and starts it.
5. Runs `subscreen.exe setup-check`.

This path does not ship Harbor's unsigned INF and does not require disabling Secure Boot. It still requires administrator approval because Windows must change the selected function driver for the USB device.

For community-facing install instructions, known limitations, and troubleshooting steps, see [COMMUNITY_INSTALL.md](COMMUNITY_INSTALL.md).

Uninstall the application while leaving the WinUSB binding intact:

```cmd
HarborSubscreenSetup.exe /uninstall
```

## Engineering INF

Development binding can still use the INF in `driver\harboros-subscreen-winusb.inf`.

This INF is for engineering validation only. It binds `USB\VID_5448&PID_0002` to the Windows inbox `winusb.sys` driver and registers the HarborOS device interface GUID. It is not the customer install path while unsigned.

On a development target, use one of these temporary approaches:

- Boot once with driver signature enforcement disabled, then install the INF for that boot session.
- Use a test-signed driver package on a machine configured for test signing. Secure Boot must be disabled for Windows test-signing mode.

Do not ship unsigned INF, Zadig/libwdi-generated bindings, or test-signing instructions as the customer installer.

For test-signing mode on the target Windows 11 machine, elevated Command Prompt:

```cmd
bcdedit /set testsigning on
shutdown -r -t 0
```

After reboot:

```cmd
pnputil /add-driver driver\harboros-subscreen-winusb.inf /install
```

Confirm enumeration:

```powershell
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'VID_5448|PID_0002' } | Select Status,Class,FriendlyName,InstanceId
```

For a customer no-INF path, update device firmware to expose Microsoft OS descriptors that report compatible ID `WINUSB` and set the same `DeviceInterfaceGUIDs` value. Windows 8 and later can then bind WinUSB automatically by using the inbox WinUSB INF.

If firmware cannot expose WinUSB descriptors, the customer-ready alternatives are a properly signed driver package or a protocol redesign on a Windows inbox USB class such as HID.

## Diagnostics

Run protocol-only tests without hardware:

```cmd
subscreen.exe selftest
```

Run one mock cycle and print packets:

```cmd
subscreen.exe console --mock-usb --once
```

Probe the real WinUSB device:

```cmd
subscreen.exe probe
```

Check customer setup state:

```cmd
subscreen.exe setup-check
```

Expected failure modes:

- `Target USB device is not present in PnP`: the hardware is not connected or did not enumerate.
- `No WinUSB interface registered`: the hardware is connected, but no WinUSB binding is installed.
- `Endpoint mismatch`: WinUSB is bound, but the USB interface does not expose usable bulk IN/OUT pipes.

Run one real hardware cycle:

```cmd
subscreen.exe console --once
```

Run continuous console mode:

```cmd
subscreen.exe console
```

Manual firmware commands:

```cmd
subscreen.exe fw-version
subscreen.exe fw-update firmware.bin
```

Stop the service before `fw-update` if it already owns the USB device.

## Service Install

Elevated Command Prompt:

```cmd
subscreen.exe install
net start HarborOSSubscreenService
```

Uninstall:

```cmd
net stop HarborOSSubscreenService
subscreen.exe uninstall
```

The service keeps running when the USB device is absent and retries connection every two seconds. If the device is unplugged after a failed write, the service closes the WinUSB handle and reconnects when the device returns.

## Project Structure

```text
windows-version/
├── build.bat
├── driver/
│   └── harboros-subscreen-winusb.inf
├── include/
│   ├── protocol.h
│   ├── setup_resource.h
│   ├── service.h
│   ├── system_monitor.h
│   └── usb_comm.h
└── src/
    ├── firmware.c
    ├── main.c
    ├── protocol.c
    ├── service.c
    ├── setup.c
    ├── setup.manifest
    ├── setup_resources.rc
    ├── system_monitor.c
    └── usb_comm.c
```
