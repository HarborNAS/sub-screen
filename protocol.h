typedef struct __attribute__((__packed__)) {
    unsigned int timestamp;
} Time;

typedef struct __attribute__((__packed__)) {
    unsigned char temperature;
    unsigned char usage;
    unsigned char rpm;
} GPU;

typedef struct __attribute__((__packed__)) {
    unsigned short online;
} User;

typedef struct __attribute__((__packed__)) {
    unsigned char temerature;
    unsigned char usage;
    unsigned char rpm;
} CPU;

typedef struct __attribute__((__packed__)) {
    unsigned char software_version;
    unsigned char hardware_version;  
} About;

typedef struct __attribute__((__packed__)) {
    unsigned int usage;
} Memory;

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
            GPU gpu_info;
            unsigned char crc;
        } gpu_data;

        struct __attribute__((__packed__)) {
            Time time_info;
            unsigned char crc;
        }time_data;

        struct __attribute__((__packed__)){
            CPU cpu_info;
            unsigned char crc;
        } cpu_data;

        struct __attribute__((__packed__)){
            User user_info;
            unsigned char crc;
        } user_data;

        struct __attribute__((__packed__))
        {
            Memory memory_info;
            unsigned char crc;
        } memory_data;
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
#define TIME_AIM 0x00

#define SILENCE_AIM 0x10
#define BALANCE_AIM 0x11
#define PERFORMANCE_AIM 0x12

#define USER_AIM 0x20

#define WATCHFACE_AIM 0x30
#define USEWATCHFACE_AIM 0x31

// AIM SIDESCREEN
#define NOTIFY_AIM 0x40
#define COMPONENT_AIM 0x41


// AIM SIDESCREENCONF
#define BRIGHTNESS_AIM 0x42
#define DISPLAYMODE_AIM 0x43
#define CHANGESKIN_AIM 0x44
#define ABOUT_AIM 0x45

// AIM HIBERNATE
#define COUNTDOWN_AIM 0x50
#define HIBERNATECANCEL_AIM 0x51
#define HIBERNATEATONCE_AIM 0x52

// AIM SYSTEM
#define CPU_AIM 0x60
#define GPU_AIM 0x61
#define MEMORY_AIM 0x62
