#include "setup_resource.h"
#include "service.h"
#include "usb_comm.h"

#include <cfgmgr32.h>
#include <newdev.h>
#include <setupapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "shell32.lib")

#define INSTALL_DIR_SUFFIX L"Harbor\\Subscreen"
#define LOG_DIR_SUFFIX L"HarborSubscreen\\logs"
#define INSTALLED_EXE_NAME L"subscreen.exe"
#define SETUP_GUID_MULTI L"{63730607-e5e5-48c9-b9de-e808f11c6a35}\0\0"

typedef struct {
    HDEVINFO deviceInfoSet;
    SP_DEVINFO_DATA deviceInfoData;
    WCHAR instanceId[512];
    BOOL winusbService;
    BOOL guidRegistered;
} TargetDevice;

static void PrintWin32Error(const WCHAR* label, DWORD error)
{
    LPWSTR message = NULL;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL,
                   error,
                   0,
                   (LPWSTR)&message,
                   0,
                   NULL);
    fwprintf(stderr, L"%ls failed (%lu)%ls%ls\n", label, error, message ? L": " : L"", message ? message : L"");
    if (message) {
        LocalFree(message);
    }
}

static BOOL IsElevated(void)
{
    HANDLE token = NULL;
    TOKEN_ELEVATION elevation;
    DWORD size = 0;
    BOOL elevated = FALSE;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return FALSE;
    }
    if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
        elevated = elevation.TokenIsElevated != 0;
    }
    CloseHandle(token);
    return elevated;
}

static BOOL RelaunchElevated(void)
{
    WCHAR exePath[MAX_PATH];
    WCHAR params[256] = L"";
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    SHELLEXECUTEINFOW sei;

    if (!GetModuleFileNameW(NULL, exePath, ARRAYSIZE(exePath))) {
        return FALSE;
    }

    if (argv && argc > 1) {
        for (int i = 1; i < argc; ++i) {
            if (i > 1) {
                wcscat_s(params, ARRAYSIZE(params), L" ");
            }
            wcscat_s(params, ARRAYSIZE(params), argv[i]);
        }
    }
    if (argv) {
        LocalFree(argv);
    }

    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = params[0] ? params : NULL;
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei);
}

static BOOL MultiSzContains(const WCHAR* values, const WCHAR* needle)
{
    const WCHAR* item = values;

    if (!values || !needle) {
        return FALSE;
    }

    while (*item) {
        if (wcsstr(item, needle)) {
            return TRUE;
        }
        item += wcslen(item) + 1;
    }
    return FALSE;
}

static BOOL GetDeviceStringProperty(HDEVINFO deviceInfoSet,
                                    PSP_DEVINFO_DATA deviceInfoData,
                                    DWORD property,
                                    WCHAR* buffer,
                                    DWORD bufferCount)
{
    DWORD propertyType = 0;

    if (!buffer || bufferCount == 0) {
        return FALSE;
    }
    buffer[0] = L'\0';

    return SetupDiGetDeviceRegistryPropertyW(deviceInfoSet,
                                             deviceInfoData,
                                             property,
                                             &propertyType,
                                             (PBYTE)buffer,
                                             bufferCount * sizeof(WCHAR),
                                             NULL);
}

static BOOL DeviceHasGuid(HDEVINFO deviceInfoSet, PSP_DEVINFO_DATA deviceInfoData)
{
    HKEY key;
    WCHAR value[512];
    DWORD type = 0;
    DWORD cb = sizeof(value);
    BOOL ok = FALSE;

    key = SetupDiOpenDevRegKey(deviceInfoSet, deviceInfoData, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if (key == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    if (RegQueryValueExW(key, L"DeviceInterfaceGUIDs", NULL, &type, (LPBYTE)value, &cb) == ERROR_SUCCESS &&
        (type == REG_MULTI_SZ || type == REG_SZ)) {
        ok = wcsstr(value, L"{63730607-e5e5-48c9-b9de-e808f11c6a35}") != NULL ||
             wcsstr(value, L"{63730607-E5E5-48C9-B9DE-E808F11C6A35}") != NULL;
    }

    RegCloseKey(key);
    return ok;
}

static BOOL FindTargetDevice(TargetDevice* target)
{
    HDEVINFO deviceInfoSet;
    SP_DEVINFO_DATA deviceInfoData;
    DWORD index = 0;
    DWORD found = 0;

    memset(target, 0, sizeof(*target));
    target->deviceInfoSet = INVALID_HANDLE_VALUE;

    deviceInfoSet = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        PrintWin32Error(L"SetupDiGetClassDevs", GetLastError());
        return FALSE;
    }

    memset(&deviceInfoData, 0, sizeof(deviceInfoData));
    deviceInfoData.cbSize = sizeof(deviceInfoData);

    while (SetupDiEnumDeviceInfo(deviceInfoSet, index, &deviceInfoData)) {
        WCHAR hardwareIds[2048];
        WCHAR service[256];

        ++index;
        if (!GetDeviceStringProperty(deviceInfoSet,
                                     &deviceInfoData,
                                     SPDRP_HARDWAREID,
                                     hardwareIds,
                                     ARRAYSIZE(hardwareIds))) {
            continue;
        }
        if (!MultiSzContains(hardwareIds, L"VID_5448&PID_0002")) {
            continue;
        }

        ++found;
        target->deviceInfoData = deviceInfoData;
        SetupDiGetDeviceInstanceIdW(deviceInfoSet, &deviceInfoData, target->instanceId, ARRAYSIZE(target->instanceId), NULL);
        target->winusbService =
            GetDeviceStringProperty(deviceInfoSet, &deviceInfoData, SPDRP_SERVICE, service, ARRAYSIZE(service)) &&
            _wcsicmp(service, L"WINUSB") == 0;
        target->guidRegistered = DeviceHasGuid(deviceInfoSet, &deviceInfoData);
    }

    if (found != 1) {
        if (found == 0) {
            fwprintf(stderr, L"USB\\VID_5448&PID_0002 is not present. Plug in the fixed subscreen USB cable first.\n");
        } else {
            fwprintf(stderr, L"Found %lu matching subscreen devices; expected exactly one fixed USB device.\n", found);
        }
        SetupDiDestroyDeviceInfoList(deviceInfoSet);
        return FALSE;
    }

    target->deviceInfoSet = deviceInfoSet;
    wprintf(L"Found subscreen device: %ls\n", target->instanceId);
    return TRUE;
}

static void DestroyTargetDevice(TargetDevice* target)
{
    if (target->deviceInfoSet != INVALID_HANDLE_VALUE) {
        SetupDiDestroyDeviceInfoList(target->deviceInfoSet);
        target->deviceInfoSet = INVALID_HANDLE_VALUE;
    }
}

static BOOL WriteDeviceInterfaceGuid(TargetDevice* target)
{
    HKEY key;
    const WCHAR guidSz[] = L"{63730607-e5e5-48c9-b9de-e808f11c6a35}";
    const WCHAR guidMulti[] = SETUP_GUID_MULTI;
    DWORD multiBytes = sizeof(guidMulti);

    key = SetupDiOpenDevRegKey(target->deviceInfoSet,
                               &target->deviceInfoData,
                               DICS_FLAG_GLOBAL,
                               0,
                               DIREG_DEV,
                               KEY_SET_VALUE);
    if (key == INVALID_HANDLE_VALUE) {
        PrintWin32Error(L"SetupDiOpenDevRegKey", GetLastError());
        return FALSE;
    }

    if (RegSetValueExW(key, L"DeviceInterfaceGUIDs", 0, REG_MULTI_SZ, (const BYTE*)guidMulti, multiBytes) != ERROR_SUCCESS) {
        PrintWin32Error(L"RegSetValueEx DeviceInterfaceGUIDs", GetLastError());
        RegCloseKey(key);
        return FALSE;
    }
    RegSetValueExW(key, L"DeviceInterfaceGUID", 0, REG_SZ, (const BYTE*)guidSz, (DWORD)((wcslen(guidSz) + 1) * sizeof(WCHAR)));

    RegCloseKey(key);
    target->guidRegistered = TRUE;
    wprintf(L"Registered HarborOS device interface GUID.\n");
    return TRUE;
}

static BOOL RestartTargetDevice(TargetDevice* target)
{
    SP_PROPCHANGE_PARAMS params;
    DEVINST parent = 0;

    memset(&params, 0, sizeof(params));
    params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    params.StateChange = DICS_PROPCHANGE;
    params.Scope = DICS_FLAG_GLOBAL;
    params.HwProfile = 0;

    if (SetupDiSetClassInstallParamsW(target->deviceInfoSet,
                                      &target->deviceInfoData,
                                      &params.ClassInstallHeader,
                                      sizeof(params)) &&
        SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, target->deviceInfoSet, &target->deviceInfoData)) {
        wprintf(L"Restarted target device instance.\n");
        return TRUE;
    }

    if (CM_Get_Parent(&parent, target->deviceInfoData.DevInst, 0) == CR_SUCCESS &&
        CM_Reenumerate_DevNode(parent, 0) == CR_SUCCESS) {
        wprintf(L"Requested parent USB re-enumeration.\n");
        return TRUE;
    }

    PrintWin32Error(L"RestartTargetDevice", GetLastError());
    return FALSE;
}

static BOOL DriverDetailMatchesWinUsb(HDEVINFO deviceInfoSet,
                                      PSP_DEVINFO_DATA deviceInfoData,
                                      PSP_DRVINFO_DATA_W driverInfo)
{
    DWORD requiredSize = 0;
    PSP_DRVINFO_DETAIL_DATA_W detail;
    BOOL match = FALSE;

    SetupDiGetDriverInfoDetailW(deviceInfoSet, deviceInfoData, driverInfo, NULL, 0, &requiredSize);
    if (requiredSize == 0) {
        requiredSize = sizeof(SP_DRVINFO_DETAIL_DATA_W) + 512 * sizeof(WCHAR);
    }

    detail = (PSP_DRVINFO_DETAIL_DATA_W)calloc(1, requiredSize);
    if (!detail) {
        return FALSE;
    }
    detail->cbSize = sizeof(SP_DRVINFO_DETAIL_DATA_W);

    if (SetupDiGetDriverInfoDetailW(deviceInfoSet, deviceInfoData, driverInfo, detail, requiredSize, NULL) ||
        GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        match = wcsstr(detail->InfFileName, L"winusb.inf") != NULL ||
                wcsstr(detail->HardwareID, L"USB\\MS_COMP_WINUSB") != NULL ||
                _wcsicmp(driverInfo->Description, L"WinUsb Device") == 0;
    }

    free(detail);
    return match;
}

static BOOL ForceInstallWinUsb(TargetDevice* target)
{
    SP_DEVINSTALL_PARAMS_W installParams;
    SP_DRVINFO_DATA_W driverInfo;
    SP_DRVINFO_DATA_W selectedDriver;
    DWORD index = 0;
    BOOL found = FALSE;
    BOOL needReboot = FALSE;
    WCHAR winusbInf[MAX_PATH];

    if (target->winusbService) {
        wprintf(L"Target device is already using WinUSB.\n");
        return TRUE;
    }

    if (!GetWindowsDirectoryW(winusbInf, ARRAYSIZE(winusbInf))) {
        PrintWin32Error(L"GetWindowsDirectory", GetLastError());
        return FALSE;
    }
    wcscat_s(winusbInf, ARRAYSIZE(winusbInf), L"\\INF\\winusb.inf");

    memset(&installParams, 0, sizeof(installParams));
    installParams.cbSize = sizeof(installParams);
    if (!SetupDiGetDeviceInstallParamsW(target->deviceInfoSet, &target->deviceInfoData, &installParams)) {
        PrintWin32Error(L"SetupDiGetDeviceInstallParams", GetLastError());
        return FALSE;
    }
    installParams.Flags |= DI_ENUMSINGLEINF;
    installParams.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
    wcscpy_s(installParams.DriverPath, ARRAYSIZE(installParams.DriverPath), winusbInf);

    if (!SetupDiSetDeviceInstallParamsW(target->deviceInfoSet, &target->deviceInfoData, &installParams)) {
        PrintWin32Error(L"SetupDiSetDeviceInstallParams", GetLastError());
        return FALSE;
    }

    if (!SetupDiBuildDriverInfoList(target->deviceInfoSet, &target->deviceInfoData, SPDIT_CLASSDRIVER)) {
        PrintWin32Error(L"SetupDiBuildDriverInfoList", GetLastError());
        return FALSE;
    }

    memset(&driverInfo, 0, sizeof(driverInfo));
    driverInfo.cbSize = sizeof(driverInfo);

    while (SetupDiEnumDriverInfoW(target->deviceInfoSet, &target->deviceInfoData, SPDIT_CLASSDRIVER, index, &driverInfo)) {
        if (DriverDetailMatchesWinUsb(target->deviceInfoSet, &target->deviceInfoData, &driverInfo)) {
            selectedDriver = driverInfo;
            found = TRUE;
            break;
        }
        ++index;
    }

    if (!found) {
        SetupDiDestroyDriverInfoList(target->deviceInfoSet, &target->deviceInfoData, SPDIT_CLASSDRIVER);
        fwprintf(stderr, L"Could not find the Microsoft WinUsb Device driver node in %ls.\n", winusbInf);
        return FALSE;
    }

    if (!SetupDiSetSelectedDriverW(target->deviceInfoSet, &target->deviceInfoData, &selectedDriver)) {
        PrintWin32Error(L"SetupDiSetSelectedDriver", GetLastError());
        SetupDiDestroyDriverInfoList(target->deviceInfoSet, &target->deviceInfoData, SPDIT_CLASSDRIVER);
        return FALSE;
    }

    wprintf(L"Installing Microsoft WinUsb Device driver from %ls.\n", winusbInf);
    if (!DiInstallDevice(NULL,
                         target->deviceInfoSet,
                         &target->deviceInfoData,
                         &selectedDriver,
                         DIIDFLAG_NOFINISHINSTALLUI,
                         &needReboot)) {
        PrintWin32Error(L"DiInstallDevice", GetLastError());
        SetupDiDestroyDriverInfoList(target->deviceInfoSet, &target->deviceInfoData, SPDIT_CLASSDRIVER);
        return FALSE;
    }

    SetupDiDestroyDriverInfoList(target->deviceInfoSet, &target->deviceInfoData, SPDIT_CLASSDRIVER);
    target->winusbService = TRUE;
    if (needReboot) {
        wprintf(L"Windows reports a reboot may be required, but setup will still try to restart the device instance.\n");
    }
    return TRUE;
}

static BOOL BindWinUsbAndGuid(void)
{
    TargetDevice target;
    BOOL ok = FALSE;

    if (!FindTargetDevice(&target)) {
        return FALSE;
    }

    ok = ForceInstallWinUsb(&target) &&
         WriteDeviceInterfaceGuid(&target) &&
         RestartTargetDevice(&target);

    DestroyTargetDevice(&target);
    Sleep(1500);
    return ok;
}

static BOOL BuildInstallPath(WCHAR* installDir, DWORD installDirCount, WCHAR* exePath, DWORD exePathCount)
{
    WCHAR programFiles[MAX_PATH];

    if (!GetEnvironmentVariableW(L"ProgramFiles", programFiles, ARRAYSIZE(programFiles))) {
        PrintWin32Error(L"GetEnvironmentVariable ProgramFiles", GetLastError());
        return FALSE;
    }

    swprintf_s(installDir, installDirCount, L"%ls\\%ls", programFiles, INSTALL_DIR_SUFFIX);
    swprintf_s(exePath, exePathCount, L"%ls\\%ls", installDir, INSTALLED_EXE_NAME);
    return TRUE;
}

static BOOL EnsureDirectories(const WCHAR* installDir)
{
    WCHAR programData[MAX_PATH];
    WCHAR logDir[MAX_PATH];

    if (SHCreateDirectoryExW(NULL, installDir, NULL) != ERROR_SUCCESS && GetLastError() != ERROR_ALREADY_EXISTS) {
        PrintWin32Error(L"Create install directory", GetLastError());
        return FALSE;
    }

    if (GetEnvironmentVariableW(L"ProgramData", programData, ARRAYSIZE(programData))) {
        swprintf_s(logDir, ARRAYSIZE(logDir), L"%ls\\%ls", programData, LOG_DIR_SUFFIX);
        SHCreateDirectoryExW(NULL, logDir, NULL);
    }

    return TRUE;
}

static BOOL ExtractResourceFile(int resourceId, const WCHAR* destination, const WCHAR* label)
{
    HRSRC resource = FindResourceW(NULL, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    HGLOBAL loaded;
    DWORD size;
    const void* data;
    HANDLE file;
    DWORD written = 0;

    if (!resource) {
        PrintWin32Error(label, GetLastError());
        return FALSE;
    }

    loaded = LoadResource(NULL, resource);
    size = SizeofResource(NULL, resource);
    data = LockResource(loaded);
    if (!loaded || !data || size == 0) {
        fwprintf(stderr, L"Embedded %ls resource is invalid.\n", label);
        return FALSE;
    }

    file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 120; ++attempt) {
        file = CreateFileW(destination, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            break;
        }
        if (GetLastError() != ERROR_SHARING_VIOLATION && GetLastError() != ERROR_LOCK_VIOLATION) {
            break;
        }
        Sleep(500);
    }
    if (file == INVALID_HANDLE_VALUE) {
        PrintWin32Error(destination, GetLastError());
        return FALSE;
    }

    if (!WriteFile(file, data, size, &written, NULL) || written != size) {
        PrintWin32Error(L"Write installed subscreen.exe", GetLastError());
        CloseHandle(file);
        return FALSE;
    }

    CloseHandle(file);
    wprintf(L"Installed %ls (%lu bytes).\n", destination, size);
    return TRUE;
}

static BOOL ExtractSubscreenExe(const WCHAR* destination)
{
    return ExtractResourceFile(IDR_SUBSCREEN_EXE, destination, L"subscreen.exe");
}

static DWORD RunProcessWait(const WCHAR* commandLine)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    WCHAR* mutableCommand;
    DWORD exitCode = 1;
    size_t bytes;

    bytes = (wcslen(commandLine) + 1) * sizeof(WCHAR);
    mutableCommand = (WCHAR*)malloc(bytes);
    if (!mutableCommand) {
        return 1;
    }
    memcpy(mutableCommand, commandLine, bytes);

    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    if (!CreateProcessW(NULL, mutableCommand, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        PrintWin32Error(commandLine, GetLastError());
        free(mutableCommand);
        return 1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    free(mutableCommand);
    return exitCode;
}

static BOOL InstallPawnIoDriver(void)
{
    WCHAR tempDir[MAX_PATH];
    WCHAR setupPath[MAX_PATH];
    WCHAR commandLine[MAX_PATH * 2];
    DWORD exitCode;

    if (!GetTempPathW(ARRAYSIZE(tempDir), tempDir)) {
        PrintWin32Error(L"GetTempPath", GetLastError());
        return FALSE;
    }

    swprintf_s(setupPath, ARRAYSIZE(setupPath), L"%lsHarborSubscreen-PawnIO_setup.exe", tempDir);
    if (!ExtractResourceFile(IDR_PAWNIO_SETUP_EXE, setupPath, L"PawnIO_setup.exe")) {
        return FALSE;
    }

    swprintf_s(commandLine,
               ARRAYSIZE(commandLine),
               L"\"%ls\" -install -silent",
               setupPath);
    exitCode = RunProcessWait(commandLine);
    DeleteFileW(setupPath);
    if (exitCode == 0 || exitCode == ERROR_ALREADY_EXISTS) {
        wprintf(L"PawnIO sensor driver is installed.\n");
        return TRUE;
    }

    if (exitCode != 0) {
        fwprintf(stderr, L"PawnIO setup returned %lu; CPU temperature/fan telemetry may be unavailable.\n", exitCode);
        return FALSE;
    }

    return TRUE;
}

static BOOL StopServiceIfPresent(BOOL deleteService)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    SC_HANDLE service;
    SERVICE_STATUS_PROCESS status;
    DWORD needed;

    if (!scm) {
        return FALSE;
    }

    service = OpenServiceW(scm,
                           SUBSCREEN_SERVICE_NAME,
                           SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_START | (deleteService ? DELETE : 0));
    if (!service) {
        CloseServiceHandle(scm);
        return TRUE;
    }

    if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (LPBYTE)&status, sizeof(status), &needed) &&
        status.dwCurrentState != SERVICE_STOPPED) {
        SERVICE_STATUS stopStatus;
        ControlService(service, SERVICE_CONTROL_STOP, &stopStatus);
        for (int i = 0; i < 120; ++i) {
            Sleep(500);
            if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (LPBYTE)&status, sizeof(status), &needed) &&
                status.dwCurrentState == SERVICE_STOPPED) {
                break;
            }
        }
    }

    if (deleteService) {
        if (DeleteService(service)) {
            wprintf(L"Removed existing HarborOSSubscreenService.\n");
        } else if (GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE) {
            PrintWin32Error(L"DeleteService", GetLastError());
            CloseServiceHandle(service);
            CloseServiceHandle(scm);
            return FALSE;
        }
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return TRUE;
}

static BOOL StartServiceByName(void)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    SC_HANDLE service;
    BOOL ok;

    if (!scm) {
        PrintWin32Error(L"OpenSCManager", GetLastError());
        return FALSE;
    }

    service = OpenServiceW(scm, SUBSCREEN_SERVICE_NAME, SERVICE_START | SERVICE_QUERY_STATUS);
    if (!service) {
        PrintWin32Error(L"OpenService start", GetLastError());
        CloseServiceHandle(scm);
        return FALSE;
    }

    ok = StartServiceW(service, 0, NULL);
    if (!ok && GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
        ok = TRUE;
    }
    if (!ok) {
        PrintWin32Error(L"StartService", GetLastError());
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return ok;
}

static int InstallFlow(void)
{
    WCHAR installDir[MAX_PATH];
    WCHAR exePath[MAX_PATH];
    WCHAR ecModulePath[MAX_PATH];
    WCHAR commandLine[MAX_PATH * 2];

    if (!BuildInstallPath(installDir, ARRAYSIZE(installDir), exePath, ARRAYSIZE(exePath))) {
        return 1;
    }
    swprintf_s(ecModulePath, ARRAYSIZE(ecModulePath), L"%ls\\LpcACPIEC.bin", installDir);
    if (!EnsureDirectories(installDir)) {
        return 1;
    }

    StopServiceIfPresent(TRUE);
    InstallPawnIoDriver();

    if (!BindWinUsbAndGuid()) {
        fwprintf(stderr, L"WinUSB binding failed; setup cannot continue.\n");
        return 1;
    }

    if (!ExtractSubscreenExe(exePath)) {
        return 1;
    }
    if (!ExtractResourceFile(IDR_PAWNIO_LPC_ACPIEC_BIN, ecModulePath, L"LpcACPIEC.bin")) {
        return 1;
    }

    swprintf_s(commandLine, ARRAYSIZE(commandLine), L"\"%ls\" selftest", exePath);
    if (RunProcessWait(commandLine) != 0) {
        fwprintf(stderr, L"subscreen.exe selftest failed.\n");
        return 1;
    }

    swprintf_s(commandLine, ARRAYSIZE(commandLine), L"\"%ls\" install", exePath);
    if (RunProcessWait(commandLine) != 0) {
        fwprintf(stderr, L"Service installation failed.\n");
        return 1;
    }

    swprintf_s(commandLine, ARRAYSIZE(commandLine), L"\"%ls\" setup-check", exePath);
    if (RunProcessWait(commandLine) != 0) {
        fwprintf(stderr, L"Setup finished but device setup-check did not pass.\n");
        return 1;
    }

    if (!StartServiceByName()) {
        return 1;
    }

    wprintf(L"\nHarborOS Subscreen setup completed successfully.\n");
    return 0;
}

static int UninstallFlow(void)
{
    WCHAR installDir[MAX_PATH];
    WCHAR exePath[MAX_PATH];
    WCHAR ecModulePath[MAX_PATH];

    if (!BuildInstallPath(installDir, ARRAYSIZE(installDir), exePath, ARRAYSIZE(exePath))) {
        return 1;
    }
    swprintf_s(ecModulePath, ARRAYSIZE(ecModulePath), L"%ls\\LpcACPIEC.bin", installDir);

    StopServiceIfPresent(TRUE);
    DeleteFileW(exePath);
    DeleteFileW(ecModulePath);
    RemoveDirectoryW(installDir);
    wprintf(L"HarborOS Subscreen application removed. WinUSB binding was left intact.\n");
    return 0;
}

int wmain(int argc, wchar_t* argv[])
{
    if (!IsElevated()) {
        if (RelaunchElevated()) {
            return 0;
        }
        fwprintf(stderr, L"Administrator elevation is required.\n");
        return 1;
    }

    if (argc > 1 && (_wcsicmp(argv[1], L"/uninstall") == 0 || _wcsicmp(argv[1], L"uninstall") == 0)) {
        return UninstallFlow();
    }

    return InstallFlow();
}
