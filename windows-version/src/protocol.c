#include "protocol.h"
#include <string.h>

// CRC calculation (simple XOR-based CRC as used in the Linux version)
unsigned char calculate_crc(const unsigned char* data, int length) {
    unsigned char crc = 0;
    for (int i = 0; i < length; i++) {
        crc ^= data[i];
    }
    return crc;
}

// Append CRC to request
void append_crc(Request* request) {
    // Calculate CRC over the entire packet except the CRC field itself
    unsigned char* data = (unsigned char*)request;
    int length = request->length + 4; // header(2) + sequence(1) + length(1) + data

    // Find the CRC position and calculate
    unsigned char crc_pos = 4 + request->length - 1; // CRC is the last byte of data
    request->common_data.crc = calculate_crc(data, crc_pos);
}

// Initialize HID report with common fields
int init_hidreport(Request* request, unsigned char cmd, unsigned char aim, unsigned char index) {
    static unsigned char sequence = 0;

    memset(request, 0, sizeof(Request));

    request->header = SIGNATURE;
    request->sequence = sequence++;
    request->cmd = cmd;
    request->aim = aim;

    // Set length based on command and aim
    switch (aim) {
        case TIME_AIM:
            request->length = sizeof(Time) + 1; // +1 for CRC
            break;
        case System_AIM:
            request->length = sizeof(SystemD) + 1;
            break;
        case Disk_AIM:
            request->length = sizeof(Disk) + 1;
            break;
        case USER_AIM:
            request->length = sizeof(User) + 1;
            break;
        case WlanSpeed_AIM:
            request->length = sizeof(unsigned char) + sizeof(Wlanspeed) + 1; // id + speed_info + crc
            request->speed_data.id = index + 1;
            break;
        case WlanTotal_AIM:
            request->length = sizeof(unsigned char) + sizeof(unsigned char) + sizeof(unsigned short) + 1; // id + unit + totalflow + crc
            request->flow_data.id = index + 1;
            break;
        case WlanIP_AIM:
            request->length = sizeof(unsigned char) + 4 + 1; // id + ip[4] + crc
            request->wlanip_data.id = index + 1;
            break;
        default:
            request->length = 1 + 1; // data + crc
            break;
    }

    return request->length + 4; // Total packet size: header + sequence + length + data
}