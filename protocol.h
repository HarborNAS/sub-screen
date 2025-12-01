typedef struct __attribute__((__packed__)) {
    unsigned int timestamp;
} Time;
typedef struct __attribute__((__packed__)) {
    unsigned char disk_id;      //>= 1
    unsigned char unit;         //01 Kb 02 Mb 03 Gb
    unsigned short total_size;  //Default -1
    unsigned short used_size;   //Default -1
    unsigned char temp;         //Default -1
} Disk;

typedef struct __attribute__((__packed__)) {
    unsigned short online;
} User;
typedef struct __attribute__((__packed__)) {
    unsigned char unit;
    unsigned short uploadspeed;
    unsigned short downloadspeed;
} Wlanspeed;
typedef struct __attribute__((__packed__)) {
    unsigned char sys_id;
    unsigned char usage;
    unsigned char temerature;
    unsigned char rpm;
} SystemD;

typedef struct __attribute__((__packed__)) {
    unsigned char software_version;
    unsigned char hardware_version;  
} About;

typedef struct __attribute__((__packed__)) {
    unsigned int usage;
} Memory;

typedef struct __attribute__((__packed__)) {
    unsigned char syslength;
    unsigned char sys_id;
    unsigned char usage;
    unsigned char temp;
    unsigned char rpm;
    unsigned char name[8];
} SystemPage;
typedef struct {
    unsigned char disklength;
    unsigned char disk_id;      //>= 1
    unsigned char unit;         //01 Kb 02 Mb 03 Gb
    unsigned char reserve;
    unsigned short total_size;  //Default -1
    unsigned short used_size;   //Default -1
    unsigned char temp;         //Default -1
    char name[16];              //Default -1
} diskStruct;
typedef struct {
    unsigned char id;
    unsigned char unit;
    unsigned short uploadspeed;
    unsigned short downloadspeed;
    unsigned char ip[4];
    unsigned char name[20];
} WlanPage;


// Request HID Report
typedef struct __attribute__((__packed__)) {
    unsigned short header;
    unsigned char sequence;
    unsigned char length;
    unsigned char cmd;
    unsigned char aim;
    union {
        struct __attribute__((__packed__)){
            unsigned char data;
            unsigned char crc;
        } common_data;
        struct __attribute__((__packed__)){
            Disk disk_info;
            unsigned char crc;
        } disk_data;

        struct __attribute__((__packed__)) {
            Time time_info;
            unsigned char crc;
        }time_data;

        struct __attribute__((__packed__)){
            SystemD system_info;
            unsigned char crc;
        } system_data;

        struct __attribute__((__packed__)){
            User user_info;
            unsigned char crc;
        } user_data;
        struct __attribute__((__packed__)) {
            unsigned char id;       //default = 1
            Wlanspeed speed_info;
            unsigned char crc;
        } speed_data;
        struct __attribute__((__packed__)) {
            unsigned char id;       //default = 1
            unsigned char unit;
            unsigned short totalflow;
            unsigned char crc;
        } flow_data;
        struct __attribute__((__packed__)) {
            unsigned char id;       //default = 1
            unsigned char ip[4];
            unsigned char crc;
        } wlanip_data;
        struct __attribute__((__packed__))
        {
            Memory memory_info;
            unsigned char crc;
        } memory_data;
        //initial page
        struct __attribute__((__packed__))
        {
            unsigned char order;
            unsigned char total;
            Time time_info;
            unsigned char crc;
        } Homepage_data;
        struct __attribute__((__packed__)){
            unsigned char order;
            unsigned char total;
            unsigned char syscount;
            unsigned char count;
            SystemPage systemPage[2];
            unsigned char crc;
        } SystemPage_data;

        struct __attribute__((__packed__)){
            unsigned char order;
            unsigned char total;
            unsigned char diskcount;
            unsigned char count;
            diskStruct diskStruct[2];
            unsigned char crc;
        } DiskPage_data;
        struct __attribute__((__packed__)){
            unsigned char order;
            unsigned char total;
            unsigned char netcount;
            unsigned char count;
            unsigned char online;
            unsigned char length;
            WlanPage wlanPage;
            unsigned char crc;
        } WlanPage_data;
        struct __attribute__((__packed__)){
            unsigned char order;
            unsigned char total;
            unsigned short reserve;
            unsigned char namelength;
            unsigned char name[48];
            unsigned char crc;
        } InfoPage_data;
        struct __attribute__((__packed__)){
            unsigned char order;
            unsigned char total;
            unsigned char powcount;
            unsigned char count;
            unsigned char mute;
            unsigned char properties;
            unsigned char crc;
        } ModePage_data;
    };
} Request;

// Ack HID Report
typedef struct __attribute__((__packed__)) {
    unsigned short header;
    unsigned char sequence;
    unsigned char length;
    unsigned char cmd;
    unsigned char err;

    union {
        struct __attribute__((__packed__)) {
            // For set command
            unsigned char crc;
        } set_response;

        struct __attribute__((__packed__)) {
            About about_info;
            unsigned char crc;
        } about_data;

        struct __attribute__((__packed__)) {
            Time time_info;
            unsigned char crc;
        }time_data;

        struct __attribute__((__packed__)) {
            User user_info;
            unsigned char crc;
        } user_data;

        struct __attribute__((__packed__)){
            unsigned char data;
            unsigned char crc;
        } common_data;
    };
} Ack;

#define SIGNATURE 0x5aa5

// Command Definition
#define GET 0x1
#define SET 0x2
#define AUTOSET 0x3
// Error Definition
#define SUCCESS 0x00
#define PACKETLOSS 0x1
#define PACKETLENMISMATCH 0x02
#define PACKETLENTOOLONG 0x03
#define BADHEADER 0x04
#define DATALENMISMATCH 0x05
#define VERIFICATIONERR 0x06
#define CMDERR 0x07

// AIM
#define HomePage_AIM 0x00
#define TIME_AIM 0x01
#define SystemPage_AIM 0x10
#define System_AIM 0x11
#define DiskPage_AIM 0x50
#define Disk_AIM 0x51
#define WlanPage_AIM 0x60
#define USER_AIM 0x61
#define WlanSpeed_AIM 0x62
#define WlanTotal_AIM 0x64
#define WlanIP_AIM 0x65
#define ModePage_AIM 0x70
#define Mute_AIM 0x71
#define Properties_AIM 0x72
#define Balance_AIM 0x73
#define HIBERNATEATONCE_AIM 0x81
#define InfoPage_AIM 0xA0
