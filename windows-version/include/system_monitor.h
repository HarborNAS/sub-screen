#ifndef SYSTEM_MONITOR_H
#define SYSTEM_MONITOR_H

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <winsock2.h>
#include <windows.h>

#define SUBSCREEN_MAX_DISKS 8
#define SUBSCREEN_MAX_NETWORKS 4
#define SUBSCREEN_NAME_LEN 48

typedef struct {
    char name[16];
    double usage;
    unsigned int totalGB;
    unsigned int usedGB;
    unsigned char temperature;
} DiskStats;

typedef struct {
    char name[24];
    unsigned int uploadKBps;
    unsigned int downloadKBps;
    unsigned int totalMB;
    unsigned char ipv4[4];
} NetworkStats;

// System monitoring structures
typedef struct {
    double cpuUsage;
    double cpuTemperature;
    double memoryUsage;
    double diskUsage;
    double networkUsage;
    double gpuUsage;
    double gpuTemperature;
    unsigned int gpuFanRpm;
    unsigned int diskTotalGB;
    unsigned int diskUsedGB;
    unsigned int netUploadKBps;
    unsigned int netDownloadKBps;
    unsigned int netTotalMB;
    unsigned char ipv4[4];
    unsigned int diskCount;
    DiskStats disks[SUBSCREEN_MAX_DISKS];
    unsigned int networkCount;
    NetworkStats networks[SUBSCREEN_MAX_NETWORKS];
    BOOL hasNvidiaGpu;
    char hostName[SUBSCREEN_NAME_LEN];
    char cpuName[SUBSCREEN_NAME_LEN];
    char osName[SUBSCREEN_NAME_LEN];
    char modelName[SUBSCREEN_NAME_LEN];
    char serialNumber[SUBSCREEN_NAME_LEN];
} SystemStats;

// Function declarations
BOOL InitializePerformanceCounters(void);
void CleanupPerformanceCounters(void);
BOOL GetSystemStats(SystemStats* stats);
BOOL GetCPUUsage(double* cpuUsage);
BOOL GetMemoryUsage(double* memoryUsage);
BOOL GetDiskUsage(double* diskUsage);
BOOL GetNetworkUsage(double* networkUsage);
BOOL GetGPUTemperature(double* temperature);

#endif // SYSTEM_MONITOR_H
