#include "usb_comm.h"
#include <windows.h>
#include <stdio.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidusage.h>
#include <initguid.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

// USB Device constants
#define VENDOR_ID   0x5448
#define PRODUCT_ID  0x0002
#define TIMEOUT_MS  5000

BOOL FindUSBDevice(HANDLE* deviceHandle)
{
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO deviceInfoSet = SetupDiGetClassDevs(&hidGuid, NULL, NULL,
                                                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (deviceInfoSet == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }

    SP_DEVICE_INTERFACE_DATA deviceInterfaceData;
    deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    DWORD memberIndex = 0;

    while (SetupDiEnumDeviceInterfaces(deviceInfoSet, NULL, &hidGuid, memberIndex,
                                      &deviceInterfaceData))
    {
        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &deviceInterfaceData, NULL, 0,
                                       &requiredSize, NULL);

        PSP_DEVICE_INTERFACE_DETAIL_DATA deviceInterfaceDetailData =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(requiredSize);
        if (deviceInterfaceDetailData == NULL)
        {
            memberIndex++;
            continue;
        }

        deviceInterfaceDetailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        if (SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &deviceInterfaceData,
                                           deviceInterfaceDetailData, requiredSize,
                                           NULL, NULL))
        {
            HANDLE hDevice = CreateFile(deviceInterfaceDetailData->DevicePath,
                                       GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       NULL, OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                                       NULL);

            if (hDevice != INVALID_HANDLE_VALUE)
            {
                HIDD_ATTRIBUTES attributes;
                attributes.Size = sizeof(HIDD_ATTRIBUTES);

                if (HidD_GetAttributes(hDevice, &attributes))
                {
                    if (attributes.VendorID == VENDOR_ID && attributes.ProductID == PRODUCT_ID)
                    {
                        // Found our device
                        *deviceHandle = hDevice;
                        free(deviceInterfaceDetailData);
                        SetupDiDestroyDeviceInfoList(deviceInfoSet);
                        return TRUE;
                    }
                }

                CloseHandle(hDevice);
            }
        }

        free(deviceInterfaceDetailData);
        memberIndex++;
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return FALSE;
}

BOOL SendDataToDevice(HANDLE deviceHandle, const unsigned char* data, DWORD dataSize)
{
    if (deviceHandle == INVALID_HANDLE_VALUE || data == NULL || dataSize == 0)
    {
        return FALSE;
    }

    // Get device capabilities
    HIDP_CAPS capabilities;
    PHIDP_PREPARSED_DATA preparsedData = NULL;

    if (!HidD_GetPreparsedData(deviceHandle, &preparsedData))
    {
        return FALSE;
    }

    if (!HidP_GetCaps(preparsedData, &capabilities))
    {
        HidD_FreePreparsedData(preparsedData);
        return FALSE;
    }

    HidD_FreePreparsedData(preparsedData);

    // Prepare the report
    DWORD reportSize = capabilities.OutputReportByteLength;
    unsigned char* report = (unsigned char*)malloc(reportSize);
    if (report == NULL)
    {
        return FALSE;
    }

    memset(report, 0, reportSize);
    report[0] = 0x00; // Report ID

    // Copy data to report (ensure it fits)
    DWORD copySize = min(dataSize, reportSize - 1);
    memcpy(&report[1], data, copySize);

    // Send the report
    DWORD bytesWritten;
    OVERLAPPED overlapped = {0};
    overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    if (overlapped.hEvent == NULL)
    {
        free(report);
        return FALSE;
    }

    BOOL result = WriteFile(deviceHandle, report, reportSize, &bytesWritten, &overlapped);

    if (!result)
    {
        if (GetLastError() == ERROR_IO_PENDING)
        {
            // Wait for the operation to complete
            DWORD waitResult = WaitForSingleObject(overlapped.hEvent, TIMEOUT_MS);
            if (waitResult == WAIT_OBJECT_0)
            {
                result = TRUE;
            }
        }
    }

    CloseHandle(overlapped.hEvent);
    free(report);

    return result;
}

BOOL ReceiveDataFromDevice(HANDLE deviceHandle, unsigned char* buffer, DWORD bufferSize, DWORD* bytesRead)
{
    if (deviceHandle == INVALID_HANDLE_VALUE || buffer == NULL || bufferSize == 0)
    {
        return FALSE;
    }

    OVERLAPPED overlapped = {0};
    overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    if (overlapped.hEvent == NULL)
    {
        return FALSE;
    }

    BOOL result = ReadFile(deviceHandle, buffer, bufferSize, bytesRead, &overlapped);

    if (!result)
    {
        if (GetLastError() == ERROR_IO_PENDING)
        {
            DWORD waitResult = WaitForSingleObject(overlapped.hEvent, TIMEOUT_MS);
            if (waitResult == WAIT_OBJECT_0)
            {
                result = GetOverlappedResult(deviceHandle, &overlapped, bytesRead, FALSE);
            }
        }
    }

    CloseHandle(overlapped.hEvent);
    return result;
}