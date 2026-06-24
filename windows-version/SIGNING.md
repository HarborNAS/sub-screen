# Signing Harbor Subscreen Windows Releases

Harbor Subscreen Windows uses the Microsoft inbox WinUSB driver (`winusb.inf` / `winusb.sys`). The release does not ship a custom kernel, UMDF, or INF driver package, so the signing target for the current customer build is the application layer:

- `build\Release\subscreen.exe`
- `build\Release\HarborSubscreenSetup.exe`

Do not sign the zip archive. Sign the executables inside the release package.

## Azure Setup

Use Microsoft Artifact Signing as the default code-signing path.

1. Create or use Harbor's Microsoft Entra / Azure tenant.
2. Enable Artifact Signing and complete organization identity validation.
3. Create a code signing account.
4. Create a certificate profile for public code signing.
5. Prefer an officially supported nearby region such as `Japan East` or `Korea Central` when available for the tenant.

Keep the account values out of git. Copy `signing\metadata.sample.json` to `signing\metadata.json` and fill it with the endpoint, account name, and certificate profile name from Azure.

## Build Machine Setup

Install:

- Visual Studio Build Tools with the C++ workload
- Windows SDK, including `signtool.exe`
- Microsoft Artifact Signing Client Tools, including `Azure.CodeSigning.Dlib.dll`
- Azure CLI for interactive `az login`, or CI identity/OIDC if signing in automation

For local interactive signing:

```cmd
az login
```

If the signing Dlib is not installed in a standard location, pass it explicitly:

```cmd
sign-release.bat -DlibPath "C:\Path\To\x64\Azure.CodeSigning.Dlib.dll"
```

## Signing Flow

Run from `windows-version`:

```cmd
sign-release.bat
```

The script performs the release sequence in this order:

1. `build.bat Release`
2. Sign `build\Release\subscreen.exe`
3. `build.bat Release setup-only` so the installer embeds the signed `subscreen.exe`
4. Sign `build\Release\HarborSubscreenSetup.exe`
5. Verify both signatures with `signtool verify /pa /v`
6. Regenerate `dist\HarborSubscreenSetup.exe`, `dist\HarborSubscreen-Windows-OneClick.zip`, and `dist\SHA256SUMS.txt`

To write the final package to a separate output directory:

```cmd
sign-release.bat -DistDir "C:\Users\beanw\Documents\Codex\2026-06-18\kdnet-cd-d-c-kdnet-kdnet\outputs\signed"
```

## Manual Commands

The script wraps these SignTool calls:

```cmd
signtool sign /v /debug /fd SHA256 ^
  /tr http://timestamp.acs.microsoft.com /td SHA256 ^
  /dlib "<ArtifactSigningDlib>\x64\Azure.CodeSigning.Dlib.dll" ^
  /dmdf "signing\metadata.json" ^
  "build\Release\subscreen.exe"
```

```cmd
signtool sign /v /debug /fd SHA256 ^
  /tr http://timestamp.acs.microsoft.com /td SHA256 ^
  /dlib "<ArtifactSigningDlib>\x64\Azure.CodeSigning.Dlib.dll" ^
  /dmdf "signing\metadata.json" ^
  "build\Release\HarborSubscreenSetup.exe"
```

Verify:

```cmd
signtool verify /pa /v build\Release\subscreen.exe
signtool verify /pa /v build\Release\HarborSubscreenSetup.exe
```

## Release Notes Checklist

For every signed community build, include:

- Publisher name shown by Windows
- SHA256 for `HarborSubscreenSetup.exe`
- SHA256 for `HarborSubscreen-Windows-OneClick.zip`
- SmartScreen note: first releases may still show an unknown-app warning while reputation builds
- Install command / uninstall command
- `subscreen.exe setup-check` troubleshooting command

## Driver Signing Boundary

This is not driver-package signing. If a later release ships a Harbor-owned INF, kernel driver, or UMDF driver, use the Microsoft Hardware Developer Program and the appropriate attestation or HLK path.
