#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <hidusage.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <devguid.h>
#include <initguid.h>
#include <winioctl.h>
#include <tlhelp32.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#include <wbemidl.h>
#include <comdef.h>
#include <Wbemidl.h>
#include <oleauto.h>
#include <powrprof.h>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "powrprof.lib")

// USB Device constants
#define VENDOR_ID   0x5448
#define PRODUCT_ID  0x0002
#define TIMEOUT_MS  5000

// System monitoring structures
typedef struct {
    double cpuUsage;
    double memoryUsage;
    double diskUsage;
    double networkUsage;
    double gpuUsage;
    double temperature;
} SystemStats;

// Function declarations
BOOL InitializePerformanceCounters(void);
void CleanupPerformanceCounters(void);
BOOL GetSystemStats(SystemStats* stats);
BOOL FindUSBDevice(HANDLE* deviceHandle);
BOOL SendDataToDevice(HANDLE deviceHandle, const unsigned char* data, DWORD dataSize);
void DisplayStats(const SystemStats* stats);
BOOL InstallService(void);
BOOL UninstallService(void);
void WINAPI ServiceMain(DWORD argc, LPTSTR* argv);
void WINAPI ServiceCtrlHandler(DWORD ctrlCode);

// Global variables
SERVICE_STATUS          g_ServiceStatus;
SERVICE_STATUS_HANDLE   g_StatusHandle;
HANDLE                  g_ServiceStopEvent;
PDH_HQUERY              g_CpuQuery;
PDH_HCOUNTER            g_CpuTotal;
HANDLE                  g_hDevice = INVALID_HANDLE_VALUE;

// Service name
#define SERVICE_NAME L"HarborOSSubscreenService"

// Main entry point
int main(int argc, char* argv[])
{
    if (argc > 1)
    {
        if (strcmp(argv[1], "install") == 0)
        {
            if (InstallService())
            {
                printf("Service installed successfully.\n");
                return 0;
            }
            else
            {
                printf("Failed to install service.\n");
                return 1;
            }
        }
        else if (strcmp(argv[1], "uninstall") == 0)
        {
            if (UninstallService())
            {
                printf("Service uninstalled successfully.\n");
                return 0;
            }
            else
            {
                printf("Failed to uninstall service.\n");
                return 1;
            }
        }
        else if (strcmp(argv[1], "console") == 0)
        {
            // Run in console mode for testing
            printf("Running in console mode. Press Ctrl+C to exit.\n");

            if (!InitializePerformanceCounters())
            {
                printf("Failed to initialize performance counters.\n");
                return 1;
            }

            if (!FindUSBDevice(&g_hDevice))
            {
                printf("USB device not found. Running in demo mode.\n");
            }

            SystemStats stats;
            while (TRUE)
            {
                if (GetSystemStats(&stats))
                {
                    DisplayStats(&stats);

                    if (g_hDevice != INVALID_HANDLE_VALUE)
                    {
                        // Send data to device (implementation needed)
                        // SendDataToDevice(g_hDevice, data, size);
                    }
                }

                Sleep(1000); // Update every second
            }

            CleanupPerformanceCounters();
            if (g_hDevice != INVALID_HANDLE_VALUE)
            {
                CloseHandle(g_hDevice);
            }
        }
    }
    else
    {
        // Run as service
        SERVICE_TABLE_ENTRY ServiceTable[] =
        {
            { (LPSTR)SERVICE_NAME, (LPSERVICE_MAIN_FUNCTION)ServiceMain },
            { NULL, NULL }
        };

        if (StartServiceCtrlDispatcher(ServiceTable) == FALSE)
        {
            printf("Failed to start service control dispatcher.\n");
            return GetLastError();
        }
    }

    return 0;
}

void DisplayStats(const SystemStats* stats)
{
    printf("\rCPU: %.1f%% | MEM: %.1f%% | DISK: %.1f%% | NET: %.1f%% | GPU: %.1f%% | TEMP: %.1f°C",
           stats->cpuUsage, stats->memoryUsage, stats->diskUsage,
           stats->networkUsage, stats->gpuUsage, stats->temperature);
    fflush(stdout);
}