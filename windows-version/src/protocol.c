#include "protocol.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define REQUEST_LENGTH_OFFSET offsetof(Request, length)
#define REQUEST_DATA_OFFSET offsetof(Request, common_data.data)
#define REQUEST_COMMON_LEN ((unsigned char)(REQUEST_DATA_OFFSET - REQUEST_LENGTH_OFFSET))

typedef char request_header_offset_check[(offsetof(Request, header) == 0) ? 1 : -1];
typedef char request_sequence_offset_check[(offsetof(Request, sequence) == 2) ? 1 : -1];
typedef char request_length_offset_check[(offsetof(Request, length) == 3) ? 1 : -1];
typedef char request_cmd_offset_check[(offsetof(Request, cmd) == 4) ? 1 : -1];
typedef char request_aim_offset_check[(offsetof(Request, aim) == 5) ? 1 : -1];
typedef char request_data_offset_check[(REQUEST_DATA_OFFSET == 6) ? 1 : -1];

static unsigned char g_sequence = 0;

unsigned char calculate_crc(const unsigned char* data, int length)
{
    unsigned char crc = 0;

    for (int i = 0; i < length; ++i) {
        crc = (unsigned char)(crc + data[i]);
    }

    return crc;
}

static int FinalizeLength(Request* request, size_t packetSize)
{
    if (!request || packetSize <= REQUEST_LENGTH_OFFSET || packetSize > 255) {
        return 0;
    }

    request->length = (unsigned char)(packetSize - REQUEST_LENGTH_OFFSET);
    return (int)packetSize;
}

static void InitCommon(Request* request, unsigned char cmd, unsigned char aim)
{
    memset(request, 0, sizeof(*request));
    request->header = SIGNATURE;
    request->sequence = g_sequence++;
    request->cmd = cmd;
    request->aim = aim;
    request->length = REQUEST_COMMON_LEN;
}

static void CopyAscii(unsigned char* target, size_t targetSize, const char* value)
{
    size_t len = strlen(value);
    if (len > targetSize) {
        len = targetSize;
    }
    memcpy(target, value, len);
}

void append_crc(Request* request)
{
    int len = 0;

    if (!request) {
        return;
    }

    switch (request->aim) {
    case HomePage_AIM:
        len = (int)offsetof(Request, Homepage_data.crc);
        request->Homepage_data.crc = calculate_crc((const unsigned char*)request, len);
        return;
    case TIME_AIM:
        len = (int)offsetof(Request, time_data.crc);
        request->time_data.crc = calculate_crc((const unsigned char*)request, len);
        return;
    case SystemPage_AIM:
        len = (int)offsetof(Request, SystemPage_data.crc);
        request->SystemPage_data.crc = calculate_crc((const unsigned char*)request, len);
        return;
    case System_AIM:
        len = (int)offsetof(Request, system_data.crc);
        request->system_data.crc = calculate_crc((const unsigned char*)request, len);
        return;
    case DiskPage_AIM:
        len = (int)offsetof(Request, DiskPage_data.crc);
        request->DiskPage_data.crc = calculate_crc((const unsigned char*)request, len);
        return;
    case Disk_AIM:
        len = (int)offsetof(Request, disk_data.crc);
        request->disk_data.crc = calculate_crc((const unsigned char*)request, len);
        return;
    case ModePage_AIM:
        len = (int)offsetof(Request, ModePage_data.crc);
        request->ModePage_data.crc = calculate_crc((const unsigned char*)request, len);
        return;
    case WlanPage_AIM:
        len = (int)offsetof(Request, WlanPage_data.crc);
        request->WlanPage_data.crc = calculate_crc((const unsigned char*)request, len);
        return;
    case InfoPage_AIM:
        len = (int)offsetof(Request, InfoPage_data.crc);
        request->InfoPage_data.crc = calculate_crc((const unsigned char*)request, len);
        return;
    case USER_AIM:
        len = (int)offsetof(Request, user_data.crc);
        request->user_data.crc = calculate_crc((const unsigned char*)request, len);
        return;
    case WlanSpeed_AIM:
        len = (int)offsetof(Request, speed_data.crc);
        request->speed_data.crc = calculate_crc((const unsigned char*)request, len);
        return;
    case WlanTotal_AIM:
        len = (int)offsetof(Request, flow_data.crc);
        request->flow_data.crc = calculate_crc((const unsigned char*)request, len);
        return;
    case WlanIP_AIM:
        len = (int)offsetof(Request, wlanip_data.crc);
        request->wlanip_data.crc = calculate_crc((const unsigned char*)request, len);
        return;
    case GetVer_AIM:
        len = (int)offsetof(Request, Version_data.crc);
        request->Version_data.crc = calculate_crc((const unsigned char*)request, len);
        return;
    case Updatefw_AIM:
        len = (int)offsetof(Request, OTA_data.crc);
        request->OTA_data.crc = calculate_crc((const unsigned char*)request, len);
        return;
    case Updatefw_info_AIM:
        len = (int)offsetof(Request, UpgradeInfo_data.crc);
        request->UpgradeInfo_data.crc = calculate_crc((const unsigned char*)request, len);
        return;
    default:
        len = (int)offsetof(Request, common_data.crc);
        request->common_data.crc = calculate_crc((const unsigned char*)request, len);
        return;
    }
}

int init_hidreport(Request* request, unsigned char cmd, unsigned char aim, unsigned char index)
{
    if (!request) {
        return 0;
    }

    InitCommon(request, cmd, aim);

    switch (aim) {
    case TIME_AIM:
        request->time_data.time_info.timestamp = (unsigned int)time(NULL);
        return FinalizeLength(request, offsetof(Request, time_data.crc) + 1);
    case System_AIM:
        request->system_data.system_info.sys_id = index;
        request->system_data.system_info.usage = 0;
        request->system_data.system_info.temerature = 255;
        request->system_data.system_info.rpm = 255;
        return FinalizeLength(request, offsetof(Request, system_data.crc) + 1);
    case Disk_AIM:
        request->disk_data.disk_info.disk_id = index;
        request->disk_data.disk_info.unit = 0x22;
        request->disk_data.disk_info.total_size = 0;
        request->disk_data.disk_info.used_size = 0;
        request->disk_data.disk_info.temp = 255;
        return FinalizeLength(request, offsetof(Request, disk_data.crc) + 1);
    case USER_AIM:
        request->user_data.user_info.online = 0;
        return FinalizeLength(request, offsetof(Request, user_data.crc) + 1);
    case WlanSpeed_AIM:
        request->speed_data.id = index;
        request->speed_data.speed_info.unit = 0x00;
        return FinalizeLength(request, offsetof(Request, speed_data.crc) + 1);
    case WlanTotal_AIM:
        request->flow_data.id = index;
        request->flow_data.unit = 0x00;
        request->flow_data.totalflow = 0;
        return FinalizeLength(request, offsetof(Request, flow_data.crc) + 1);
    case WlanIP_AIM:
        request->wlanip_data.id = index;
        memset(request->wlanip_data.ip, 0, sizeof(request->wlanip_data.ip));
        return FinalizeLength(request, offsetof(Request, wlanip_data.crc) + 1);
    case GetVer_AIM:
        return FinalizeLength(request, offsetof(Request, Version_data.crc) + 1);
    default:
        return FinalizeLength(request, offsetof(Request, common_data.crc) + 1);
    }
}

int first_init_hidreport(Request* request, unsigned char cmd, unsigned char aim, unsigned char total, unsigned char order)
{
    if (!request) {
        return 0;
    }

    InitCommon(request, cmd, aim);

    switch (aim) {
    case HomePage_AIM:
        request->Homepage_data.order = order;
        request->Homepage_data.total = total;
        request->Homepage_data.time_info.timestamp = (unsigned int)time(NULL);
        return FinalizeLength(request, offsetof(Request, Homepage_data.crc) + 1);
    case SystemPage_AIM:
        request->SystemPage_data.order = order;
        request->SystemPage_data.total = total;
        request->SystemPage_data.syscount = 3;
        request->SystemPage_data.count = 2;

        request->SystemPage_data.systemPage[0].syslength = sizeof(SystemPage);
        request->SystemPage_data.systemPage[0].sys_id = (order == 1) ? 0 : 2;
        request->SystemPage_data.systemPage[0].usage = 0;
        request->SystemPage_data.systemPage[0].temp = 255;
        request->SystemPage_data.systemPage[0].rpm = 255;
        CopyAscii(request->SystemPage_data.systemPage[0].name,
                  sizeof(request->SystemPage_data.systemPage[0].name),
                  (order == 1) ? "CPU" : "Memory");

        request->SystemPage_data.systemPage[1].syslength = sizeof(SystemPage);
        request->SystemPage_data.systemPage[1].sys_id = (order == 1) ? 1 : 3;
        request->SystemPage_data.systemPage[1].usage = 0;
        request->SystemPage_data.systemPage[1].temp = 255;
        request->SystemPage_data.systemPage[1].rpm = 255;
        CopyAscii(request->SystemPage_data.systemPage[1].name,
                  sizeof(request->SystemPage_data.systemPage[1].name),
                  (order == 1) ? "iGPU" : "DGPU");
        return FinalizeLength(request, offsetof(Request, SystemPage_data.crc) + 1);
    case DiskPage_AIM:
        request->DiskPage_data.order = order;
        request->DiskPage_data.total = total;
        request->DiskPage_data.diskcount = 1;
        request->DiskPage_data.count = 1;
        request->DiskPage_data.diskStruct[0].disklength = sizeof(diskStruct);
        request->DiskPage_data.diskStruct[0].disk_id = 0;
        request->DiskPage_data.diskStruct[0].unit = 0x22;
        request->DiskPage_data.diskStruct[0].temp = 255;
        CopyAscii((unsigned char*)request->DiskPage_data.diskStruct[0].name,
                  sizeof(request->DiskPage_data.diskStruct[0].name),
                  "C:");
        return FinalizeLength(request, offsetof(Request, DiskPage_data.crc) + 1);
    case WlanPage_AIM:
        request->WlanPage_data.order = order;
        request->WlanPage_data.total = total;
        request->WlanPage_data.netcount = total;
        request->WlanPage_data.count = 1;
        request->WlanPage_data.online = 1;
        request->WlanPage_data.length = (unsigned char)(sizeof(WlanPage) + 1);
        request->WlanPage_data.wlanPage.id = (unsigned char)(order ? order - 1 : 0);
        request->WlanPage_data.wlanPage.unit = 0x00;
        CopyAscii(request->WlanPage_data.wlanPage.name,
                  sizeof(request->WlanPage_data.wlanPage.name),
                  "LAN");
        return FinalizeLength(request, offsetof(Request, WlanPage_data.crc) + 1);
    case InfoPage_AIM:
        request->InfoPage_data.order = order;
        request->InfoPage_data.total = total;
        request->InfoPage_data.namelength = 18;
        CopyAscii(request->InfoPage_data.name, sizeof(request->InfoPage_data.name), "HarborOS Windows");
        return FinalizeLength(request, offsetof(Request, InfoPage_data.crc) + 1);
    case ModePage_AIM:
        request->ModePage_data.order = 1;
        request->ModePage_data.total = 1;
        request->ModePage_data.powcount = 3;
        request->ModePage_data.count = 3;
        request->ModePage_data.mute = 1;
        request->ModePage_data.properties = 0;
        return FinalizeLength(request, offsetof(Request, ModePage_data.crc) + 1);
    default:
        return init_hidreport(request, cmd, aim, order);
    }
}

static BOOL CheckCrc(const Request* request, size_t crcOffset, unsigned char actual)
{
    return calculate_crc((const unsigned char*)request, (int)crcOffset) == actual;
}

BOOL ProtocolSelfTest(BOOL verbose)
{
    Request request;
    int size;
    BOOL ok = TRUE;
    const unsigned char crcProbe[] = { 0x01, 0x02, 0x03, 0xFE };

    ok = ok && calculate_crc(crcProbe, (int)sizeof(crcProbe)) == 0x04;

    size = first_init_hidreport(&request, SET, HomePage_AIM, 1, 1);
    append_crc(&request);
    ok = ok && size == (int)(offsetof(Request, Homepage_data.crc) + 1);
    ok = ok && request.header == SIGNATURE;
    ok = ok && request.length == (unsigned char)(size - REQUEST_LENGTH_OFFSET);
    ok = ok && CheckCrc(&request, offsetof(Request, Homepage_data.crc), request.Homepage_data.crc);

    size = init_hidreport(&request, SET, TIME_AIM, 255);
    append_crc(&request);
    ok = ok && size == (int)(offsetof(Request, time_data.crc) + 1);
    ok = ok && request.header == SIGNATURE;
    ok = ok && request.length == (unsigned char)(size - REQUEST_LENGTH_OFFSET);
    ok = ok && CheckCrc(&request, offsetof(Request, time_data.crc), request.time_data.crc);

    size = init_hidreport(&request, SET, System_AIM, 0);
    request.system_data.system_info.usage = 42;
    append_crc(&request);
    ok = ok && size == (int)(offsetof(Request, system_data.crc) + 1);
    ok = ok && request.length == (unsigned char)(size - REQUEST_LENGTH_OFFSET);
    ok = ok && CheckCrc(&request, offsetof(Request, system_data.crc), request.system_data.crc);

    size = init_hidreport(&request, SET, Disk_AIM, 0);
    append_crc(&request);
    ok = ok && size == (int)(offsetof(Request, disk_data.crc) + 1);
    ok = ok && request.length == (unsigned char)(size - REQUEST_LENGTH_OFFSET);
    ok = ok && CheckCrc(&request, offsetof(Request, disk_data.crc), request.disk_data.crc);

    size = first_init_hidreport(&request, SET, SystemPage_AIM, 2, 1);
    append_crc(&request);
    ok = ok && size == (int)(offsetof(Request, SystemPage_data.crc) + 1);
    ok = ok && request.length == (unsigned char)(size - REQUEST_LENGTH_OFFSET);
    ok = ok && CheckCrc(&request, offsetof(Request, SystemPage_data.crc), request.SystemPage_data.crc);

    if (verbose) {
        printf("Protocol selftest: %s\n", ok ? "PASS" : "FAIL");
        printf("Offsets: header=%zu sequence=%zu length=%zu cmd=%zu aim=%zu data=%zu\n",
               offsetof(Request, header),
               offsetof(Request, sequence),
               offsetof(Request, length),
               offsetof(Request, cmd),
               offsetof(Request, aim),
               REQUEST_DATA_OFFSET);
    }

    return ok;
}
