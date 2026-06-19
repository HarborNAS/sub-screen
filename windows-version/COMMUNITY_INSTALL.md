# Harbor Subscreen Windows Community Test Build

This is the Windows community test build for the Harbor subscreen module.

It installs a user-mode WinUSB service that talks to the built-in subscreen hardware. It does not add a Windows display monitor and it does not require disabling Secure Boot.

## Download

Use the latest GitHub Release asset:

- `HarborSubscreenSetup.exe` - one-click installer
- `HarborSubscreen-Windows-OneClick.zip` - the same installer in a zip archive

SHA256 for the 2026-06-18 build:

```text
HarborSubscreenSetup.exe
A90E0A2937DE4E9795C9A156F004A7C2C697A06CF8FF3CDB55893D404DF917B3

HarborSubscreen-Windows-OneClick.zip
BCEBC45285E3A0A51C08B7928E807899C62CD66AD668568A13CC1FFF365DF102
```

## Requirements

- Windows 10 or Windows 11, x64
- Harbor device with the internal subscreen connected as `USB\VID_5448&PID_0002`
- Administrator approval for the installer
- The subscreen must stay on the original internal USB port

## Install

1. Download `HarborSubscreenSetup.exe` from the GitHub Release.
2. Double-click it.
3. Accept the Windows UAC prompt.
4. If Windows SmartScreen says the app is from an unknown publisher, choose `More info`, then `Run anyway`.
5. Wait for the installer to finish. It installs and starts `HarborOSSubscreenService`.

The installer uses Microsoft's built-in `winusb.inf` and `winusb.sys`. It does not ship an unsigned Harbor driver package.

## What Should Work

- Home / time page
- System page: CPU, iGPU, memory, NVIDIA dGPU when available
- Disk page: disk usage and temperature when Windows exposes it
- WLAN page: online users, upload/download speed, total traffic, IPv4 address, adapter name
- Mode page: mute/performance/balanced commands with Windows user-mode best effort
- Info page: device name, CPU name, Windows version, serial number
- Reconnect handling after unplug/replug on the same fixed USB port

## Known Limitations

- The installer is not code-signed yet, so SmartScreen may warn about an unknown publisher.
- This is a subscreen status display, not a Windows extended monitor.
- The first public build uses WinUSB binding on the current fixed USB device instance. Moving the hardware to another USB port is not the supported path.
- Some hardware-only Linux features, such as EC register writes, are mapped to Windows user-mode best effort behavior.
- Disk temperature depends on what Windows exposes for the drive.

## Troubleshooting

Open PowerShell as Administrator and run:

```powershell
cd "$env:ProgramFiles\Harbor\Subscreen"
.\subscreen.exe setup-check
```

Expected result:

```text
Device present: PASS
WinUSB service: PASS
DeviceInterfaceGUIDs: PASS
Bulk endpoints OUT=0x02 IN=0x81: PASS
```

If the service is running, `setup-check` may say:

```text
PASS (service owns device)
```

That is normal.

For a one-time hardware refresh test:

```powershell
Stop-Service HarborOSSubscreenService
cd "$env:ProgramFiles\Harbor\Subscreen"
.\subscreen.exe console --once
Start-Service HarborOSSubscreenService
```

## Uninstall

Run:

```powershell
HarborSubscreenSetup.exe /uninstall
```

The uninstaller removes the app and service. It intentionally does not roll back the WinUSB binding, because this hardware is fixed inside the device.

## Reporting Issues

When reporting issues in Discord or GitHub, please include:

- Windows version
- Whether Secure Boot is enabled
- Output from `subscreen.exe setup-check`
- A photo of the subscreen page that looks wrong
- Whether NVIDIA GPU is present
