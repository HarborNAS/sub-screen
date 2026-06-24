#include "usb_comm.h"

#include <cfgmgr32.h>
#include <setupapi.h>
#include <stdlib.h>
#include <string.h>
#include <usb.h>
#include <winusb.h>
#include <wchar.h>

#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")

struct SubscreenUsbDevice {
    HANDLE deviceHandle;
    WINUSB_INTERFACE_HANDLE winUsbHandle;
    UCHAR bulkInPipe;
    UCHAR bulkOutPipe;
    WCHAR devicePath[MAX_PATH];
};

static const GUID GUID_DEVINTERFACE_HARBOROS_SUBSCREEN =
    {0x63730607, 0xe5e5, 0x48c9, {0xb9, 0xde, 0xe8, 0x08, 0xf1, 0x1c, 0x6a, 0x35}};

static DWORD g_lastUsbError = ERROR_SUCCESS;

const GUID* SubscreenDeviceInterfaceGuid(void)
{
    return &GUID_DEVINTERFACE_HARBOROS_SUBSCREEN;
}

DWORD UsbLastError(void)
{
    return g_lastUsbError;
}

static void SetUsbLastError(DWORD error)
{
    g_lastUsbError = error;
}

static BOOL WideToUtf8(const WCHAR* value, char* buffer, DWORD bufferSize)
{
    int written;

    if (!value || !buffer || bufferSize == 0) {
        return FALSE;
    }

    written = WideCharToMultiByte(CP_UTF8, 0, value, -1, buffer, (int)bufferSize, NULL, NULL);
    if (written <= 0) {
        strncpy_s(buffer, bufferSize, "<conversion failed>", _TRUNCATE);
        return FALSE;
    }
    return TRUE;
}

static void PrintWideValue(FILE* output, const char* label, const WCHAR* value)
{
    char utf8[1024];

    if (!value || value[0] == L'\0') {
        fprintf(output, "%s<empty>\n", label);
        return;
    }

    WideToUtf8(value, utf8, sizeof(utf8));
    fprintf(output, "%s%s\n", label, utf8);
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

static BOOL ReadDeviceInterfaceGuidValue(HDEVINFO deviceInfoSet,
                                         PSP_DEVINFO_DATA deviceInfoData,
                                         WCHAR* buffer,
                                         DWORD bufferCount)
{
    HKEY key;
    DWORD type = 0;
    DWORD cb;
    BOOL ok = FALSE;

    if (!buffer || bufferCount == 0) {
        return FALSE;
    }
    buffer[0] = L'\0';

    key = SetupDiOpenDevRegKey(deviceInfoSet,
                               deviceInfoData,
                               DICS_FLAG_GLOBAL,
                               0,
                               DIREG_DEV,
                               KEY_READ);
    if (key == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    cb = bufferCount * sizeof(WCHAR);
    if (RegQueryValueExW(key, L"DeviceInterfaceGUIDs", NULL, &type, (LPBYTE)buffer, &cb) == ERROR_SUCCESS &&
        (type == REG_MULTI_SZ || type == REG_SZ)) {
        ok = TRUE;
    } else {
        cb = bufferCount * sizeof(WCHAR);
        if (RegQueryValueExW(key, L"DeviceInterfaceGUID", NULL, &type, (LPBYTE)buffer, &cb) == ERROR_SUCCESS &&
            type == REG_SZ) {
            ok = TRUE;
        }
    }

    RegCloseKey(key);
    return ok;
}

static BOOL DeviceHasExpectedGuid(HDEVINFO deviceInfoSet, PSP_DEVINFO_DATA deviceInfoData)
{
    WCHAR guidValue[512];

    if (!ReadDeviceInterfaceGuidValue(deviceInfoSet, deviceInfoData, guidValue, ARRAYSIZE(guidValue))) {
        return FALSE;
    }

    return wcsstr(guidValue, L"{63730607-e5e5-48c9-b9de-e808f11c6a35}") != NULL ||
           wcsstr(guidValue, L"{63730607-E5E5-48C9-B9DE-E808F11C6A35}") != NULL;
}

static BOOL PrintTargetPnpDevices(FILE* output)
{
    HDEVINFO deviceInfoSet;
    SP_DEVINFO_DATA deviceInfoData;
    DWORD index = 0;
    BOOL found = FALSE;

    deviceInfoSet = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        fprintf(output, "PnP scan failed (%lu).\n", GetLastError());
        return FALSE;
    }

    memset(&deviceInfoData, 0, sizeof(deviceInfoData));
    deviceInfoData.cbSize = sizeof(deviceInfoData);

    while (SetupDiEnumDeviceInfo(deviceInfoSet, index, &deviceInfoData)) {
        WCHAR hardwareIds[2048];

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

        found = TRUE;
        fprintf(output, "Target USB device is present in PnP.\n");
        PrintWideValue(output, "  HardwareId: ", hardwareIds);

        {
            WCHAR friendlyName[512];
            if (GetDeviceStringProperty(deviceInfoSet,
                                        &deviceInfoData,
                                        SPDRP_FRIENDLYNAME,
                                        friendlyName,
                                        ARRAYSIZE(friendlyName))) {
                PrintWideValue(output, "  FriendlyName: ", friendlyName);
            }
        }

        {
            WCHAR deviceDesc[512];
            if (GetDeviceStringProperty(deviceInfoSet,
                                        &deviceInfoData,
                                        SPDRP_DEVICEDESC,
                                        deviceDesc,
                                        ARRAYSIZE(deviceDesc))) {
                PrintWideValue(output, "  DeviceDesc: ", deviceDesc);
            }
        }

        {
            WCHAR service[256];
            if (GetDeviceStringProperty(deviceInfoSet,
                                        &deviceInfoData,
                                        SPDRP_SERVICE,
                                        service,
                                        ARRAYSIZE(service))) {
                PrintWideValue(output, "  Service: ", service);
            } else {
                fprintf(output, "  Service: <none>\n");
            }
        }

        {
            WCHAR guidValue[512];
            if (ReadDeviceInterfaceGuidValue(deviceInfoSet, &deviceInfoData, guidValue, ARRAYSIZE(guidValue))) {
                PrintWideValue(output, "  DeviceInterfaceGUIDs: ", guidValue);
            } else {
                fprintf(output, "  DeviceInterfaceGUIDs: <none>\n");
            }
        }

        {
            WCHAR className[256];
            if (GetDeviceStringProperty(deviceInfoSet,
                                        &deviceInfoData,
                                        SPDRP_CLASS,
                                        className,
                                        ARRAYSIZE(className))) {
                PrintWideValue(output, "  Class: ", className);
            } else {
                fprintf(output, "  Class: <none>\n");
            }
        }

        {
            ULONG status = 0;
            ULONG problem = 0;
            CONFIGRET cr = CM_Get_DevNode_Status(&status, &problem, deviceInfoData.DevInst, 0);
            if (cr == CR_SUCCESS) {
                fprintf(output, "  PnP status=0x%08lX problem=%lu\n", status, problem);
            }
        }
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return found;
}

static BOOL ScanTargetSetupStatus(BOOL* present, BOOL* winusbService, BOOL* guidRegistered)
{
    HDEVINFO deviceInfoSet;
    SP_DEVINFO_DATA deviceInfoData;
    DWORD index = 0;

    if (present) {
        *present = FALSE;
    }
    if (winusbService) {
        *winusbService = FALSE;
    }
    if (guidRegistered) {
        *guidRegistered = FALSE;
    }

    deviceInfoSet = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
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

        if (present) {
            *present = TRUE;
        }

        if (winusbService &&
            GetDeviceStringProperty(deviceInfoSet, &deviceInfoData, SPDRP_SERVICE, service, ARRAYSIZE(service)) &&
            _wcsicmp(service, L"WINUSB") == 0) {
            *winusbService = TRUE;
        }

        if (guidRegistered && DeviceHasExpectedGuid(deviceInfoSet, &deviceInfoData)) {
            *guidRegistered = TRUE;
        }
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return TRUE;
}

static BOOL IsSubscreenServiceRunning(void)
{
    SC_HANDLE scm;
    SC_HANDLE service;
    SERVICE_STATUS_PROCESS status;
    DWORD needed = 0;
    BOOL running = FALSE;

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        return FALSE;
    }

    service = OpenServiceW(scm, L"HarborOSSubscreenService", SERVICE_QUERY_STATUS);
    if (!service) {
        CloseServiceHandle(scm);
        return FALSE;
    }

    if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (LPBYTE)&status, sizeof(status), &needed)) {
        running = status.dwCurrentState == SERVICE_RUNNING ||
                  status.dwCurrentState == SERVICE_START_PENDING;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return running;
}

static BOOL IsBulkInPipe(UCHAR pipeId)
{
    return (pipeId & USB_ENDPOINT_DIRECTION_MASK) != 0;
}

static BOOL QueryPipes(SubscreenUsbDevice* device, FILE* output)
{
    USB_INTERFACE_DESCRIPTOR interfaceDescriptor;
    BOOL foundIn = FALSE;
    BOOL foundOut = FALSE;

    if (!WinUsb_QueryInterfaceSettings(device->winUsbHandle, 0, &interfaceDescriptor)) {
        if (output) {
            fprintf(output, "WinUsb_QueryInterfaceSettings failed (%lu)\n", GetLastError());
        }
        SetUsbLastError(GetLastError());
        return FALSE;
    }

    if (output) {
        fprintf(output, "Interface: %u endpoint(s)\n", interfaceDescriptor.bNumEndpoints);
    }

    for (UCHAR i = 0; i < interfaceDescriptor.bNumEndpoints; ++i) {
        WINUSB_PIPE_INFORMATION pipeInfo;
        if (!WinUsb_QueryPipe(device->winUsbHandle, 0, i, &pipeInfo)) {
            if (output) {
                fprintf(output, "WinUsb_QueryPipe(%u) failed (%lu)\n", i, GetLastError());
            }
            SetUsbLastError(GetLastError());
            continue;
        }

        if (output) {
            fprintf(output, "Pipe %u: id=0x%02X type=%u maxPacket=%u interval=%u\n",
                    i,
                    pipeInfo.PipeId,
                    pipeInfo.PipeType,
                    pipeInfo.MaximumPacketSize,
                    pipeInfo.Interval);
        }

        if (pipeInfo.PipeType != UsbdPipeTypeBulk) {
            continue;
        }

        if (pipeInfo.PipeId == SUBSCREEN_EXPECTED_EP_IN) {
            device->bulkInPipe = pipeInfo.PipeId;
            foundIn = TRUE;
        } else if (pipeInfo.PipeId == SUBSCREEN_EXPECTED_EP_OUT) {
            device->bulkOutPipe = pipeInfo.PipeId;
            foundOut = TRUE;
        } else if (IsBulkInPipe(pipeInfo.PipeId) && !foundIn) {
            device->bulkInPipe = pipeInfo.PipeId;
            foundIn = TRUE;
        } else if (!IsBulkInPipe(pipeInfo.PipeId) && !foundOut) {
            device->bulkOutPipe = pipeInfo.PipeId;
            foundOut = TRUE;
        }
    }

    if (output) {
        fprintf(output, "Selected bulk OUT=0x%02X IN=0x%02X\n", device->bulkOutPipe, device->bulkInPipe);
        if (!foundIn || !foundOut) {
            fprintf(output,
                    "Endpoint mismatch: WinUSB is bound, but both bulk directions were not found. Expected OUT=0x%02X IN=0x%02X.\n",
                    SUBSCREEN_EXPECTED_EP_OUT,
                    SUBSCREEN_EXPECTED_EP_IN);
        } else if (device->bulkInPipe != SUBSCREEN_EXPECTED_EP_IN || device->bulkOutPipe != SUBSCREEN_EXPECTED_EP_OUT) {
            fprintf(output,
                    "Endpoint warning: using fallback bulk endpoints instead of expected OUT=0x%02X IN=0x%02X.\n",
                    SUBSCREEN_EXPECTED_EP_OUT,
                    SUBSCREEN_EXPECTED_EP_IN);
        }
    }

    if (!foundIn || !foundOut) {
        SetUsbLastError(ERROR_NOT_FOUND);
    }
    return foundIn && foundOut;
}

static BOOL OpenDevicePath(const WCHAR* devicePath, SubscreenUsbDevice** outDevice, FILE* output)
{
    SubscreenUsbDevice* device = (SubscreenUsbDevice*)calloc(1, sizeof(SubscreenUsbDevice));
    if (!device) {
        return FALSE;
    }

    device->deviceHandle = CreateFileW(devicePath,
                                       GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       NULL,
                                       OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                                       NULL);
    if (device->deviceHandle == INVALID_HANDLE_VALUE) {
        if (output) {
            fprintf(output, "CreateFileW failed (%lu)\n", GetLastError());
        }
        SetUsbLastError(GetLastError());
        free(device);
        return FALSE;
    }

    if (!WinUsb_Initialize(device->deviceHandle, &device->winUsbHandle)) {
        if (output) {
            fprintf(output, "WinUsb_Initialize failed (%lu)\n", GetLastError());
        }
        SetUsbLastError(GetLastError());
        CloseHandle(device->deviceHandle);
        free(device);
        return FALSE;
    }

    wcsncpy_s(device->devicePath, MAX_PATH, devicePath, _TRUNCATE);

    if (!QueryPipes(device, output)) {
        if (output) {
            fprintf(output, "Bulk pipe discovery failed. Expected OUT=0x%02X IN=0x%02X.\n",
                    SUBSCREEN_EXPECTED_EP_OUT,
                    SUBSCREEN_EXPECTED_EP_IN);
        }
        UsbClose(device);
        return FALSE;
    }

    *outDevice = device;
    return TRUE;
}

BOOL UsbOpen(SubscreenUsbDevice** outDevice)
{
    HDEVINFO deviceInfoSet;
    SP_DEVICE_INTERFACE_DATA interfaceData;
    DWORD index = 0;
    BOOL opened = FALSE;

    SetUsbLastError(ERROR_SUCCESS);

    if (!outDevice) {
        SetUsbLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *outDevice = NULL;

    deviceInfoSet = SetupDiGetClassDevsW(SubscreenDeviceInterfaceGuid(),
                                         NULL,
                                         NULL,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        SetUsbLastError(GetLastError());
        return FALSE;
    }

    memset(&interfaceData, 0, sizeof(interfaceData));
    interfaceData.cbSize = sizeof(interfaceData);

    while (SetupDiEnumDeviceInterfaces(deviceInfoSet, NULL, SubscreenDeviceInterfaceGuid(), index, &interfaceData)) {
        DWORD requiredSize = 0;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detailData;

        SetupDiGetDeviceInterfaceDetailW(deviceInfoSet, &interfaceData, NULL, 0, &requiredSize, NULL);
        if (requiredSize == 0) {
            ++index;
            continue;
        }

        detailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)calloc(1, requiredSize);
        if (!detailData) {
            SetUsbLastError(ERROR_OUTOFMEMORY);
            break;
        }

        detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(deviceInfoSet,
                                             &interfaceData,
                                             detailData,
                                             requiredSize,
                                             NULL,
                                             NULL)) {
            opened = OpenDevicePath(detailData->DevicePath, outDevice, NULL);
        }

        free(detailData);
        if (opened) {
            break;
        }

        ++index;
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    if (!opened && g_lastUsbError == ERROR_SUCCESS) {
        SetUsbLastError(ERROR_NOT_FOUND);
    }
    return opened;
}

void UsbClose(SubscreenUsbDevice* device)
{
    if (!device) {
        return;
    }

    if (device->winUsbHandle) {
        WinUsb_Free(device->winUsbHandle);
        device->winUsbHandle = NULL;
    }

    if (device->deviceHandle && device->deviceHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(device->deviceHandle);
        device->deviceHandle = INVALID_HANDLE_VALUE;
    }

    free(device);
}

BOOL UsbWrite(SubscreenUsbDevice* device, const uint8_t* data, DWORD dataSize, DWORD timeoutMs)
{
    ULONG transferred = 0;
    ULONG timeout = timeoutMs;

    if (!device || !device->winUsbHandle || !data || dataSize == 0 || device->bulkOutPipe == 0) {
        SetUsbLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!WinUsb_SetPipePolicy(device->winUsbHandle, device->bulkOutPipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout)) {
        SetUsbLastError(GetLastError());
        return FALSE;
    }

    if (!WinUsb_WritePipe(device->winUsbHandle, device->bulkOutPipe, (PUCHAR)data, dataSize, &transferred, NULL)) {
        SetUsbLastError(GetLastError());
        return FALSE;
    }

    if (transferred != dataSize) {
        SetUsbLastError(ERROR_WRITE_FAULT);
        return FALSE;
    }
    SetUsbLastError(ERROR_SUCCESS);
    return transferred == dataSize;
}

BOOL UsbRead(SubscreenUsbDevice* device, uint8_t* buffer, DWORD bufferSize, DWORD* bytesRead, DWORD timeoutMs)
{
    ULONG transferred = 0;
    ULONG timeout = timeoutMs;

    if (bytesRead) {
        *bytesRead = 0;
    }

    if (!device || !device->winUsbHandle || !buffer || bufferSize == 0 || device->bulkInPipe == 0) {
        SetUsbLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!WinUsb_SetPipePolicy(device->winUsbHandle, device->bulkInPipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout)) {
        SetUsbLastError(GetLastError());
        return FALSE;
    }

    if (!WinUsb_ReadPipe(device->winUsbHandle, device->bulkInPipe, buffer, bufferSize, &transferred, NULL)) {
        SetUsbLastError(GetLastError());
        return FALSE;
    }

    if (bytesRead) {
        *bytesRead = transferred;
    }
    SetUsbLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL UsbProbe(FILE* output)
{
    HDEVINFO deviceInfoSet;
    SP_DEVICE_INTERFACE_DATA interfaceData;
    DWORD index = 0;
    BOOL found = FALSE;
    BOOL interfaceSeen = FALSE;
    BOOL pnpPresent;

    if (!output) {
        output = stdout;
    }

    fprintf(output, "HarborOS Subscreen WinUSB probe\n");
    fprintf(output, "VID=0x%04X PID=0x%04X\n", SUBSCREEN_VENDOR_ID, SUBSCREEN_PRODUCT_ID);
    fprintf(output, "Interface GUID={63730607-e5e5-48c9-b9de-e808f11c6a35}\n");
    pnpPresent = PrintTargetPnpDevices(output);
    if (!pnpPresent) {
        fprintf(output, "Target USB device is not present in PnP. Plug in USB\\VID_5448&PID_0002 first.\n");
    }

    deviceInfoSet = SetupDiGetClassDevsW(SubscreenDeviceInterfaceGuid(),
                                         NULL,
                                         NULL,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        fprintf(output, "SetupDiGetClassDevs failed (%lu). Is the INF installed?\n", GetLastError());
        if (pnpPresent) {
            fprintf(output, "Diagnosis: target device exists, but the WinUSB interface GUID is not registered.\n");
        }
        return FALSE;
    }

    memset(&interfaceData, 0, sizeof(interfaceData));
    interfaceData.cbSize = sizeof(interfaceData);

    while (SetupDiEnumDeviceInterfaces(deviceInfoSet, NULL, SubscreenDeviceInterfaceGuid(), index, &interfaceData)) {
        DWORD requiredSize = 0;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detailData;
        SubscreenUsbDevice* device = NULL;

        interfaceSeen = TRUE;
        SetupDiGetDeviceInterfaceDetailW(deviceInfoSet, &interfaceData, NULL, 0, &requiredSize, NULL);
        detailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)calloc(1, requiredSize);
        if (!detailData) {
            fprintf(output, "Out of memory while reading interface detail.\n");
            break;
        }

        detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(deviceInfoSet,
                                             &interfaceData,
                                             detailData,
                                             requiredSize,
                                             NULL,
                                             NULL)) {
            char devicePath[1024];
            WideToUtf8(detailData->DevicePath, devicePath, sizeof(devicePath));
            fprintf(output, "Device path: %s\n", devicePath);
            if (OpenDevicePath(detailData->DevicePath, &device, output)) {
                found = TRUE;
                UsbClose(device);
            } else {
                fprintf(output, "Diagnosis: WinUSB interface is registered, but it could not be opened or its endpoints do not match.\n");
            }
        }
        free(detailData);
        ++index;
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    if (!found) {
        if (pnpPresent && !interfaceSeen) {
            fprintf(output, "No WinUSB interface registered for the target device.\n");
            fprintf(output, "Diagnosis: the device is enumerated, but it is not bound to WinUSB. Install a signed WinUSB binding package or add Microsoft OS descriptors in firmware.\n");
        } else if (!pnpPresent) {
            fprintf(output, "No usable WinUSB subscreen interface found because the USB device is absent.\n");
        } else {
            fprintf(output, "No usable WinUSB subscreen interface found.\n");
        }
    }
    return found;
}

BOOL UsbSetupCheck(FILE* output)
{
    BOOL present = FALSE;
    BOOL winusbService = FALSE;
    BOOL guidRegistered = FALSE;
    BOOL endpoints = FALSE;
    BOOL serviceRunning = FALSE;
    SubscreenUsbDevice* device = NULL;

    if (!output) {
        output = stdout;
    }

    fprintf(output, "HarborOS Subscreen setup check\n");
    ScanTargetSetupStatus(&present, &winusbService, &guidRegistered);

    fprintf(output, "  Device present: %s\n", present ? "PASS" : "FAIL");
    fprintf(output, "  WinUSB service: %s\n", winusbService ? "PASS" : "FAIL");
    fprintf(output, "  DeviceInterfaceGUIDs: %s\n", guidRegistered ? "PASS" : "FAIL");

    if (UsbOpen(&device)) {
        endpoints = TRUE;
        UsbClose(device);
    }
    serviceRunning = IsSubscreenServiceRunning();
    if (!endpoints && present && winusbService && guidRegistered && serviceRunning) {
        endpoints = TRUE;
        fprintf(output,
                "  Bulk endpoints OUT=0x%02X IN=0x%02X: PASS (service owns device)\n",
                SUBSCREEN_EXPECTED_EP_OUT,
                SUBSCREEN_EXPECTED_EP_IN);
    } else {
        fprintf(output,
                "  Bulk endpoints OUT=0x%02X IN=0x%02X: %s\n",
                SUBSCREEN_EXPECTED_EP_OUT,
                SUBSCREEN_EXPECTED_EP_IN,
                endpoints ? "PASS" : "FAIL");
    }

    if (!present) {
        fprintf(output, "Diagnosis: USB\\VID_5448&PID_0002 is not currently present.\n");
    } else if (!winusbService) {
        fprintf(output, "Diagnosis: device is present, but the current function driver is not WinUSB.\n");
    } else if (!guidRegistered) {
        fprintf(output, "Diagnosis: WinUSB is installed, but the HarborOS device interface GUID is missing.\n");
    } else if (!endpoints) {
        fprintf(output, "Diagnosis: WinUSB/GUID are present, but the device could not be opened or bulk endpoints do not match.\n");
    } else if (serviceRunning) {
        fprintf(output, "Setup check: PASS (service is running and owns the USB handle)\n");
    } else {
        fprintf(output, "Setup check: PASS\n");
    }

    return present && winusbService && guidRegistered && endpoints;
}
