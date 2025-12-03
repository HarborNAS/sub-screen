#include <hidapi/hidapi.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include "protocol.h"
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <stddef.h>
#include <sys/sysinfo.h>
#include <sys/statvfs.h>
#include <mntent.h>
#include <dirent.h>
#include <utmp.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <sys/io.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>
//异步HID通信
#include <pthread.h>
#include <signal.h>

#include <ctype.h>
#include <stdarg.h>
//Discrete GPU
#include <nvml.h>
#include <glob.h>

//WLAN
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#define APIC_ADDRESS 0xFEC00000
#define APIC_IRQ 0x09
#define DebugToken   true
#define SensorLog   true
#define IfNoPanel   false
#define hidwritedebug true
#define MAXLEN 0x40
#define PRODUCTID 0x0002
#define VENDORID 0x5448
#define DURATION 1
#define MAX_PATH 256
#define MAX_LINE 512
#define MAX_DISKS 16
// ITE
#define ITE_EC_DATA_PORT    0x62
#define ITE_EC_INDEX_PORT   0x66
#define ITE_EC_CMD_PORT     0x66

// EC RAM
#define EC_CMD_READ_RAM     0x80    //Test for read CPUTemp
#define EC_CMD_WRITE_RAM    0x81
#define EC_CMD_QUERY        0x84

// CPU使用率计算结构体
typedef struct {
    unsigned long user;
    unsigned long nice;
    unsigned long system;
    unsigned long idle;
    unsigned long iowait;
    unsigned long irq;
    unsigned long softirq;
} CPUData;
typedef struct {
    char device[32];           // 设备名 (sda, nvme0n1等)
    char model[128];           // 硬盘型号
    char serial[64];           // 序列号
    char mountpoint[MAX_PATH]; // 挂载点
    unsigned long long total_size;    // 总容量 (bytes)
    unsigned long long free_size;     // 可用容量 (bytes)
    unsigned long long used_size;     // 已用容量 (bytes)
    double usage_percent;      // 使用百分比
    int temperature;           // 温度 (°C)
    char type[16];             // 硬盘类型 (SATA/NVMe)
} disk_info_t;

typedef struct {
    char name[128];
    int temperature;          // 温度 (°C)
    int utilization_gpu;      // GPU使用率 (%)
    int utilization_memory;   // 显存使用率 (%)
    long memory_used;         // 已用显存 (MB)
    long memory_total;        // 总显存 (MB)
    double power_draw;        // 功耗 (W)
    int fan_speed;           // 风扇转速 (%)
    char driver_version[32]; // 驱动版本
} nvidia_gpu_info_t;
#define MAX_INTERFACES 32
#define MAX_IP_LENGTH 46  // IPv6地址最大长度
// 网络接口信息结构体
typedef struct {
    char interface_name[32];
    char ip_address[INET_ADDRSTRLEN];
    char netmask[INET_ADDRSTRLEN];
    char mac_address[18];
    char status[16];
    unsigned long long rx_bytes;
    unsigned long long tx_bytes;
    unsigned long long rx_bytes_prev;
    unsigned long long tx_bytes_prev;
    time_t last_update;
    double rx_speed_kb;
    double tx_speed_kb;
    double rx_total_mb;
    double tx_total_mb;
    int initialized;  // 标记是否已初始化
} network_interface_t;
// 全局接口管理器
typedef struct {
    network_interface_t *interfaces;
    int count;
    int capacity;
} interface_manager_t;
static unsigned char COMMLEN = offsetof(Request, common_data.data) - offsetof(Request, length);
void TimeSleep1Sec();
int init_hidreport(Request *request, unsigned char cmd, unsigned char aim, unsigned char id);
int first_init_hidreport(Request* request, unsigned char cmd, unsigned char aim,unsigned char total,unsigned char order);
void append_crc(Request *request);
unsigned char cal_crc(unsigned char * data, int len);
int is_truenas_system(void);
int get_cpu_temperature();
int read_temperature_from_hwmon(void);
int read_temperature_for_truenas(void);
int read_temperature_via_truenas_api(void);
int read_temperature_freebsd_style(void);
void read_cpu_data(CPUData *data);
float calculate_cpu_usage(const CPUData *prev, const CPUData *curr);
int get_igpu_temperature();
float get_igpu_usage();
unsigned int get_memory_usage();
int GetUserCount();
int file_exists(const char *filename);
int read_file(const char *filename, char *buffer, size_t buffer_size);
int get_disk_temperature(const char *device);
void get_disk_identity(const char *device, char *model, char *serial);
unsigned long long get_disk_size(const char *device);
void get_mountpoint(const char *device, char *mountpoint);
int get_mountpoint_usage_statvfs(const char *mountpoint, unsigned long long *total, 
                        unsigned long long *free, unsigned long long *used);
int scan_disk_devices(disk_info_t *disks, int max_disks);


//EC6266
int acquire_io_permissions();
void release_io_permissions();
int ec_wait_ready();
void ec_write_index(unsigned char index);
void ec_write_data(unsigned char data);
unsigned char ec_read_data();
int ec_ram_read_byte(unsigned char address, unsigned char *value);
int ec_ram_write_byte(unsigned char address, unsigned char value);
int ec_ram_read_block(unsigned char start_addr, unsigned char *buffer, int length);
int ec_ram_write_block(unsigned char start_addr, unsigned char *data, int length);
int ec_query_version(char *version, int max_len);
void nvidia_print_info();
//异步HID
void signal_handler(int sig);
void* hid_read_thread(void *arg);
void* hid_send_thread(void* arg);
int safe_hid_write(hid_device *handle, const unsigned char *data, int length);
void systemoperation(unsigned char time,unsigned char cmd);
//WLAN
static interface_manager_t g_iface_manager = {0};
network_interface_t *ifaces;
int init_network_monitor();
int register_interface(const char *ifname);
void monitor_all_interfaces();
int register_all_physical_interfaces();
int get_interface_basic_info(const char *ifname, 
                           char *status, 
                           char *mac_addr,
                           char *ip_addr,
                           char *netmask);
int get_interface_traffic_info(const char *ifname,
                             double *rx_speed_kb,
                             double *tx_speed_kb,
                             double *rx_total_mb,
                             double *tx_total_mb);
int get_registered_interfaces(char interfaces[][32], int max_interfaces);
void cleanup_network_monitor();



bool IsNvidiaGPU;
int disk_count;
char PageIndex = 0;

//1Hour Count
int HourTimeDiv = 0;
disk_info_t disks[MAX_DISKS];
static volatile bool running = true;
static pthread_t read_thread,send_thread;
static pthread_mutex_t hid_mutex = PTHREAD_MUTEX_INITIALIZER;
struct utmp *ut;
//Discrete GPU
typedef struct {
    unsigned int index;
    char name[64];
    nvmlTemperatureSensors_t temp_sensor;
    unsigned int temperature;
    unsigned int utilization;
    unsigned int memory_used;
    unsigned int memory_total;
    unsigned int power_usage;
    unsigned int fan_speed;
} gpu_info_t;
int nvidia_smi_available();
int nvidia_get_gpu_temperature();
int nvidia_get_gpu_utilization();
int nvidia_get_gpu_fan_speed();
typedef struct {
    char devicename[64];
    char cpuname[128];
    char operatename[128];
    char serial_number[64];
} system_info_t;
void get_system_info(system_info_t *info);






int cpusuage,cpufan,memoryusage;
bool Isinitial = false;
unsigned char cputemp = 0;
unsigned char hid_report[MAXLEN] = {0};
unsigned char ack[MAXLEN] = {0};
system_info_t sys_info;
Request* request = (Request *)hid_report;
hid_device *handle;
CPUData prev_data, curr_data;
int main(void) {
    // 设置信号处理
    #if !IfNoPanel
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    #if DebugToken
    printf("Step 1: Initializing HIDAPI...\n");
    #endif
    int res = hid_init();
    if (res != 0) {
        printf("ERROR: HIDAPI initialization failed with code %d\n", res);
        return -1;
    }
    #if DebugToken
    printf("HIDAPI initialized successfully\n");
    #endif
    // 初始化数据
    handle = hid_open(VENDORID, PRODUCTID, NULL);
    
    #if !IfNoPanel
    if (handle == NULL) {
        printf("ERROR: Failed to open device %04x:%04x\n", VENDORID, PRODUCTID);
        res = hid_exit();
        return 0;
    }
    #endif
   
    #if DebugToken
    printf("HID device opened successfully\n");
    #endif
    #endif
    IsNvidiaGPU = nvidia_smi_available();

     #if DebugToken
    printf("WLAN Port=======================\n\n");
    #endif
    // 扫描所有物理网络接口
    if (init_network_monitor() < 0) {
        printf("No WLAN Port!\n");
        return -1;
    }
    // 注册所有物理接口
    int registered = register_all_physical_interfaces();
    monitor_all_interfaces();
    printf("CPUTemp:,%d\n",get_cpu_temperature());
    // 创建读取线程
    #if !IfNoPanel
    if (pthread_create(&read_thread, NULL, hid_read_thread, handle) != 0) {
        printf("Failed to create read thread\n");
        hid_close(handle);
        hid_exit();
        return -1;
    }
    #endif

    #if DebugToken
    // printf("CPUTemp:%d\n",cputemp);
    // unsigned char ECcputemp = 0;
    // // 获取 I/O 权限
    // acquire_io_permissions();
    // ec_ram_read_byte(0x70,&ECcputemp);
    // printf("EC响应CPU Temp: 0x%02X\n", ECcputemp);
    // ec_ram_write_byte(0x40,0xFF);

    // // 释放 I/O 权限
    // release_io_permissions();
    #endif
    // 扫描硬盘设备
    disk_count = scan_disk_devices(disks, MAX_DISKS);
    if (disk_count <= 0) {
        printf("未找到硬盘设备\n");
        return 1;
    }
    else
    {
        #if DebugToken
        printf("Diskcount:%d\n",disk_count);
        #endif
    }
    #if SensorLog
    for (int i = 0; i < disk_count; i++) {
        //if (disks[i].temperature != -1) {
            printf("%s:\n",disks[i].device);
            printf("Temp:%d°C\n", disks[i].temperature);
            printf("Totalsize:%lld\n", disks[i].total_size);
            printf("Usedsize:%lld\n", disks[i].used_size);
            printf("UsedPercent:%f\n", disks[i].usage_percent);
        //}
    }
    #endif
    
    //HomePage
    #if DebugToken
    printf("-----------------------------------HomePage initial start-----------------------------------\n");
    #endif
    int homepage = init_hidreport(request, SET, TIME_AIM, 255);
    append_crc(request);
    if (safe_hid_write(handle, hid_report, homepage) == -1) {
        printf("Failed to write HomePage data\n");
    }
    sleep(1);
    memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
    #if DebugToken
    printf("-----------------------------------HomePage initial end-----------------------------------\n");
    #endif
    //SystemPage
    #if DebugToken
    printf("-----------------------------------SystemPage initial start-----------------------------------\n");
    #endif
    int systempage1 = first_init_hidreport(request, SET, SystemPage_AIM, 2, 1);
    append_crc(request);
    if (safe_hid_write(handle, hid_report, systempage1) == -1) {
        printf("Failed to write SystemPage data\n");
    }
    sleep(3);
    memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
    #if DebugToken
    printf("-----------------------------------SystemPage initial second-----------------------------------\n");
    #endif
    int systempage2 = first_init_hidreport(request, SET, SystemPage_AIM, 2, 2);
    append_crc(request);
    if (safe_hid_write(handle, hid_report, systempage2) == -1) {
        printf("Failed to write SystemPage data\n");
    }
    sleep(1);
    memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
    #if DebugToken
    printf("-----------------------------------SystemPage initial end-----------------------------------\n");
    #endif
    //DiskPage
    #if DebugToken
    printf("-----------------------------------DiskPage initial start-----------------------------------\n");
    #endif
    int diskforcount,diskpage;
    if(disk_count % 2 == 0)
        diskforcount = disk_count / 2;
    else
        diskforcount = disk_count / 2 + 1;
    for (int i = 0; i < diskforcount; i++)
    {
        diskpage = first_init_hidreport(request, SET, DiskPage_AIM, diskforcount, (i + 1));
        append_crc(request);
        if (safe_hid_write(handle, hid_report, diskpage) == -1) {
            printf("Failed to write DiskPage data\n");
            break;
        }
        #if DebugToken
        // printf("-----------------------------------DiskPage send %d times-----------------------------------\n",(i+1));
        // printf("Diskpage Head: %x\n",request->header);
        // printf("sequence %d\n",request->sequence);
        // printf("lenth %d\n",request->length);
        // printf("cmd %d\n",request->cmd);
        // printf("aim %d\n",request->aim);
        // printf("order %d\n",request->DiskPage_data.order);
        // printf("total: %d\n \n",request->DiskPage_data.total);
        // printf("diskcount %d\n",request->DiskPage_data.diskcount);
        // printf("count: %d\n",request->DiskPage_data.count);
        // printf("DiskLength: %d\n",request->DiskPage_data.diskStruct[0].disklength);
        // printf("Diskid: %d\n",request->DiskPage_data.diskStruct[0].disk_id);
        // printf("Diskunit: %d\n",request->DiskPage_data.diskStruct[0].unit);
        // printf("Disktotal: %d\n",request->DiskPage_data.diskStruct[0].total_size);
        // printf("Diskused: %d\n",request->DiskPage_data.diskStruct[0].used_size);
        // printf("Disktemp: %d\n",request->DiskPage_data.diskStruct[0].temp);
        // printf("Diskname: %s\n",request->DiskPage_data.diskStruct[0].name);
        // if(disk_count-(i*2)>1)
        // {
        //     printf("DiskLength: %d\n",request->DiskPage_data.diskStruct[1].disklength);
        //     printf("Diskid: %d\n",request->DiskPage_data.diskStruct[1].disk_id);
        //     printf("Diskunit: %d\n",request->DiskPage_data.diskStruct[1].unit);
        //     printf("Disktotal: %d\n",request->DiskPage_data.diskStruct[1].total_size);
        //     printf("Diskused: %d\n",request->DiskPage_data.diskStruct[1].used_size);
        //     printf("Disktemp: %d\n",request->DiskPage_data.diskStruct[1].temp);
        //     printf("Diskname: %s\n",request->DiskPage_data.diskStruct[1].name);
        // }
        // printf("CRC:%d\n",request->DiskPage_data.crc);
        // printf("Send %d time\n",(i+1));
        #endif
        sleep(3);
        memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
        
    }
    #if DebugToken
    printf("-----------------------------------DiskPage initial end-----------------------------------\n");
    #endif
    //ModePage
    #if DebugToken
    printf("-----------------------------------ModePage initial start-----------------------------------\n");
    #endif
    int modepage = first_init_hidreport(request, SET, ModePage_AIM, 255, 255);
    append_crc(request);
    if (safe_hid_write(handle, hid_report, modepage) == -1) {
        printf("Failed to write ModePage data\n");
    }
    #if DebugToken
    printf("-----------------------------------ModePage initial end-----------------------------------\n");
    #endif
    sleep(3);
    memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
    //WLANPage
    #if DebugToken
    printf("-----------------------------------WLANPage initial start-----------------------------------\n");
    #endif
    int wlanpage;
    for (int i = 0; i < g_iface_manager.count; i++)
    {
        wlanpage = first_init_hidreport(request, SET, WlanPage_AIM, g_iface_manager.count, i + 1);
        append_crc(request);
        if (safe_hid_write(handle, hid_report, wlanpage) == -1) {
            printf("Failed to write WlanPage data\n");
        }
        sleep(3);
        memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
        /* code */
    }
    #if DebugToken
    printf("-----------------------------------WLANPage initial end-----------------------------------\n");
    #endif

    get_system_info(&sys_info);
    printf("Device Name:%s\n",sys_info.devicename);
    printf("CPU Name:%s\n",sys_info.cpuname);
    printf("OS Name:%s\n",sys_info.operatename);
    printf("SN:%s\n",sys_info.serial_number);
    #if DebugToken
    printf("-----------------------------------InfoPage initial start-----------------------------------\n");
    #endif
    int infopage;
    infopage = first_init_hidreport(request, SET, InfoPage_AIM, 4, 1);
    append_crc(request);
    if (safe_hid_write(handle, hid_report, infopage) == -1) {
        printf("Failed to write InfoPage data\n");
    }
    sleep(1);
    memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
    infopage = first_init_hidreport(request, SET, InfoPage_AIM, 4, 2);
    append_crc(request);
    if (safe_hid_write(handle, hid_report, infopage) == -1) {
        printf("Failed to write InfoPage data\n");
    }
    sleep(1);
    memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
    infopage = first_init_hidreport(request, SET, InfoPage_AIM, 4, 3);
    append_crc(request);
    if (safe_hid_write(handle, hid_report, infopage) == -1) {
        printf("Failed to write InfoPage data\n");
    }
    sleep(1);
    memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
    infopage = first_init_hidreport(request, SET, InfoPage_AIM, 4, 4);
    append_crc(request);
    if (safe_hid_write(handle, hid_report, infopage) == -1) {
        printf("Failed to write InfoPage data\n");
    }
    sleep(1);
    memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
    #if DebugToken
    printf("-----------------------------------InfoPage initial end-----------------------------------\n");
    #endif

    // 创建读取线程
    #if !IfNoPanel
    if (pthread_create(&send_thread, NULL, hid_send_thread, handle) != 0) {
        printf("Failed to create send thread\n");
        hid_close(handle);
        hid_exit();
        return -1;
    }
    #endif
    Isinitial = true;

    while (running) {

        //TimeSleep1Sec();
    }
    // 释放内存
    #if !IfNoPanel
    if (read_thread) {
        pthread_join(read_thread, NULL);
    }
    if (send_thread) {
        pthread_join(send_thread, NULL);
    }
    hid_close(handle);
    res = hid_exit();
    #endif
    printf("程序已安全退出\n");
    return 0;
}

void TimeSleep1Sec()
{
    if(HourTimeDiv == 3061)
        HourTimeDiv = 0;
    HourTimeDiv ++;
    // 休眠1秒，但分段休眠以便及时响应退出
    for (int i = 0; i < 10 && running; i++) {
        usleep(100000); // 100ms
    }
}
int init_hidreport(Request* request, unsigned char cmd, unsigned char aim,unsigned char id) {
    request->header = SIGNATURE;
    request->cmd = cmd;
    request->aim = aim;
    request->length = COMMLEN;

    switch (aim)
    {
    case TIME_AIM:
        request->length += sizeof(request->time_data);
        request->time_data.time_info.timestamp = time(NULL) + 28800;
        return offsetof(Request, time_data.crc) + 1;
    case System_AIM:
        request->length += sizeof(request->system_data);
        request->system_data.system_info.sys_id = id;
        if(id == 0)
        {
            
            // 读取当前CPU数据
            read_cpu_data(&curr_data);
            // 计算CPU使用率
            //printf("CPU usage: %2f\n", calculate_cpu_usage(&prev_data, &curr_data));
            request->system_data.system_info.usage = calculate_cpu_usage(&prev_data, &curr_data);
            // 更新前一次的数据
            prev_data = curr_data;
            // 获取 I/O 权限
            acquire_io_permissions();
            ec_ram_read_byte(0x70,&cputemp);
            request->system_data.system_info.temerature = cputemp;
            
            unsigned char CPU_fan_H = 0,CPU_fan_L = 0;
            ec_ram_read_byte(0x76,&CPU_fan_H);
            ec_ram_read_byte(0x77,&CPU_fan_L);
            request->system_data.system_info.rpm = (CPU_fan_H * 0xFF + CPU_fan_L) / 42;
            // 释放 I/O 权限
            release_io_permissions();
        }
        else if(id == 1)
        {
            request->system_data.system_info.usage = get_igpu_usage();
            request->system_data.system_info.temerature = get_igpu_temperature();
            acquire_io_permissions();
            unsigned char CPU_fan_H = 0,CPU_fan_L = 0;
            ec_ram_read_byte(0x76,&CPU_fan_H);
            ec_ram_read_byte(0x77,&CPU_fan_L);
            request->system_data.system_info.rpm = (CPU_fan_H * 0xFF + CPU_fan_L) / 42;
            // 释放 I/O 权限
            release_io_permissions();
        }
        else if(id == 2)
        {
            //memory
            
            request->system_data.system_info.usage = get_memory_usage();
            request->system_data.system_info.temerature = 255;
            request->system_data.system_info.rpm = 255;
        }
        else if (id == 3)
        {
            if(IsNvidiaGPU)
            {
                unsigned char DGPUtemp;
                DGPUtemp = nvidia_get_gpu_temperature();
                request->system_data.system_info.usage = nvidia_get_gpu_utilization();
                request->system_data.system_info.temerature = DGPUtemp;
                request->system_data.system_info.rpm = nvidia_get_gpu_fan_speed();
                // 获取 I/O 权限
                acquire_io_permissions();
                ec_ram_write_byte(0xB0,DGPUtemp);
                // 释放 I/O 权限
                release_io_permissions();
            }
        }
        return offsetof(Request, system_data.crc) + 1;
    case Disk_AIM:
        request->length += sizeof(request->disk_data);
        request->disk_data.disk_info.disk_id = id;
        request->disk_data.disk_info.unit = 0x33;
        request->disk_data.disk_info.total_size = disks[id].total_size;
        request->disk_data.disk_info.used_size = disks[id].used_size;
        request->disk_data.disk_info.temp = disks[id].temperature;
        return offsetof(Request, disk_data.crc) + 1;
    // case GPU_AIM:
    //     request->length += sizeof(request->gpu_data);
    //     // Code
    //     unsigned char DGPUtemp = 0;
    //     if(IsNvidiaGPU)
    //     {
    //         DGPUtemp = nvidia_get_gpu_temperature();
    //         request->gpu_data.gpu_info.temperature = DGPUtemp;
    //         request->gpu_data.gpu_info.usage = nvidia_get_gpu_utilization();
    //         request->gpu_data.gpu_info.rpm = nvidia_get_gpu_fan_speed();

    //     }
    //     return offsetof(Request, gpu_data.crc) + 1;
    // case CPU_AIM:
        
        // request->length += sizeof(request->cpu_data);
        // // Code
        // request->cpu_data.cpu_info.temerature = get_cpu_temperature();
        // // 读取当前CPU数据
        // read_cpu_data(&curr_data);
        // // 计算CPU使用率
        // //printf("CPU usage: %2f\n", calculate_cpu_usage(&prev_data, &curr_data));
        // request->cpu_data.cpu_info.usage = calculate_cpu_usage(&prev_data, &curr_data);
        // // 更新前一次的数据
        // prev_data = curr_data;
        // // 获取 I/O 权限
        // acquire_io_permissions();
        // unsigned char CPU_fan = 0;
        // ec_ram_read_byte(0x70,&CPU_fan);
        // request->cpu_data.cpu_info.rpm = CPU_fan;
        // // 释放 I/O 权限
        // release_io_permissions();
        // return offsetof(Request, cpu_data.crc) + 1;
    case USER_AIM:
        request->length += sizeof(request->user_data);
        // Code
        request->user_data.user_info.online = GetUserCount();
        return offsetof(Request, user_data.crc) + 1;
    case WlanSpeed_AIM:
        request->length += sizeof(request->speed_data);
        request->speed_data.id = id;
        double rx_speed = 0, tx_speed = 0, rx_total = 0, tx_total = 0;
        //unit default use KB/S
        request->speed_data.speed_info.unit = 0x00;
        network_interface_t *get_iface = &g_iface_manager.interfaces[id];
        if(get_interface_traffic_info(get_iface->interface_name, &rx_speed, &tx_speed, &rx_total, &tx_total) == 0)
        {
            printf("RX Speed:  %.2f KB/s\n", rx_speed);
            printf("TX Speed:  %.2f KB/s\n", tx_speed);
            printf("Total RX:  %.2f MB\n", rx_total);
            printf("Total TX:  %.2f MB\n", tx_total);
        }
        if(tx_speed > 1024)
        {
            request->speed_data.speed_info.unit += 0x10;
            tx_speed /= 1024;
            if(tx_speed > 1024)
            {
                request->speed_data.speed_info.unit += 0x10;
                tx_speed /= 1024;
                if(tx_speed > 1024)
                {
                    request->speed_data.speed_info.unit += 0x10;
                    tx_speed /= 1024;
                }
            }
        }
        if(rx_speed > 1024)
        {
            request->speed_data.speed_info.unit += 0x01;
            rx_speed /= 1024;
            if(rx_speed > 1024)
            {
                request->speed_data.speed_info.unit += 0x10;
                rx_speed /= 1024;
                if(rx_speed > 1024)
                {
                    request->speed_data.speed_info.unit += 0x10;
                    rx_speed /= 1024;
                }
            }
        }
        
        request->speed_data.speed_info.uploadspeed = tx_speed;
        request->speed_data.speed_info.downloadspeed = rx_speed;

        return offsetof(Request, speed_data.crc) + 1;
    case WlanTotal_AIM:
        request->length += sizeof(request->flow_data);
        request->flow_data.id = id;
        request->flow_data.unit = 0x01;
        //double rx_speed = 0, tx_speed = 0, rx_total = 0, tx_total = 0,total = 0;
        double total = 0;
        //network_interface_t 

        get_iface = &g_iface_manager.interfaces[id];
        printf("Interface: %s\n", get_iface->interface_name);
        if(get_interface_traffic_info(get_iface->interface_name, &rx_speed, &tx_speed, &rx_total, &tx_total) == 0)
        {
            printf("RX Speed:  %.2f KB/s\n", rx_speed);
            printf("TX Speed:  %.2f KB/s\n", tx_speed);
            printf("Total RX:  %.2f MB\n", rx_total);
            printf("Total TX:  %.2f MB\n", tx_total);
        }
        total = tx_total + rx_total;

        if(total > 1024)
        {
            request->flow_data.unit += 0x01;
            total /= 1024;
            if(total > 1024)
            {
                request->flow_data.unit += 0x01;
                total /= 1024;
                if(total > 1024)
                {
                    request->flow_data.unit += 0x01;
                    total /= 1024;
                }
            }
        }
        request->flow_data.totalflow = total;
        return offsetof(Request, flow_data.crc) + 1;
    case WlanIP_AIM:
        request->length += sizeof(request->wlanip_data);
        request->wlanip_data.id = id;
        char status[16], mac[18], ip[INET_ADDRSTRLEN], mask[INET_ADDRSTRLEN];
        //network_interface_t 
        get_iface = &g_iface_manager.interfaces[id];
        printf("Interface: %s\n", get_iface->interface_name);
        if(get_interface_basic_info(get_iface->interface_name,status, mac, ip, mask) == 0)
        {
            printf("Status:    %s\n", status);
            printf("MAC:       %s\n", mac);
            printf("IP:        %s\n", ip);
            printf("Netmask:   %s\n", mask);
        }
        int a,b,c,d;
        if (sscanf(ip, "%d.%d.%d.%d", &a, &b, &c, &d) == 4) 
        {
            request->wlanip_data.ip[0] = (unsigned char)a;
            request->wlanip_data.ip[1] = (unsigned char)b;
            request->wlanip_data.ip[2] = (unsigned char)c;
            request->wlanip_data.ip[3] = (unsigned char)d;
        }
        return offsetof(Request, wlanip_data.crc) + 1;
    default:
        request->length += sizeof(request->common_data);
        return offsetof(Request, common_data.crc) + 1;
    }
}

int first_init_hidreport(Request* request, unsigned char cmd, unsigned char aim,unsigned char total,unsigned char order) {
    request->header = SIGNATURE;
    request->cmd = cmd;
    request->aim = aim;
    request->length = COMMLEN;

    switch (aim)
    {
    case HomePage_AIM:
        request->length += sizeof(request->Homepage_data);
        request->Homepage_data.time_info.timestamp = time(NULL) + 28800;
        request->Homepage_data.total = total;
        request->Homepage_data.order = order;
        return offsetof(Request, Homepage_data.crc) + 1;
    case SystemPage_AIM:
        request->length += sizeof(request->SystemPage_data);
        request->SystemPage_data.order = order;
        request->SystemPage_data.total = total;
        if(IsNvidiaGPU)
            request->SystemPage_data.syscount = 4;
        else
            request->SystemPage_data.syscount = 3;
        if(order == 1)
        {
            request->SystemPage_data.count = 2;
            // 读取当前CPU数据
            read_cpu_data(&curr_data);
            request->SystemPage_data.systemPage[0].syslength = sizeof(request->SystemPage_data.systemPage[0]);
            request->SystemPage_data.systemPage[0].sys_id = 0; 
            request->SystemPage_data.systemPage[0].usage = calculate_cpu_usage(&prev_data, &curr_data);
            // 更新前一次的数据
            prev_data = curr_data;
            // 获取 I/O 权限
            acquire_io_permissions();
            ec_ram_read_byte(0x70,&cputemp);
            request->SystemPage_data.systemPage[0].temp = cputemp;
            unsigned char CPU_fan_H = 0,CPU_fan_L = 0;
            ec_ram_read_byte(0x76,&CPU_fan_H);
            ec_ram_read_byte(0x77,&CPU_fan_L);
            request->SystemPage_data.systemPage[0].rpm = (CPU_fan_H * 0xFF + CPU_fan_L) / 42;
            request->SystemPage_data.systemPage[0].name[0] = 'C';
            request->SystemPage_data.systemPage[0].name[1] = 'P';
            request->SystemPage_data.systemPage[0].name[2] = 'U';
            //CPU OK
            request->SystemPage_data.systemPage[1].syslength = sizeof(request->SystemPage_data.systemPage[1]);
            request->SystemPage_data.systemPage[1].sys_id = 1;
            //Todo
            request->SystemPage_data.systemPage[1].usage = get_igpu_usage();;
            request->SystemPage_data.systemPage[1].temp = get_igpu_temperature();
            request->SystemPage_data.systemPage[1].rpm = (CPU_fan_H * 0xFF + CPU_fan_L) / 42;
            request->SystemPage_data.systemPage[1].name[0] = 'i';
            request->SystemPage_data.systemPage[1].name[1] = 'G';
            request->SystemPage_data.systemPage[1].name[2] = 'P';
            request->SystemPage_data.systemPage[1].name[3] = 'U';
        }
        else if(order == 2)
        {
            request->SystemPage_data.systemPage[0].syslength = sizeof(request->SystemPage_data.systemPage[0]);
            request->SystemPage_data.systemPage[0].sys_id = 2;
            request->SystemPage_data.systemPage[0].usage = get_memory_usage();
            request->SystemPage_data.systemPage[0].temp = 255;//No temp
            request->SystemPage_data.systemPage[0].rpm = 255;//No fan
            request->SystemPage_data.systemPage[0].name[0] = 'M';
            request->SystemPage_data.systemPage[0].name[1] = 'e';
            request->SystemPage_data.systemPage[0].name[2] = 'm';
            request->SystemPage_data.systemPage[0].name[3] = 'o';
            request->SystemPage_data.systemPage[0].name[4] = 'r';
            request->SystemPage_data.systemPage[0].name[5] = 'y';
            if(IsNvidiaGPU)
            {
                request->SystemPage_data.count = 2;
            }
            else
                request->SystemPage_data.count = 1;
        }
        return offsetof(Request, SystemPage_data.crc) + 1;
    case DiskPage_AIM:
        request->length += sizeof(request->DiskPage_data);
        request->DiskPage_data.diskcount = disk_count;
        request->DiskPage_data.total = total;
        request->DiskPage_data.order = order;
        if((disk_count - (order-1)*2) == 1)
        {
            request->DiskPage_data.count = 1;
            request->DiskPage_data.diskStruct[0].disk_id = (order - 1) * 2; //id >= 0
            request->DiskPage_data.diskStruct[0].unit = 0x33;
            request->DiskPage_data.diskStruct[0].reserve = 0;
            request->DiskPage_data.diskStruct[0].total_size = disks[(order - 1) * 2].total_size;
            request->DiskPage_data.diskStruct[0].used_size = disks[(order - 1) * 2].used_size;
            request->DiskPage_data.diskStruct[0].temp = disks[(order - 1) * 2].temperature;
            //reserve name
            for (int i = 0; i < sizeof(disks[(order - 1) * 2].device); i++)
            {
                request->DiskPage_data.diskStruct[0].name[i] = disks[(order - 1) * 2].device[i];
            }
            request->DiskPage_data.diskStruct[0].name[sizeof(disks[(order - 1) * 2].device) + 1] = 0;
            request->DiskPage_data.diskStruct[0].disklength = sizeof(request->DiskPage_data.diskStruct[0]);
        }
        else
        {
            //First
            request->DiskPage_data.count = 2;
            request->DiskPage_data.diskStruct[0].disk_id = (order - 1) * 2;//id >= 0
            request->DiskPage_data.diskStruct[0].unit = 0x33;
            request->DiskPage_data.diskStruct[0].reserve = 0;
            request->DiskPage_data.diskStruct[0].total_size = disks[(order - 1) * 2].total_size;
            request->DiskPage_data.diskStruct[0].used_size = disks[(order - 1) * 2].used_size;
            request->DiskPage_data.diskStruct[0].temp = disks[(order - 1) * 2].temperature;
            for (int i = 0; i < sizeof(disks[(order - 1) * 2].device); i++)
            {
                request->DiskPage_data.diskStruct[0].name[i] = disks[(order - 1) * 2].device[i];
            }
            request->DiskPage_data.diskStruct[0].name[sizeof(disks[(order - 1) * 2].device) + 1] = 0;
            request->DiskPage_data.diskStruct[0].disklength = sizeof(request->DiskPage_data.diskStruct[0]);
            //Second
            request->DiskPage_data.diskStruct[1].disk_id = 1 + (order - 1) * 2;
            request->DiskPage_data.diskStruct[1].unit = 0x33;
            request->DiskPage_data.diskStruct[1].reserve = 0;
            request->DiskPage_data.diskStruct[1].total_size = disks[(order - 1) * 2 +1].total_size;
            request->DiskPage_data.diskStruct[1].used_size = disks[(order - 1) * 2 + 1].used_size;
            request->DiskPage_data.diskStruct[1].temp = disks[(order - 1) * 2 + 1].temperature;
            for (int i = 0; i < sizeof(disks[(order - 1) * 2 + 1].device); i++)
            {
                request->DiskPage_data.diskStruct[1].name[i] = disks[(order - 1) * 2 + 1].device[i];
            }
            request->DiskPage_data.diskStruct[1].name[sizeof(disks[(order - 1) * 2 + 1].device) + 1] = 0;
            request->DiskPage_data.diskStruct[1].disklength = sizeof(request->DiskPage_data.diskStruct[1]);
        }
        return offsetof(Request, DiskPage_data.crc) + 1;
    case ModePage_AIM:
        request->length += sizeof(request->ModePage_data);
        request->ModePage_data.mute = 1;
        request->ModePage_data.properties = 0;
        return offsetof(Request, ModePage_data.crc) + 1;
    case WlanPage_AIM:
        request->length += sizeof(request->WlanPage_data);
        request->WlanPage_data.order = order;
        request->WlanPage_data.total = total;
        request->WlanPage_data.netcount = total;
        request->WlanPage_data.count = 1;
        request->WlanPage_data.online = GetUserCount();
        request->WlanPage_data.length = (sizeof(request->WlanPage_data.wlanPage) + 1);
        request->WlanPage_data.wlanPage.id = order - 1;//order from 1 id from 0
        request->WlanPage_data.wlanPage.unit = 0x00;
        double rx_speed, tx_speed, rx_total, tx_total;
        char status[16], mac[18], ip[INET_ADDRSTRLEN], mask[INET_ADDRSTRLEN];
        
        network_interface_t *get_iface = &g_iface_manager.interfaces[order - 1];//order from 1 id from 0
        printf("Interface: %s\n", get_iface->interface_name);
        if(get_interface_basic_info(get_iface->interface_name,status, mac, ip, mask) == 0)
        {
            printf("Status:    %s\n", status);
            printf("MAC:       %s\n", mac);
            printf("IP:        %s\n", ip);
            printf("Netmask:   %s\n", mask);
        }
        if(get_interface_traffic_info(get_iface->interface_name, &rx_speed, &tx_speed, &rx_total, &tx_total) == 0)
        {
            printf("RX Speed:  %.2f KB/s\n", rx_speed);
            printf("TX Speed:  %.2f KB/s\n", tx_speed);
            printf("Total RX:  %.2f MB\n", rx_total);
            printf("Total TX:  %.2f MB\n", tx_total);
        }
        if(tx_speed > 1024)
        {
            request->WlanPage_data.wlanPage.unit += 0x10;
            tx_speed /= 1024;
            if(tx_speed > 1024)
            {
                request->WlanPage_data.wlanPage.unit += 0x10;
                tx_speed /= 1024;
                if(tx_speed > 1024)
                {
                    request->WlanPage_data.wlanPage.unit += 0x10;
                    tx_speed /= 1024;
                }
            }
        }
        if(rx_speed > 1024)
        {
            request->WlanPage_data.wlanPage.unit += 0x01;
            rx_speed /= 1024;
            if(rx_speed > 1024)
            {
                request->WlanPage_data.wlanPage.unit += 0x01;
                rx_speed /= 1024;
                if(rx_speed > 1024)
                {
                    request->WlanPage_data.wlanPage.unit += 0x01;
                    rx_speed /= 1024;
                    
                }
            }
        }
        printf("TX Speed: %f,RX Speed: %f\n",tx_speed,rx_speed);
        request->WlanPage_data.wlanPage.uploadspeed = tx_speed;
        request->WlanPage_data.wlanPage.downloadspeed = rx_speed;
        printf("TX Speed: %d,RX Speed: %d\n",request->WlanPage_data.wlanPage.uploadspeed,request->WlanPage_data.wlanPage.downloadspeed);
        int a,b,c,d;
        if (sscanf(ip, "%d.%d.%d.%d", &a, &b, &c, &d) == 4) 
        {
            request->WlanPage_data.wlanPage.ip[0] = (unsigned char)a;
            request->WlanPage_data.wlanPage.ip[1] = (unsigned char)b;
            request->WlanPage_data.wlanPage.ip[2] = (unsigned char)c;
            request->WlanPage_data.wlanPage.ip[3] = (unsigned char)d;
        }
        
        return offsetof(Request, WlanPage_data.crc) + 1;
    case InfoPage_AIM:
        request->length += sizeof(request->InfoPage_data);
        request->InfoPage_data.order = order;
        request->InfoPage_data.total = total;
        request->InfoPage_data.reserve = 0;
        request->InfoPage_data.namelength = sizeof(request->InfoPage_data.name);
        switch (order)
        {
        case 1:
            for (int i = 0; i < sizeof(request->InfoPage_data.name); i++)
            {
                request->InfoPage_data.name[i] = sys_info.devicename[i];
            }
            break;
        case 2:
            for (int i = 0; i < sizeof(request->InfoPage_data.name); i++)
            {
                request->InfoPage_data.name[i] = sys_info.cpuname[i];
            }
            break;
        case 3:
            for (int i = 0; i < sizeof(request->InfoPage_data.name); i++)
            {
                request->InfoPage_data.name[i] = sys_info.operatename[i];
            }
            break;
        case 4:
            for (int i = 0; i < sizeof(request->InfoPage_data.name); i++)
            {
                request->InfoPage_data.name[i] = sys_info.serial_number[i];
            }
            break;
        default:
            break;
        }
        return offsetof(Request, InfoPage_data.crc) + 1;
    default:
        return 0;
    }
}
void append_crc(Request *request) {
    int len = 0;
    int off = 0;
    unsigned short crc = 0;
    switch (request->aim)
    {
        case TIME_AIM:
            len = offsetof(Request, time_data.crc);
            request->time_data.crc = cal_crc((unsigned char *)request, len);
            return;
        case SystemPage_AIM:
            len = offsetof(Request, SystemPage_data.crc);
            request->SystemPage_data.crc = cal_crc((unsigned char *)request, len);
            return;
        case System_AIM:
            len = offsetof(Request, system_data.crc);
            request->system_data.crc = cal_crc((unsigned char *)request, len);
            return;
        case DiskPage_AIM:
            len = offsetof(Request, DiskPage_data.crc);
            request->DiskPage_data.crc = cal_crc((unsigned char *)request, len);
            return;
        case Disk_AIM:
            len = offsetof(Request, disk_data.crc);
            request->disk_data.crc = cal_crc((unsigned char *)request, len);
            return;
        case ModePage_AIM:
            len = offsetof(Request, ModePage_data.crc);
            request->ModePage_data.crc = cal_crc((unsigned char *)request, len);
            return;
        case WlanPage_AIM:
            len = offsetof(Request, WlanPage_data.crc);
            request->WlanPage_data.crc = cal_crc((unsigned char *)request, len);
            return;
        case InfoPage_AIM:
            len = offsetof(Request, InfoPage_data.crc);
            request->InfoPage_data.crc = cal_crc((unsigned char *)request, len);
            return;
        case USER_AIM:
            len = offsetof(Request, user_data.crc);
            request->user_data.crc = cal_crc((unsigned char *)request, len);
            return;
        case WlanSpeed_AIM:
            len = offsetof(Request, speed_data.crc);
            request->speed_data.crc = cal_crc((unsigned char *)request, len);
            return;
        case WlanTotal_AIM:
            len = offsetof(Request, flow_data.crc);
            request->flow_data.crc = cal_crc((unsigned char *)request, len);
            return;
        case WlanIP_AIM:
            len = offsetof(Request, wlanip_data.crc);
            request->wlanip_data.crc = cal_crc((unsigned char *)request, len);
            return;
        default:
            len = offsetof(Request, common_data.crc);
            request->common_data.crc = cal_crc((unsigned char *)request, len);
            return;
    }
}

// unsafe
unsigned char cal_crc(unsigned char * data, int len) {
    int off = 0;
    unsigned short crc = 0;

    for (; off < len; off++) {
        crc += *((unsigned char *)(data) + off);
        #if hidwritedebug
        if(off == 5)
        {
            printf("AIM:");
        }
        printf("0x%02X ",*((unsigned char *)(data) + off));
        #endif
    }
    #if hidwritedebug
    printf("\n");
    #endif
    return (unsigned char)(crc & 0xff);
}
// 检查是否为TrueNAS系统
int is_truenas_system(void) {
    FILE *fp;
    char line[256];
    
    // 方法1: 检查/etc/os-release
    fp = fopen("/etc/os-release", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "truenas") || strstr(line, "TrueNAS")) {
                fclose(fp);
                return 1;
            }
        }
        fclose(fp);
    }
    
    // 方法2: 检查是否存在TrueNAS特定文件
    if (access("/usr/local/bin/midclt", F_OK) == 0 ||
        access("/etc/network/truenas", F_OK) == 0) {
        return 1;
    }
    
    return 0;
}
int get_cpu_temperature() {
    FILE *temp_file;
    char path[512];
    int temperature = -1;
    
    // 方法1: 尝试标准thermal zones
    for (int i = 0; i < 20; i++) {
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", i);
        temp_file = fopen(path, "r");
        if (temp_file != NULL) {
            int temp_raw;
            if (fscanf(temp_file, "%d", &temp_raw) == 1) {
                temperature = temp_raw / 1000;
                fclose(temp_file);
                
                // 验证温度值是否合理
                if (temperature > 0 && temperature < 150) {
                    return temperature;
                }
            }
            fclose(temp_file);
        }
    }
    // 方法2: 尝试hwmon接口 (更通用)
    temperature = read_temperature_from_hwmon();
    if (temperature != -1) {
        return temperature;
    }
    // 方法5: 如果是TrueNAS，尝试特定路径
    if (is_truenas_system()) {
        temperature = read_temperature_for_truenas();
        if (temperature != -1) {
            return temperature;
        }
    }
    
    return -1;
}
// 安全路径构建辅助函数
static int safe_path_join(char *dest, size_t dest_size, 
                         const char *path1, const char *path2) {
    if (!dest || !path1 || !path2) return -1;
    
    size_t len1 = strlen(path1);
    size_t len2 = strlen(path2);
    size_t total_len = len1 + len2 + 2; // +1 for '/', +1 for '\0'
    
    if (total_len > dest_size) {
        // 缓冲区不足，进行安全截断
        if (dest_size > len2 + 2) {
            // 可以容纳部分路径1
            size_t max_len1 = dest_size - len2 - 2;
            strncpy(dest, path1, max_len1);
            dest[max_len1] = '\0';
            strcat(dest, "/");
            strcat(dest, path2);
            return 1; // 表示有截断
        } else {
            // 空间严重不足
            dest[0] = '\0';
            return -1;
        }
    }
    
    snprintf(dest, dest_size, "%s/%s", path1, path2);
    return 0;
}
// 从hwmon子系统读取温度
int read_temperature_from_hwmon(void) {
    DIR *dir;
    struct dirent *entry;
    char hwmon_path[PATH_MAX];
    char temp_path[PATH_MAX];
    char name_path[PATH_MAX];
    FILE *temp_file;
    int max_temp = -1;
    
    // 使用PATH_MAX常量（通常4096）
    #ifndef PATH_MAX
    #define PATH_MAX 4096
    #endif
    
    // 打开hwmon目录
    dir = opendir("/sys/class/hwmon");
    if (!dir) {
        return -1;
    }
    
    while ((entry = readdir(dir)) != NULL) {
        // 跳过.和..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        // 构建hwmon路径
        if (snprintf(hwmon_path, sizeof(hwmon_path), 
                    "/sys/class/hwmon/%s", entry->d_name) >= (int)sizeof(hwmon_path)) {
            // 路径过长，跳过这个设备
            continue;
        }
        
        // 安全构建name路径
        if (safe_path_join(name_path, sizeof(name_path), hwmon_path, "name") < 0) {
            continue;
        }
        
        FILE *name_file = fopen(name_path, "r");
        if (name_file) {
            char sensor_name[64];
            if (fgets(sensor_name, sizeof(sensor_name), name_file)) {
                // 去除换行符
                sensor_name[strcspn(sensor_name, "\n")] = 0;
                
                // 检查是否是CPU温度传感器
                if (strstr(sensor_name, "coretemp") || 
                    strstr(sensor_name, "k10temp") ||
                    strstr(sensor_name, "cpu") ||
                    strstr(sensor_name, "Core")) {
                    
                    // 查找温度文件
                    DIR *temp_dir = opendir(hwmon_path);
                    if (temp_dir) {
                        struct dirent *temp_entry;
                        while ((temp_entry = readdir(temp_dir)) != NULL) {
                            // 查找temp*_input文件
                            if (strstr(temp_entry->d_name, "temp") && 
                                strstr(temp_entry->d_name, "_input")) {
                                
                                // 安全构建温度文件路径
                                if (safe_path_join(temp_path, sizeof(temp_path), 
                                                  hwmon_path, temp_entry->d_name) < 0) {
                                    continue;
                                }
                                
                                temp_file = fopen(temp_path, "r");
                                if (temp_file) {
                                    int temp_raw;
                                    if (fscanf(temp_file, "%d", &temp_raw) == 1) {
                                        int temp_c = temp_raw / 1000;
                                        if (temp_c > max_temp) {
                                            max_temp = temp_c;
                                        }
                                    }
                                    fclose(temp_file);
                                }
                            }
                        }
                        closedir(temp_dir);
                    }
                }
            }
            fclose(name_file);
        }
    }
    
    closedir(dir);
    return max_temp;
}
// TrueNAS专用温度读取
int read_temperature_for_truenas(void) {

    // 方法2: 尝试TrueNAS API
    int temp = read_temperature_via_truenas_api();
    if (temp != -1) {
        return temp;
    }
    
    // 方法3: 尝试FreeBSD风格的路径（如果是TrueNAS CORE）
    temp = read_temperature_freebsd_style();
    
    return temp;
}
int read_temperature_via_truenas_api(void) {
    FILE *fp;
    char command[256];
    char response[1024];
    
    // 尝试使用midclt命令读取系统信息
    snprintf(command, sizeof(command), 
            "midclt call system.info 2>/dev/null | grep -i temp");
    
    fp = popen(command, "r");
    if (!fp) {
        return -1;
    }
    
    if (fgets(response, sizeof(response), fp)) {
        // 解析响应中的温度信息
        char *temp_str = strstr(response, "temperature");
        if (!temp_str) {
            temp_str = strstr(response, "temp");
        }
        
        if (temp_str) {
            // 提取数字
            for (char *p = temp_str; *p; p++) {
                if (isdigit(*p)) {
                    int temp = atoi(p);
                    pclose(fp);
                    return temp;
                }
            }
        }
    }
    
    pclose(fp);
    return -1;
}

// FreeBSD风格的温度读取（TrueNAS CORE）
int read_temperature_freebsd_style(void) {
    // 尝试FreeBSD的sysctl接口
    FILE *fp = popen("sysctl -a 2>/dev/null | grep -i temperature", "r");
    if (!fp) {
        return -1;
    }
    
    char line[256];
    int max_temp = -1;
    
    while (fgets(line, sizeof(line), fp)) {
        // 解析类似: dev.cpu.0.temperature: 45.0C
        char *colon = strchr(line, ':');
        if (colon) {
            colon++;
            // 查找数字
            char *num_start = colon;
            while (*num_start && !isdigit(*num_start)) {
                num_start++;
            }
            
            if (isdigit(*num_start)) {
                int temp = atoi(num_start);
                if (temp > max_temp) {
                    max_temp = temp;
                }
            }
        }
    }
    
    pclose(fp);
    
    // 如果没找到，尝试内核温度设备
    fp = fopen("/dev/systemperature", "r");
    if (fp) {
        // 读取温度...
        fclose(fp);
    }
    
    return max_temp;
}
// 读取CPU数据
void read_cpu_data(CPUData *data) {
    FILE *file = fopen("/proc/stat", "r");
    if (file == NULL) {
        perror("Error opening /proc/stat");
        exit(EXIT_FAILURE);
    }
    
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, "cpu ", 4) == 0) {
            sscanf(line + 5, "%lu %lu %lu %lu %lu %lu %lu",
                   &data->user, &data->nice, &data->system, &data->idle,
                   &data->iowait, &data->irq, &data->softirq);
            break;
        }
    }
    
    fclose(file);
}
// 计算CPU使用率
float calculate_cpu_usage(const CPUData *prev, const CPUData *curr) {
    unsigned long prev_idle = prev->idle + prev->iowait;
    unsigned long curr_idle = curr->idle + curr->iowait;
    
    unsigned long prev_non_idle = prev->user + prev->nice + prev->system + 
                                 prev->irq + prev->softirq;
    unsigned long curr_non_idle = curr->user + curr->nice + curr->system + 
                                 curr->irq + curr->softirq;
    
    unsigned long prev_total = prev_idle + prev_non_idle;
    unsigned long curr_total = curr_idle + curr_non_idle;
    
    unsigned long total_delta = curr_total - prev_total;
    unsigned long idle_delta = curr_idle - prev_idle;
    
    if (total_delta == 0) return 0.0;
    //printf("CPU total_delta: %ld, idle_delta: %ld\n", total_delta, idle_delta);
    return (total_delta - idle_delta) * 100.0 / total_delta;
}
// 获取iGPU温度（摄氏度）
int get_igpu_temperature() {
    FILE *file;
    char path[512];//Use 512
    char line[256];
    
    // 直接尝试已知的AMD温度传感器路径
    const char *temp_patterns[] = {
        "/sys/class/drm/card0/device/hwmon/hwmon*/temp1_input",
        "/sys/class/drm/card1/device/hwmon/hwmon*/temp1_input",
        "/sys/class/hwmon/hwmon*/temp1_input",
        NULL
    };
    
    for (int i = 0; temp_patterns[i] != NULL; i++) {
        // 使用glob处理通配符
        glob_t globbuf;
        if (glob(temp_patterns[i], 0, NULL, &globbuf) == 0) {
            for (size_t j = 0; j < globbuf.gl_pathc; j++) {
                // 检查路径长度
                if (strlen(globbuf.gl_pathv[j]) >= 512) {
                    continue;
                }
                
                file = fopen(globbuf.gl_pathv[j], "r");
                if (file) {
                    if (fgets(line, sizeof(line), file)) {
                        int temp = atoi(line) / 1000;
                        fclose(file);
                        globfree(&globbuf);
                        return temp;
                    }
                    fclose(file);
                }
            }
            globfree(&globbuf);
        }
    }
    
    return -1;
}

// 获取iGPU使用率
float get_igpu_usage() {
    FILE *file;
    char path[512];
    char line[256];
    
    const char *usage_paths[] = {
        "/sys/class/drm/card0/device/gpu_busy_percent",
        "/sys/class/drm/card1/device/gpu_busy_percent",
        "/sys/class/drm/card2/device/gpu_busy_percent",
        NULL
    };
    
    for (int i = 0; usage_paths[i] != NULL; i++) {
        file = fopen(usage_paths[i], "r");
        if (file) {
            if (fgets(line, sizeof(line), file)) {
                float usage = atof(line);
                fclose(file);
                return usage;
            }
            fclose(file);
        }
    }
    
    return -1.0;
}
//ReadMemory Useage
unsigned int get_memory_usage(){
    struct sysinfo info;
    if(sysinfo(&info) != 0)
    {
        return 0.0;
        /* data */
    }
    else
    {
        unsigned usage = (info.totalram-info.freeram-info.bufferram) * 100 / info.totalram;
        return usage;
    }
}

// void parse_request(Request *request) {
//     for (int off = 0; off < 0x40; off++) {
//         printf("%d ", *(((unsigned char *)request) + off));
//     }
//     printf("\n");
//     printf("length: %u\n", request->length);
// }

// void parse_ack(Ack *ack, unsigned char aim) {
//     int len = 0;
//     switch (aim)
//     {
//         case TIME_AIM:

//             len = offsetof(Ack, time_data.crc);
//             break;

//         case USER_AIM:
//             len = offsetof(Ack, user_data.crc);
//             break;

//         default:
//             len = offsetof(Ack, common_data.crc);
//             break;
//     }

//     for (int off = 0; off < len; off++) {
//         printf("%u ", *(((unsigned char *)ack) + off));
//     }
//     printf("\n");

//     printf("[debug] ack->header: %x\n", ack->header);
//     printf("[debug] ack->sequence: %x\n", ack->sequence);
//     printf("[debug] ack->length: %x\n", ack->length);
//     printf("[debug] ack->cmd: %x\n", ack->cmd);
//     printf("[debug] ack->err: %x\n", ack->err);
// }
//Disk
int file_exists(const char *filename) {
    struct stat st;
    return (stat(filename, &st) == 0);
}
// 读取文件内容到缓冲区
int read_file(const char *filename, char *buffer, size_t buffer_size) {
    FILE *file = fopen(filename, "r");
    if (!file) return -1;
    
    size_t bytes_read = fread(buffer, 1, buffer_size - 1, file);
    buffer[bytes_read] = '\0';
    fclose(file);
    return 0;
}
// 执行命令并获取输出
int execute_command(const char *cmd, char *output, size_t output_size) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    
    if (fgets(output, output_size, fp) == NULL) {
        pclose(fp);
        return -1;
    }
    
    output[strcspn(output, "\n")] = 0;
    pclose(fp);
    return 0;
}
// 获取硬盘温度
int get_disk_temperature(const char *device) {
    char path[MAX_PATH];
    char temp_value[32];
    
    // 方法1: 尝试smartctl
    char cmd[MAX_PATH];
    // 使用 -C 选项强制转换为摄氏度
    snprintf(cmd, sizeof(cmd), 
            "sudo smartctl -C -A /dev/%s 2>/dev/null | grep -i 'Temperature' | head -1", device);

    FILE *fp = popen(cmd, "r");
    if (fp) {
        char output[256];
        if (fgets(output, sizeof(output), fp)) {
            // 改进的数字提取逻辑
            char *ptr = output;
            while (*ptr) {
                // 查找连续的数字（支持2-3位数）
                if (*ptr >= '0' && *ptr <= '9') {
                    char *num_start = ptr;
                    while (*ptr >= '0' && *ptr <= '9') ptr++;
                    char saved_char = *ptr;
                    *ptr = '\0'; // 临时终止字符串
                    
                    int temp = atoi(num_start);
                    *ptr = saved_char; // 恢复原字符
                    
                    // 放宽温度范围，但仍保持合理限制
                    if (temp >= 10 && temp <= 80) {
                        pclose(fp);
                        return temp;
                    }
                }
                ptr++;
            }
        }
        pclose(fp);
    }
    
    // 方法2: 尝试NVMe温度文件
    if (strncmp(device, "nvme", 4) == 0) {
        snprintf(path, sizeof(path), "/sys/class/nvme/%s/temperature", device);
        if (file_exists(path)) {
            if (read_file(path, temp_value, sizeof(temp_value)) == 0) {
                return atoi(temp_value);
            }
        }
    }
    
    // 方法3: 尝试SATA温度文件
    snprintf(path, sizeof(path), "/sys/block/%s/device/hwmon/hwmon1/temp1_input", device);
    if (file_exists(path)) {
        if (read_file(path, temp_value, sizeof(temp_value)) == 0) {
            int temp = atoi(temp_value);
            if (temp > 1000) temp = temp / 1000;
            return temp;
        }
    }
    
    return -1; // 无法获取温度
}
// 获取硬盘型号和序列号
void get_disk_identity(const char *device, char *model, char *serial) {
    char path[MAX_PATH];
    
    // 获取型号
    if (strncmp(device, "nvme", 4) == 0) {
        snprintf(path, sizeof(path), "/sys/class/nvme/%s/model", device);
    } else {
        snprintf(path, sizeof(path), "/sys/block/%s/device/model", device);
    }
    
    if (read_file(path, model, 128) != 0) {
        strcpy(model, "Unknown");
    } else {
        // 去除换行符
        model[strcspn(model, "\n")] = 0;
    }
    
    // 获取序列号
    if (strncmp(device, "nvme", 4) == 0) {
        snprintf(path, sizeof(path), "/sys/class/nvme/%s/serial", device);
    } else {
        snprintf(path, sizeof(path), "/sys/block/%s/device/serial", device);
    }
    
    if (read_file(path, serial, 64) != 0) {
        strcpy(serial, "Unknown");
    } else {
        serial[strcspn(serial, "\n")] = 0;
    }
}

// 获取硬盘总容量
unsigned long long get_disk_size(const char *device) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "/sys/block/%s/size", device);
    
    char size_str[32];
    if (read_file(path, size_str, sizeof(size_str)) == 0) {
        unsigned long long sectors = strtoull(size_str, NULL, 10);
        return sectors * 512; // 转换为字节
    }
    return 0;
}
int GetUserCount()
{
    FILE *fp;
    char buffer[1024];
    int count = 0;
    
    fp = popen("who | wc -l", "r");
    if (fp == NULL) {
        return -1;
    }
    
    if (fgets(buffer, sizeof(buffer), fp) != NULL) {
        count = atoi(buffer);
    }
    
    pclose(fp);
    return count;
}
// 获取挂载点
void get_mountpoint(const char *device, char *mountpoint) {
    FILE *mtab;
    struct mntent *entry;
    char full_device[64];
    
    // 构建完整的设备路径
    snprintf(full_device, sizeof(full_device), "/dev/%s", device);
    
    mtab = setmntent("/proc/mounts", "r");
    if (mtab == NULL) {
        return;
    }
    
    // 查找设备的挂载点
    while ((entry = getmntent(mtab)) != NULL) {
        if (strcmp(entry->mnt_fsname, full_device) == 0) {
            strncpy(mountpoint, entry->mnt_dir, 255);
            mountpoint[255] = '\0';
            break;
        }
        // 也检查分区（如 sda1, sda2 等）
        if (strncmp(entry->mnt_fsname, full_device, strlen(full_device)) == 0) {
            strncpy(mountpoint, entry->mnt_dir, 255);
            mountpoint[255] = '\0';
            break;
        }
    }
    
    endmntent(mtab);
}

// 获取挂载点使用情况
int get_mountpoint_usage(const char *mountpoint, unsigned long long *total, 
                        unsigned long long *free, unsigned long long *used) {
    struct statvfs buf;
    if (statvfs(mountpoint, &buf) != 0) {
        return -1;
    }
    
    *total = (unsigned long long)buf.f_blocks * buf.f_frsize;
    *free = (unsigned long long)buf.f_bfree * buf.f_frsize;
    *used = *total - *free;
    
    return 0;
}

// 扫描所有硬盘设备
int scan_disk_devices(disk_info_t *disks, int max_disks) {
    DIR *block_dir = opendir("/sys/block");
    if (!block_dir) {
        return 0;
    }
    
    struct dirent *entry;
    int disk_count = 0;
    char path[1024];
    FILE *file;
    
    while ((entry = readdir(block_dir)) != NULL && disk_count < max_disks) {
        // 跳过当前目录和上级目录
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        // 过滤虚拟设备
        if (strncmp(entry->d_name, "loop", 4) == 0 ||
            strncmp(entry->d_name, "ram", 3) == 0 ||
            strncmp(entry->d_name, "fd", 2) == 0 ||
            strncmp(entry->d_name, "sr", 2) == 0 ||    // 光驱
            strncmp(entry->d_name, "dm-", 3) == 0) {  // device mapper
            continue;
        }
        
        // 检查是否为真实物理设备
        snprintf(path, sizeof(path), "/sys/block/%s/device", entry->d_name);
        if (access(path, F_OK) != 0) {
            continue; // 没有device目录，可能是虚拟设备
        }
        
        // 检查是否可移动设备
        int removable = 0;
        snprintf(path, sizeof(path), "/sys/block/%s/removable", entry->d_name);
        file = fopen(path, "r");
        if (file) {
            fscanf(file, "%d", &removable);
            fclose(file);
        }
        
        // 跳过可移动设备
        if (removable) {
            continue;
            // // 检查是否有介质
            // snprintf(path, sizeof(path), "/sys/block/%s/size", entry->d_name);
            // file = fopen(path, "r");
            // if (file) {
            //     unsigned long long size_blocks = 0;
            //     fscanf(file, "%llu", &size_blocks);
            //     fclose(file);
            //     if (size_blocks == 0) {
            //         continue; // 可移动设备但没有介质
            //     }
            // } else {
            //     continue; // 无法读取size，可能是无效设备
            // }
        }
        
        // 检查设备大小，过滤掉大小为0的设备
        unsigned long long size = get_disk_size(entry->d_name);
        if (size == 0) {
            continue;
        }
        
        // 更准确的分区检测
        snprintf(path, sizeof(path), "/sys/block/%s/partition", entry->d_name);
        if (access(path, F_OK) == 0) {
            continue; // 这是一个分区
        }
        
        // 检查是否有子分区（如果有分区表，通常会有子设备）
        int has_partitions = 0;
        DIR *subdir;
        char subpath[1024];
        snprintf(path, sizeof(path), "/sys/block/%s", entry->d_name);
        subdir = opendir(path);
        if (subdir) {
            struct dirent *subentry;
            while ((subentry = readdir(subdir)) != NULL) {
                if (strncmp(subentry->d_name, entry->d_name, strlen(entry->d_name)) == 0) {
                    // 检查是否是分区设备（如sdb1, sdb2等）
                    snprintf(subpath, sizeof(subpath), "/sys/block/%s/%s/partition", 
                            entry->d_name, subentry->d_name);
                    if (access(subpath, F_OK) == 0) {
                        has_partitions = 1;
                        break;
                    }
                }
            }
            closedir(subdir);
        }
        
        // 支持更多设备类型
        if (strncmp(entry->d_name, "sd", 2) == 0 || 
            strncmp(entry->d_name, "nvme", 4) == 0 ||
            strncmp(entry->d_name, "hd", 2) == 0 ||      // IDE设备
            strncmp(entry->d_name, "vd", 2) == 0) {     // VirtIO设备
            
            // 额外的设备可用性检查
            snprintf(path, sizeof(path), "/dev/%s", entry->d_name);
            if (access(path, F_OK) != 0) {
                continue; // 设备节点不存在
            }
            
            strncpy(disks[disk_count].device, entry->d_name, 32);
            
            // 确定硬盘类型
            if (strncmp(entry->d_name, "nvme", 4) == 0) {
                strcpy(disks[disk_count].type, "NVMe");
            } else if (strncmp(entry->d_name, "sd", 2) == 0) {
                // 进一步区分SATA和USB
                snprintf(path, sizeof(path), "/sys/block/%s/device/vendor", entry->d_name);
                file = fopen(path, "r");
                if (file) {
                    char vendor[64] = {0};
                    if (fgets(vendor, sizeof(vendor), file)) {
                        // 可以根据vendor信息更准确判断
                        if (strstr(vendor, "USB") || strstr(vendor, "usb")) {
                            strcpy(disks[disk_count].type, "USB");
                        } else {
                            strcpy(disks[disk_count].type, "SATA");
                        }
                    }
                    fclose(file);
                } else {
                    strcpy(disks[disk_count].type, "SATA/USB");
                }
            } else if (strncmp(entry->d_name, "hd", 2) == 0) {
                strcpy(disks[disk_count].type, "IDE");
            } else if (strncmp(entry->d_name, "vd", 2) == 0) {
                strcpy(disks[disk_count].type, "VirtIO");
            }
            
            // 标记是否为可移动设备
            //disks[disk_count].removable = removable;
            
            // 获取基本信息
            get_disk_identity(entry->d_name, 
                            disks[disk_count].model, 
                            disks[disk_count].serial);
            
            disks[disk_count].total_size = size/1024/1024/1024; // Change to GB
            
            get_mountpoint(entry->d_name, disks[disk_count].mountpoint);
            
            // 如果有挂载点，获取使用情况
            if (strlen(disks[disk_count].mountpoint) > 0) {
                unsigned long long total, free, used;
                if (get_mountpoint_usage(disks[disk_count].mountpoint, &total, &free, &used) == 0) {
                    disks[disk_count].free_size = free/1024/1024/1024; // Change to GB
                    disks[disk_count].used_size = used/1024/1024/1024; // Change to GB
                    if (total > 0) {
                        disks[disk_count].usage_percent = ((double)used / total) * 100.0;
                    }
                }
            }
            
            // 获取温度
            disks[disk_count].temperature = get_disk_temperature(entry->d_name);
            
            disk_count++;
        }
    }
    
    closedir(block_dir);
    return disk_count;
}

//6266 CMD
// 获取 I/O 端口权限
int acquire_io_permissions() {
    // 请求62/66端口权限
    if (ioperm(ITE_EC_DATA_PORT, 1, 1) != 0) {
        printf("错误: 无法获取端口 0x%02X 权限\n", ITE_EC_DATA_PORT);
        return -1;
    }
    if (ioperm(ITE_EC_CMD_PORT, 1, 1) != 0) {
        printf("错误: 无法获取端口 0x%02X 权限\n", ITE_EC_CMD_PORT);
        ioperm(ITE_EC_DATA_PORT, 1, 0);
        return -1;
    }
    #if DebugToken
    printf("EC 62/66 Port Permission Get OK\n");
    #endif
    return 0;
}

// 释放 I/O 端口权限
void release_io_permissions() {
    ioperm(ITE_EC_DATA_PORT, 1, 0);
    ioperm(ITE_EC_CMD_PORT, 1, 0);
}
// 等待 EC 就绪
int ec_wait_ready() {
    int timeout = 1000; // 超时时间
    unsigned char status;
    
    while (timeout--) {
        status = inb(ITE_EC_CMD_PORT);
        if (!(status & 0x02)) { // 检查忙标志位
            return 0;
        }
        usleep(100); // 等待 100μs
    }
    
    fprintf(stderr, "EC timeout waiting for ready\n");
    return -1;
}

// 写入 EC 索引
void ec_write_index(unsigned char index) {
    outb(index, ITE_EC_CMD_PORT);
}

// 写入 EC 数据
void ec_write_data(unsigned char data) {
    outb(data, ITE_EC_DATA_PORT);
}

// 读取 EC 数据
unsigned char ec_read_data() {
    return inb(ITE_EC_DATA_PORT);
}
// 读取 EC RAM 字节
int ec_ram_read_byte(unsigned char address, unsigned char *value) {
    if (ec_wait_ready() < 0) {
        return -1;
    }
    
    // 发送读取命令和地址
    ec_write_index(EC_CMD_READ_RAM);
    if (ec_wait_ready() < 0) {
        return -1;
    }
    ec_write_data(address);
    
    if (ec_wait_ready() < 0) {
        return -1;
    }
    //测试发现需要额外加一个delay不加会读上一次数据，Spec没有看到相关的说明，暂时为3000us
    usleep(3000);
    // 读取数据
    *value = ec_read_data();
    #if DebugToken
    printf("EC RAM Read OK\n");
    #endif
    return 0;
}

// 写入 EC RAM 字节
int ec_ram_write_byte(unsigned char address, unsigned char value) {
    if (ec_wait_ready() < 0) {
        return -1;
    }
    
    // 发送写入命令、地址和数据
    ec_write_index(EC_CMD_WRITE_RAM);
    if (ec_wait_ready() < 0) {
        return -1;
    }
    ec_write_data(address);
    if (ec_wait_ready() < 0) {
        return -1;
    }
    ec_write_data(value);
    #if DebugToken
    printf("EC RAM Write OK\n");
    #endif
    return ec_wait_ready();
}

// 读取 EC RAM 区域
int ec_ram_read_block(unsigned char start_addr, unsigned char *buffer, int length) {
    for (int i = 0; i < length; i++) {
        if (ec_ram_read_byte(start_addr + i, &buffer[i]) < 0) {
            return -1;
        }
    }
    return 0;
}

// 写入 EC RAM 区域
int ec_ram_write_block(unsigned char start_addr, unsigned char *data, int length) {
    for (int i = 0; i < length; i++) {
        if (ec_ram_write_byte(start_addr + i, data[i]) < 0) {
            return -1;
        }
    }
    return 0;
}
// 查询 EC 版本信息
int ec_query_version(char *version, int max_len) {
    if (ec_wait_ready() < 0) {
        return -1;
    }
    
    ec_write_index(EC_CMD_QUERY);
    ec_write_data(0x00); // 查询版本命令
    
    if (ec_wait_ready() < 0) {
        return -1;
    }
    
    // 读取版本字符串
    for (int i = 0; i < max_len - 1; i++) {
        unsigned char c = ec_read_data();
        if (c == 0) {
            version[i] = '\0';
            break;
        }
        version[i] = c;
    }
    version[max_len - 1] = '\0';
    
    return 0;
}

// 检查nvidia-smi是否可用
int nvidia_smi_available() {
    return system("which nvidia-smi > /dev/null 2>&1") == 0;
}

// 获取单个GPU的完整信息
#if DebugToken
int nvidia_get_single_gpu_info(nvidia_gpu_info_t *gpu) { 
    FILE *fp = popen("nvidia-smi --query-gpu=name,temperature.gpu,utilization.gpu,utilization.memory,memory.used,memory.total,power.draw,fan.speed,driver_version --format=csv,noheader,nounits 2>/dev/null", "r");
    if (!fp) {
        return -1;
    }
    
    char line[512];
    if (fgets(line, sizeof(line), fp)) {
        // 解析CSV格式
        char *tokens[9];
        int token_count = 0;
        char *token = strtok(line, ",");
        
        while (token && token_count < 9) {
            // 去除前后空格和换行符
            while (*token == ' ' || *token == '\t') token++;
            char *end = token + strlen(token) - 1;
            while (end > token && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t')) *end-- = '\0';
            
            tokens[token_count++] = token;
            token = strtok(NULL, ",");
        }
        
        if (token_count >= 8) {
            // 名称
            strncpy(gpu->name, tokens[0], sizeof(gpu->name) - 1);
            
            // 温度
            gpu->temperature = atoi(tokens[1]);
            
            // GPU使用率
            gpu->utilization_gpu = atoi(tokens[2]);
            
            // 显存使用率
            gpu->utilization_memory = atoi(tokens[3]);
            
            // 显存
            gpu->memory_used = atol(tokens[4]);
            gpu->memory_total = atol(tokens[5]);
            
            // 功耗
            gpu->power_draw = atof(tokens[6]);
            
            // 风扇速度
            if (token_count >= 8 && tokens[7][0] != '\0') {
                gpu->fan_speed = atoi(tokens[7]);
            } else {
                gpu->fan_speed = 0;
            }
            
            // 驱动版本
            if (token_count >= 9) {
                strncpy(gpu->driver_version, tokens[8], sizeof(gpu->driver_version) - 1);
            } else {
                strcpy(gpu->driver_version, "Unknown");
            }
            
            pclose(fp);
            return 0;
        }
    }
    
    pclose(fp);
    return -1;
}
#endif
// 快速获取GPU温度
int nvidia_get_gpu_temperature() {
    char command[] = "nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader,nounits 2>/dev/null";
    
    FILE *fp = popen(command, "r");
    if (!fp) return -1;
    
    char temp_str[16];
    int temperature = -1;
    if (fgets(temp_str, sizeof(temp_str), fp)) {
        temperature = atoi(temp_str);
    }
    pclose(fp);
    return temperature;
}

// 快速获取GPU使用率
int nvidia_get_gpu_utilization() {
    char command[] = "nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits 2>/dev/null";
    
    FILE *fp = popen(command, "r");
    if (!fp) return -1;
    
    char usage_str[16];
    int utilization = -1;
    if (fgets(usage_str, sizeof(usage_str), fp)) {
        utilization = atoi(usage_str);
    }
    pclose(fp);
    return utilization;
}

// 快速获取GPU风扇转速
int nvidia_get_gpu_fan_speed() {
    char command[] = "nvidia-smi --query-gpu=fan.speed --format=csv,noheader,nounits 2>/dev/null";
    
    FILE *fp = popen(command, "r");
    if (!fp) return -1;
    
    char speed_str[16];
    int fan_speed = -1;
    if (fgets(speed_str, sizeof(speed_str), fp)) {
        fan_speed = atoi(speed_str);
    }
    
    pclose(fp);
    return fan_speed;
}

// 获取GPU核心三要素：温度、使用率、风扇转速
int nvidia_get_gpu_status(int *temp, int *usage, int *fan) {
    *temp = nvidia_get_gpu_temperature();
    *usage = nvidia_get_gpu_utilization();
    *fan = nvidia_get_gpu_fan_speed();
    
    return (*temp >= 0 && *usage >= 0 && *fan >= 0) ? 0 : -1;
}
void nvidia_print_info() {
    int temp, usage, fan;
    
    if (nvidia_get_gpu_status(&temp, &usage, &fan) != 0) {
        printf("GPU: 无法获取信息\n");
        return;
    }
    
    printf("GPU: %d°C %d%% %d%%\n", temp, usage, fan);
}
// 信号处理函数
void signal_handler(int sig) {
    running = false;
    printf("Received signal %d, shutting down...\n", sig);
}
// 阻塞读取线程函数
void* hid_read_thread(void *arg) {
    hid_device *handle = (hid_device *)arg;
    unsigned char read_buf[MAXLEN] = {0};
    int heartbeat_count = 0;
    #if DebugToken
    printf("HID read thread started (blocking mode)\n");
    #endif
    // 使用阻塞模式 - 这样会一直等待直到有数据到达
    hid_set_nonblocking(handle, 0);
    
    while (running) {
        int res = hid_read(handle, read_buf, sizeof(read_buf));
        
        if (res > 0) {
            #if DebugToken
            printf("HID Received %d bytes: ", res);
            for (int i = 0; i < res; i++) {
                printf("%02x ", read_buf[i]);
            }
            printf("\n");
            #endif
            // 检测心跳包
            if (res >= 7 && 
                read_buf[0] == 0xa5 && read_buf[1] == 0x5a && 
                read_buf[2] == 0xff && read_buf[3] == 0x04 &&
                read_buf[4] == 0x00 && read_buf[5] == 0x00 && 
                read_buf[6] == 0x02) {
                heartbeat_count++;
                printf(">>> HEARTBEAT detected! Count: %d\n", heartbeat_count);
            }
            // 检测其他命令...
            else if (res >= 6 && 
                     read_buf[0] == 0xa5 && read_buf[1] == 0x5a && 
                     read_buf[2] == 0xff && read_buf[3] == 0x04 &&
                     read_buf[4] == 0x03 && read_buf[5] == 0x82) {
                printf(">>> Hibernate command received!\n");
                systemoperation(HIBERNATEATONCE_AIM,0);
                //system("shutdown -h now");
            }
            else if (res >= 6 && 
                     read_buf[0] == 0xa5 && read_buf[1] == 0x5a && 
                     read_buf[2] == 0xff && read_buf[3] == 0x04 &&
                     read_buf[4] == 0x03) {
                    switch (read_buf[5])
                    {
                    case HomePage_AIM:
                        PageIndex = HomePage_AIM;
                        break;
                    case SystemPage_AIM:
                        PageIndex = SystemPage_AIM;
                        break;
                    case DiskPage_AIM:
                        PageIndex = DiskPage_AIM;
                        break;
                    case WlanPage_AIM:
                        PageIndex = WlanPage_AIM;
                        break;
                    case Properties_AIM:
                        PageIndex = HomePage_AIM;
                        acquire_io_permissions();
                        ec_ram_write_byte(0x98,0x05);//Performance
                        // 释放 I/O 权限
                        release_io_permissions();
                        break;
                    case Balance_AIM:
                        PageIndex = HomePage_AIM;
                        acquire_io_permissions();
                        ec_ram_write_byte(0x98,0x03);//Balance
                        // 释放 I/O 权限
                        release_io_permissions();
                        break;
                    case InfoPage_AIM:
                        PageIndex = InfoPage_AIM;
                        break;
                    default:
                        PageIndex = HomePage_AIM;
                        break;
                    }
                    #if DebugToken
                    printf(">>> PageChange command received!0x%02X\n",PageIndex);
                    #endif
            }
            else
            {

            }
            
            memset(read_buf, 0, sizeof(read_buf));
        } else if (res < 0) {
            printf("Error reading from HID device\n");
            break;
        }
        
    }
    
    printf("HID read thread exited\n");
    return NULL;
}
// 发送线程函数
void* hid_send_thread(void* arg) {
    printf("HID send thread start\n");
    
    while (running) {
        if(Isinitial)
        {
            // 发送HID数据
            if(HourTimeDiv % 60 == 0)
            {
                //1 Min Do
                for (int i = 0; i < disk_count; i++) {
                    if (disks[i].temperature != -1) {
                        int diskreportsize = init_hidreport(request, SET, Disk_AIM, i);

                        append_crc(request);
                        if (safe_hid_write(handle, hid_report, diskreportsize) == -1) {
                        printf("Failed to write Disk data\n");
                        break;
                        }
                        #if DebugToken
                        printf("-----------------------------------DiskSendOK %d times-----------------------------------\n",(i+1));
                        #endif
                        // printf("diskreportsize: %d\n",diskreportsize);
                        // printf("DiskId: %d\n",request->disk_data.disk_info.disk_id);
                        // printf("Diskunit: %d\n",request->disk_data.disk_info.unit);
                        // printf("Disktotal: %d\n",request->disk_data.disk_info.total_size);
                        // printf("Diskused: %d\n",request->disk_data.disk_info.used_size);
                        // printf("Disktemp: %d\n",request->disk_data.disk_info.temp);
                        // printf("DiskCRC: %d\n",request->disk_data.crc);
                        memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
                        // 休眠1秒，但分段休眠以便及时响应退出
                        for (int i = 0; i < 10 && running; i++) {
                            usleep(100000); // 100ms
                        }
                    }
                }
            }
            if(HourTimeDiv % 600 == 0)
            {
                // Time
                int timereportsize = init_hidreport(request, SET, TIME_AIM, 255);
                append_crc(request);
                if (safe_hid_write(handle, hid_report, timereportsize) == -1) {
                    printf("Failed to write TIME data\n");
                    break;
                }
                memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
                TimeSleep1Sec();
                #if DebugToken
                printf("-----------------------------------TimeSendOK-----------------------------------\n");
                #endif
                //WLAN IP
                int wlansize;
                for (int i = 0; i < g_iface_manager.count; i++)
                {
                    wlansize = init_hidreport(request, SET, WlanTotal_AIM,i);
                    append_crc(request);
                    if (safe_hid_write(handle, hid_report, wlansize) == -1) {
                        printf("Failed to write WLANTotal data\n");
                    break;
                    }
                    memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
                    TimeSleep1Sec();
                }
                #if DebugToken
                printf("-----------------------------------TotalflowSendOK-----------------------------------\n");
                #endif
                for (int i = 0; i < g_iface_manager.count; i++)
                {
                    wlansize = init_hidreport(request, SET, WlanIP_AIM, i);
                    append_crc(request);
                    if (safe_hid_write(handle, hid_report, wlansize) == -1) {
                        printf("Failed to write WlanIP data\n");
                    }
                    TimeSleep1Sec();
                    memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
                }
                #if DebugToken
                printf("-----------------------------------WLANIPSendOK-----------------------------------\n");
                #endif
            }
            switch (PageIndex)
            {
                case HomePage_AIM:
                    //Use 10Mins send once
                    // // Time
                    // int timereportsize = init_hidreport(request, SET, TIME_AIM, 255);
                    // append_crc(request);
                    // if (safe_hid_write(handle, hid_report, timereportsize) == -1) {
                    //     printf("Failed to write TIME data\n");
                    //     break;
                    // }
                    // memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
                    // TimeSleep1Sec();
                    // #if DebugToken
                    // printf("-----------------------------------TimeSendOK-----------------------------------\n");
                    // #endif
                    break;
                case SystemPage_AIM:
                    //*****************************************************/
                    // CPU
                    int systemreportsize = init_hidreport(request, SET, System_AIM,0);
                    append_crc(request);
                    if (safe_hid_write(handle, hid_report, systemreportsize) == -1) {
                        printf("Failed to write CPU data\n");
                        break;
                    }
                    memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
                    TimeSleep1Sec();
                    #if DebugToken
                    printf("-----------------------------------CPUSendOK-----------------------------------\n");
                    #endif

                    systemreportsize = init_hidreport(request, SET, System_AIM,1);        
                    append_crc(request);
                    if (safe_hid_write(handle, hid_report, systemreportsize) == -1) {
                        printf("Failed to write iGPU data\n");
                        break;
                    }
                    memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
                    TimeSleep1Sec();
                    #if DebugToken
                    printf("-----------------------------------iGPUSendOK-----------------------------------\n");
                    #endif
                    //*****************************************************/
                    // Memory Usage
                    int memusagesize = init_hidreport(request, SET, System_AIM,2);
                    append_crc(request);
                    if (safe_hid_write(handle, hid_report, memusagesize) == -1) {
                        printf("Failed to write MEMORY data\n");
                        break;
                    }
                    memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
                    TimeSleep1Sec();
                    #if DebugToken
                    printf("-----------------------------------MemorySendOK-----------------------------------\n");
                    #endif
                    if(IsNvidiaGPU)
                    {
                        int dgpusize = init_hidreport(request, SET, System_AIM,3);
                        append_crc(request);
                        if (safe_hid_write(handle, hid_report, dgpusize) == -1) {
                            printf("Failed to write GPU data\n");
                        break;
                        }
                        memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
                        TimeSleep1Sec();
                        #if DebugToken
                        printf("-----------------------------------GPUSendOK-----------------------------------\n");
                        #endif
                    }
                    break;
                case DiskPage_AIM:
                    int disk_maxtemp = 0;
                    for (int i = 0; i < disk_count; i++) {
                        if (disks[i].temperature != -1) {
                            if(disk_maxtemp < disks[i].temperature)
                            {
                                disk_maxtemp = disks[i].temperature;
                            }
                            int diskreportsize = init_hidreport(request, SET, Disk_AIM, i);
                            append_crc(request);
                            if (safe_hid_write(handle, hid_report, diskreportsize) == -1) {
                            printf("Failed to write Disk data\n");
                            break;
                            }
                            // printf("diskreportsize: %d\n",diskreportsize);
                            // printf("DiskId: %d\n",request->disk_data.disk_info.disk_id);
                            // printf("Diskunit: %d\n",request->disk_data.disk_info.unit);
                            // printf("Disktotal: %d\n",request->disk_data.disk_info.total_size);
                            // printf("Diskused: %d\n",request->disk_data.disk_info.used_size);
                            // printf("Disktemp: %d\n",request->disk_data.disk_info.temp);
                            // printf("DiskCRC: %d\n",request->disk_data.crc);
                            memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
                            // 休眠1秒，但分段休眠以便及时响应退出
                            for (int i = 0; i < 10 && running; i++) {
                                usleep(100000); // 100ms
                            }
                            #if DebugToken
                            printf("-----------------------------------DiskSendOK %d times-----------------------------------\n",(i+1));
                            #endif
                        }
                    }
                    // 获取 I/O 权限
                    acquire_io_permissions();
                    ec_ram_write_byte(0xB1,disk_maxtemp);
                    // 释放 I/O 权限
                    release_io_permissions();
                    break;
                case WlanPage_AIM:
                    //*****************************************************/
                    // User Online
                    int usersize = init_hidreport(request, SET, USER_AIM,255);
                    append_crc(request);
                    if (safe_hid_write(handle, hid_report, usersize) == -1) {
                        printf("Failed to write USER data\n");
                        break;
                    }
                    memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
                    TimeSleep1Sec();
                    #if DebugToken
                    printf("-----------------------------------UserSendOK-----------------------------------\n");
                    #endif
                    
                    int wlanspeedsize,wlantotalsize;
                    for (int i = 0; i < g_iface_manager.count; i++)
                    {
                        wlanspeedsize = init_hidreport(request, SET, WlanSpeed_AIM,i);
                        append_crc(request);
                        if (safe_hid_write(handle, hid_report, wlanspeedsize) == -1) {
                            printf("Failed to write WLANSpeed data\n");
                        break;
                        }
                        memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
                        TimeSleep1Sec();
                         wlantotalsize = init_hidreport(request, SET, WlanTotal_AIM,i);
                        append_crc(request);
                        if (safe_hid_write(handle, hid_report, wlantotalsize) == -1) {
                            printf("Failed to write WLANTotal data\n");
                        break;
                        }
                        memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
                        TimeSleep1Sec();
                        #if DebugToken
                        printf("-----------------------------------WLANSpeedTotalSendOK%dTime-----------------------------------\n",i);
                        #endif
                    }
                   
                    #if DebugToken
                    printf("-----------------------------------WLANSpeedTotalSendOK-----------------------------------\n");
                    #endif
                    break;
            
            default:
                break;
            }
        }
    }
    printf("HID send thread exited\n");
    return NULL;
}
// 线程安全的写入函数
int safe_hid_write(hid_device *handle, const unsigned char *data, int length) {
    pthread_mutex_lock(&hid_mutex);
    int result = hid_write(handle, data, length);
    pthread_mutex_unlock(&hid_mutex);
    return result;
}

void systemoperation(unsigned char cmd,unsigned char time)
{
    char *command = NULL;
    for (unsigned char i = 0; i < time; i++)
    {
        TimeSleep1Sec();
    }
    switch (cmd)
    {
    case HIBERNATEATONCE_AIM:
        command = "systemctl suspend";
        break;
    
    default:
        break;
    }
    system(command);
}
// 从/proc/net/dev获取网络流量统计
static int get_interface_stats_raw(const char *ifname, unsigned long long *rx_bytes, unsigned long long *tx_bytes) {
    FILE *fp;
    char line[512];
    char iface[32];
    
    fp = fopen("/proc/net/dev", "r");
    if (fp == NULL) {
        printf("ERROR Failed to open /proc/net/dev: %s\n", strerror(errno));
        return -1;
    }
    
    // 跳过前两行标题
    fgets(line, sizeof(line), fp);
    fgets(line, sizeof(line), fp);
    
    *rx_bytes = 0;
    *tx_bytes = 0;
    
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *colon = strchr(line, ':');
        if (colon == NULL) continue;
        
        // 提取接口名称
        *colon = '\0';
        strcpy(iface, line);
        
        // 移除前导空格
        char *iface_name = iface;
        while (*iface_name == ' ') iface_name++;
        
        if (strcmp(iface_name, ifname) == 0) {
            // 解析接收和发送字节数
            sscanf(colon + 1, "%llu %*u %*u %*u %*u %*u %*u %*u %llu",
                   rx_bytes, tx_bytes);
            fclose(fp);
            return 0;
        }
    }
    
    fclose(fp);
    return -1;
}

// 获取MAC地址
static int get_interface_mac_address(const char *ifname, char *mac_addr) {
    struct ifreq ifr;
    int fd;
    
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
        close(fd);
        return -1;
    }
    
    unsigned char *mac = (unsigned char *)ifr.ifr_hwaddr.sa_data;
    snprintf(mac_addr, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    close(fd);
    return 0;
}

// 获取接口状态
static int get_interface_status(const char *ifname, char *status) {
    struct ifreq ifr;
    int fd;
    
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        close(fd);
        return -1;
    }
    
    if (ifr.ifr_flags & IFF_UP) {
        strcpy(status, "UP");
    } else {
        strcpy(status, "DOWN");
    }
    
    if (ifr.ifr_flags & IFF_RUNNING) {
        strcat(status, " (RUNNING)");
    }
    
    close(fd);
    return 0;
}

// 获取IP地址信息
static void get_interface_ip_info(const char *ifname, network_interface_t *iface) {
    struct ifaddrs *ifaddr, *ifa;
    
    strcpy(iface->ip_address, "0.0.0.0");
    strcpy(iface->netmask, "0.0.0.0");
    
    if (getifaddrs(&ifaddr) == -1) {
        return;
    }
    
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (strcmp(ifa->ifa_name, ifname) != 0) continue;
        
        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
            struct sockaddr_in *netmask = (struct sockaddr_in *)ifa->ifa_netmask;
            
            char ip_str[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &addr->sin_addr, ip_str, INET_ADDRSTRLEN)) {
                if (strcmp(ip_str, "0.0.0.0") != 0) {
                    strncpy(iface->ip_address, ip_str, sizeof(iface->ip_address) - 1);
                }
            }
            
            if (netmask != NULL) {
                char mask_str[INET_ADDRSTRLEN];
                if (inet_ntop(AF_INET, &netmask->sin_addr, mask_str, INET_ADDRSTRLEN)) {
                    strncpy(iface->netmask, mask_str, sizeof(iface->netmask) - 1);
                }
            }
            
            break;
        }
    }
    
    freeifaddrs(ifaddr);
}

// 在管理器中查找接口
static network_interface_t* find_interface(const char *ifname) {
    for (int i = 0; i < g_iface_manager.count; i++) {
        if (strcmp(g_iface_manager.interfaces[i].interface_name, ifname) == 0) {
            return &g_iface_manager.interfaces[i];
        }
    }
    return NULL;
}


// 初始化网络监控系统
int init_network_monitor() {
    printf("INFO Initializing network monitor\n");
    
    // 初始分配8个接口的空间
    g_iface_manager.capacity = 8;
    g_iface_manager.interfaces = (network_interface_t*)malloc(
        g_iface_manager.capacity * sizeof(network_interface_t));
    
    if (g_iface_manager.interfaces == NULL) {
        printf("ERROR Failed to allocate memory for interfaces\n");
        return -1;
    }
    
    g_iface_manager.count = 0;
    printf("INFO Network monitor initialized successfully\n");
    return 0;
}

// 注册一个接口到监控系统
int register_interface(const char *ifname) {
    // 检查接口是否已注册
    if (find_interface(ifname) != NULL) {
        printf("WARNING Interface %s is already registered\n", ifname);
        return 0; // 已存在，不算错误
    }
    
    // 检查是否需要扩容
    if (g_iface_manager.count >= g_iface_manager.capacity) {
        int new_capacity = g_iface_manager.capacity * 2;
        network_interface_t *new_interfaces = (network_interface_t*)realloc(
            g_iface_manager.interfaces, new_capacity * sizeof(network_interface_t));
        
        if (new_interfaces == NULL) {
            printf("ERROR Failed to expand interface array\n");
            return -1;
        }
        
        g_iface_manager.interfaces = new_interfaces;
        g_iface_manager.capacity = new_capacity;
    }
    
    // 初始化新接口
    network_interface_t *iface = &g_iface_manager.interfaces[g_iface_manager.count];
    memset(iface, 0, sizeof(network_interface_t));
    strncpy(iface->interface_name, ifname, sizeof(iface->interface_name) - 1);
    
    // 获取静态信息
    if (get_interface_status(ifname, iface->status) < 0) {
        strcpy(iface->status, "UNKNOWN");
    }
    
    if (get_interface_mac_address(ifname, iface->mac_address) < 0) {
        strcpy(iface->mac_address, "00:00:00:00:00:00");
    }
    
    get_interface_ip_info(ifname, iface);
    
    // 获取初始流量统计
    if (get_interface_stats_raw(ifname, &iface->rx_bytes, &iface->tx_bytes) == 0) {
        iface->rx_bytes_prev = iface->rx_bytes;
        iface->tx_bytes_prev = iface->tx_bytes;
        iface->initialized = 1;
        time(&iface->last_update);
        
        // 计算初始总流量
        iface->rx_total_mb = (double)iface->rx_bytes / (1024.0 * 1024.0);
        iface->tx_total_mb = (double)iface->tx_bytes / (1024.0 * 1024.0);
    } else {
        printf("WARNING Failed to get initial stats for %s\n", ifname);
        iface->initialized = 0;
    }
    
    g_iface_manager.count++;
    printf("INFO Registered interface: %s\n", ifname);
    
    return 0;
}

// 注册所有物理接口
int register_all_physical_interfaces() {
    struct ifaddrs *ifaddr, *ifa;
    int registered_count = 0;
    
    if (getifaddrs(&ifaddr) == -1) {
        printf("ERROR getifaddrs failed: %s\n", strerror(errno));
        return 0;
    }
    
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        
        const char *ifname = ifa->ifa_name;
        
        // 过滤条件
        if (strcmp(ifname, "lo") == 0) continue;
        if (strstr(ifname, "docker") != NULL) continue;
        if (strstr(ifname, "veth") != NULL) continue;
        if (strstr(ifname, "br-") != NULL) continue;
        if (strstr(ifname, "virbr") != NULL) continue;
        if (strstr(ifname, "tun") != NULL) continue;
        if (strstr(ifname, "tap") != NULL) continue;
        
        // 检查是否已注册
        if (find_interface(ifname) == NULL) {
            if (register_interface(ifname) == 0) {
                registered_count++;
            }
        }
    }
    
    freeifaddrs(ifaddr);
    printf("INFO Registered %d physical interfaces\n", registered_count);
    return registered_count;
}

// 获取接口基本信息
int get_interface_basic_info(const char *ifname, 
                           char *status, 
                           char *mac_addr,
                           char *ip_addr,
                           char *netmask) {
    
    network_interface_t *iface = find_interface(ifname);
    if (iface == NULL) {
        printf("ERROR Interface %s not found\n", ifname);
        return -1;
    }
    
    if (status) strcpy(status, iface->status);
    if (mac_addr) strcpy(mac_addr, iface->mac_address);
    if (ip_addr) strcpy(ip_addr, iface->ip_address);
    if (netmask) strcpy(netmask, iface->netmask);
    
    return 0;
}

// 主要API：获取接口流量信息
int get_interface_traffic_info(const char *ifname,
                             double *rx_speed_kb,
                             double *tx_speed_kb,
                             double *rx_total_mb,
                             double *tx_total_mb) {
    
    network_interface_t *iface = find_interface(ifname);
    if (iface == NULL) {
        printf("ERROR Interface %s not registered\n", ifname);
        return -1;
    }
    
    // 获取当前流量统计
    unsigned long long current_rx, current_tx;
    if (get_interface_stats_raw(ifname, &current_rx, &current_tx) < 0) {
        printf("WARNING Failed to get stats for %s\n", ifname);
        return -1;
    }
    
    time_t now;
    time(&now);
    
    // 如果这是第一次获取，初始化
    if (!iface->initialized) {
        iface->rx_bytes = current_rx;
        iface->tx_bytes = current_tx;
        iface->rx_bytes_prev = current_rx;
        iface->tx_bytes_prev = current_tx;
        iface->last_update = now;
        iface->initialized = 1;
        
        // 计算总流量
        iface->rx_total_mb = (double)current_rx / (1024.0 * 1024.0);
        iface->tx_total_mb = (double)current_tx / (1024.0 * 1024.0);
        
        // 速度初始为0
        iface->rx_speed_kb = 0.0;
        iface->tx_speed_kb = 0.0;
    } else {
        // 更新当前值
        iface->rx_bytes = current_rx;
        iface->tx_bytes = current_tx;
        
        // 计算时间差
        double time_diff = difftime(now, iface->last_update);
        
        // 计算速度（处理时间差过小的情况）
        if (time_diff > 0.1) {  // 至少0.1秒才有意义
            // 检查计数器是否重置
            unsigned long long rx_diff = (current_rx >= iface->rx_bytes_prev) ? 
                                        (current_rx - iface->rx_bytes_prev) : current_rx;
            unsigned long long tx_diff = (current_tx >= iface->tx_bytes_prev) ? 
                                        (current_tx - iface->tx_bytes_prev) : current_tx;
            
            // 计算速度（KB/s）
            iface->rx_speed_kb = (double)rx_diff / time_diff / 1024.0;
            iface->tx_speed_kb = (double)tx_diff / time_diff / 1024.0;
            
            // 更新previous值
            iface->rx_bytes_prev = current_rx;
            iface->tx_bytes_prev = current_tx;
        }
        // 如果时间差太小，保持之前的速度值
        
        // 更新最后更新时间
        iface->last_update = now;
        
        // 更新总流量
        iface->rx_total_mb = (double)current_rx / (1024.0 * 1024.0);
        iface->tx_total_mb = (double)current_tx / (1024.0 * 1024.0);
    }
    
    // 返回结果
    if (rx_speed_kb) *rx_speed_kb = iface->rx_speed_kb;
    if (tx_speed_kb) *tx_speed_kb = iface->tx_speed_kb;
    if (rx_total_mb) *rx_total_mb = iface->rx_total_mb;
    if (tx_total_mb) *tx_total_mb = iface->tx_total_mb;
    
    return 0;
}

// 获取所有已注册的接口名称
int get_registered_interfaces(char interfaces[][32], int max_interfaces) {
    int count = 0;
    
    for (int i = 0; i < g_iface_manager.count && count < max_interfaces; i++) {
        strncpy(interfaces[count], g_iface_manager.interfaces[i].interface_name, 31);
        interfaces[count][31] = '\0';
        count++;
    }
    
    return count;
}

// 清理资源
void cleanup_network_monitor() {
    if (g_iface_manager.interfaces != NULL) {
        free(g_iface_manager.interfaces);
        g_iface_manager.interfaces = NULL;
    }
    g_iface_manager.count = 0;
    g_iface_manager.capacity = 0;
    printf("INFO Network monitor cleaned up\n");
}

// ========== 工具函数 ==========

// 显示接口完整信息
void display_interface_info(const char *ifname) {
    char status[16], mac[18], ip[INET_ADDRSTRLEN], mask[INET_ADDRSTRLEN];
    double rx_speed = 0, tx_speed = 0, rx_total = 0, tx_total = 0;
    
    printf("\n=== Network Interface: %s ===\n", ifname);
    
    // 获取基本信息
    if (get_interface_basic_info(ifname, status, mac, ip, mask) == 0) {
        printf("Status:    %s\n", status);
        printf("MAC:       %s\n", mac);
        printf("IP:        %s\n", ip);
        printf("Netmask:   %s\n", mask);
    }
    
    // 获取流量信息
    if (get_interface_traffic_info(ifname, &rx_speed, &tx_speed, &rx_total, &tx_total) == 0) {
        printf("RX Speed:  %.2f KB/s\n", rx_speed);
        printf("TX Speed:  %.2f KB/s\n", tx_speed);
        printf("Total RX:  %.2f MB\n", rx_total);
        printf("Total TX:  %.2f MB\n", tx_total);
    }
    
    printf("===============================\n");
}

// 获取系统所有物理接口的总流量

// 获取接口流量
void monitor_interface_periodically(const char *ifname) {
    double rx_speed, tx_speed, rx_total, tx_total;
    
    if (get_interface_traffic_info(ifname, &rx_speed, &tx_speed, &rx_total, &tx_total) == 0) {
        printf("INFO %s - RX: %.2f KB/s, TX: %.2f KB/s, Total: RX=%.2f MB, TX=%.2f MB\n",
                   ifname, rx_speed, tx_speed, rx_total, tx_total);
    }
}

// 获取所有接口的流量信息
void monitor_all_interfaces() {
    char interfaces[10][32];
    int count = get_registered_interfaces(interfaces, 10);
    
    for (int i = 0; i < count; i++) {
        monitor_interface_periodically(interfaces[i]);
    }
}

void get_system_info(system_info_t *info) {
    // 设备名称（主机名）
    gethostname(info->devicename, sizeof(info->devicename));
    
    // 处理器信息
    FILE *cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo) {
        char line[256];
        while (fgets(line, sizeof(line), cpuinfo)) {
            if (strstr(line, "model name")) {
                char *colon = strchr(line, ':');
                if (colon) {
                    strcpy(info->cpuname, colon + 2);
                    info->cpuname[strcspn(info->cpuname, "\n")] = 0;
                    break;
                }
            }
        }
        fclose(cpuinfo);
    }
    
    // 操作系统信息
    FILE *os_release = fopen("/etc/os-release", "r");
    if (os_release) {
        char line[256];
        while (fgets(line, sizeof(line), os_release)) {
            if (strstr(line, "PRETTY_NAME")) {
                char *start = strchr(line, '"') + 1;
                char *end = strrchr(line, '"');
                if (start && end) {
                    strncpy(info->operatename, start, end - start);
                    info->operatename[end - start] = '\0';
                    break;
                }
            }
        }
        fclose(os_release);
    }
    
    // 序列号
    FILE *serial_file = fopen("/sys/class/dmi/id/product_serial", "r");
    if (serial_file) {
        fgets(info->serial_number, sizeof(info->serial_number), serial_file);
        info->serial_number[strcspn(info->serial_number, "\n")] = 0;
        fclose(serial_file);
    } else {
        strcpy(info->serial_number, "Not Available");
    }
}
