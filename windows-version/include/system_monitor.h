#ifndef SYSTEM_MONITOR_H
#define SYSTEM_MONITOR_H

#include <windows.h>

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
BOOL GetCPUUsage(double* cpuUsage);
BOOL GetMemoryUsage(double* memoryUsage);
BOOL GetDiskUsage(double* diskUsage);
BOOL GetNetworkUsage(double* networkUsage);
BOOL GetGPUTemperature(double* temperature);

#endif // SYSTEM_MONITOR_H