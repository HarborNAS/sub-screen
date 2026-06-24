#include "service.h"

#include "protocol.h"
#include "usb_comm.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OTA_CHUNK_SIZE 56

static unsigned char SumCrc(const unsigned char* data, int length)
{
    unsigned int crc = 0;
    for (int i = 0; i < length; ++i) {
        crc += data[i];
    }
    return (unsigned char)(crc & 0xFF);
}

static void FinalizeSumCrc(unsigned char* packet, int packetSize)
{
    if (packet && packetSize > 0) {
        packet[packetSize - 1] = SumCrc(packet, packetSize - 1);
    }
}

static BOOL SendPacketAndRead(SubscreenUsbDevice* device,
                              const unsigned char* packet,
                              DWORD packetSize,
                              unsigned char response[64],
                              DWORD* responseSize,
                              DWORD timeoutMs)
{
    if (responseSize) {
        *responseSize = 0;
    }
    memset(response, 0, 64);

    if (!UsbWrite(device, packet, packetSize, SUBSCREEN_TIMEOUT_MS)) {
        printf("USB write failed (%lu).\n", UsbLastError());
        return FALSE;
    }

    if (!UsbRead(device, response, 64, responseSize, timeoutMs)) {
        printf("USB read failed or timed out (%lu).\n", UsbLastError());
        return FALSE;
    }

    return TRUE;
}

int RunFirmwareVersion(void)
{
    SubscreenUsbDevice* device = NULL;
    Request request;
    int packetSize;
    unsigned char response[64];
    DWORD responseSize = 0;
    int result = 1;

    if (!UsbOpen(&device)) {
        printf("Cannot open subscreen USB device (%lu).\n", UsbLastError());
        return 1;
    }

    packetSize = init_hidreport(&request, GET, GetVer_AIM, 255);
    FinalizeSumCrc((unsigned char*)&request, packetSize);

    if (SendPacketAndRead(device, (const unsigned char*)&request, (DWORD)packetSize, response, &responseSize, 3000)) {
        printf("Firmware response (%lu bytes):", responseSize);
        for (DWORD i = 0; i < responseSize; ++i) {
            if ((i % 16) == 0) {
                printf("\n  ");
            }
            printf("%02X ", response[i]);
        }
        printf("\n");

        if (responseSize >= 9 && response[0] == 0xA5 && response[1] == 0x5A) {
            printf("Firmware version: %u.%u.%u.%u\n", response[5], response[6], response[7], response[8]);
            result = 0;
        } else {
            printf("Firmware version response is not recognized.\n");
        }
    }

    UsbClose(device);
    return result;
}

static BOOL ReadFirmwareFile(const char* path, unsigned char** outData, uint32_t* outSize)
{
    FILE* file = NULL;
    long size;
    unsigned char* data;

    if (!path || !outData || !outSize) {
        return FALSE;
    }
    *outData = NULL;
    *outSize = 0;

    if (fopen_s(&file, path, "rb") != 0 || !file) {
        printf("Cannot open firmware file: %s\n", path);
        return FALSE;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return FALSE;
    }
    size = ftell(file);
    if (size <= 0 || size > (960L * 1024L)) {
        printf("Firmware file size is invalid: %ld bytes\n", size);
        fclose(file);
        return FALSE;
    }
    rewind(file);

    data = (unsigned char*)malloc((size_t)size);
    if (!data) {
        fclose(file);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, file) != (size_t)size) {
        printf("Failed to read firmware file.\n");
        free(data);
        fclose(file);
        return FALSE;
    }

    fclose(file);
    *outData = data;
    *outSize = (uint32_t)size;
    return TRUE;
}

static BOOL SendUpgradeInfo(SubscreenUsbDevice* device, uint32_t firmwareSize)
{
    Request request;
    int packetSize;
    unsigned char response[64];
    DWORD responseSize = 0;

    memset(&request, 0, sizeof(request));
    request.header = SIGNATURE;
    request.sequence = 0;
    request.cmd = UPDATE;
    request.aim = Updatefw_info_AIM;
    request.UpgradeInfo_data.build = 0;
    request.UpgradeInfo_data.major = 3;
    request.UpgradeInfo_data.minor = 1;
    request.UpgradeInfo_data.patch = 0;
    request.UpgradeInfo_data.size = firmwareSize;
    packetSize = (int)(offsetof(Request, UpgradeInfo_data.crc) + 1);
    request.length = (unsigned char)(packetSize - offsetof(Request, length));
    FinalizeSumCrc((unsigned char*)&request, packetSize);

    printf("Sending firmware metadata, size=%u bytes.\n", firmwareSize);
    if (!SendPacketAndRead(device, (const unsigned char*)&request, (DWORD)packetSize, response, &responseSize, 5000)) {
        return FALSE;
    }

    if (responseSize >= 6 && response[5] != 0) {
        printf("Device rejected firmware metadata, error=%u.\n", response[5]);
        return FALSE;
    }
    return TRUE;
}

static BOOL SendFirmwareChunks(SubscreenUsbDevice* device, const unsigned char* firmware, uint32_t firmwareSize)
{
    uint32_t sent = 0;
    unsigned char seq = 0;

    while (sent < firmwareSize) {
        uint32_t remaining = firmwareSize - sent;
        int sendLen = (remaining < OTA_CHUNK_SIZE) ? (int)remaining : OTA_CHUNK_SIZE;
        unsigned char packet[64];
        int packetSize = 6 + sendLen + 1;
        unsigned char response[64];
        DWORD responseSize = 0;

        memset(packet, 0, sizeof(packet));
        packet[0] = 0xA5;
        packet[1] = 0x5A;
        packet[2] = seq++;
        packet[3] = (unsigned char)(packetSize - 3);
        packet[4] = UPDATE;
        packet[5] = Updatefw_AIM;
        memcpy(packet + 6, firmware + sent, (size_t)sendLen);
        FinalizeSumCrc(packet, packetSize);

        if (!SendPacketAndRead(device, packet, (DWORD)packetSize, response, &responseSize, 5000)) {
            return FALSE;
        }
        if (responseSize >= 6 && response[5] != 0) {
            printf("Device rejected OTA chunk at %u, error=%u.\n", sent, response[5]);
            return FALSE;
        }

        sent += (uint32_t)sendLen;
        printf("\rFirmware upload: %u%% (%u/%u)", (unsigned int)((sent * 100ULL) / firmwareSize), sent, firmwareSize);
        fflush(stdout);
    }

    printf("\n");
    return TRUE;
}

static BOOL SendFinishSignal(SubscreenUsbDevice* device)
{
    Request request;
    int packetSize;
    unsigned char response[64];
    DWORD responseSize = 0;

    memset(&request, 0, sizeof(request));
    request.header = SIGNATURE;
    request.sequence = 0;
    request.cmd = UPDATE;
    request.aim = Updatefw_AIM;
    packetSize = (int)(offsetof(Request, OTA_Enddata.crc) + 1);
    request.length = (unsigned char)(packetSize - offsetof(Request, length));
    FinalizeSumCrc((unsigned char*)&request, packetSize);

    printf("Sending firmware finish signal.\n");
    if (!SendPacketAndRead(device, (const unsigned char*)&request, (DWORD)packetSize, response, &responseSize, 10000)) {
        return FALSE;
    }

    if (responseSize >= 6 && response[5] != 0) {
        printf("Device rejected finish signal, error=%u.\n", response[5]);
        return FALSE;
    }
    return TRUE;
}

int RunFirmwareUpdate(const char* firmwarePath)
{
    SubscreenUsbDevice* device = NULL;
    unsigned char* firmware = NULL;
    uint32_t firmwareSize = 0;
    int result = 1;

    if (!ReadFirmwareFile(firmwarePath, &firmware, &firmwareSize)) {
        return 1;
    }

    if (!UsbOpen(&device)) {
        printf("Cannot open subscreen USB device (%lu). Stop the service if it is using the device.\n", UsbLastError());
        free(firmware);
        return 1;
    }

    if (SendUpgradeInfo(device, firmwareSize) &&
        SendFirmwareChunks(device, firmware, firmwareSize) &&
        SendFinishSignal(device)) {
        printf("Firmware update command completed. The panel may reboot.\n");
        result = 0;
    }

    UsbClose(device);
    free(firmware);
    return result;
}
