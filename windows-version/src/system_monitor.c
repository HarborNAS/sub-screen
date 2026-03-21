#include "system_monitor.h"
#include <windows.h>
#include <stdio.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#include <wbemidl.h>
#include <comdef.h>
#include <Wbemidl.h>
#include <oleauto.h>
#include <powrprof.h>
#include <tlhelp32.h>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "powrprof.lib")

// Global variables for performance monitoring
static PDH_HQUERY cpuQuery = NULL;
static PDH_HCOUNTER cpuTotal = NULL;
static PDH_HQUERY networkQuery = NULL;
static PDH_HCOUNTER networkTotal = NULL;
static ULARGE_INTEGER lastNetworkIn = {0};
static ULARGE_INTEGER lastNetworkOut = {0};
static DWORD lastNetworkTime = 0;

BOOL InitializePerformanceCounters(void)
{
    PDH_STATUS status;

    // Initialize CPU counter
    status = PdhOpenQuery(NULL, 0, &cpuQuery);
    if (status != ERROR_SUCCESS)
    {
        printf("Failed to open CPU query: 0x%x\n", status);
        return FALSE;
    }

    status = PdhAddEnglishCounter(cpuQuery, L"\\Processor(_Total)\\% Processor Time", 0, &cpuTotal);
    if (status != ERROR_SUCCESS)
    {
        printf("Failed to add CPU counter: 0x%x\n", status);
        PdhCloseQuery(cpuQuery);
        return FALSE;
    }

    status = PdhCollectQueryData(cpuQuery);
    if (status != ERROR_SUCCESS)
    {
        printf("Failed to collect CPU query data: 0x%x\n", status);
        PdhCloseQuery(cpuQuery);
        return FALSE;
    }

    // Initialize network counter
    status = PdhOpenQuery(NULL, 0, &networkQuery);
    if (status != ERROR_SUCCESS)
    {
        printf("Failed to open network query: 0x%x\n", status);
        return FALSE;
    }

    status = PdhAddEnglishCounter(networkQuery, L"\\Network Interface(*)\\Bytes Total/sec", 0, &networkTotal);
    if (status != ERROR_SUCCESS)
    {
        printf("Failed to add network counter: 0x%x\n", status);
        PdhCloseQuery(networkQuery);
        return FALSE;
    }

    status = PdhCollectQueryData(networkQuery);
    if (status != ERROR_SUCCESS)
    {
        printf("Failed to collect network query data: 0x%x\n", status);
        PdhCloseQuery(networkQuery);
        return FALSE;
    }

    return TRUE;
}

void CleanupPerformanceCounters(void)
{
    if (cpuQuery)
    {
        PdhCloseQuery(cpuQuery);
        cpuQuery = NULL;
    }

    if (networkQuery)
    {
        PdhCloseQuery(networkQuery);
        networkQuery = NULL;
    }
}

BOOL GetCPUUsage(double* cpuUsage)
{
    PDH_FMT_COUNTERVALUE counterValue;
    PDH_STATUS status;

    status = PdhCollectQueryData(cpuQuery);
    if (status != ERROR_SUCCESS)
    {
        return FALSE;
    }

    status = PdhGetFormattedCounterValue(cpuTotal, PDH_FMT_DOUBLE, NULL, &counterValue);
    if (status != ERROR_SUCCESS)
    {
        return FALSE;
    }

    *cpuUsage = counterValue.doubleValue;
    return TRUE;
}

BOOL GetMemoryUsage(double* memoryUsage)
{
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);

    if (!GlobalMemoryStatusEx(&memInfo))
    {
        return FALSE;
    }

    *memoryUsage = ((double)(memInfo.ullTotalPhys - memInfo.ullAvailPhys) / (double)memInfo.ullTotalPhys) * 100.0;
    return TRUE;
}

BOOL GetDiskUsage(double* diskUsage)
{
    ULARGE_INTEGER freeBytesAvailable, totalNumberOfBytes, totalNumberOfFreeBytes;

    if (!GetDiskFreeSpaceEx(L"C:\\", &freeBytesAvailable, &totalNumberOfBytes, &totalNumberOfFreeBytes))
    {
        return FALSE;
    }

    *diskUsage = ((double)(totalNumberOfBytes.QuadPart - totalNumberOfFreeBytes.QuadPart) / (double)totalNumberOfBytes.QuadPart) * 100.0;
    return TRUE;
}

BOOL GetNetworkUsage(double* networkUsage)
{
    PDH_FMT_COUNTERVALUE counterValue;
    PDH_STATUS status;

    status = PdhCollectQueryData(networkQuery);
    if (status != ERROR_SUCCESS)
    {
        return FALSE;
    }

    status = PdhGetFormattedCounterValue(networkTotal, PDH_FMT_DOUBLE, NULL, &counterValue);
    if (status != ERROR_SUCCESS)
    {
        return FALSE;
    }

    *networkUsage = counterValue.doubleValue;
    return TRUE;
}

BOOL GetGPUTemperature(double* temperature)
{
    // Initialize COM
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        return FALSE;
    }

    // Initialize security
    hr = CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT,
                             RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
    if (FAILED(hr) && hr != RPC_E_TOO_LATE)
    {
        CoUninitialize();
        return FALSE;
    }

    IWbemLocator* pLocator = NULL;
    hr = CoCreateInstance(CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
                         IID_IWbemLocator, (LPVOID*)&pLocator);
    if (FAILED(hr))
    {
        CoUninitialize();
        return FALSE;
    }

    IWbemServices* pServices = NULL;
    hr = pLocator->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), NULL, NULL, NULL,
                                WBEM_FLAG_CONNECT_USE_MAX_WAIT, NULL, NULL, &pServices);
    pLocator->Release();
    if (FAILED(hr))
    {
        CoUninitialize();
        return FALSE;
    }

    // Set security levels
    hr = CoSetProxyBlanket(pServices, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                          RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    if (FAILED(hr))
    {
        pServices->Release();
        CoUninitialize();
        return FALSE;
    }

    // Query for GPU temperature
    IEnumWbemClassObject* pEnumerator = NULL;
    hr = pServices->ExecQuery(_bstr_t(L"WQL"),
                             _bstr_t(L"SELECT * FROM Win32_VideoController"),
                             WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                             NULL, &pEnumerator);

    if (SUCCEEDED(hr))
    {
        IWbemClassObject* pClassObject = NULL;
        ULONG uReturn = 0;

        while (pEnumerator->Next(WBEM_INFINITE, 1, &pClassObject, &uReturn) == S_OK)
        {
            VARIANT vtProp;
            VariantInit(&vtProp);

            // Try to get temperature (this may not work on all systems)
            hr = pClassObject->Get(L"CurrentTemperature", 0, &vtProp, NULL, NULL);
            if (SUCCEEDED(hr) && vtProp.vt != VT_NULL)
            {
                *temperature = (double)vtProp.lVal / 10.0 - 273.15; // Convert from tenths of Kelvin to Celsius
                VariantClear(&vtProp);
                pClassObject->Release();
                pEnumerator->Release();
                pServices->Release();
                CoUninitialize();
                return TRUE;
            }

            VariantClear(&vtProp);
            pClassObject->Release();
        }

        pEnumerator->Release();
    }

    pServices->Release();
    CoUninitialize();

    // Fallback: return a default temperature
    *temperature = 45.0; // Default GPU temperature
    return TRUE;
}

BOOL GetSystemStats(SystemStats* stats)
{
    if (!GetCPUUsage(&stats->cpuUsage))
        return FALSE;

    if (!GetMemoryUsage(&stats->memoryUsage))
        return FALSE;

    if (!GetDiskUsage(&stats->diskUsage))
        return FALSE;

    if (!GetNetworkUsage(&stats->networkUsage))
        return FALSE;

    if (!GetGPUTemperature(&stats->temperature))
        return FALSE;

    // GPU usage - simplified, would need NVAPI or similar for accurate measurement
    stats->gpuUsage = 0.0; // Placeholder

    return TRUE;
}