#include "system_monitor.h"

#include <ws2def.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ws2tcpip.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "ws2_32.lib")

typedef LONG (WINAPI* RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
typedef int (*NvmlInitFn)(void);
typedef int (*NvmlShutdownFn)(void);
typedef int (*NvmlDeviceGetHandleByIndexFn)(unsigned int index, void** device);
typedef int (*NvmlDeviceGetTemperatureFn)(void* device, unsigned int sensorType, unsigned int* temp);
typedef int (*NvmlDeviceGetFanSpeedFn)(void* device, unsigned int* speed);

typedef struct {
    unsigned int gpu;
    unsigned int memory;
} NvmlUtilization;

typedef int (*NvmlDeviceGetUtilizationRatesFn)(void* device, NvmlUtilization* utilization);

typedef struct {
    ULONGLONG luidValue;
    ULONG64 inOctets;
    ULONG64 outOctets;
    ULONG64 totalBytes;
    DWORD tick;
    BOOL valid;
} NetworkHistory;

static PDH_HQUERY g_cpuQuery = NULL;
static PDH_HCOUNTER g_cpuTotal = NULL;
static PDH_HQUERY g_thermalQuery = NULL;
static PDH_HCOUNTER g_thermalZoneTemperature = NULL;
static BOOL g_cpuTimesValid = FALSE;
static ULONGLONG g_prevIdleTime = 0;
static ULONGLONG g_prevKernelTime = 0;
static ULONGLONG g_prevUserTime = 0;
static double g_lastCpuUsage = -1.0;
static DWORD g_lastCpuTick = 0;
static double g_lastCpuTemperature = 255.0;
static DWORD g_lastCpuTemperatureTick = 0;
static unsigned int g_cachedDiskTemps[SUBSCREEN_MAX_DISKS];
static unsigned int g_cachedDiskTempCount = 0;
static DWORD g_lastDiskTempTick = 0;
static unsigned int g_cachedSystemFan = 255;
static DWORD g_lastSystemFanTick = 0;
static NetworkHistory g_netHistory[SUBSCREEN_MAX_NETWORKS * 2];
static HMODULE g_nvml = NULL;
static BOOL g_nvmlAttempted = FALSE;
static BOOL g_nvmlReady = FALSE;
static NvmlInitFn pNvmlInit = NULL;
static NvmlShutdownFn pNvmlShutdown = NULL;
static NvmlDeviceGetHandleByIndexFn pNvmlDeviceGetHandleByIndex = NULL;
static NvmlDeviceGetTemperatureFn pNvmlDeviceGetTemperature = NULL;
static NvmlDeviceGetFanSpeedFn pNvmlDeviceGetFanSpeed = NULL;
static NvmlDeviceGetUtilizationRatesFn pNvmlDeviceGetUtilizationRates = NULL;

static void CopyString(char* target, size_t targetSize, const char* value)
{
    if (!target || targetSize == 0) {
        return;
    }
    if (!value || value[0] == '\0') {
        target[0] = '\0';
        return;
    }
    strncpy_s(target, targetSize, value, _TRUNCATE);
}

static void WideToAnsi(const WCHAR* value, char* target, size_t targetSize)
{
    if (!target || targetSize == 0) {
        return;
    }
    target[0] = '\0';
    if (!value || value[0] == L'\0') {
        return;
    }
    WideCharToMultiByte(CP_ACP, 0, value, -1, target, (int)targetSize, NULL, NULL);
}

static void TrimAscii(char* value)
{
    char* start = value;
    char* end;

    if (!value) {
        return;
    }

    while (*start && isspace((unsigned char)*start)) {
        ++start;
    }
    if (start != value) {
        memmove(value, start, strlen(start) + 1);
    }

    end = value + strlen(value);
    while (end > value && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
}

static BOOL ParseUnsigned(const char* value, unsigned int* parsed)
{
    char* end = NULL;
    unsigned long result;

    if (!value || !parsed) {
        return FALSE;
    }

    while (*value && !isdigit((unsigned char)*value)) {
        if (*value == '-' || *value == 'N' || *value == 'n') {
            return FALSE;
        }
        ++value;
    }
    if (!*value) {
        return FALSE;
    }

    result = strtoul(value, &end, 10);
    if (end == value || result > 65535UL) {
        return FALSE;
    }

    *parsed = (unsigned int)result;
    return TRUE;
}

static BOOL ReadFirstCommandLine(const char* command, char* line, size_t lineSize)
{
    FILE* pipe;

    if (!command || !line || lineSize == 0) {
        return FALSE;
    }

    line[0] = '\0';
    pipe = _popen(command, "r");
    if (!pipe) {
        return FALSE;
    }

    while (fgets(line, (int)lineSize, pipe)) {
        TrimAscii(line);
        if (line[0] != '\0') {
            _pclose(pipe);
            return TRUE;
        }
    }

    _pclose(pipe);
    line[0] = '\0';
    return FALSE;
}

static unsigned int ReadUnsignedCommandLines(const char* command,
                                             unsigned int* values,
                                             unsigned int maxValues,
                                             unsigned int minValue,
                                             unsigned int maxValue)
{
    FILE* pipe;
    char line[128];
    unsigned int count = 0;

    if (!command || !values || maxValues == 0) {
        return 0;
    }

    pipe = _popen(command, "r");
    if (!pipe) {
        return 0;
    }

    while (count < maxValues && fgets(line, sizeof(line), pipe)) {
        unsigned int value;
        TrimAscii(line);
        if (ParseUnsigned(line, &value) && value >= minValue && value <= maxValue) {
            values[count++] = value;
        }
    }

    _pclose(pipe);
    return count;
}

static unsigned char SensorTemperatureByte(unsigned int value)
{
    if (value == 0 || value > 254U) {
        return 255;
    }
    return (unsigned char)value;
}

static ULONGLONG FileTimeToUInt64(const FILETIME* value)
{
    return ((ULONGLONG)value->dwHighDateTime << 32) | value->dwLowDateTime;
}

static BOOL SeedCpuTimes(void)
{
    FILETIME idleTime;
    FILETIME kernelTime;
    FILETIME userTime;

    if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
        g_cpuTimesValid = FALSE;
        return FALSE;
    }

    g_prevIdleTime = FileTimeToUInt64(&idleTime);
    g_prevKernelTime = FileTimeToUInt64(&kernelTime);
    g_prevUserTime = FileTimeToUInt64(&userTime);
    g_lastCpuTick = GetTickCount();
    g_cpuTimesValid = TRUE;
    return TRUE;
}

static BOOL GetCPUUsageFromSystemTimes(double* cpuUsage)
{
    FILETIME idleTime;
    FILETIME kernelTime;
    FILETIME userTime;
    ULONGLONG idle;
    ULONGLONG kernel;
    ULONGLONG user;
    ULONGLONG idleDelta;
    ULONGLONG kernelDelta;
    ULONGLONG userDelta;
    ULONGLONG totalDelta;
    DWORD now;
    DWORD elapsed;

    if (!cpuUsage) {
        return FALSE;
    }

    if (!g_cpuTimesValid && !SeedCpuTimes()) {
        return FALSE;
    }

    now = GetTickCount();
    elapsed = now - g_lastCpuTick;
    if (elapsed < 200U && g_lastCpuUsage < 0.0) {
        Sleep(200U - elapsed);
    }

    if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
        return FALSE;
    }

    idle = FileTimeToUInt64(&idleTime);
    kernel = FileTimeToUInt64(&kernelTime);
    user = FileTimeToUInt64(&userTime);

    if (idle < g_prevIdleTime || kernel < g_prevKernelTime || user < g_prevUserTime) {
        SeedCpuTimes();
        return FALSE;
    }

    idleDelta = idle - g_prevIdleTime;
    kernelDelta = kernel - g_prevKernelTime;
    userDelta = user - g_prevUserTime;
    totalDelta = kernelDelta + userDelta;

    g_prevIdleTime = idle;
    g_prevKernelTime = kernel;
    g_prevUserTime = user;
    g_lastCpuTick = GetTickCount();

    if (totalDelta == 0) {
        if (g_lastCpuUsage >= 0.0) {
            *cpuUsage = g_lastCpuUsage;
            return TRUE;
        }
        return FALSE;
    }

    if (idleDelta > totalDelta) {
        idleDelta = totalDelta;
    }

    g_lastCpuUsage = ((double)(totalDelta - idleDelta) * 100.0) / (double)totalDelta;
    if (g_lastCpuUsage < 0.0) {
        g_lastCpuUsage = 0.0;
    } else if (g_lastCpuUsage > 100.0) {
        g_lastCpuUsage = 100.0;
    }

    *cpuUsage = g_lastCpuUsage;
    return TRUE;
}

static double NormalizeThermalZoneTemperature(double value)
{
    if (value > 200.0 && value < 500.0) {
        value -= 273.15;
    }

    if (value <= 0.0 || value > 125.0) {
        return 255.0;
    }

    return value;
}

static void InitializeThermalCounters(void)
{
    PDH_STATUS status;

    status = PdhOpenQuery(NULL, 0, &g_thermalQuery);
    if (status != ERROR_SUCCESS) {
        g_thermalQuery = NULL;
        return;
    }

    status = PdhAddEnglishCounterW(g_thermalQuery,
                                   L"\\Thermal Zone Information(*)\\Temperature",
                                   0,
                                   &g_thermalZoneTemperature);
    if (status != ERROR_SUCCESS) {
        PdhCloseQuery(g_thermalQuery);
        g_thermalQuery = NULL;
        g_thermalZoneTemperature = NULL;
        return;
    }

    PdhCollectQueryData(g_thermalQuery);
}

BOOL InitializePerformanceCounters(void)
{
    PDH_STATUS status;

    if (!SeedCpuTimes()) {
        printf("Failed to seed CPU times (%lu).\n", GetLastError());
        return FALSE;
    }

    status = PdhOpenQuery(NULL, 0, &g_cpuQuery);
    if (status != ERROR_SUCCESS) {
        printf("Warning: failed to open CPU PDH query: 0x%lx\n", status);
        g_cpuQuery = NULL;
        InitializeThermalCounters();
        return TRUE;
    }

    status = PdhAddEnglishCounterW(g_cpuQuery, L"\\Processor(_Total)\\% Processor Time", 0, &g_cpuTotal);
    if (status != ERROR_SUCCESS) {
        printf("Warning: failed to add CPU PDH counter: 0x%lx\n", status);
        PdhCloseQuery(g_cpuQuery);
        g_cpuQuery = NULL;
        g_cpuTotal = NULL;
        InitializeThermalCounters();
        return TRUE;
    }

    status = PdhCollectQueryData(g_cpuQuery);
    if (status != ERROR_SUCCESS) {
        printf("Warning: failed to collect CPU PDH query data: 0x%lx\n", status);
        PdhCloseQuery(g_cpuQuery);
        g_cpuQuery = NULL;
        g_cpuTotal = NULL;
    }

    InitializeThermalCounters();
    return TRUE;
}

void CleanupPerformanceCounters(void)
{
    if (g_cpuQuery) {
        PdhCloseQuery(g_cpuQuery);
        g_cpuQuery = NULL;
        g_cpuTotal = NULL;
    }
    if (g_thermalQuery) {
        PdhCloseQuery(g_thermalQuery);
        g_thermalQuery = NULL;
        g_thermalZoneTemperature = NULL;
    }
    g_cpuTimesValid = FALSE;
    g_lastCpuUsage = -1.0;
    g_lastCpuTemperature = 255.0;
    g_lastCpuTemperatureTick = 0;
    g_cachedDiskTempCount = 0;
    g_lastDiskTempTick = 0;
    g_cachedSystemFan = 255;
    g_lastSystemFanTick = 0;
    if (g_nvmlReady && pNvmlShutdown) {
        pNvmlShutdown();
    }
    if (g_nvml) {
        FreeLibrary(g_nvml);
        g_nvml = NULL;
    }
    g_nvmlReady = FALSE;
    g_nvmlAttempted = FALSE;
}

BOOL GetCPUUsage(double* cpuUsage)
{
    PDH_FMT_COUNTERVALUE counterValue;
    PDH_STATUS status;

    if (!cpuUsage) {
        return FALSE;
    }

    if (GetCPUUsageFromSystemTimes(cpuUsage)) {
        return TRUE;
    }

    if (!g_cpuQuery || !g_cpuTotal) {
        return FALSE;
    }

    status = PdhCollectQueryData(g_cpuQuery);
    if (status != ERROR_SUCCESS) {
        return FALSE;
    }

    status = PdhGetFormattedCounterValue(g_cpuTotal, PDH_FMT_DOUBLE, NULL, &counterValue);
    if (status != ERROR_SUCCESS) {
        return FALSE;
    }

    *cpuUsage = counterValue.doubleValue;
    return TRUE;
}

BOOL GetCPUTemperature(double* temperature)
{
    DWORD now = GetTickCount();
    PDH_STATUS status;
    DWORD bufferSize = 0;
    DWORD itemCount = 0;
    PPDH_FMT_COUNTERVALUE_ITEM_W items;
    double best = 255.0;

    if (!temperature) {
        return FALSE;
    }

    if (g_lastCpuTemperatureTick != 0 && (now - g_lastCpuTemperatureTick) < 30000U) {
        *temperature = g_lastCpuTemperature;
        return g_lastCpuTemperature < 255.0;
    }

    g_lastCpuTemperatureTick = now;
    g_lastCpuTemperature = 255.0;

    if (!g_thermalQuery || !g_thermalZoneTemperature) {
        *temperature = g_lastCpuTemperature;
        return FALSE;
    }

    status = PdhCollectQueryData(g_thermalQuery);
    if (status != ERROR_SUCCESS) {
        *temperature = g_lastCpuTemperature;
        return FALSE;
    }

    status = PdhGetFormattedCounterArrayW(g_thermalZoneTemperature,
                                          PDH_FMT_DOUBLE,
                                          &bufferSize,
                                          &itemCount,
                                          NULL);
    if (status != PDH_MORE_DATA || bufferSize == 0 || itemCount == 0) {
        *temperature = g_lastCpuTemperature;
        return FALSE;
    }

    items = (PPDH_FMT_COUNTERVALUE_ITEM_W)calloc(1, bufferSize);
    if (!items) {
        *temperature = g_lastCpuTemperature;
        return FALSE;
    }

    status = PdhGetFormattedCounterArrayW(g_thermalZoneTemperature,
                                          PDH_FMT_DOUBLE,
                                          &bufferSize,
                                          &itemCount,
                                          items);
    if (status == ERROR_SUCCESS) {
        for (DWORD i = 0; i < itemCount; ++i) {
            double value = NormalizeThermalZoneTemperature(items[i].FmtValue.doubleValue);
            if (value < 255.0 && (best >= 255.0 || value > best)) {
                best = value;
            }
        }
    }

    free(items);
    g_lastCpuTemperature = best;
    *temperature = g_lastCpuTemperature;
    return g_lastCpuTemperature < 255.0;
}

BOOL GetMemoryUsage(double* memoryUsage)
{
    MEMORYSTATUSEX memInfo;

    if (!memoryUsage) {
        return FALSE;
    }

    memset(&memInfo, 0, sizeof(memInfo));
    memInfo.dwLength = sizeof(memInfo);

    if (!GlobalMemoryStatusEx(&memInfo) || memInfo.ullTotalPhys == 0) {
        return FALSE;
    }

    *memoryUsage = ((double)(memInfo.ullTotalPhys - memInfo.ullAvailPhys) / (double)memInfo.ullTotalPhys) * 100.0;
    return TRUE;
}

static void RefreshDiskTemperatureCache(void)
{
    DWORD now = GetTickCount();
    const char* command =
        "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "
        "\"try { Get-PhysicalDisk | Get-StorageReliabilityCounter | "
        "ForEach-Object { if ($_.Temperature -gt 0) { [int]$_.Temperature } } } catch {}\" 2>nul";

    if (g_lastDiskTempTick != 0 && (now - g_lastDiskTempTick) < 60000U) {
        return;
    }

    g_lastDiskTempTick = now;
    memset(g_cachedDiskTemps, 0, sizeof(g_cachedDiskTemps));
    g_cachedDiskTempCount = ReadUnsignedCommandLines(command,
                                                     g_cachedDiskTemps,
                                                     ARRAYSIZE(g_cachedDiskTemps),
                                                     1,
                                                     120);
}

static void ApplyDiskTemperatures(SystemStats* stats)
{
    if (!stats) {
        return;
    }

    RefreshDiskTemperatureCache();
    if (g_cachedDiskTempCount == 0) {
        return;
    }

    for (unsigned int i = 0; i < stats->diskCount; ++i) {
        unsigned int source = (i < g_cachedDiskTempCount) ? i : 0;
        stats->disks[i].temperature = SensorTemperatureByte(g_cachedDiskTemps[source]);
    }
}

static unsigned int GetSystemFanBestEffort(void)
{
    DWORD now = GetTickCount();
    const char* command =
        "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "
        "\"try { Get-CimInstance Win32_Fan | ForEach-Object { "
        "if ($_.CurrentReading -gt 0) { [int]$_.CurrentReading } "
        "elseif ($_.DesiredSpeed -gt 0) { [int]$_.DesiredSpeed } } } catch {}\" 2>nul";
    unsigned int values[4];
    unsigned int count;

    if (g_lastSystemFanTick != 0 && (now - g_lastSystemFanTick) < 60000U) {
        return g_cachedSystemFan;
    }

    g_lastSystemFanTick = now;
    g_cachedSystemFan = 255;
    count = ReadUnsignedCommandLines(command, values, ARRAYSIZE(values), 1, 20000);
    if (count > 0) {
        g_cachedSystemFan = values[0];
    }

    return g_cachedSystemFan;
}

static BOOL CollectDisks(SystemStats* stats)
{
    WCHAR drives[512];
    WCHAR* drive;
    DWORD chars;
    ULONGLONG totalBytes = 0;
    ULONGLONG usedBytes = 0;
    const ULONGLONG gb = 1024ULL * 1024ULL * 1024ULL;

    chars = GetLogicalDriveStringsW(ARRAYSIZE(drives), drives);
    if (chars == 0 || chars >= ARRAYSIZE(drives)) {
        return FALSE;
    }

    drive = drives;
    while (*drive && stats->diskCount < SUBSCREEN_MAX_DISKS) {
        ULARGE_INTEGER freeBytesAvailable;
        ULARGE_INTEGER totalNumberOfBytes;
        ULARGE_INTEGER totalNumberOfFreeBytes;

        if (GetDriveTypeW(drive) == DRIVE_FIXED &&
            GetDiskFreeSpaceExW(drive, &freeBytesAvailable, &totalNumberOfBytes, &totalNumberOfFreeBytes) &&
            totalNumberOfBytes.QuadPart > 0) {
            DiskStats* disk = &stats->disks[stats->diskCount++];
            ULONGLONG diskUsed = totalNumberOfBytes.QuadPart - totalNumberOfFreeBytes.QuadPart;

            WideToAnsi(drive, disk->name, sizeof(disk->name));
            if (strlen(disk->name) > 2) {
                disk->name[2] = '\0';
            }
            disk->usage = ((double)diskUsed / (double)totalNumberOfBytes.QuadPart) * 100.0;
            disk->totalGB = (unsigned int)(totalNumberOfBytes.QuadPart / gb);
            disk->usedGB = (unsigned int)(diskUsed / gb);
            disk->temperature = 255;

            totalBytes += totalNumberOfBytes.QuadPart;
            usedBytes += diskUsed;
        }

        drive += wcslen(drive) + 1;
    }

    if (stats->diskCount == 0 || totalBytes == 0) {
        return FALSE;
    }

    stats->diskUsage = ((double)usedBytes / (double)totalBytes) * 100.0;
    stats->diskTotalGB = (unsigned int)(totalBytes / gb);
    stats->diskUsedGB = (unsigned int)(usedBytes / gb);
    return TRUE;
}

static BOOL GetDiskStats(double* diskUsage, unsigned int* totalGB, unsigned int* usedGB)
{
    SystemStats stats;
    memset(&stats, 0, sizeof(stats));
    if (!CollectDisks(&stats)) {
        return FALSE;
    }
    if (diskUsage) {
        *diskUsage = stats.diskUsage;
    }
    if (totalGB) {
        *totalGB = stats.diskTotalGB;
    }
    if (usedGB) {
        *usedGB = stats.diskUsedGB;
    }
    return TRUE;
}

BOOL GetDiskUsage(double* diskUsage)
{
    return GetDiskStats(diskUsage, NULL, NULL);
}

static int ScoreIPv4(const unsigned char candidate[4])
{
    if (candidate[0] == 0 ||
        candidate[0] == 127 ||
        (candidate[0] == 169 && candidate[1] == 254)) {
        return 0;
    }

    if (candidate[0] == 192 && candidate[1] == 168) {
        return 5;
    }
    if (candidate[0] == 10) {
        return 5;
    }
    if (candidate[0] == 172 && candidate[1] >= 16 && candidate[1] <= 31) {
        return 5;
    }
    if (candidate[0] == 100 && candidate[1] >= 64 && candidate[1] <= 127) {
        return 1;
    }

    return 2;
}

static int ExtractBestIPv4(const IP_ADAPTER_ADDRESSES* adapter, unsigned char ip[4])
{
    int bestScore = 0;

    memset(ip, 0, 4);

    if (!adapter) {
        return 0;
    }

    for (IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
        SOCKADDR_IN* sa = (SOCKADDR_IN*)unicast->Address.lpSockaddr;
        if (sa && sa->sin_family == AF_INET) {
            unsigned char candidate[4];
            int score;
            memcpy(candidate, &sa->sin_addr.S_un.S_un_b, 4);
            score = ScoreIPv4(candidate);
            if (score > bestScore) {
                memcpy(ip, candidate, 4);
                bestScore = score;
            }
        }
    }

    return bestScore;
}

static IP_ADAPTER_ADDRESSES* LoadAdapterAddresses(void)
{
    ULONG bufferSize = 15000;
    IP_ADAPTER_ADDRESSES* addresses = (IP_ADAPTER_ADDRESSES*)malloc(bufferSize);
    DWORD result;

    if (!addresses) {
        return NULL;
    }

    result = GetAdaptersAddresses(AF_INET,
                                  GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  NULL,
                                  addresses,
                                  &bufferSize);
    if (result == ERROR_BUFFER_OVERFLOW) {
        IP_ADAPTER_ADDRESSES* bigger = (IP_ADAPTER_ADDRESSES*)realloc(addresses, bufferSize);
        if (!bigger) {
            free(addresses);
            return NULL;
        }
        addresses = bigger;
        result = GetAdaptersAddresses(AF_INET,
                                      GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                      NULL,
                                      addresses,
                                      &bufferSize);
    }

    if (result != NO_ERROR) {
        free(addresses);
        return NULL;
    }

    return addresses;
}

static NetworkHistory* FindNetworkHistory(ULONGLONG luidValue)
{
    NetworkHistory* empty = NULL;

    for (int i = 0; i < ARRAYSIZE(g_netHistory); ++i) {
        if (g_netHistory[i].valid && g_netHistory[i].luidValue == luidValue) {
            return &g_netHistory[i];
        }
        if (!g_netHistory[i].valid && !empty) {
            empty = &g_netHistory[i];
        }
    }

    if (empty) {
        memset(empty, 0, sizeof(*empty));
        empty->luidValue = luidValue;
        empty->tick = GetTickCount();
        empty->valid = TRUE;
    }
    return empty;
}

static const MIB_IF_ROW2* FindInterfaceRow(const MIB_IF_TABLE2* table, ULONGLONG luidValue)
{
    if (!table) {
        return NULL;
    }

    for (ULONG i = 0; i < table->NumEntries; ++i) {
        if (table->Table[i].InterfaceLuid.Value == luidValue) {
            return &table->Table[i];
        }
    }

    return NULL;
}

static BOOL IsUsableAdapter(const IP_ADAPTER_ADDRESSES* adapter)
{
    if (!adapter ||
        adapter->OperStatus != IfOperStatusUp ||
        adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
        adapter->IfType == IF_TYPE_TUNNEL) {
        return FALSE;
    }

    return TRUE;
}

static void FillNetworkFromRow(NetworkStats* net,
                               const IP_ADAPTER_ADDRESSES* adapter,
                               const MIB_IF_ROW2* row,
                               DWORD now)
{
    NetworkHistory* history;
    ULONG64 inOctets = row->InOctets;
    ULONG64 outOctets = row->OutOctets;
    ULONG64 totalBytes = inOctets + outOctets;
    DWORD elapsedMs;

    memset(net, 0, sizeof(*net));
    WideToAnsi(adapter->FriendlyName, net->name, sizeof(net->name));
    if (net->name[0] == '\0') {
        WideToAnsi(adapter->Description, net->name, sizeof(net->name));
    }
    if (net->name[0] == '\0') {
        CopyString(net->name, sizeof(net->name), "LAN");
    }
    ExtractBestIPv4(adapter, net->ipv4);

    history = FindNetworkHistory(row->InterfaceLuid.Value);
    if (!history) {
        net->totalMB = (unsigned int)(totalBytes / (1024ULL * 1024ULL));
        return;
    }

    elapsedMs = now - history->tick;
    if (history->tick != 0 &&
        elapsedMs > 0 &&
        inOctets >= history->inOctets &&
        outOctets >= history->outOctets) {
        ULONG64 deltaIn = inOctets - history->inOctets;
        ULONG64 deltaOut = outOctets - history->outOctets;
        net->downloadKBps = (unsigned int)((deltaIn * 1000ULL) / elapsedMs / 1024ULL);
        net->uploadKBps = (unsigned int)((deltaOut * 1000ULL) / elapsedMs / 1024ULL);
    }

    history->inOctets = inOctets;
    history->outOctets = outOctets;
    history->totalBytes = totalBytes;
    history->tick = now;
    history->valid = TRUE;
    net->totalMB = (unsigned int)(totalBytes / (1024ULL * 1024ULL));
}

static void FillFallbackNetwork(SystemStats* stats)
{
    NetworkStats* net = &stats->networks[0];

    memset(net, 0, sizeof(*net));
    CopyString(net->name, sizeof(net->name), "LAN");
    stats->networkCount = 1;
}

static BOOL UpdateNetworkStats(SystemStats* stats)
{
    IP_ADAPTER_ADDRESSES* addresses = LoadAdapterAddresses();
    MIB_IF_TABLE2* table = NULL;
    DWORD now = GetTickCount();
    const int priorities[] = { 5, 2, 1 };

    if (!addresses) {
        FillFallbackNetwork(stats);
        return TRUE;
    }

    if (GetIfTable2(&table) != NO_ERROR || !table) {
        free(addresses);
        FillFallbackNetwork(stats);
        return TRUE;
    }

    for (int priorityIndex = 0;
         priorityIndex < ARRAYSIZE(priorities) && stats->networkCount < SUBSCREEN_MAX_NETWORKS;
         ++priorityIndex) {
        for (const IP_ADAPTER_ADDRESSES* adapter = addresses;
             adapter && stats->networkCount < SUBSCREEN_MAX_NETWORKS;
             adapter = adapter->Next) {
            unsigned char ip[4];
            const MIB_IF_ROW2* row;
            NetworkStats* net;
            int score;

            if (!IsUsableAdapter(adapter)) {
                continue;
            }

            score = ExtractBestIPv4(adapter, ip);
            if (score != priorities[priorityIndex]) {
                continue;
            }

            row = FindInterfaceRow(table, adapter->Luid.Value);
            if (!row || row->OperStatus != IfOperStatusUp) {
                continue;
            }

            net = &stats->networks[stats->networkCount++];
            FillNetworkFromRow(net, adapter, row, now);
            stats->netDownloadKBps += net->downloadKBps;
            stats->netUploadKBps += net->uploadKBps;
            stats->netTotalMB += net->totalMB;
            if (stats->ipv4[0] == 0) {
                memcpy(stats->ipv4, net->ipv4, sizeof(stats->ipv4));
            }
        }
    }

    if (table) {
        FreeMibTable(table);
    }
    free(addresses);

    if (stats->networkCount == 0) {
        FillFallbackNetwork(stats);
    }

    stats->networkUsage = (double)(stats->netUploadKBps + stats->netDownloadKBps);
    return TRUE;
}

BOOL GetNetworkUsage(double* networkUsage)
{
    SystemStats stats;
    memset(&stats, 0, sizeof(stats));
    if (!UpdateNetworkStats(&stats)) {
        return FALSE;
    }
    if (networkUsage) {
        *networkUsage = stats.networkUsage;
    }
    return TRUE;
}

static void TryLoadNvml(void)
{
    WCHAR systemPath[MAX_PATH];

    if (g_nvmlAttempted) {
        return;
    }
    g_nvmlAttempted = TRUE;

    g_nvml = LoadLibraryW(L"nvml.dll");
    if (!g_nvml && GetSystemDirectoryW(systemPath, ARRAYSIZE(systemPath))) {
        wcscat_s(systemPath, ARRAYSIZE(systemPath), L"\\nvml.dll");
        g_nvml = LoadLibraryW(systemPath);
    }
    if (!g_nvml) {
        g_nvml = LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    }
    if (!g_nvml) {
        return;
    }

    pNvmlInit = (NvmlInitFn)GetProcAddress(g_nvml, "nvmlInit_v2");
    pNvmlShutdown = (NvmlShutdownFn)GetProcAddress(g_nvml, "nvmlShutdown");
    pNvmlDeviceGetHandleByIndex = (NvmlDeviceGetHandleByIndexFn)GetProcAddress(g_nvml, "nvmlDeviceGetHandleByIndex_v2");
    pNvmlDeviceGetTemperature = (NvmlDeviceGetTemperatureFn)GetProcAddress(g_nvml, "nvmlDeviceGetTemperature");
    pNvmlDeviceGetFanSpeed = (NvmlDeviceGetFanSpeedFn)GetProcAddress(g_nvml, "nvmlDeviceGetFanSpeed");
    pNvmlDeviceGetUtilizationRates = (NvmlDeviceGetUtilizationRatesFn)GetProcAddress(g_nvml, "nvmlDeviceGetUtilizationRates");

    if (pNvmlInit &&
        pNvmlDeviceGetHandleByIndex &&
        pNvmlDeviceGetTemperature &&
        pNvmlDeviceGetUtilizationRates &&
        pNvmlInit() == 0) {
        g_nvmlReady = TRUE;
    }
}

static BOOL DetectNvidiaDisplayAdapter(void)
{
    DISPLAY_DEVICEA device;

    memset(&device, 0, sizeof(device));
    device.cb = sizeof(device);

    for (DWORD i = 0; EnumDisplayDevicesA(NULL, i, &device, 0); ++i) {
        if ((device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) == 0 &&
            (strstr(device.DeviceString, "NVIDIA") ||
             strstr(device.DeviceString, "GeForce") ||
             strstr(device.DeviceString, "RTX"))) {
            return TRUE;
        }
        memset(&device, 0, sizeof(device));
        device.cb = sizeof(device);
    }

    return FALSE;
}

static BOOL ParseNvidiaSmiLine(const char* line,
                               double* usage,
                               double* temperature,
                               unsigned int* fan)
{
    char copy[256];
    char* context = NULL;
    char* token;
    unsigned int parsed;
    BOOL gotUsage = FALSE;
    BOOL gotTemp = FALSE;

    if (!line) {
        return FALSE;
    }

    strncpy_s(copy, sizeof(copy), line, _TRUNCATE);

    token = strtok_s(copy, ",", &context);
    if (token && ParseUnsigned(token, &parsed)) {
        *usage = (double)parsed;
        gotUsage = TRUE;
    }

    token = strtok_s(NULL, ",", &context);
    if (token && ParseUnsigned(token, &parsed)) {
        *temperature = (double)parsed;
        gotTemp = TRUE;
    }

    token = strtok_s(NULL, ",", &context);
    if (token && ParseUnsigned(token, &parsed)) {
        *fan = parsed;
    }

    return gotUsage || gotTemp;
}

static BOOL QueryNvidiaSmi(SystemStats* stats)
{
    static const char* commands[] = {
        "nvidia-smi --query-gpu=utilization.gpu,temperature.gpu,fan.speed --format=csv,noheader,nounits 2>nul",
        "\"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvidia-smi.exe\" --query-gpu=utilization.gpu,temperature.gpu,fan.speed --format=csv,noheader,nounits 2>nul",
        "\"C:\\Windows\\System32\\nvidia-smi.exe\" --query-gpu=utilization.gpu,temperature.gpu,fan.speed --format=csv,noheader,nounits 2>nul"
    };
    char line[256];

    if (!stats) {
        return FALSE;
    }

    for (int i = 0; i < ARRAYSIZE(commands); ++i) {
        double usage = stats->gpuUsage;
        double temperature = stats->gpuTemperature;
        unsigned int fan = stats->gpuFanRpm;

        if (ReadFirstCommandLine(commands[i], line, sizeof(line)) &&
            ParseNvidiaSmiLine(line, &usage, &temperature, &fan)) {
            stats->hasNvidiaGpu = TRUE;
            stats->gpuUsage = usage;
            stats->gpuTemperature = temperature;
            stats->gpuFanRpm = fan;
            return TRUE;
        }
    }

    return FALSE;
}

BOOL GetGPUTemperature(double* temperature)
{
    void* device = NULL;
    unsigned int temp = 0;

    if (!temperature) {
        return FALSE;
    }

    *temperature = 255.0;
    TryLoadNvml();
    if (!g_nvmlReady ||
        pNvmlDeviceGetHandleByIndex(0, &device) != 0 ||
        pNvmlDeviceGetTemperature(device, 0, &temp) != 0) {
        return TRUE;
    }

    *temperature = (double)temp;
    return TRUE;
}

static void FillGpuStats(SystemStats* stats)
{
    void* device = NULL;
    unsigned int temp = 0;
    unsigned int fan = 0;
    NvmlUtilization utilization;

    stats->gpuTemperature = 255.0;
    stats->gpuUsage = 0.0;
    stats->gpuFanRpm = 255;

    TryLoadNvml();
    if (!g_nvmlReady || pNvmlDeviceGetHandleByIndex(0, &device) != 0) {
        QueryNvidiaSmi(stats);
        if (!stats->hasNvidiaGpu && DetectNvidiaDisplayAdapter()) {
            stats->hasNvidiaGpu = TRUE;
        }
        return;
    }

    stats->hasNvidiaGpu = TRUE;
    if (pNvmlDeviceGetTemperature(device, 0, &temp) == 0) {
        stats->gpuTemperature = (double)temp;
    }
    memset(&utilization, 0, sizeof(utilization));
    if (pNvmlDeviceGetUtilizationRates(device, &utilization) == 0) {
        stats->gpuUsage = (double)utilization.gpu;
    }
    if (pNvmlDeviceGetFanSpeed && pNvmlDeviceGetFanSpeed(device, &fan) == 0) {
        stats->gpuFanRpm = fan;
    }
    QueryNvidiaSmi(stats);
}

static void FillIdentity(SystemStats* stats)
{
    DWORD size = SUBSCREEN_NAME_LEN;
    HKEY key;

    CopyString(stats->hostName, sizeof(stats->hostName), "Windows");
    GetComputerNameA(stats->hostName, &size);

    CopyString(stats->cpuName, sizeof(stats->cpuName), "CPU");
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD type;
        DWORD cb = SUBSCREEN_NAME_LEN;
        char value[SUBSCREEN_NAME_LEN];

        if (RegQueryValueExA(key, "ProcessorNameString", NULL, &type, (LPBYTE)value, &cb) == ERROR_SUCCESS && type == REG_SZ) {
            CopyString(stats->cpuName, sizeof(stats->cpuName), value);
        }

        RegCloseKey(key);
    }

    CopyString(stats->osName, sizeof(stats->osName), "Windows");
    {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        RtlGetVersionFn pRtlGetVersion = ntdll ? (RtlGetVersionFn)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
        RTL_OSVERSIONINFOW version;
        if (pRtlGetVersion) {
            memset(&version, 0, sizeof(version));
            version.dwOSVersionInfoSize = sizeof(version);
            if (pRtlGetVersion(&version) == 0) {
                sprintf_s(stats->osName,
                          sizeof(stats->osName),
                          "Windows %lu.%lu.%lu",
                          version.dwMajorVersion,
                          version.dwMinorVersion,
                          version.dwBuildNumber);
            }
        }
    }

    CopyString(stats->modelName, sizeof(stats->modelName), "Harbor PC");
    CopyString(stats->serialNumber, sizeof(stats->serialNumber), "Unknown");

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD type;
        DWORD cb;
        char value[SUBSCREEN_NAME_LEN];

        cb = sizeof(value);
        if (RegQueryValueExA(key, "SystemProductName", NULL, &type, (LPBYTE)value, &cb) == ERROR_SUCCESS && type == REG_SZ) {
            CopyString(stats->modelName, sizeof(stats->modelName), value);
        }

        cb = sizeof(value);
        if (RegQueryValueExA(key, "SystemSerialNumber", NULL, &type, (LPBYTE)value, &cb) == ERROR_SUCCESS && type == REG_SZ) {
            CopyString(stats->serialNumber, sizeof(stats->serialNumber), value);
        }

        RegCloseKey(key);
    }
}

BOOL GetSystemStats(SystemStats* stats)
{
    if (!stats) {
        return FALSE;
    }

    memset(stats, 0, sizeof(*stats));
    stats->cpuTemperature = 255.0;
    stats->cpuFanRpm = 255;
    FillIdentity(stats);

    if (!GetCPUUsage(&stats->cpuUsage)) {
        return FALSE;
    }
    GetCPUTemperature(&stats->cpuTemperature);
    stats->cpuFanRpm = GetSystemFanBestEffort();
    if (!GetMemoryUsage(&stats->memoryUsage)) {
        return FALSE;
    }
    if (!CollectDisks(stats)) {
        return FALSE;
    }
    ApplyDiskTemperatures(stats);
    if (!UpdateNetworkStats(stats)) {
        return FALSE;
    }
    FillGpuStats(stats);
    return TRUE;
}
