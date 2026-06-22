#include "service.h"

#include "protocol.h"
#include "system_monitor.h"
#include "usb_comm.h"

#include <powrprof.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SUBSCREEN_PAGE_HIBERNATE 0x82

static const GUID SUBSCREEN_POWER_HIGH_PERFORMANCE =
    { 0x8c5e7fda, 0xe8bf, 0x4a96, { 0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c } };
static const GUID SUBSCREEN_POWER_BALANCED =
    { 0x381b4222, 0xf694, 0x41f0, { 0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e } };

typedef struct SubscreenRuntime {
    SubscreenUsbDevice* device;
    BOOL mockUsb;
    BOOL startupSent;
    DWORD lastConnectAttempt;
    HANDLE readThread;
    HANDLE readStopEvent;
    CRITICAL_SECTION ioLock;
    BOOL ioLockReady;
    volatile LONG currentPage;
    volatile LONG deviceFault;
} SubscreenRuntime;

static SERVICE_STATUS g_serviceStatus;
static SERVICE_STATUS_HANDLE g_statusHandle = NULL;
static HANDLE g_serviceStopEvent = NULL;
static HANDLE g_consoleStopEvent = NULL;
static volatile LONG g_manualSequence = 0;

static unsigned char ClampPercent(double value)
{
    if (value < 0.0) {
        return 0;
    }
    if (value > 100.0) {
        return 100;
    }
    return (unsigned char)(value + 0.5);
}

static unsigned char ClampTemperature(double value)
{
    if (value < 0.0 || value > 254.0) {
        return 255;
    }
    return (unsigned char)(value + 0.5);
}

static unsigned char ClampByte(unsigned int value)
{
    return (value > 254U) ? 254U : (unsigned char)value;
}

static unsigned short ClampU16(unsigned int value)
{
    return (value > 65535U) ? 65535U : (unsigned short)value;
}

static void CopyAscii(unsigned char* target, size_t targetSize, const char* value)
{
    size_t len;

    if (!target || targetSize == 0) {
        return;
    }
    memset(target, 0, targetSize);
    if (!value) {
        return;
    }
    len = strlen(value);
    if (len > targetSize) {
        len = targetSize;
    }
    memcpy(target, value, len);
}

static void PrintHexPacket(const char* label, const unsigned char* data, int length)
{
    printf("%s (%d bytes):", label, length);
    for (int i = 0; i < length; ++i) {
        if ((i % 16) == 0) {
            printf("\n  ");
        }
        printf("%02X ", data[i]);
    }
    printf("\n");
}

static const char* AimName(unsigned char aim)
{
    switch (aim) {
    case HomePage_AIM: return "HomePage";
    case TIME_AIM: return "Time";
    case SystemPage_AIM: return "SystemPage";
    case System_AIM: return "System";
    case DiskPage_AIM: return "DiskPage";
    case Disk_AIM: return "Disk";
    case WlanPage_AIM: return "WlanPage";
    case USER_AIM: return "User";
    case WlanSpeed_AIM: return "WlanSpeed";
    case WlanTotal_AIM: return "WlanTotal";
    case WlanIP_AIM: return "WlanIP";
    case ModePage_AIM: return "ModePage";
    case Mute_AIM: return "Mute";
    case Properties_AIM: return "Properties";
    case Balance_AIM: return "Balance";
    case HIBERNATEATONCE_AIM: return "Hibernate";
    case InfoPage_AIM: return "InfoPage";
    case Updatefw_info_AIM: return "UpdateFwInfo";
    case GetVer_AIM: return "GetVersion";
    case Updatefw_AIM: return "UpdateFw";
    default: return "Unknown";
    }
}

static unsigned char RequestCrc(const Request* request)
{
    switch (request->aim) {
    case HomePage_AIM: return request->Homepage_data.crc;
    case TIME_AIM: return request->time_data.crc;
    case SystemPage_AIM: return request->SystemPage_data.crc;
    case System_AIM: return request->system_data.crc;
    case DiskPage_AIM: return request->DiskPage_data.crc;
    case Disk_AIM: return request->disk_data.crc;
    case WlanPage_AIM: return request->WlanPage_data.crc;
    case InfoPage_AIM: return request->InfoPage_data.crc;
    case USER_AIM: return request->user_data.crc;
    case WlanSpeed_AIM: return request->speed_data.crc;
    case WlanTotal_AIM: return request->flow_data.crc;
    case WlanIP_AIM: return request->wlanip_data.crc;
    case ModePage_AIM: return request->ModePage_data.crc;
    case GetVer_AIM: return request->Version_data.crc;
    case Updatefw_AIM: return request->OTA_data.crc;
    case Updatefw_info_AIM: return request->UpgradeInfo_data.crc;
    default: return request->common_data.crc;
    }
}

static void CopyField(char* target, size_t targetSize, const unsigned char* source, size_t sourceSize)
{
    size_t len;

    if (!target || targetSize == 0) {
        return;
    }
    memset(target, 0, targetSize);
    if (!source || sourceSize == 0) {
        return;
    }
    len = sourceSize;
    if (len > targetSize - 1) {
        len = targetSize - 1;
    }
    memcpy(target, source, len);
}

static void PrintPacketTrace(const char* label, const Request* request, int packetSize)
{
    printf("TRACE packet label=\"%s\" aim=0x%02X(%s) cmd=0x%02X seq=%u len=%u bytes=%d crc=0x%02X\n",
           label,
           request->aim,
           AimName(request->aim),
           request->cmd,
           request->sequence,
           request->length,
           packetSize,
           RequestCrc(request));

    switch (request->aim) {
    case HomePage_AIM:
        printf("TRACE   HomePage order=%u total=%u timestamp=%u\n",
               request->Homepage_data.order,
               request->Homepage_data.total,
               request->Homepage_data.time_info.timestamp);
        break;
    case TIME_AIM:
        printf("TRACE   Time timestamp=%u\n", request->time_data.time_info.timestamp);
        break;
    case SystemPage_AIM:
        printf("TRACE   SystemPage order=%u total=%u syscount=%u count=%u\n",
               request->SystemPage_data.order,
               request->SystemPage_data.total,
               request->SystemPage_data.syscount,
               request->SystemPage_data.count);
        for (unsigned int i = 0; i < request->SystemPage_data.count && i < 2U; ++i) {
            char name[9];
            CopyField(name, sizeof(name), request->SystemPage_data.systemPage[i].name, sizeof(request->SystemPage_data.systemPage[i].name));
            printf("TRACE     item%u id=%u name=\"%s\" usage=%u temp=%u rpm=%u\n",
                   i,
                   request->SystemPage_data.systemPage[i].sys_id,
                   name,
                   request->SystemPage_data.systemPage[i].usage,
                   request->SystemPage_data.systemPage[i].temp,
                   request->SystemPage_data.systemPage[i].rpm);
        }
        break;
    case System_AIM:
        printf("TRACE   System id=%u usage=%u temp=%u rpm=%u\n",
               request->system_data.system_info.sys_id,
               request->system_data.system_info.usage,
               request->system_data.system_info.temerature,
               request->system_data.system_info.rpm);
        break;
    case DiskPage_AIM:
        printf("TRACE   DiskPage order=%u total=%u diskcount=%u count=%u\n",
               request->DiskPage_data.order,
               request->DiskPage_data.total,
               request->DiskPage_data.diskcount,
               request->DiskPage_data.count);
        for (unsigned int i = 0; i < request->DiskPage_data.count && i < 2U; ++i) {
            char name[17];
            CopyField(name, sizeof(name), (const unsigned char*)request->DiskPage_data.diskStruct[i].name, sizeof(request->DiskPage_data.diskStruct[i].name));
            printf("TRACE     disk%u id=%u name=\"%s\" unit=0x%02X total=%u used=%u temp=%u\n",
                   i,
                   request->DiskPage_data.diskStruct[i].disk_id,
                   name,
                   request->DiskPage_data.diskStruct[i].unit,
                   request->DiskPage_data.diskStruct[i].total_size,
                   request->DiskPage_data.diskStruct[i].used_size,
                   request->DiskPage_data.diskStruct[i].temp);
        }
        break;
    case Disk_AIM:
        printf("TRACE   Disk id=%u unit=0x%02X total=%u used=%u temp=%u\n",
               request->disk_data.disk_info.disk_id,
               request->disk_data.disk_info.unit,
               request->disk_data.disk_info.total_size,
               request->disk_data.disk_info.used_size,
               request->disk_data.disk_info.temp);
        break;
    case WlanPage_AIM: {
        char name[21];
        CopyField(name, sizeof(name), request->WlanPage_data.wlanPage.name, sizeof(request->WlanPage_data.wlanPage.name));
        printf("TRACE   WlanPage order=%u total=%u netcount=%u count=%u online=%u id=%u name=\"%s\" unit=0x%02X up=%u down=%u ip=%u.%u.%u.%u\n",
               request->WlanPage_data.order,
               request->WlanPage_data.total,
               request->WlanPage_data.netcount,
               request->WlanPage_data.count,
               request->WlanPage_data.online,
               request->WlanPage_data.wlanPage.id,
               name,
               request->WlanPage_data.wlanPage.unit,
               request->WlanPage_data.wlanPage.uploadspeed,
               request->WlanPage_data.wlanPage.downloadspeed,
               request->WlanPage_data.wlanPage.ip[0],
               request->WlanPage_data.wlanPage.ip[1],
               request->WlanPage_data.wlanPage.ip[2],
               request->WlanPage_data.wlanPage.ip[3]);
        break;
    }
    case USER_AIM:
        printf("TRACE   User online=%u\n", request->user_data.user_info.online);
        break;
    case WlanSpeed_AIM:
        printf("TRACE   WlanSpeed id=%u unit=0x%02X up=%u down=%u\n",
               request->speed_data.id,
               request->speed_data.speed_info.unit,
               request->speed_data.speed_info.uploadspeed,
               request->speed_data.speed_info.downloadspeed);
        break;
    case WlanTotal_AIM:
        printf("TRACE   WlanTotal id=%u unit=0x%02X totalflow=%u\n",
               request->flow_data.id,
               request->flow_data.unit,
               request->flow_data.totalflow);
        break;
    case WlanIP_AIM:
        printf("TRACE   WlanIP id=%u ip=%u.%u.%u.%u\n",
               request->wlanip_data.id,
               request->wlanip_data.ip[0],
               request->wlanip_data.ip[1],
               request->wlanip_data.ip[2],
               request->wlanip_data.ip[3]);
        break;
    case ModePage_AIM:
        printf("TRACE   ModePage order=%u total=%u powcount=%u count=%u mute=%u properties=%u\n",
               request->ModePage_data.order,
               request->ModePage_data.total,
               request->ModePage_data.powcount,
               request->ModePage_data.count,
               request->ModePage_data.mute,
               request->ModePage_data.properties);
        break;
    case InfoPage_AIM: {
        char name[49];
        CopyField(name, sizeof(name), request->InfoPage_data.name, sizeof(request->InfoPage_data.name));
        printf("TRACE   InfoPage order=%u total=%u namelength=%u name=\"%s\"\n",
               request->InfoPage_data.order,
               request->InfoPage_data.total,
               request->InfoPage_data.namelength,
               name);
        break;
    }
    default:
        break;
    }
}

static void PrintMenuMatrix(void)
{
    printf("MENU primary aim=0x00 HomePage secondary=0x01 Time\n");
    printf("MENU primary aim=0x10 SystemPage secondary=0x11 System ids=0:CPU,1:iGPU,2:Memory,3:DGPU\n");
    printf("MENU primary aim=0x50 DiskPage secondary=0x51 Disk per-disk\n");
    printf("MENU primary aim=0x60 WlanPage secondary=0x61 User,0x62 WlanSpeed,0x64 WlanTotal,0x65 WlanIP\n");
    printf("MENU primary aim=0x70 ModePage secondary=0x71 Mute,0x72 Properties,0x73 Balance\n");
    printf("MENU primary aim=0xA0 InfoPage secondary=order1:Device,order2:CPU,order3:OS,order4:Serial\n");
}

static int FinalizeManualRequest(Request* request, int packetSize)
{
    if (!request || packetSize <= (int)offsetof(Request, length) || packetSize > 255) {
        return 0;
    }
    request->sequence = (unsigned char)((InterlockedIncrement(&g_manualSequence) - 1) & 0xFF);
    request->length = (unsigned char)(packetSize - offsetof(Request, length));
    return packetSize;
}

static void EncodeSpeed(unsigned int uploadKBps,
                        unsigned int downloadKBps,
                        unsigned char* unit,
                        unsigned short* upload,
                        unsigned short* download)
{
    unsigned char encodedUnit = 0;
    unsigned int up = uploadKBps;
    unsigned int down = downloadKBps;

    while (up > 1024U && (encodedUnit & 0xF0) < 0x30) {
        encodedUnit += 0x10;
        up /= 1024U;
    }
    while (down > 1024U && (encodedUnit & 0x0F) < 0x03) {
        encodedUnit += 0x01;
        down /= 1024U;
    }

    *unit = encodedUnit;
    *upload = ClampU16(up);
    *download = ClampU16(down);
}

static void EncodeDisk(unsigned int totalGB,
                       unsigned int usedGB,
                       unsigned char* unit,
                       unsigned short* total,
                       unsigned short* used)
{
    unsigned char encodedUnit = 0x22;
    unsigned int scaledTotal = totalGB;
    unsigned int scaledUsed = usedGB;

    while (scaledTotal > 4000U && (encodedUnit & 0x0F) < 0x0F) {
        scaledTotal /= 1024U;
        encodedUnit += 0x01;
    }
    while (scaledUsed > 4000U && (encodedUnit & 0xF0) < 0xF0) {
        scaledUsed /= 1024U;
        encodedUnit += 0x10;
    }

    *unit = encodedUnit;
    *total = ClampU16(scaledTotal);
    *used = ClampU16(scaledUsed);
}

static void ApplyModeCommand(unsigned char aim)
{
    DWORD result = ERROR_SUCCESS;

    if (aim == Properties_AIM) {
        result = PowerSetActiveScheme(NULL, &SUBSCREEN_POWER_HIGH_PERFORMANCE);
        OutputDebugStringW(result == ERROR_SUCCESS ?
                           L"HarborOS Subscreen switched Windows power mode to high performance.\n" :
                           L"HarborOS Subscreen failed to switch Windows power mode to high performance.\n");
    } else if (aim == Balance_AIM) {
        result = PowerSetActiveScheme(NULL, &SUBSCREEN_POWER_BALANCED);
        OutputDebugStringW(result == ERROR_SUCCESS ?
                           L"HarborOS Subscreen switched Windows power mode to balanced.\n" :
                           L"HarborOS Subscreen failed to switch Windows power mode to balanced.\n");
    } else if (aim == Mute_AIM) {
        OutputDebugStringW(L"HarborOS Subscreen mute command received; no kernel/audio driver action is required.\n");
    }
}

static void ProcessDevicePacket(SubscreenRuntime* runtime, const unsigned char* packet, DWORD length)
{
    if (!packet || length < 6 || packet[0] != 0xA5 || packet[1] != 0x5A) {
        return;
    }

    if (packet[2] == 0xFF && packet[3] == 0x04 && packet[4] == AUTOSET) {
        switch (packet[5]) {
        case HomePage_AIM:
        case SystemPage_AIM:
        case DiskPage_AIM:
        case WlanPage_AIM:
        case InfoPage_AIM:
            InterlockedExchange(&runtime->currentPage, packet[5]);
            break;
        case Mute_AIM:
        case Properties_AIM:
        case Balance_AIM:
            InterlockedExchange(&runtime->currentPage, HomePage_AIM);
            ApplyModeCommand(packet[5]);
            break;
        case HIBERNATEATONCE_AIM:
        case SUBSCREEN_PAGE_HIBERNATE:
            OutputDebugStringW(L"HarborOS Subscreen requested hibernate.\n");
            if (!SetSuspendState(TRUE, FALSE, FALSE)) {
                OutputDebugStringW(L"HarborOS Subscreen hibernate request failed.\n");
            }
            break;
        default:
            InterlockedExchange(&runtime->currentPage, HomePage_AIM);
            break;
        }
        return;
    }

    if (packet[3] == 0x07 && length >= 9) {
        WCHAR message[128];
        swprintf_s(message,
                   ARRAYSIZE(message),
                   L"HarborOS Subscreen firmware version event: %u.%u.%u.%u\n",
                   packet[5],
                   packet[6],
                   packet[7],
                   packet[8]);
        OutputDebugStringW(message);
        return;
    }

    if (packet[4] == UPDATE && length >= 6 && packet[5] != 0) {
        WCHAR message[128];
        swprintf_s(message, ARRAYSIZE(message), L"HarborOS Subscreen OTA ACK error: %u\n", packet[5]);
        OutputDebugStringW(message);
    }
}

static DWORD WINAPI UsbReadThread(LPVOID param)
{
    SubscreenRuntime* runtime = (SubscreenRuntime*)param;

    while (WaitForSingleObject(runtime->readStopEvent, 0) != WAIT_OBJECT_0) {
        unsigned char buffer[64];
        DWORD bytesRead = 0;
        BOOL ok;
        DWORD error;

        memset(buffer, 0, sizeof(buffer));
        EnterCriticalSection(&runtime->ioLock);
        ok = runtime->device && UsbRead(runtime->device, buffer, sizeof(buffer), &bytesRead, 500);
        error = UsbLastError();
        LeaveCriticalSection(&runtime->ioLock);

        if (ok && bytesRead > 0) {
            ProcessDevicePacket(runtime, buffer, bytesRead);
        } else if (!ok && error != ERROR_SEM_TIMEOUT && error != ERROR_TIMEOUT && error != ERROR_OPERATION_ABORTED) {
            InterlockedExchange(&runtime->deviceFault, 1);
            break;
        }
    }

    return 0;
}

static void StopReadThread(SubscreenRuntime* runtime)
{
    if (runtime->readStopEvent) {
        SetEvent(runtime->readStopEvent);
    }
    if (runtime->readThread) {
        WaitForSingleObject(runtime->readThread, INFINITE);
        CloseHandle(runtime->readThread);
        runtime->readThread = NULL;
    }
    if (runtime->readStopEvent) {
        CloseHandle(runtime->readStopEvent);
        runtime->readStopEvent = NULL;
    }
}

static BOOL StartReadThread(SubscreenRuntime* runtime)
{
    if (runtime->mockUsb || runtime->readThread) {
        return TRUE;
    }

    runtime->readStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!runtime->readStopEvent) {
        return FALSE;
    }

    runtime->readThread = CreateThread(NULL, 0, UsbReadThread, runtime, 0, NULL);
    if (!runtime->readThread) {
        CloseHandle(runtime->readStopEvent);
        runtime->readStopEvent = NULL;
        return FALSE;
    }

    return TRUE;
}

static void CloseRuntimeDevice(SubscreenRuntime* runtime)
{
    StopReadThread(runtime);

    if (runtime->device) {
        EnterCriticalSection(&runtime->ioLock);
        UsbClose(runtime->device);
        runtime->device = NULL;
        LeaveCriticalSection(&runtime->ioLock);
    }

    runtime->startupSent = FALSE;
    InterlockedExchange(&runtime->deviceFault, 0);
}

static BOOL SendRequest(SubscreenRuntime* runtime, const char* label, Request* request, int packetSize)
{
    BOOL ok;

    append_crc(request);

    if (packetSize <= 0) {
        return FALSE;
    }

    if (runtime->mockUsb) {
        PrintPacketTrace(label, request, packetSize);
        PrintHexPacket(label, (const unsigned char*)request, packetSize);
        return TRUE;
    }

    if (!runtime->device) {
        return FALSE;
    }

    EnterCriticalSection(&runtime->ioLock);
    ok = UsbWrite(runtime->device, (const unsigned char*)request, (DWORD)packetSize, SUBSCREEN_TIMEOUT_MS);
    LeaveCriticalSection(&runtime->ioLock);

    if (!ok) {
        WCHAR message[256];
        swprintf_s(message,
                   ARRAYSIZE(message),
                   L"HarborOS Subscreen USB write failed on %S (error %lu); closing device.\n",
                   label,
                   UsbLastError());
        OutputDebugStringW(message);
        CloseRuntimeDevice(runtime);
        return FALSE;
    }

    return TRUE;
}

static int BuildHomePage(Request* request, unsigned int order, unsigned int total)
{
    memset(request, 0, sizeof(*request));
    request->header = SIGNATURE;
    request->sequence = 0;
    request->cmd = SET;
    request->aim = HomePage_AIM;
    request->Homepage_data.order = (unsigned char)order;
    request->Homepage_data.total = (unsigned char)total;
    request->Homepage_data.time_info.timestamp = ProtocolTimestamp();
    return FinalizeManualRequest(request, (int)(offsetof(Request, Homepage_data.crc) + 1));
}

static void FillSystemPageItem(SystemPage* item,
                               unsigned char id,
                               double usage,
                               double temperature,
                               unsigned int rpm,
                               const char* name)
{
    memset(item, 0, sizeof(*item));
    item->syslength = sizeof(SystemPage);
    item->sys_id = id;
    item->usage = ClampPercent(usage);
    item->temp = ClampTemperature(temperature);
    item->rpm = ClampByte(rpm);
    CopyAscii(item->name, sizeof(item->name), name);
}

static int BuildSystemPage(Request* request, const SystemStats* stats, unsigned int order)
{
    memset(request, 0, sizeof(*request));
    request->header = SIGNATURE;
    request->sequence = 0;
    request->cmd = SET;
    request->aim = SystemPage_AIM;
    request->SystemPage_data.order = (unsigned char)order;
    request->SystemPage_data.total = 2;
    request->SystemPage_data.syscount = stats->hasNvidiaGpu ? 4 : 3;

    if (order == 1) {
        request->SystemPage_data.count = 2;
        FillSystemPageItem(&request->SystemPage_data.systemPage[0],
                           0,
                           stats->cpuUsage,
                           stats->cpuTemperature,
                           255,
                           "CPU");
        FillSystemPageItem(&request->SystemPage_data.systemPage[1],
                           1,
                           0.0,
                           255.0,
                           255,
                           "iGPU");
    } else {
        request->SystemPage_data.count = stats->hasNvidiaGpu ? 2 : 1;
        FillSystemPageItem(&request->SystemPage_data.systemPage[0],
                           2,
                           stats->memoryUsage,
                           255.0,
                           255,
                           "Memory");
        if (stats->hasNvidiaGpu) {
            FillSystemPageItem(&request->SystemPage_data.systemPage[1],
                               3,
                               stats->gpuUsage,
                               stats->gpuTemperature,
                               stats->gpuFanRpm,
                               "DGPU");
        }
    }

    return FinalizeManualRequest(request, (int)(offsetof(Request, SystemPage_data.crc) + 1));
}

static int BuildDiskPage(Request* request, const SystemStats* stats, unsigned int order, unsigned int total)
{
    unsigned int first = (order - 1U) * 2U;
    unsigned int count = 0;

    memset(request, 0, sizeof(*request));
    request->header = SIGNATURE;
    request->sequence = 0;
    request->cmd = SET;
    request->aim = DiskPage_AIM;
    request->DiskPage_data.order = (unsigned char)order;
    request->DiskPage_data.total = (unsigned char)total;
    request->DiskPage_data.diskcount = (unsigned char)stats->diskCount;

    for (unsigned int i = 0; i < 2U && first + i < stats->diskCount; ++i) {
        const DiskStats* disk = &stats->disks[first + i];
        unsigned char unit;
        unsigned short diskTotal;
        unsigned short diskUsed;

        EncodeDisk(disk->totalGB, disk->usedGB, &unit, &diskTotal, &diskUsed);
        request->DiskPage_data.diskStruct[i].disklength = sizeof(diskStruct);
        request->DiskPage_data.diskStruct[i].disk_id = (unsigned char)(first + i);
        request->DiskPage_data.diskStruct[i].unit = unit;
        request->DiskPage_data.diskStruct[i].total_size = diskTotal;
        request->DiskPage_data.diskStruct[i].used_size = diskUsed;
        request->DiskPage_data.diskStruct[i].temp = disk->temperature;
        CopyAscii((unsigned char*)request->DiskPage_data.diskStruct[i].name,
                  sizeof(request->DiskPage_data.diskStruct[i].name),
                  disk->name);
        ++count;
    }

    request->DiskPage_data.count = (unsigned char)count;
    return FinalizeManualRequest(request, (int)(offsetof(Request, DiskPage_data.crc) + 1));
}

static int BuildWlanPage(Request* request, const SystemStats* stats, unsigned int order, unsigned int total)
{
    const NetworkStats* net = &stats->networks[order - 1U];
    unsigned char unit;
    unsigned short upload;
    unsigned short download;

    memset(request, 0, sizeof(*request));
    request->header = SIGNATURE;
    request->sequence = 0;
    request->cmd = SET;
    request->aim = WlanPage_AIM;
    request->WlanPage_data.order = (unsigned char)order;
    request->WlanPage_data.total = (unsigned char)total;
    request->WlanPage_data.netcount = (unsigned char)stats->networkCount;
    request->WlanPage_data.count = 1;
    request->WlanPage_data.online = 1;
    request->WlanPage_data.length = (unsigned char)(sizeof(WlanPage) + 1);
    request->WlanPage_data.wlanPage.id = (unsigned char)(order - 1U);
    EncodeSpeed(net->uploadKBps, net->downloadKBps, &unit, &upload, &download);
    request->WlanPage_data.wlanPage.unit = unit;
    request->WlanPage_data.wlanPage.uploadspeed = upload;
    request->WlanPage_data.wlanPage.downloadspeed = download;
    memcpy(request->WlanPage_data.wlanPage.ip, net->ipv4, sizeof(request->WlanPage_data.wlanPage.ip));
    CopyAscii(request->WlanPage_data.wlanPage.name, sizeof(request->WlanPage_data.wlanPage.name), net->name);
    return FinalizeManualRequest(request, (int)(offsetof(Request, WlanPage_data.crc) + 1));
}

static int BuildInfoPage(Request* request, const SystemStats* stats, unsigned int order)
{
    const char* value = stats->hostName;

    if (order == 2) {
        value = stats->cpuName;
    } else if (order == 3) {
        value = stats->osName;
    } else if (order == 4) {
        value = stats->serialNumber;
    }

    memset(request, 0, sizeof(*request));
    request->header = SIGNATURE;
    request->sequence = 0;
    request->cmd = SET;
    request->aim = InfoPage_AIM;
    request->InfoPage_data.order = (unsigned char)order;
    request->InfoPage_data.total = 4;
    request->InfoPage_data.namelength = (unsigned char)min(strlen(value), sizeof(request->InfoPage_data.name));
    CopyAscii(request->InfoPage_data.name, sizeof(request->InfoPage_data.name), value);
    return FinalizeManualRequest(request, (int)(offsetof(Request, InfoPage_data.crc) + 1));
}

static BOOL SendStartupPackets(SubscreenRuntime* runtime, const SystemStats* stats)
{
    Request request;
    int packetSize;
    BOOL ok = TRUE;
    unsigned int diskPages = (stats->diskCount + 1U) / 2U;
    unsigned int netPages = stats->networkCount ? stats->networkCount : 1U;

    packetSize = BuildHomePage(&request, 1, 1);
    ok = SendRequest(runtime, "init home page", &request, packetSize) && ok;

    packetSize = init_hidreport(&request, SET, TIME_AIM, 255);
    ok = SendRequest(runtime, "init time", &request, packetSize) && ok;

    packetSize = BuildSystemPage(&request, stats, 1);
    ok = SendRequest(runtime, "init system page 1", &request, packetSize) && ok;

    packetSize = BuildSystemPage(&request, stats, 2);
    ok = SendRequest(runtime, "init system page 2", &request, packetSize) && ok;

    for (unsigned int page = 1; page <= diskPages; ++page) {
        packetSize = BuildDiskPage(&request, stats, page, diskPages);
        ok = SendRequest(runtime, "init disk page", &request, packetSize) && ok;
    }

    for (unsigned int page = 1; page <= netPages; ++page) {
        packetSize = BuildWlanPage(&request, stats, page, netPages);
        ok = SendRequest(runtime, "init wlan page", &request, packetSize) && ok;
    }

    packetSize = first_init_hidreport(&request, SET, ModePage_AIM, 1, 1);
    ok = SendRequest(runtime, "init mode page", &request, packetSize) && ok;

    for (unsigned int page = 1; page <= 4U; ++page) {
        packetSize = BuildInfoPage(&request, stats, page);
        ok = SendRequest(runtime, "init info page", &request, packetSize) && ok;
    }

    runtime->startupSent = ok;
    return ok;
}

static BOOL SendSystemPacket(SubscreenRuntime* runtime,
                             unsigned char id,
                             double usage,
                             double temperature,
                             unsigned int rpm,
                             const char* label)
{
    Request request;
    int packetSize = init_hidreport(&request, SET, System_AIM, id);

    request.system_data.system_info.sys_id = id;
    request.system_data.system_info.usage = ClampPercent(usage);
    request.system_data.system_info.temerature = ClampTemperature(temperature);
    request.system_data.system_info.rpm = ClampByte(rpm);
    return SendRequest(runtime, label, &request, packetSize);
}

static BOOL SendStatsPackets(SubscreenRuntime* runtime, const SystemStats* stats)
{
    Request request;
    int packetSize;
    unsigned char unit;
    unsigned short valueA;
    unsigned short valueB;
    BOOL ok = TRUE;

    ok = SendSystemPacket(runtime, 0, stats->cpuUsage, stats->cpuTemperature, 255, "cpu") && ok;
    ok = SendSystemPacket(runtime, 1, 0.0, 255.0, 255, "igpu") && ok;
    ok = SendSystemPacket(runtime, 2, stats->memoryUsage, 255.0, 255, "memory") && ok;
    if (stats->hasNvidiaGpu) {
        ok = SendSystemPacket(runtime, 3, stats->gpuUsage, stats->gpuTemperature, stats->gpuFanRpm, "dgpu") && ok;
    }

    for (unsigned int i = 0; i < stats->diskCount; ++i) {
        const DiskStats* disk = &stats->disks[i];
        EncodeDisk(disk->totalGB, disk->usedGB, &unit, &valueA, &valueB);
        packetSize = init_hidreport(&request, SET, Disk_AIM, (unsigned char)i);
        request.disk_data.disk_info.disk_id = (unsigned char)i;
        request.disk_data.disk_info.unit = unit;
        request.disk_data.disk_info.total_size = valueA;
        request.disk_data.disk_info.used_size = valueB;
        request.disk_data.disk_info.temp = disk->temperature;
        ok = SendRequest(runtime, "disk", &request, packetSize) && ok;
    }

    for (unsigned int i = 0; i < stats->networkCount; ++i) {
        const NetworkStats* net = &stats->networks[i];
        EncodeSpeed(net->uploadKBps, net->downloadKBps, &unit, &valueA, &valueB);
        packetSize = init_hidreport(&request, SET, WlanSpeed_AIM, (unsigned char)i);
        request.speed_data.speed_info.unit = unit;
        request.speed_data.speed_info.uploadspeed = valueA;
        request.speed_data.speed_info.downloadspeed = valueB;
        ok = SendRequest(runtime, "network speed", &request, packetSize) && ok;

        packetSize = init_hidreport(&request, SET, WlanTotal_AIM, (unsigned char)i);
        request.flow_data.unit = 0x00;
        request.flow_data.totalflow = ClampU16(net->totalMB);
        ok = SendRequest(runtime, "network total", &request, packetSize) && ok;

        packetSize = init_hidreport(&request, SET, WlanIP_AIM, (unsigned char)i);
        memcpy(request.wlanip_data.ip, net->ipv4, sizeof(request.wlanip_data.ip));
        ok = SendRequest(runtime, "network ip", &request, packetSize) && ok;
    }

    packetSize = init_hidreport(&request, SET, USER_AIM, 255);
    request.user_data.user_info.online = 1;
    ok = SendRequest(runtime, "user", &request, packetSize) && ok;

    packetSize = init_hidreport(&request, SET, TIME_AIM, 255);
    ok = SendRequest(runtime, "time", &request, packetSize) && ok;

    return ok;
}

static BOOL EnsureConnected(SubscreenRuntime* runtime)
{
    DWORD now = GetTickCount();

    if (runtime->mockUsb) {
        return TRUE;
    }

    if (runtime->device && InterlockedCompareExchange(&runtime->deviceFault, 0, 0) == 0) {
        return TRUE;
    }

    if (runtime->device) {
        CloseRuntimeDevice(runtime);
    }

    if (runtime->lastConnectAttempt != 0 && (now - runtime->lastConnectAttempt) < 2000U) {
        return FALSE;
    }

    runtime->lastConnectAttempt = now;
    EnterCriticalSection(&runtime->ioLock);
    if (UsbOpen(&runtime->device)) {
        LeaveCriticalSection(&runtime->ioLock);
        OutputDebugStringW(L"HarborOS Subscreen USB device connected.\n");
        runtime->startupSent = FALSE;
        InterlockedExchange(&runtime->currentPage, HomePage_AIM);
        return StartReadThread(runtime);
    }
    LeaveCriticalSection(&runtime->ioLock);

    return FALSE;
}

static void DisplayStats(const SystemStats* stats)
{
    char cpuTempText[16];

    if (stats->cpuTemperature > 0.0 && stats->cpuTemperature < 255.0) {
        sprintf_s(cpuTempText, sizeof(cpuTempText), "%.0fC", stats->cpuTemperature);
    } else {
        strcpy_s(cpuTempText, sizeof(cpuTempText), "N/A");
    }

    printf("\rCPU: %.1f%% %s | MEM: %.1f%% | DISKS: %u %.1f%% (%u/%u GB) | NETS: %u U %u KB/s D %u KB/s | IP: %u.%u.%u.%u",
           stats->cpuUsage,
           cpuTempText,
           stats->memoryUsage,
           stats->diskCount,
           stats->diskUsage,
           stats->diskUsedGB,
           stats->diskTotalGB,
           stats->networkCount,
           stats->netUploadKBps,
           stats->netDownloadKBps,
           stats->ipv4[0],
           stats->ipv4[1],
           stats->ipv4[2],
           stats->ipv4[3]);
    fflush(stdout);
}

static BOOL CollectSystemStatsWithRetry(SystemStats* stats, HANDLE stopEvent)
{
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (GetSystemStats(stats)) {
            return TRUE;
        }

        if (attempt == 4) {
            break;
        }

        if (stopEvent) {
            if (WaitForSingleObject(stopEvent, 250) == WAIT_OBJECT_0) {
                return FALSE;
            }
        } else {
            Sleep(250);
        }
    }

    return FALSE;
}

static int RunSubscreenLoop(BOOL mockUsb, BOOL once, HANDLE stopEvent)
{
    SubscreenRuntime runtime;
    int exitCode = 0;

    memset(&runtime, 0, sizeof(runtime));
    runtime.mockUsb = mockUsb;
    runtime.currentPage = HomePage_AIM;
    InitializeCriticalSection(&runtime.ioLock);
    runtime.ioLockReady = TRUE;

    if (!InitializePerformanceCounters()) {
        DeleteCriticalSection(&runtime.ioLock);
        printf("Failed to initialize performance counters.\n");
        return 1;
    }

    if (mockUsb) {
        printf("Running with mock USB output.\n");
        PrintMenuMatrix();
    }

    while (TRUE) {
        SystemStats stats;

        if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
            break;
        }

        if (EnsureConnected(&runtime)) {
            if (CollectSystemStatsWithRetry(&stats, stopEvent)) {
                DisplayStats(&stats);
                printf("\n");

                if (!runtime.startupSent && !SendStartupPackets(&runtime, &stats)) {
                    printf("Failed to send startup packets.\n");
                    if (once) {
                        exitCode = 1;
                    }
                }

                if (!SendStatsPackets(&runtime, &stats)) {
                    printf("Failed to send stats packets.\n");
                    if (once) {
                        exitCode = 1;
                    }
                }
            } else {
                printf("Failed to collect system stats.\n");
                if (once) {
                    exitCode = 1;
                }
            }
        } else if (!once) {
            OutputDebugStringW(L"HarborOS Subscreen device not available; retrying.\n");
        } else {
            printf("Subscreen USB device is not available.\n");
            exitCode = 1;
        }

        if (once) {
            break;
        }

        if (stopEvent) {
            if (WaitForSingleObject(stopEvent, 1000) == WAIT_OBJECT_0) {
                break;
            }
        } else {
            Sleep(1000);
        }
    }

    CloseRuntimeDevice(&runtime);
    CleanupPerformanceCounters();
    if (runtime.ioLockReady) {
        DeleteCriticalSection(&runtime.ioLock);
    }
    return exitCode;
}

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
{
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        if (g_consoleStopEvent) {
            SetEvent(g_consoleStopEvent);
            return TRUE;
        }
    }
    return FALSE;
}

int RunSubscreenConsole(BOOL mockUsb, BOOL once)
{
    int result;

    printf("Running in console mode. Press Ctrl+C to exit.\n");

    g_consoleStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_consoleStopEvent) {
        printf("CreateEvent failed (%lu).\n", GetLastError());
        return 1;
    }

    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    result = RunSubscreenLoop(mockUsb, once, g_consoleStopEvent);
    SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);

    CloseHandle(g_consoleStopEvent);
    g_consoleStopEvent = NULL;
    return result;
}

static void ReportServiceStatus(DWORD currentState, DWORD win32ExitCode, DWORD waitHint)
{
    static DWORD checkPoint = 1;

    g_serviceStatus.dwCurrentState = currentState;
    g_serviceStatus.dwWin32ExitCode = win32ExitCode;
    g_serviceStatus.dwWaitHint = waitHint;
    g_serviceStatus.dwControlsAccepted = (currentState == SERVICE_START_PENDING) ? 0 : SERVICE_ACCEPT_STOP;
    g_serviceStatus.dwCheckPoint = (currentState == SERVICE_RUNNING || currentState == SERVICE_STOPPED) ? 0 : checkPoint++;

    if (g_statusHandle) {
        SetServiceStatus(g_statusHandle, &g_serviceStatus);
    }
}

void WINAPI ServiceCtrlHandler(DWORD ctrlCode)
{
    if (ctrlCode == SERVICE_CONTROL_STOP) {
        ReportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 0);
        if (g_serviceStopEvent) {
            SetEvent(g_serviceStopEvent);
        }
    }
}

static DWORD WINAPI ServiceWorkerThread(LPVOID lpParam)
{
    (void)lpParam;
    return (DWORD)RunSubscreenLoop(FALSE, FALSE, g_serviceStopEvent);
}

void WINAPI ServiceMain(DWORD argc, LPWSTR* argv)
{
    HANDLE threadHandle;

    (void)argc;
    (void)argv;

    g_statusHandle = RegisterServiceCtrlHandlerW(SUBSCREEN_SERVICE_NAME, ServiceCtrlHandler);
    if (!g_statusHandle) {
        return;
    }

    memset(&g_serviceStatus, 0, sizeof(g_serviceStatus));
    g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_serviceStatus.dwServiceSpecificExitCode = 0;

    ReportServiceStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    g_serviceStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_serviceStopEvent) {
        ReportServiceStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    threadHandle = CreateThread(NULL, 0, ServiceWorkerThread, NULL, 0, NULL);
    if (!threadHandle) {
        DWORD err = GetLastError();
        CloseHandle(g_serviceStopEvent);
        g_serviceStopEvent = NULL;
        ReportServiceStatus(SERVICE_STOPPED, err, 0);
        return;
    }

    ReportServiceStatus(SERVICE_RUNNING, NO_ERROR, 0);
    WaitForSingleObject(threadHandle, INFINITE);

    CloseHandle(threadHandle);
    CloseHandle(g_serviceStopEvent);
    g_serviceStopEvent = NULL;

    ReportServiceStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

BOOL InstallService(void)
{
    SC_HANDLE scm;
    SC_HANDLE service;
    WCHAR path[MAX_PATH];

    if (!GetModuleFileNameW(NULL, path, MAX_PATH)) {
        printf("Cannot get module file name (%lu).\n", GetLastError());
        return FALSE;
    }

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        printf("OpenSCManager failed (%lu).\n", GetLastError());
        return FALSE;
    }

    service = CreateServiceW(scm,
                             SUBSCREEN_SERVICE_NAME,
                             L"HarborOS Subscreen Service",
                             SERVICE_ALL_ACCESS,
                             SERVICE_WIN32_OWN_PROCESS,
                             SERVICE_AUTO_START,
                             SERVICE_ERROR_NORMAL,
                             path,
                             NULL,
                             NULL,
                             NULL,
                             NULL,
                             NULL);
    if (!service) {
        DWORD err = GetLastError();
        printf("CreateService failed (%lu).\n", err);
        CloseServiceHandle(scm);
        return FALSE;
    }

    printf("Service installed successfully.\n");
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return TRUE;
}

BOOL UninstallService(void)
{
    SC_HANDLE scm;
    SC_HANDLE service;
    SERVICE_STATUS_PROCESS status;
    DWORD needed;
    BOOL result;

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        printf("OpenSCManager failed (%lu).\n", GetLastError());
        return FALSE;
    }

    service = OpenServiceW(scm, SUBSCREEN_SERVICE_NAME, DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!service) {
        DWORD err = GetLastError();
        CloseServiceHandle(scm);
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            printf("Service is not installed.\n");
            return TRUE;
        }
        printf("OpenService failed (%lu).\n", err);
        return FALSE;
    }

    if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (LPBYTE)&status, sizeof(status), &needed) &&
        status.dwCurrentState != SERVICE_STOPPED) {
        SERVICE_STATUS stopStatus;
        ControlService(service, SERVICE_CONTROL_STOP, &stopStatus);
        for (int i = 0; i < 30; ++i) {
            Sleep(500);
            if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (LPBYTE)&status, sizeof(status), &needed) &&
                status.dwCurrentState == SERVICE_STOPPED) {
                break;
            }
        }
    }

    result = DeleteService(service);
    if (!result) {
        printf("DeleteService failed (%lu).\n", GetLastError());
    } else {
        printf("Service uninstalled successfully.\n");
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return result;
}
