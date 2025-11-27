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
    char name[IFNAMSIZ];          // 接口名称
    int is_physical;              // 是否是物理接口
    char operstate[16];           // 操作状态 (up/down)
    char ip_address[MAX_IP_LENGTH]; // IP地址
    char mac_address[18];         // MAC地址
    char netmask[MAX_IP_LENGTH];  // 子网掩码
    int max_speed;                // 最大支持速度 (Mbps)
    int current_speed;            // 当前连接速度 (Mbps)
    char duplex[16];              // 双工模式
    int mtu;                      // MTU值
    unsigned long long rx_bytes;  // 接收字节数
    unsigned long long tx_bytes;  // 发送字节数
} network_interface_t;
typedef struct {
    unsigned long long total_rx;  // 系统总接收字节
    unsigned long long total_tx;  // 系统总发送字节
    unsigned long long last_rx;   // 上一次总接收字节
    unsigned long long last_tx;   // 上一次总发送字节
    time_t last_time;             // 上一次更新时间
    int first_call;               // 是否是第一次调用
} system_traffic_t;
static unsigned char COMMLEN = offsetof(Request, common_data.data) - offsetof(Request, length);
void TimeSleep1Sec();
int init_hidreport(Request *request, unsigned char cmd, unsigned char aim, unsigned char id);
int first_init_hidreport(Request* request, unsigned char cmd, unsigned char aim,unsigned char total,unsigned char order);
void append_crc(Request *request);
unsigned char cal_crc(unsigned char * data, int len);
int get_cpu_temperature();
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
int get_mountpoint_usage(const char *mountpoint, unsigned long long *total, 
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
int safe_hid_write(hid_device *handle, const unsigned char *data, int length);
void systemoperation(unsigned char time,unsigned char cmd);
int is_physical_interface(const char *ifname);
void get_interface_basic_info(const char *ifname, network_interface_t *iface);
void get_interface_ip_info(const char *ifname, network_interface_t *iface);
void get_interface_speed_info(const char *ifname, network_interface_t *iface);
void get_interface_stats(const char *ifname, network_interface_t *iface);
int scan_network_interfaces();
void print_interface_info(const network_interface_t *iface);
void print_all_interfaces();
void init_traffic(system_traffic_t *t);
int get_system_total_traffic(system_traffic_t *t, 
                            double *rx_speed, double *tx_speed);
system_traffic_t traffic;
double rx_speed, tx_speed;
CPUData prev_data, curr_data;
bool IsNvidiaGPU;
int disk_count;
network_interface_t wlaninterfaces[MAX_INTERFACES];
int interface_count = 0;
//1Hour Count
int HourTimeDiv = 0;
disk_info_t disks[MAX_DISKS];
static volatile bool running = true;
static pthread_t read_thread;
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
system_info_t sys_info;
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
    hid_device *handle = hid_open(VENDORID, PRODUCTID, NULL);
    
    if (handle == NULL) {
        printf("ERROR: Failed to open device %04x:%04x\n", VENDORID, PRODUCTID);
        res = hid_exit();
        return 0;
    }
    #if DebugToken
    printf("物理网络接口信息获取工具\n");
    printf("=======================\n\n");
    #endif
    // 扫描所有物理网络接口
    int wlancount = scan_network_interfaces();
    if (wlancount <= 0) {
        printf("未发现物理网络接口\n");
        return 1;
    }
    #if DebugToken
    print_all_interfaces();
    
    printf("HID device opened successfully\n");
    #endif
    #endif
    // 创建读取线程
    #if !IfNoPanel
    if (pthread_create(&read_thread, NULL, hid_read_thread, handle) != 0) {
        printf("Failed to create read thread\n");
        hid_close(handle);
        hid_exit();
        return -1;
    }
    #endif
    // 初始化数据
    int cputemp,cpusuage,cpufan,memoryusage;
    IsNvidiaGPU = nvidia_smi_available();
    unsigned char hid_report[MAXLEN] = {0};
    unsigned char ack[MAXLEN] = {0};
    cputemp = get_cpu_temperature();
    init_traffic(&traffic);
    get_system_total_traffic(&traffic, &rx_speed, &tx_speed);
    //get_system_total_traffic(&traffic, &rx_speed, &tx_speed);
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
    Request* request = (Request *)hid_report;
    //HomePage
    #if DebugToken
    printf("-----------------------------------HomePage initial start-----------------------------------\n");
    #endif
    int homepage = init_hidreport(request, SET, TIME_AIM, 255);
    append_crc(request);
    if (safe_hid_write(handle, hid_report, homepage) == -1) {
        printf("Failed to write HomePage data\n");
    }
    sleep(3);
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
    for (int i = 0; i < wlancount; i++)
    {
        wlanpage = first_init_hidreport(request, SET, WlanPage_AIM, wlancount, i + 1);
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
    #if DebugToken
    printf("-----------------------------------InfoPage initial start-----------------------------------\n");
    #endif
    int infopage = first_init_hidreport(request, SET, InfoPage_AIM, 1, 1);
    append_crc(request);
    if (safe_hid_write(handle, hid_report, infopage) == -1) {
        printf("Failed to write InfoPage data\n");
    }
    sleep(3);
    memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
    #if DebugToken
    printf("-----------------------------------InfoPage initial end-----------------------------------\n");
    #endif
    while (running) {
        #if !IfNoPanel
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
                    TimeSleep1Sec();
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
            int wlanipsize;
            for (int i = 0; i < interface_count; i++)
            {
            wlanipsize = init_hidreport(request, SET, WlanTotal_AIM,i);
            append_crc(request);
            if (safe_hid_write(handle, hid_report, wlanipsize) == -1) {
                printf("Failed to write WLANip data\n");
            break;
            }
            memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
            TimeSleep1Sec();
        }
        #if DebugToken
        printf("-----------------------------------WLANIPSendOK-----------------------------------\n");
        #endif
        }
        #endif
        #if !IfNoPanel
        //*****************************************************/
        // CPU
        int systemreportsize = init_hidreport(request, SET, System_AIM,0);
        append_crc(request);
        if (safe_hid_write(handle, hid_report, systemreportsize) == -1) {
            printf("Failed to write CPU data\n");
            break;
        }
        #if DebugToken
        printf("-----------------------------------CPUSendOK-----------------------------------\n");
        #endif
        memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
        TimeSleep1Sec();


        systemreportsize = init_hidreport(request, SET, System_AIM,1);        
        append_crc(request);
        if (safe_hid_write(handle, hid_report, systemreportsize) == -1) {
            printf("Failed to write iGPU data\n");
            break;
        }
        #if DebugToken
        printf("-----------------------------------iGPUSendOK-----------------------------------\n");
        #endif
        memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
        TimeSleep1Sec();
        //*****************************************************/
        // Memory Usage
        int memusagesize = init_hidreport(request, SET, System_AIM,2);
        append_crc(request);
        if (safe_hid_write(handle, hid_report, memusagesize) == -1) {
            printf("Failed to write MEMORY data\n");
            break;
        }
        #if DebugToken
        printf("-----------------------------------MemorySendOK-----------------------------------\n");
        #endif
        memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
        TimeSleep1Sec();

        if(IsNvidiaGPU)
        {
            int dgpusize = init_hidreport(request, SET, System_AIM,3);
            append_crc(request);
            if (safe_hid_write(handle, hid_report, dgpusize) == -1) {
                printf("Failed to write GPU data\n");
            break;
            }
            #if DebugToken
            printf("-----------------------------------GPUSendOK-----------------------------------\n");
            #endif
            memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
            TimeSleep1Sec();
        }
        

        //*****************************************************/
        // User Online
        int usersize = init_hidreport(request, SET, USER_AIM,255);
        append_crc(request);
        if (safe_hid_write(handle, hid_report, usersize) == -1) {
            printf("Failed to write USER data\n");
            break;
        }
        #if DebugToken
        printf("-----------------------------------UserSendOK-----------------------------------\n");
        #endif
        memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
        TimeSleep1Sec();
        
        
        int wlanspeedsize = init_hidreport(request, SET, WlanSpeed_AIM,255);
        append_crc(request);
        if (safe_hid_write(handle, hid_report, wlanspeedsize) == -1) {
            printf("Failed to write WLANSpeed data\n");
        break;
        }
        #if DebugToken
        printf("-----------------------------------WLANSpeedSendOK-----------------------------------\n");
        #endif
        memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
        TimeSleep1Sec();

        int totalflowsize = init_hidreport(request, SET, WlanTotal_AIM,255);
        append_crc(request);
        if (safe_hid_write(handle, hid_report, totalflowsize) == -1) {
            printf("Failed to write Totalflow data\n");
        break;
        }
        #if DebugToken
        printf("-----------------------------------TotalflowSendOK-----------------------------------\n");
        #endif
        memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
        TimeSleep1Sec();





        //*****************************************************/
        //Get Error
        // int result = hid_read_timeout(handle, ack, 0x40, -1);
        // if (result == -1) {
        //     break;
        // }

        // if (result > 0) {
        //     parse_ack((Ack *)ack, request->aim);
        // }
        // if (hid_read(handle, hid_report, 0x40) == -1) {
        //     break;
        // }
        #if DebugToken
        printf("ReadData:0x%02x\n",hid_report[1]);
        #endif
        memset(hid_report, 0x0, sizeof(unsigned char) * 0x40);
        memset(ack, 0x0, sizeof(unsigned char) * 0x40);
        #endif
        //*****************************************************/
        
        //*****************************************************/
        TimeSleep1Sec();
    }
    // 释放内存
    #if !IfNoPanel
    if (read_thread) {
        pthread_join(read_thread, NULL);
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
    sleep(1);
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
            request->system_data.system_info.temerature = get_cpu_temperature();
            // 读取当前CPU数据
            read_cpu_data(&curr_data);
            // 计算CPU使用率
            //printf("CPU usage: %2f\n", calculate_cpu_usage(&prev_data, &curr_data));
            request->system_data.system_info.usage = calculate_cpu_usage(&prev_data, &curr_data);
            // 更新前一次的数据
            prev_data = curr_data;
            // 获取 I/O 权限
            acquire_io_permissions();
            unsigned char CPU_fan = 0;
            ec_ram_read_byte(0x70,&CPU_fan);
            request->system_data.system_info.rpm = CPU_fan;
            // 释放 I/O 权限
            release_io_permissions();
            
        }
        else if(id == 1)
        {
            request->system_data.system_info.usage = get_igpu_usage();
            request->system_data.system_info.temerature = get_igpu_temperature();
            acquire_io_permissions();
            unsigned char CPU_fan = 0;
            ec_ram_read_byte(0x70,&CPU_fan);
            request->system_data.system_info.rpm = CPU_fan;
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
        request->disk_data.disk_info.unit = 3;
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
        request->speed_data.id = 1;
        get_system_total_traffic(&traffic, &rx_speed, &tx_speed);
        //unit default use KB/S
        if(tx_speed > 1024)
        {
            request->speed_data.speed_info.unit += 1;//Upto MB/S
            tx_speed /= 1024;
        }
        if(tx_speed > 1024)
        {
            request->speed_data.speed_info.unit += 1;//Upto GB/S
            tx_speed /= 1024;
        }
        if(rx_speed > 1024)
        {
            request->speed_data.speed_info.unit += 4;//Upto GB
            rx_speed /= 1024;
        }
        if(rx_speed > 1024)
        {
            request->speed_data.speed_info.unit += 4;//Upto GB
            rx_speed /= 1024;
        }
        request->speed_data.speed_info.uploadspeed = tx_speed;
        request->speed_data.speed_info.downloadspeed = rx_speed;
        return offsetof(Request, speed_data.crc) + 1;
    case WlanTotal_AIM:
        request->length += sizeof(request->flow_data);
        request->flow_data.id = 1;
        
        request->flow_data.totalflow = (traffic.total_rx / 1024)  + (traffic.total_tx / 1024);
        if(request->flow_data.totalflow > 1024)
        {
            request->flow_data.unit = 2;
            request->flow_data.totalflow /= 1024;
        }
        else
            request->flow_data.unit = 1;
        return offsetof(Request, flow_data.crc) + 1;
    case WlanIP_AIM:
        request->length += sizeof(request->wlanip_data);
        request->wlanip_data.id = id;
        request->wlanip_data.ip[0] = wlaninterfaces[id].ip_address[0];
        request->wlanip_data.ip[1] = wlaninterfaces[id].ip_address[1];
        request->wlanip_data.ip[2] = wlaninterfaces[id].ip_address[2];
        request->wlanip_data.ip[3] = wlaninterfaces[id].ip_address[3];
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
            request->SystemPage_data.systemPage[0].temp = get_cpu_temperature();
            // 获取 I/O 权限
            acquire_io_permissions();
            unsigned char CPU_fan = 0;
            ec_ram_read_byte(0x70,&CPU_fan);
            request->SystemPage_data.systemPage[0].rpm = CPU_fan;
            request->SystemPage_data.systemPage[0].name[0] = 'C';
            request->SystemPage_data.systemPage[0].name[1] = 'P';
            request->SystemPage_data.systemPage[0].name[2] = 'U';
            //CPU OK
            request->SystemPage_data.systemPage[1].syslength = sizeof(request->SystemPage_data.systemPage[1]);
            request->SystemPage_data.systemPage[1].sys_id = 1;
            //Todo
            request->SystemPage_data.systemPage[1].usage = get_igpu_usage();;
            request->SystemPage_data.systemPage[1].temp = get_igpu_temperature();
            request->SystemPage_data.systemPage[1].rpm = CPU_fan;
            request->SystemPage_data.systemPage[1].name[0] = 'i';
            request->SystemPage_data.systemPage[1].name[1] = 'G';
            request->SystemPage_data.systemPage[1].name[2] = 'P';
            request->SystemPage_data.systemPage[1].name[3] = 'U';
        }
        else if(order == 2)
        {
            request->SystemPage_data.systemPage[0].syslength = sizeof(request->SystemPage_data.systemPage[0]);
            request->SystemPage_data.systemPage[0].sys_id = 3;
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
            request->DiskPage_data.diskStruct[0].unit = 3;
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
            request->DiskPage_data.diskStruct[0].unit = 3;
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
            request->DiskPage_data.diskStruct[1].unit = 3;
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
        request->ModePage_data.properties = 1;
        request->ModePage_data.balance = 1;
        return offsetof(Request, ModePage_data.crc) + 1;
    case WlanPage_AIM:
        request->length += sizeof(request->WlanPage_data);
        request->WlanPage_data.order = order;
        request->WlanPage_data.total = total;
        request->WlanPage_data.netcount = total;
        request->WlanPage_data.count = 1;
        request->WlanPage_data.online = GetUserCount();
        request->WlanPage_data.length = sizeof(request->WlanPage_data.wlanPage);
        request->WlanPage_data.wlanPage.id = order;
        request->WlanPage_data.wlanPage.unit = 3;
        get_system_total_traffic(&traffic, &rx_speed, &tx_speed);
        if(wlaninterfaces[order - 1].operstate[1] == 'p')//Up
        {
            request->WlanPage_data.wlanPage.uploadspeed = tx_speed;
            request->WlanPage_data.wlanPage.downloadspeed = rx_speed;
        }
        else
        {
            request->WlanPage_data.wlanPage.uploadspeed = 0;
            request->WlanPage_data.wlanPage.downloadspeed = 0;
        }
        if (strcmp(wlaninterfaces[order - 1].ip_address, "未分配") != 0 && 
        strcmp(wlaninterfaces[order - 1].ip_address, "0.0.0.0") != 0) {
        
        unsigned int a, b, c, d;
        if (sscanf(wlaninterfaces[order - 1].ip_address, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            printf("IP字节分解: %d.%d.%d.%d\n", a, b, c, d);
            printf("IP十六进制: 0x%02X.0x%02X.0x%02X.0x%02X\n", a, b, c, d);
            request->WlanPage_data.wlanPage.ip[0] = a;
            request->WlanPage_data.wlanPage.ip[1] = b;
            request->WlanPage_data.wlanPage.ip[2] = c;
            request->WlanPage_data.wlanPage.ip[3] = d;
            }
        }
        else
        {
            request->WlanPage_data.wlanPage.ip[0] = 0;
            request->WlanPage_data.wlanPage.ip[1] = 0;
            request->WlanPage_data.wlanPage.ip[2] = 0;
            request->WlanPage_data.wlanPage.ip[3] = 0;
        }
        //request->WlanPage_data.wlanPage.totalflow = traffic.total_rx / 1024 / 1024 + traffic.total_tx / 1024 /1024;

        for (int i = 0; i < sizeof(wlaninterfaces[order].name); i++)
        {
            request->WlanPage_data.wlanPage.name[i] = wlaninterfaces[order - 1].name[i];
        }
        return offsetof(Request, WlanPage_data.crc) + 1;
    case InfoPage_AIM:
        request->length += sizeof(request->InfoPage_data);
        request->InfoPage_data.order = order;
        request->InfoPage_data.total = total;
        request->InfoPage_data.nouse = 0;
        request->InfoPage_data.count = 0;
        int i = 0;
        request->InfoPage_data.devnamelength = sizeof(request->InfoPage_data.devname);
        for (i = 0; i < sizeof(request->InfoPage_data.devname); i++)
        {
            request->InfoPage_data.devname[i] = sys_info.devicename[i];
        }
        request->InfoPage_data.cpunamelength = sizeof(request->InfoPage_data.cpuname);
        for (i = 0; i < sizeof(request->InfoPage_data.cpuname); i++)
        {
            request->InfoPage_data.cpuname[i] = sys_info.cpuname[i];
        }
        request->InfoPage_data.operatelength = sizeof(request->InfoPage_data.operate);
        for (i = 0; i < sizeof(request->InfoPage_data.operate); i++)
        {
            request->InfoPage_data.operate[i] = sys_info.operatename[i];
        }
        request->InfoPage_data.snlength = sizeof(request->InfoPage_data.sn);
        for (i = 0; i < sizeof(request->InfoPage_data.sn); i++)
        {
            request->InfoPage_data.sn[i] = sys_info.serial_number[i];
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
            request->user_data.crc = cal_crc((unsigned char *)request, len);
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

int get_cpu_temperature() {
    FILE *thermal_file;
    int temperature = 0;
    char path[256] = {0};
    
    // 尝试不同的thermal zone
    for (int i = 0; i < 10; i++) {
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", i);
        thermal_file = fopen(path, "r");
        if (thermal_file != NULL) {
            int temp;
            if (fscanf(thermal_file, "%d", &temp) == 1) {
                temperature = temp / 1000.0;
                fclose(thermal_file);
                return temperature;
            }
            fclose(thermal_file);
        }
    }
    
    // 如果找不到温度文件，返回错误值
    return -1;
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
    setutent();
    //Clear count
    int user_count = 0;
    // 遍历所有记录
    while ((ut = getutent()) != NULL) {
        // USER_PROCESS 表示活跃的用户登录会话
        if (ut->ut_type == USER_PROCESS) {
            user_count++;
        }
    }
    // 关闭文件
    endutent();
    return user_count;
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
    
    while ((entry = readdir(block_dir)) != NULL && disk_count < max_disks) {
        // 过滤掉虚拟设备和分区
        if (strncmp(entry->d_name, "loop", 4) == 0 ||
            strncmp(entry->d_name, "ram", 3) == 0 ||
            strncmp(entry->d_name, "fd", 2) == 0 ||
            strchr(entry->d_name, 'p') != NULL) { // 排除分区
            continue;
        }
        
        // 只关注SATA和NVMe设备
        if (strncmp(entry->d_name, "sd", 2) == 0 || 
            strncmp(entry->d_name, "nvme", 4) == 0) {
            
            strncpy(disks[disk_count].device, entry->d_name, 32);
            
            // 确定硬盘类型
            if (strncmp(entry->d_name, "nvme", 4) == 0) {
                strcpy(disks[disk_count].type, "NVMe");
            } else {
                strcpy(disks[disk_count].type, "SATA");
            }
            
            // 获取基本信息
            get_disk_identity(entry->d_name, 
                            disks[disk_count].model, 
                            disks[disk_count].serial);
            
            disks[disk_count].total_size = get_disk_size(entry->d_name)/1024/1024/1024;//Change to Gb
            get_mountpoint(entry->d_name, disks[disk_count].mountpoint);
            // 如果有挂载点，获取使用情况
            if (strlen(disks[disk_count].mountpoint) > 0) {
                
                unsigned long long total, free, used;
                if (get_mountpoint_usage(disks[disk_count].mountpoint, &total, &free, &used) == 0) {
                    disks[disk_count].free_size = free/1024/1024/1024;//Change to Gb
                    disks[disk_count].used_size = used/1024/1024/1024;//Change to Gb
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
    printf("EC 62/66端口权限获取成功\n");
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
            // printf("HID Received %d bytes: ", res);
            // for (int i = 0; i < res; i++) {
            //     printf("%02x ", read_buf[i]);
            // }
            // printf("\n");
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
                     read_buf[2] == 0x00 && read_buf[3] == 0x06 &&
                     read_buf[4] == 0x02 && read_buf[5] == 0x81 &&
                     read_buf[6] == 0x3C && read_buf[7] == 0x00 &&
                    read_buf[8] == 0xC4) {
                printf(">>> Hibernate 60S command received!\n");
                systemoperation(HIBERNATEATONCE_AIM,60);
                //system("shutdown -h now");
            }
            else if (res >= 6 && 
                     read_buf[0] == 0xa5 && read_buf[1] == 0x5a && 
                     read_buf[2] == 0xff && read_buf[3] == 0x04 &&
                     read_buf[4] == 0x03 && read_buf[5] == 0x82 &&
                     read_buf[6] == 0x87) {
                printf(">>> Hibernate command received!\n");
                systemoperation(HIBERNATEATONCE_AIM,0);
                //system("shutdown -h now");
            }
            else {
                //printf(">>> Other command/data\n");
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

// 线程安全的写入函数
int safe_hid_write(hid_device *handle, const unsigned char *data, int length) {
    pthread_mutex_lock(&hid_mutex);
    int result = hid_write(handle, data, length);
    pthread_mutex_unlock(&hid_mutex);
    return result;
}

void systemoperation(unsigned char time,unsigned char cmd)
{
    char *command = NULL;
    for (unsigned char i = 0; i < time; i++)
    {
        TimeSleep1Sec();
    }
    switch (cmd)
    {
    case HIBERNATEATONCE_AIM:
        command = "systemctl hibernate";
        break;
    
    default:
        break;
    }
    system(command);
}
// 检查是否是物理接口
int is_physical_interface(const char *ifname) {
    char path[256];
    
    // 检查是否存在device目录（物理接口的特征）
    snprintf(path, sizeof(path), "/sys/class/net/%s/device", ifname);
    if (access(path, F_OK) == 0) {
        return 1;
    }
    
    // 排除常见的虚拟接口
    if (strncmp(ifname, "lo", 2) == 0 ||      // 回环接口
        strncmp(ifname, "virbr", 5) == 0 ||   // 虚拟网桥
        strncmp(ifname, "vnet", 4) == 0 ||    // 虚拟网络
        strncmp(ifname, "docker", 6) == 0 ||  // Docker接口
        strncmp(ifname, "br-", 3) == 0 ||     // 网桥
        strncmp(ifname, "veth", 4) == 0) {    // 虚拟以太网
        return 0;
    }
    
    return 1;
}

// 获取接口基本状态信息
void get_interface_basic_info(const char *ifname, network_interface_t *iface) {
    char path[256];
    FILE *file;
    char line[256];
    
    strncpy(iface->name, ifname, IFNAMSIZ - 1);
    iface->is_physical = is_physical_interface(ifname);
    
    // 获取操作状态
    snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", ifname);
    file = fopen(path, "r");
    if (file) {
        if (fgets(line, sizeof(line), file)) {
            line[strcspn(line, "\n")] = 0;
            strncpy(iface->operstate, line, sizeof(iface->operstate) - 1);
        }
        fclose(file);
    } else {
        strcpy(iface->operstate, "unknown");
    }
    
    // 获取MTU
    snprintf(path, sizeof(path), "/sys/class/net/%s/mtu", ifname);
    file = fopen(path, "r");
    if (file) {
        if (fgets(line, sizeof(line), file)) {
            iface->mtu = atoi(line);
        }
        fclose(file);
    }
    
    // 获取MAC地址
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", ifname);
    file = fopen(path, "r");
    if (file) {
        if (fgets(line, sizeof(line), file)) {
            line[strcspn(line, "\n")] = 0;
            strncpy(iface->mac_address, line, sizeof(iface->mac_address) - 1);
        }
        fclose(file);
    }
}

// 获取IP地址信息
void get_interface_ip_info(const char *ifname, network_interface_t *iface) {
    int sockfd;
    struct ifreq ifr;
    struct sockaddr_in *addr;
    
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return;
    }
    
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    
    // 获取IP地址
    if (ioctl(sockfd, SIOCGIFADDR, &ifr) == 0) {
        addr = (struct sockaddr_in *)&ifr.ifr_addr;
        const char *ip_str = inet_ntoa(addr->sin_addr);
        if (ip_str && strcmp(ip_str, "0.0.0.0") != 0) {
            strncpy(iface->ip_address, ip_str, sizeof(iface->ip_address) - 1);
        } else {
            strcpy(iface->ip_address, "0.0.0.0");
        }
    } else {
        strcpy(iface->ip_address, "0.0.0.0");
    }
    
    // 获取子网掩码
    if (ioctl(sockfd, SIOCGIFNETMASK, &ifr) == 0) {
        addr = (struct sockaddr_in *)&ifr.ifr_netmask;
        const char *netmask_str = inet_ntoa(addr->sin_addr);
        if (netmask_str && strcmp(netmask_str, "0.0.0.0") != 0) {
            strncpy(iface->netmask, netmask_str, sizeof(iface->netmask) - 1);
        } else {
            strcpy(iface->netmask, "未分配");
        }
    } else {
        strcpy(iface->netmask, "未分配");
    }
    
    close(sockfd);
}

// 获取接口速度信息
void get_interface_speed_info(const char *ifname, network_interface_t *iface) {
    char path[256];
    FILE *file;
    char line[256];
    int sockfd;
    struct ifreq ifr;
    struct ethtool_cmd edata;
    
    // 初始化默认值
    iface->max_speed = -1;
    iface->current_speed = -1;
    strcpy(iface->duplex, "未知");
    
    // 从sysfs获取最大速度
    snprintf(path, sizeof(path), "/sys/class/net/%s/speed", ifname);
    file = fopen(path, "r");
    if (file) {
        if (fgets(line, sizeof(line), file)) {
            iface->max_speed = atoi(line);
        }
        fclose(file);
    }
    
    // 使用ethtool获取当前速度和双工模式
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd >= 0) {
        strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
        edata.cmd = ETHTOOL_GSET;
        ifr.ifr_data = (caddr_t)&edata;
        
        if (ioctl(sockfd, SIOCETHTOOL, &ifr) == 0) {
            int speed = ethtool_cmd_speed(&edata);
            if (speed != 0 && speed != 65535) {
                iface->current_speed = speed;
            }
            
            if (edata.duplex == DUPLEX_FULL) {
                strcpy(iface->duplex, "全双工");
            } else if (edata.duplex == DUPLEX_HALF) {
                strcpy(iface->duplex, "半双工");
            }
        }
        close(sockfd);
    }
    
    // 如果无法获取当前速度，使用最大速度作为当前速度
    if (iface->current_speed == -1 && iface->max_speed > 0) {
        iface->current_speed = iface->max_speed;
    }
}

// 获取网络统计信息
void get_interface_stats(const char *ifname, network_interface_t *iface) {
    FILE *file;
    char line[512];
    char dev_name[32];
    
    file = fopen("/proc/net/dev", "r");
    if (!file) {
        return;
    }
    
    // 跳过前两行标题
    fgets(line, sizeof(line), file);
    fgets(line, sizeof(line), file);
    
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, " %[^:]: %llu %*u %*u %*u %*u %*u %*u %*u %llu",
                   dev_name, &iface->rx_bytes, &iface->tx_bytes) == 3) {
            if (strcmp(dev_name, ifname) == 0) {
                break;
            }
        }
    }
    
    fclose(file);
}


// 扫描所有网络接口
int scan_network_interfaces() {
    DIR *dir;
    struct dirent *entry;
    
    dir = opendir("/sys/class/net");
    if (!dir) {
        perror("无法访问 /sys/class/net");
        return -1;
    }
    
    interface_count = 0;
    
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' || interface_count >= MAX_INTERFACES) {
            continue;
        }
        
        // 只处理物理接口
        if (!is_physical_interface(entry->d_name)) {
            continue;
        }
        
        network_interface_t *iface = &wlaninterfaces[interface_count];
        
        // 获取各种信息
        get_interface_basic_info(entry->d_name, iface);
        get_interface_ip_info(entry->d_name, iface);
        get_interface_speed_info(entry->d_name, iface);
        get_interface_stats(entry->d_name, iface);
        
        interface_count++;
    }
    
    closedir(dir);
    return interface_count;
}

// 打印接口信息
void print_interface_info(const network_interface_t *iface) {
    printf("接口名称: %s\n", iface->name);
    printf("  物理接口: %s\n", iface->is_physical ? "是" : "否");
    printf("  操作状态: %s\n", iface->operstate);
    printf("  IP地址: %s\n", iface->ip_address);
    printf("  子网掩码: %s\n", iface->netmask);
    printf("  MAC地址: %s\n", iface->mac_address);
    printf("  最大速度: %d Mbps\n", iface->max_speed);
    printf("  当前速度: %d Mbps\n", iface->current_speed);
    printf("  双工模式: %s\n", iface->duplex);
    printf("  MTU: %d\n", iface->mtu);
    printf("  接收字节: %llu\n", iface->rx_bytes);
    printf("  发送字节: %llu\n", iface->tx_bytes);
    printf("  %s\n", "─");
}

// 打印所有接口信息
void print_all_interfaces() {
    printf("=== 物理网络接口信息 ===\n");
    printf("发现 %d 个物理接口:\n\n", interface_count);
    
    for (int i = 0; i < interface_count; i++) {
        printf("%d. ", i + 1);
        print_interface_info(&wlaninterfaces[i]);
        printf("\n");
    }
}

// 按名称查找接口
network_interface_t* find_interface_by_name(const char *name) {
    for (int i = 0; i < interface_count; i++) {
        if (strcmp(wlaninterfaces[i].name, name) == 0) {
            return &wlaninterfaces[i];
        }
    }
    return NULL;
}

// 获取特定接口的详细信息
void get_specific_interface_info(const char *ifname) {
    network_interface_t iface;
    
    if (!is_physical_interface(ifname)) {
        printf("%s 不是物理接口或不存在\n", ifname);
        return;
    }
    
    get_interface_basic_info(ifname, &iface);
    get_interface_ip_info(ifname, &iface);
    get_interface_speed_info(ifname, &iface);
    get_interface_stats(ifname, &iface);
    
    printf("=== %s 详细信息 ===\n", ifname);
    print_interface_info(&iface);
}
// 流量获取


void init_traffic(system_traffic_t *t) {
    memset(t, 0, sizeof(system_traffic_t));
    t->first_call = 1;
}

// 获取系统所有物理接口的总流量
int get_system_total_traffic(system_traffic_t *t, 
                            double *rx_speed, double *tx_speed) {
    FILE *fp = fopen("/proc/net/dev", "r");
    if (!fp) return -1;
    
    char line[256];
    char ifname[32];
    unsigned long long rx, tx;
    unsigned long long current_rx = 0, current_tx = 0;
    
    // 跳过标题行
    fgets(line, sizeof(line), fp);
    fgets(line, sizeof(line), fp);
    
    // 汇总所有物理接口的流量（排除虚拟接口）
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, " %[^:]: %llu %*u %*u %*u %*u %*u %*u %*u %llu", 
                   ifname, &rx, &tx) == 3) {
            // 排除虚拟接口
            if (strncmp(ifname, "lo", 2) != 0 &&
                strncmp(ifname, "virbr", 5) != 0 &&
                strncmp(ifname, "docker", 6) != 0 &&
                strncmp(ifname, "br-", 3) != 0 &&
                strncmp(ifname, "veth", 4) != 0 &&
                strncmp(ifname, "tun", 3) != 0) {
                current_rx += rx;
                current_tx += tx;
            }
        }
    }
    
    fclose(fp);
    
    time_t now = time(NULL);
    t->total_rx = (current_rx / 1024);//KB
    t->total_tx = (current_tx / 1024);//KB
    
    // 计算速度
    if (!t->first_call) {
        double secs = difftime(now, t->last_time);
        if (secs > 0) {
            *rx_speed = (double)(current_rx - t->last_rx) / (1024 * secs);//KB
            *tx_speed = (double)(current_tx - t->last_tx) / (1024 * secs);//KB
        }
    } else {
        *rx_speed = *tx_speed = 0;
        t->first_call = 0;
    }
    
    t->last_rx = current_rx;
    t->last_tx = current_tx;
    t->last_time = now;
    
    return 0;
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
