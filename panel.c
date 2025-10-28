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

#define APIC_ADDRESS 0xFEC00000
#define APIC_IRQ 0x09
#define DebugToken   false

#define MAXLEN 0x40
#define PRODUCTID 0x0002
#define VENDORID 0x5448
#define DURATION 1
#define MAX_PATH 256
#define MAX_LINE 512
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
    char device[32];        // 设备名 (sda, sdb, nvme0n1等)
    char mountpoint[256];   // 主要挂载点
    unsigned long long total_size;  // 总容量 (bytes)
    unsigned long long free_size;   // 可用容量 (bytes)
    unsigned long long used_size;   // 已用容量 (bytes)
    double usage_percent;   // 使用百分比
    int temperature;  // 温度信息
} DiskInfo;

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

static unsigned char COMMLEN = offsetof(Request, common_data.data) - offsetof(Request, length);

int init_hidreport(Request *request, unsigned char cmd, unsigned char aim);
void append_crc(Request *request);
unsigned char cal_crc(unsigned char * data, int len);
int get_cpu_temperature();
void read_cpu_data(CPUData *data);
float calculate_cpu_usage(const CPUData *prev, const CPUData *curr);
unsigned int get_memory_usage();
void parse_request(Request *request);
void parse_ack(Ack *ack, unsigned char aim);

int get_physical_disks(DiskInfo **disks);
long long get_disk_size(const char *device_name);
void get_disk_mountpoint(const char *device_name, char *mountpoint);
int get_mountpoint_usage(const char *mountpoint, unsigned long long *total, 
                        unsigned long long *free, unsigned long long *used);
int get_all_disk_info(DiskInfo **disks);
void print_modify_disk_size(unsigned long long bytes);
int file_exists(const char *filename);
int read_file(const char *filename, char *buffer, size_t buffer_size);
int get_disk_temperature_sysfs(const char *device);
void get_sata_disk_info(const char *device, char *model, char *serial, size_t size);
int list_sata_devices(char devices[][32], int max_devices);
int get_disk_temperature(const char *device);
int get_sata_disk_temperature(const char *device);
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
//Memory
volatile uint32_t* map_apic_memory();
void unmap_apic_memory(volatile uint32_t* addr);
uint32_t RMem32(volatile uint32_t* base, uint32_t offset);
void WMem32(volatile uint32_t* base, uint32_t offset, uint32_t value);
int DisableSci();
int EnableSci();
volatile uint32_t* map_physical_memory(uint64_t phys_addr, size_t size);
void unmap_physical_memory(volatile uint32_t* addr, size_t size);
void nvidia_print_info();
//异步HID
void signal_handler(int sig);
void* hid_read_thread(void *arg);
int safe_hid_write(hid_device *handle, const unsigned char *data, int length);

CPUData prev_data, curr_data;
bool IsNvidiaGPU;
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
int main(void) {
    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int res = hid_init();
    hid_device *handle = hid_open(VENDORID, PRODUCTID, NULL);
    
    if (handle == NULL) {
        res = hid_exit();
        return 0;
    }
    #if DebugToken
    printf("HID device opened successfully\n");
    #endif
    // 创建读取线程
    if (pthread_create(&read_thread, NULL, hid_read_thread, handle) != 0) {
        printf("Failed to create read thread\n");
        hid_close(handle);
        hid_exit();
        return -1;
    }
    // 初始化数据
    int cputemp,cpusuage,cpufan,memoryusage;
    IsNvidiaGPU = nvidia_smi_available();
    unsigned char hid_report[MAXLEN] = {0};
    unsigned char ack[MAXLEN] = {0};
    cputemp = get_cpu_temperature();
    DiskInfo *disks = NULL;
    int disk_count;
    unsigned long long disk_total_capacity = 0;
    unsigned long long disk_total_free = 0;
    unsigned long long disk_total_used = 0;
    
    #if DebugToken
    printf("CPUTemp:%d\n",cputemp);
    unsigned char ECcputemp = 0;
    // 获取 I/O 权限
    acquire_io_permissions();
    ec_ram_read_byte(0x70,&ECcputemp);
    printf("EC响应CPU Temp: 0x%02X\n", ECcputemp);
    ec_ram_write_byte(0x40,0xFF);

    // 释放 I/O 权限
    release_io_permissions();
    #endif
    #if DebugToken
    printf("=== SATA硬盘温度监测 ===\n\n");
    
        char devices[10][32];
    int device_count = list_sata_devices(devices, 10);
    //Remove if SATA return
    // if (device_count == 0) {
    //     printf("未找到SATA硬盘设备\n");
    //     return 1;
    // }
    
    printf("找到 %d 个SATA设备:\n", device_count);
    
    for (int i = 0; i < device_count; i++) {
        char model[128], serial[64];
        get_sata_disk_info(devices[i], model, serial, sizeof(model));
        
        printf("\n设备: /dev/%s\n", devices[i]);
        printf("型号: %s\n", model);
        printf("序列号: %s\n", serial);
        
        int temperature = get_sata_disk_temperature(devices[i]);
        if (temperature != -1) {
            printf("温度: %d°C\n", temperature);
            
            if (temperature < 40) {
                printf("状态: 正常\n");
            } else if (temperature < 50) {
                printf("状态: 温热\n");
            } else if (temperature < 60) {
                printf("状态: 较热\n");
            } else {
                printf("状态: 过热警告！\n");
            }
        } else {
            printf("温度: 无法获取\n");
        }
        
        printf("---\n");
    }
    #endif    
    while (true) {
        Request* request = (Request *)hid_report;
        // Time
        int timereportsize = init_hidreport(request, SET, TIME_AIM);
        append_crc(request);
        if (safe_hid_write(handle, hid_report, timereportsize) == -1) {
            printf("Failed to write TIME data\n");
            break;
        }
        memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
        #if DebugToken
        printf("TimeSendOK\n");
        #endif
        sleep(1);
        //*****************************************************/
        // CPU
        int cpureportsize = init_hidreport(request, SET, CPU_AIM);
        append_crc(request);
        if (safe_hid_write(handle, hid_report, cpureportsize) == -1) {
            printf("Failed to write CPU data\n");
            break;
        }
        memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
        #if DebugToken
        printf("CPUSendOK\n");
        #endif
        sleep(1);
        //*****************************************************/
        // Memory Usage
        int memusagesize = init_hidreport(request, SET, MEMORY_AIM);
        append_crc(request);
        if (safe_hid_write(handle, hid_report, memusagesize) == -1) {
            printf("Failed to write MEMORY data\n");
            break;
        }
        memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
        sleep(1);
        //*****************************************************/
        //User Online
        // User Online
        int usersize = init_hidreport(request, SET, USER_AIM);
        append_crc(request);
        if (safe_hid_write(handle, hid_report, usersize) == -1) {
            printf("Failed to write USER data\n");
            break;
        }
        memset(hid_report, 0x0, sizeof(unsigned char) * MAXLEN);
        #if DebugToken
        printf("UserSendOK\n");
        #endif
        sleep(1);
        if(IsNvidiaGPU)
        {
            int dgpusize = init_hidreport(request, SET, GPU_AIM);
            append_crc(request);
            if (hid_write(handle, hid_report, dgpusize) == -1) {
                break;
            }
        }
        sleep(1);
        //*****************************************************/
        //Get Error
        int result = hid_read_timeout(handle, ack, 0x40, -1);
        if (result == -1) {
            break;
        }

        if (result > 0) {
            parse_ack((Ack *)ack, request->aim);
        }
        if (hid_read(handle, hid_report, 0x40) == -1) {
            break;
        }
        #if DebugToken
        printf("ReadData:0x%02x\n",hid_report[1]);
        #endif
        memset(hid_report, 0x0, sizeof(unsigned char) * 0x40);
        memset(ack, 0x0, sizeof(unsigned char) * 0x40);
        //*****************************************************/
        //dynamic read diskcount

        disk_count = get_all_disk_info(&disks);
        if (disk_count <= 0) {
            printf("未找到物理磁盘设备\n");
            return 1;
        }
        for (int i = 0; i < disk_count; i++) {
            #if DebugToken
            printf("磁盘 %d: %s\n", i + 1, disks[i].device);
            printf("总容量: ");
            print_modify_disk_size(disks[i].total_size);
            printf("\n");
            
            if (strlen(disks[i].mountpoint) > 0) {
                printf("可用容量: ");
                print_modify_disk_size(disks[i].free_size);
                printf("\n");
                printf("已用容量: ");
                print_modify_disk_size(disks[i].used_size);
                printf("\n");
                printf("使用率: %.1f%%", disks[i].usage_percent);
                printf("\n");
                printf("Temp: %d\n", disks[i].temperature);
            } else {
                printf("状态: 未挂载\n");
            }
            #endif
        }
        //*****************************************************/
        sleep(DURATION);
    }
    // 释放内存
    free(disks);
    hid_close(handle);
    res = hid_exit();

    return 0;
}

int init_hidreport(Request* request, unsigned char cmd, unsigned char aim) {
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
    case GPU_AIM:
        request->length += sizeof(request->gpu_data);
        // Code
        unsigned char DGPUtemp = 0;
        if(IsNvidiaGPU)
        {
            DGPUtemp = nvidia_get_gpu_temperature();
            request->gpu_data.gpu_info.temperature = DGPUtemp;
            request->gpu_data.gpu_info.usage = nvidia_get_gpu_utilization();
            request->gpu_data.gpu_info.rpm = nvidia_get_gpu_fan_speed();
            // 获取 I/O 权限
            acquire_io_permissions();
            ec_ram_write_byte(0xB0,DGPUtemp);
            // 释放 I/O 权限
            release_io_permissions();
        }
        return offsetof(Request, gpu_data.crc) + 1;
    case CPU_AIM:
        
        request->length += sizeof(request->cpu_data);
        // Code
        request->cpu_data.cpu_info.temerature = get_cpu_temperature();
        // 读取当前CPU数据
        read_cpu_data(&curr_data);
        // 计算CPU使用率
        //printf("CPU usage: %2f\n", calculate_cpu_usage(&prev_data, &curr_data));
        request->cpu_data.cpu_info.usage = calculate_cpu_usage(&prev_data, &curr_data);
        // 更新前一次的数据
        prev_data = curr_data;
        // 获取 I/O 权限
        acquire_io_permissions();
        unsigned char CPU_fan = 0;
        ec_ram_read_byte(0x70,&CPU_fan);
        request->cpu_data.cpu_info.rpm = CPU_fan;
        // 释放 I/O 权限
        release_io_permissions();
        return offsetof(Request, cpu_data.crc) + 1;
    case USER_AIM:
        request->length += sizeof(request->user_data);
        // Code
        // 打开 utmp 文件
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
        //printf("User Online Count:%d\n",user_count);
        request->user_data.user_info.online = user_count;
        return offsetof(Request, user_data.crc) + 1;
    case MEMORY_AIM:
        request->length += sizeof(request->memory_data);
        // Code
        request->memory_data.memory_info.usage = get_memory_usage();
        return offsetof(Request, memory_data.crc) + 1;
    
    
    default:
        request->length += sizeof(request->common_data);
        return offsetof(Request, common_data.crc) + 1;
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

        case GPU_AIM:

            len = offsetof(Request, gpu_data.crc);
            request->gpu_data.crc = cal_crc((unsigned char *)request, len);
            return;

        case CPU_AIM:

            len = offsetof(Request, cpu_data.crc);
            request->cpu_data.crc = cal_crc((unsigned char *)request, len);
            return;

        case USER_AIM:
            len = offsetof(Request, user_data.crc);
            request->user_data.crc = cal_crc((unsigned char *)request, len);
            return;
        case MEMORY_AIM:
            len = offsetof(Request, memory_data.crc);
            request->memory_data.crc = cal_crc((unsigned char *)request, len);
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
    }

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

void parse_request(Request *request) {
    for (int off = 0; off < 0x40; off++) {
        printf("%d ", *(((unsigned char *)request) + off));
    }
    printf("\n");
    printf("length: %u\n", request->length);
}

void parse_ack(Ack *ack, unsigned char aim) {
    int len = 0;
    switch (aim)
    {
        case TIME_AIM:

            len = offsetof(Ack, time_data.crc);
            break;

        case USER_AIM:
            len = offsetof(Ack, user_data.crc);
            break;
        case MEMORY_AIM:
            len = offsetof(Ack, about_data.crc);
            break;

        default:
            len = offsetof(Ack, common_data.crc);
            break;
    }

    for (int off = 0; off < len; off++) {
        printf("%u ", *(((unsigned char *)ack) + off));
    }
    printf("\n");

    printf("[debug] ack->header: %x\n", ack->header);
    printf("[debug] ack->sequence: %x\n", ack->sequence);
    printf("[debug] ack->length: %x\n", ack->length);
    printf("[debug] ack->cmd: %x\n", ack->cmd);
    printf("[debug] ack->err: %x\n", ack->err);
}
//Disk
// 获取物理磁盘列表（去重）
int get_physical_disks(DiskInfo **disks) {
    DIR *dir;
    struct dirent *entry;
    int count = 0;
    int capacity = 10;
    
    *disks = malloc(capacity * sizeof(DiskInfo));
    if (*disks == NULL) {
        perror("malloc failed");
        return -1;
    }
    
    // 打开 /sys/block 目录获取物理磁盘
    dir = opendir("/sys/block");
    if (dir == NULL) {
        perror("opendir /sys/block failed");
        free(*disks);
        return -1;
    }
    
    while ((entry = readdir(dir)) != NULL) {
        // 只处理物理磁盘设备
        if (strncmp(entry->d_name, "sd", 2) == 0 ||
            strncmp(entry->d_name, "hd", 2) == 0 ||
            strncmp(entry->d_name, "nvme", 4) == 0 ||
            strncmp(entry->d_name, "vd", 2) == 0) {  // 虚拟磁盘
            
            if (count >= capacity) {
                capacity *= 2;
                DiskInfo *temp = realloc(*disks, capacity * sizeof(DiskInfo));
                if (temp == NULL) {
                    perror("realloc failed");
                    closedir(dir);
                    return -1;
                }
                *disks = temp;
            }
            
            // 初始化磁盘信息
            strncpy((*disks)[count].device, entry->d_name, sizeof((*disks)[count].device) - 1);
            (*disks)[count].device[sizeof((*disks)[count].device) - 1] = '\0';
            (*disks)[count].mountpoint[0] = '\0';
            (*disks)[count].total_size = 0;
            (*disks)[count].free_size = 0;
            (*disks)[count].used_size = 0;
            (*disks)[count].usage_percent = 0.0;
            
            count++;
        }
    }
    
    closedir(dir);
    return count;
}

// 获取磁盘大小（从 /sys/block/）
long long get_disk_size(const char *device_name) {
    char path[256];
    FILE *file;
    char line[64];
    long long size = 0;
    
    snprintf(path, sizeof(path), "/sys/block/%s/size", device_name);
    file = fopen(path, "r");
    if (file) {
        if (fgets(line, sizeof(line), file)) {
            size = atoll(line) * 512;  // 转换为字节 (sector size = 512)
        }
        fclose(file);
    }
    return size;
}

// 获取磁盘的主要挂载点
void get_disk_mountpoint(const char *device_name, char *mountpoint) {
    FILE *mtab;
    struct mntent *entry;
    char full_device[64];
    
    // 构建完整的设备路径
    snprintf(full_device, sizeof(full_device), "/dev/%s", device_name);
    
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

// 获取挂载点的磁盘使用情况
int get_mountpoint_usage(const char *mountpoint, unsigned long long *total, 
                        unsigned long long *free, unsigned long long *used) {
    struct statvfs vfs;
    
    if (statvfs(mountpoint, &vfs) != 0) {
        return -1;
    }
    
    unsigned long long block_size = vfs.f_frsize;
    unsigned long long total_blocks = vfs.f_blocks;
    unsigned long long free_blocks = vfs.f_bfree;
    
    *total = total_blocks * block_size;
    *free = free_blocks * block_size;
    *used = *total - *free;
    
    return 0;
}

// 获取所有磁盘的完整信息
int get_all_disk_info(DiskInfo **disks) {
    int disk_count = get_physical_disks(disks);
    
    if (disk_count <= 0) {
        return disk_count;
    }
    
    // 为每个磁盘获取详细信息
    for (int i = 0; i < disk_count; i++) {
        // 获取磁盘大小
        (*disks)[i].total_size = get_disk_size((*disks)[i].device);
        
        // 获取挂载点
        get_disk_mountpoint((*disks)[i].device, (*disks)[i].mountpoint);
        
        // 如果有挂载点，获取使用情况
        if (strlen((*disks)[i].mountpoint) > 0) {
            unsigned long long total, free, used;
            if (get_mountpoint_usage((*disks)[i].mountpoint, &total, &free, &used) == 0) {
                (*disks)[i].free_size = free;
                (*disks)[i].used_size = used;
                if (total > 0) {
                    (*disks)[i].usage_percent = ((double)used / total) * 100.0;
                }
            }
        }
        // 新增：获取硬盘温度
        (*disks)[i].temperature = get_disk_temperature((*disks)[i].device);
    }
    
    return disk_count;
}
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
// 安全构建路径
void build_safe_path(char *dest, size_t dest_size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(dest, dest_size, fmt, args);
    va_end(args);
    
    // 确保字符串以null结尾
    dest[dest_size - 1] = '\0';
}
// 获取SATA硬盘温度 - 主要方法
int get_sata_disk_temperature(const char *device) {
    char hwmon_path[MAX_PATH];
    char temp_path[MAX_PATH];
    
    // 方法1: 通过hwmon获取温度
    build_safe_path(hwmon_path, sizeof(hwmon_path), "/sys/block/%s/device/hwmon", device);
    
    if (file_exists(hwmon_path)) {
        DIR *dir = opendir(hwmon_path);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (strncmp(entry->d_name, "hwmon", 5) == 0) {
                    // 安全构建路径
                    build_safe_path(temp_path, sizeof(temp_path), 
                                   "%s/%s/temp1_input", hwmon_path, entry->d_name);
                    
                    if (file_exists(temp_path)) {
                        char temp_value[32];
                        if (read_file(temp_path, temp_value, sizeof(temp_value)) == 0) {
                            int temp = atoi(temp_value);
                            closedir(dir);
                            
                            if (temp > 1000) {
                                return temp / 1000;
                            } else {
                                return temp;
                            }
                        }
                    }
                }
            }
            closedir(dir);
        }
    }
    
    // 方法2: 使用smartctl获取温度
    char cmd[MAX_PATH];
    char output[64];
    
    const char *smartctl_patterns[] = {
        "smartctl -A /dev/%s 2>/dev/null | grep -i 'Temperature_Celsius'",
        "smartctl -A /dev/%s 2>/dev/null | grep -i 'Current Drive Temperature'",
        "smartctl -A /dev/%s 2>/dev/null | grep -i 'Temperature' | head -1",
        NULL
    };
    
    for (int i = 0; smartctl_patterns[i] != NULL; i++) {
        build_safe_path(cmd, sizeof(cmd), smartctl_patterns[i], device);
        if (execute_command(cmd, output, sizeof(output)) == 0) {
            // 在输出中查找数字
            char *ptr = output;
            while (*ptr) {
                if (isdigit((unsigned char)*ptr)) {
                    int temp = atoi(ptr);
                    if (temp > 0 && temp < 100) {
                        return temp;
                    }
                }
                ptr++;
            }
        }
    }
    
    return -1;
}
// 获取SATA硬盘信息
void get_sata_disk_info(const char *device, char *model, char *serial, size_t size) {
    char path[MAX_PATH];
    
    // 获取型号
    snprintf(path, sizeof(path), "/sys/block/%s/device/model", device);
    if (read_file(path, model, size) != 0) {
        strncpy(model, "Unknown", size);
    }
    
    // 去除换行符
    model[strcspn(model, "\n")] = 0;
    
    // 获取序列号
    snprintf(path, sizeof(path), "/sys/block/%s/device/serial", device);
    if (read_file(path, serial, size) != 0) {
        strncpy(serial, "Unknown", size);
    }
    
    // 去除换行符
    serial[strcspn(serial, "\n")] = 0;
}

// 列出所有SATA设备
int list_sata_devices(char devices[][32], int max_devices) {
    int count = 0;
    DIR *block_dir = opendir("/sys/block");
    
    if (!block_dir) return 0;
    
    struct dirent *entry;
    while ((entry = readdir(block_dir)) != NULL && count < max_devices) {
        // 只关注sdX设备（SATA/SCSI）
        if (strncmp(entry->d_name, "sd", 2) == 0) {
            // 确保是物理设备而不是分区
            if (strlen(entry->d_name) == 3 || 
                (strlen(entry->d_name) == 4 && entry->d_name[3] >= 'a' && entry->d_name[3] <= 'z')) {
                
                strncpy(devices[count], entry->d_name, 32);
                
                printf("1111111111\n");
            }
            count++;
        }
    }
    
    closedir(block_dir);
    return count;
}
// 获取硬盘温度的通用函数
int get_disk_temperature(const char *device) {
    int temperature = -1;
    
    // 使用sysfs
    //temperature = get_disk_temperature_sysfs(device);
    if (temperature != -1) {
        return temperature;
    }
    return 0;
}
// Modify size
void print_modify_disk_size(unsigned long long bytes) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_index = 0;
    double size = bytes;
    
    while (size >= 1024 && unit_index < 4) {
        size /= 1024;
        unit_index++;
    }
    
    printf("%.1f %s", size, units[unit_index]);
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

volatile uint32_t* map_apic_memory() {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("打开/dev/mem失败");
        return NULL;
    }
    
    void* mapped = mmap(NULL, 4096, PROT_READ | PROT_WRITE, 
                       MAP_SHARED, fd, APIC_ADDRESS);
    close(fd);
    
    if (mapped == MAP_FAILED) {
        perror("内存映射失败");
        return NULL;
    }
    
    return (volatile uint32_t*)mapped;
}

void unmap_apic_memory(volatile uint32_t* addr) {
    if (addr) {
        munmap((void*)addr, 4096);
    }
}

// 读取32位内存
uint32_t RMem32(volatile uint32_t* base, uint32_t offset) {
    return *(base + (offset / sizeof(uint32_t)));
}

// 写入32位内存
void WMem32(volatile uint32_t* base, uint32_t offset, uint32_t value) {
    *(base + (offset / sizeof(uint32_t))) = value;
}

int DisableSci() {
    volatile uint32_t* apic_base = map_physical_memory(APIC_ADDRESS, 0x1000);
    if (!apic_base) {
        fprintf(stderr, "无法映射APIC内存\n");
        return 0;
    }
    
    uint32_t v = (2 * APIC_IRQ) + 0x10;
    WMem32(apic_base, v, 0);  // 写入偏移地址
    
    uint32_t sci = RMem32(apic_base, 0x10);
    printf("当前SCI值: 0x%08X\n", sci);
    
    sci |= 0x00010000;  // 设置屏蔽位
    WMem32(apic_base, 0x10, sci);
    
    printf("禁用SCI后的值: 0x%08X\n", RMem32(apic_base, 0x10));
    
    unmap_physical_memory(apic_base, 0x1000);
    return 1;
}

int EnableSci() {
    volatile uint32_t* apic_base = map_physical_memory(APIC_ADDRESS, 0x1000);
    if (!apic_base) {
        fprintf(stderr, "无法映射APIC内存\n");
        return 0;
    }
    
    uint32_t v = (2 * APIC_IRQ) + 0x10;
    WMem32(apic_base, v, 0);  // 写入偏移地址
    
    uint32_t sci = RMem32(apic_base, 0x10);
    printf("当前SCI值: 0x%08X\n", sci);
    
    sci &= 0xFFFEFFFF;  // 清除屏蔽位
    WMem32(apic_base, 0x10, sci);
    
    printf("启用SCI后的值: 0x%08X\n", RMem32(apic_base, 0x10));
    
    unmap_physical_memory(apic_base, 0x1000);
    return 1;
}
// 映射物理内存到用户空间
volatile uint32_t* map_physical_memory(uint64_t phys_addr, size_t size) {
    int mem_fd;
    void *mapped_addr;
    
    // 打开 /dev/mem
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        perror("无法打开 /dev/mem");
        return NULL;
    }
    
    // 映射物理内存
    mapped_addr = mmap(NULL, size, PROT_READ | PROT_WRITE, 
                      MAP_SHARED, mem_fd, phys_addr);
    close(mem_fd);
    
    if (mapped_addr == MAP_FAILED) {
        perror("内存映射失败");
        return NULL;
    }
    
    return (volatile uint32_t*)mapped_addr;
}

// 解除内存映射
void unmap_physical_memory(volatile uint32_t* addr, size_t size) {
    if (addr) {
        munmap((void*)addr, size);
    }
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
    
    printf("HID read thread started (blocking mode)\n");
    
    // 使用阻塞模式 - 这样会一直等待直到有数据到达
    hid_set_nonblocking(handle, 0);
    
    while (running) {
        int res = hid_read(handle, read_buf, sizeof(read_buf));
        
        if (res > 0) {
            printf("HID Received %d bytes: ", res);
            for (int i = 0; i < res; i++) {
                printf("%02x ", read_buf[i]);
            }
            printf("\n");
            
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
                     read_buf[2] == 0xff && read_buf[3] == 0x08 &&
                     read_buf[4] == 0x00 && read_buf[5] == 0x01) {
                printf(">>> SHUTDOWN command received!\n");
                system("shutdown -h now");
            }
            else {
                printf(">>> Other command/data\n");
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