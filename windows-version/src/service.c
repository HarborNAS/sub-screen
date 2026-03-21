#include "service.h"
#include "system_monitor.h"
#include "usb_comm.h"
#include "protocol.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// Global variables
SERVICE_STATUS          g_ServiceStatus;
SERVICE_STATUS_HANDLE   g_StatusHandle;
HANDLE                  g_ServiceStopEvent;
HANDLE                  g_hDevice = INVALID_HANDLE_VALUE;

// Service name
#define SERVICE_NAME L"HarborOSSubscreenService"

void ReportServiceStatus(DWORD currentState, DWORD win32ExitCode, DWORD waitHint)
{
    static DWORD checkPoint = 1;

    g_ServiceStatus.dwCurrentState = currentState;
    g_ServiceStatus.dwWin32ExitCode = win32ExitCode;
    g_ServiceStatus.dwWaitHint = waitHint;

    if (currentState == SERVICE_START_PENDING)
        g_ServiceStatus.dwControlsAccepted = 0;
    else
        g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;

    if ((currentState == SERVICE_RUNNING) || (currentState == SERVICE_STOPPED))
        g_ServiceStatus.dwCheckPoint = 0;
    else
        g_ServiceStatus.dwCheckPoint = checkPoint++;

    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}

void WINAPI ServiceCtrlHandler(DWORD ctrlCode)
{
    switch (ctrlCode)
    {
    case SERVICE_CONTROL_STOP:
        ReportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 0);

        // Signal the service to stop
        SetEvent(g_ServiceStopEvent);
        ReportServiceStatus(g_ServiceStatus.dwCurrentState, NO_ERROR, 0);
        break;

    default:
        break;
    }
}

DWORD WINAPI ServiceWorkerThread(LPVOID lpParam)
{
    // Initialize performance counters
    if (!InitializePerformanceCounters())
    {
        ReportServiceStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 0);
        return 1;
    }

    // Try to find USB device
    if (!FindUSBDevice(&g_hDevice))
    {
        // Device not found, but continue running (could be connected later)
        OutputDebugString(L"USB device not found. Service will continue running.");
    }

    // Main service loop
    while (WaitForSingleObject(g_ServiceStopEvent, 1000) != WAIT_OBJECT_0)
    {
        SystemStats stats;

        if (GetSystemStats(&stats))
        {
            // Send data to device if connected
            if (g_hDevice != INVALID_HANDLE_VALUE)
            {
                Request request;
                unsigned char buffer[256];
                int reportSize;

                // Send CPU data (System_AIM with index 0)
                reportSize = init_hidreport(&request, SET, System_AIM, 0);
                request.system_data.system_info.sys_id = 0; // CPU
                request.system_data.system_info.usage = (unsigned char)(stats.cpuUsage * 2.55); // Convert to 0-255 range
                request.system_data.system_info.temerature = (unsigned char)stats.temperature;
                request.system_data.system_info.rpm = 0; // Not available on Windows
                append_crc(&request);
                memcpy(buffer, &request, reportSize);
                SendDataToDevice(g_hDevice, buffer, reportSize);

                Sleep(100); // Small delay between packets

                // Send Memory data (System_AIM with index 2)
                reportSize = init_hidreport(&request, SET, System_AIM, 2);
                request.memory_data.memory_info.usage = (unsigned int)(stats.memoryUsage * 100); // Convert to percentage * 100
                append_crc(&request);
                memcpy(buffer, &request, reportSize);
                SendDataToDevice(g_hDevice, buffer, reportSize);

                Sleep(100);

                // Send Disk data (Disk_AIM with index 0)
                reportSize = init_hidreport(&request, SET, Disk_AIM, 0);
                request.disk_data.disk_info.disk_id = 1;
                request.disk_data.disk_info.unit = 3; // GB
                request.disk_data.disk_info.total_size = 1000; // Placeholder
                request.disk_data.disk_info.used_size = (unsigned short)(stats.diskUsage * 10); // Convert to used size
                request.disk_data.disk_info.temp = (unsigned char)stats.temperature;
                append_crc(&request);
                memcpy(buffer, &request, reportSize);
                SendDataToDevice(g_hDevice, buffer, reportSize);

                Sleep(100);

                // Send Time data
                reportSize = init_hidreport(&request, SET, TIME_AIM, 255);
                request.time_data.time_info.timestamp = (unsigned int)time(NULL);
                append_crc(&request);
                memcpy(buffer, &request, reportSize);
                SendDataToDevice(g_hDevice, buffer, reportSize);
            }
        }
    }

    // Cleanup
    CleanupPerformanceCounters();
    if (g_hDevice != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_hDevice);
        g_hDevice = INVALID_HANDLE_VALUE;
    }

    return 0;
}

void WINAPI ServiceMain(DWORD argc, LPTSTR* argv)
{
    // Register the handler function
    g_StatusHandle = RegisterServiceCtrlHandler(SERVICE_NAME, ServiceCtrlHandler);

    if (g_StatusHandle == NULL)
    {
        return;
    }

    // Report initial status
    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwServiceSpecificExitCode = 0;

    ReportServiceStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    // Create stop event
    g_ServiceStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (g_ServiceStopEvent == NULL)
    {
        ReportServiceStatus(SERVICE_STOPPED, NO_ERROR, 0);
        return;
    }

    // Start the worker thread
    HANDLE hThread = CreateThread(NULL, 0, ServiceWorkerThread, NULL, 0, NULL);
    if (hThread == NULL)
    {
        ReportServiceStatus(SERVICE_STOPPED, NO_ERROR, 0);
        CloseHandle(g_ServiceStopEvent);
        return;
    }

    // Report running status
    ReportServiceStatus(SERVICE_RUNNING, NO_ERROR, 0);

    // Wait for stop signal
    WaitForSingleObject(g_ServiceStopEvent, INFINITE);

    // Cleanup
    CloseHandle(hThread);
    CloseHandle(g_ServiceStopEvent);

    ReportServiceStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

BOOL InstallService(void)
{
    SC_HANDLE schSCManager;
    SC_HANDLE schService;
    TCHAR szPath[MAX_PATH];
    BOOL result = FALSE;

    if (!GetModuleFileName(NULL, szPath, MAX_PATH))
    {
        printf("Cannot get module file name.\n");
        return FALSE;
    }

    // Get a handle to the SCM database
    schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (schSCManager == NULL)
    {
        printf("OpenSCManager failed (%d)\n", GetLastError());
        return FALSE;
    }

    // Create the service
    schService = CreateService(
        schSCManager,              // SCM database
        SERVICE_NAME,              // name of service
        L"HarborOS Subscreen Service", // service name to display
        SERVICE_ALL_ACCESS,        // desired access
        SERVICE_WIN32_OWN_PROCESS, // service type
        SERVICE_AUTO_START,        // start type
        SERVICE_ERROR_NORMAL,      // error control type
        szPath,                    // path to service's binary
        NULL,                      // no load ordering group
        NULL,                      // no tag identifier
        NULL,                      // no dependencies
        NULL,                      // LocalSystem account
        NULL);                     // no password

    if (schService == NULL)
    {
        printf("CreateService failed (%d)\n", GetLastError());
        CloseServiceHandle(schSCManager);
        return FALSE;
    }
    else
    {
        printf("Service installed successfully\n");
        result = TRUE;
    }

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);

    return result;
}

BOOL UninstallService(void)
{
    SC_HANDLE schSCManager;
    SC_HANDLE schService;
    BOOL result = FALSE;

    // Get a handle to the SCM database
    schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (schSCManager == NULL)
    {
        printf("OpenSCManager failed (%d)\n", GetLastError());
        return FALSE;
    }

    // Get a handle to the service
    schService = OpenService(schSCManager, SERVICE_NAME, DELETE);
    if (schService == NULL)
    {
        printf("OpenService failed (%d)\n", GetLastError());
        CloseServiceHandle(schSCManager);
        return FALSE;
    }

    // Delete the service
    if (!DeleteService(schService))
    {
        printf("DeleteService failed (%d)\n", GetLastError());
    }
    else
    {
        printf("Service uninstalled successfully\n");
        result = TRUE;
    }

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);

    return result;
}