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


#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>

#define APIC_ADDRESS 0xFEC00000
#define APIC_IRQ 0x09


#define MAXLEN 0x40
#define PRODUCTID 0x0002
#define VENDORID 0x5448
#define DURATION 1

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
} DiskInfo;

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
int ec_check_alive();
void print_ec_status();
void diagnose_ec_issue();
volatile uint32_t* map_apic_memory();
void unmap_apic_memory(volatile uint32_t* addr);
uint32_t RMem32(volatile uint32_t* base, uint32_t offset);
void WMem32(volatile uint32_t* base, uint32_t offset, uint32_t value);
int DisableSci();
int EnableSci();
volatile uint32_t* map_physical_memory(uint64_t phys_addr, size_t size);
void unmap_physical_memory(volatile uint32_t* addr, size_t size);
void scan_ec_ports();
int try_alternative_protocols(unsigned char address, unsigned char *value);
CPUData prev_data, curr_data;
struct utmp *ut;

int main(void) {
    int res = hid_init();
    hid_device *handle = hid_open(VENDORID, PRODUCTID, NULL);
    
    if (handle == NULL) {
        res = hid_exit();
        return 0;
    }
    //initial
    int cputemp,cpusuage,cpufan,memoryusage;
    unsigned char hid_report[MAXLEN] = {0};
    unsigned char ack[MAXLEN] = {0};
    cputemp = get_cpu_temperature();
    DiskInfo *disks = NULL;
    int disk_count;
    unsigned long long disk_total_capacity = 0;
    unsigned long long disk_total_free = 0;
    unsigned long long disk_total_used = 0;
    
    try_alternative_protocols(0xA0,hid_report);



        //Disable SCI
        volatile uint32_t* apic_base = map_apic_memory();
        if (!apic_base) {
            return 1;
        }
        uint32_t v = (2 * APIC_IRQ) + 0x10;
        uint32_t sci_reg = RMem32(apic_base, 0x10);
        
        //printf("APIC基地址: 0x%p\n", (void*)APIC_ADDRESS);
        //printf("IRQ: %d\n", APIC_IRQ);
        //printf("当前SCI寄存器值: 0x%08X\n", sci_reg);
        // printf("屏蔽位状态: %s\n", (sci_reg & 0x00010000) ? "已屏蔽" : "未屏蔽");
        // DisableSci();
        // sci_reg = RMem32(apic_base, 0x10);
        // printf("屏蔽位状态: %s\n", (sci_reg & 0x00010000) ? "已屏蔽" : "未屏蔽");
        scan_ec_ports();
        //EnableSci();
        //unmap_apic_memory(apic_base);
    while (true) {
        // Request* request = (Request *)hid_report;
        // //Time
        // int timereportsize = init_hidreport(request, SET, TIME_AIM);
        // append_crc(request);
        // if (hid_write(handle, hid_report, timereportsize) == -1) {
        //     break;
        // }
        // // if (hid_read(handle, hid_report, 0x40) == -1) {
        // //     break;
        // // }
        // memset(hid_report, 0x0, sizeof(unsigned char) * 0x40);
        // sleep(1);
        // //*****************************************************/
        // // //CPU
        // int cpureportsize = init_hidreport(request, SET, CPU_AIM);
        // append_crc(request);
        // if (hid_write(handle, hid_report, cpureportsize) == -1) {
        //     break;
        // }

        // // // if (hid_read(handle, hid_report, 0x40) == -1) {
        // // //     break;
        // // // }
        // memset(hid_report, 0x0, sizeof(unsigned char) * 0x40);
        // sleep(1);
        // //*****************************************************/
        // // //Memory Usage
        // int memusagesize = init_hidreport(request, SET, MEMORY_AIM);
        // append_crc(request);
        // if (hid_write(handle, hid_report, memusagesize) == -1) {
        //     break;
        // }
        // memset(hid_report, 0x0, sizeof(unsigned char) * 0x40);
        // sleep(1);
        // //*****************************************************/
        // //User Online
        // int usersize = init_hidreport(request, SET, USER_AIM);
        // append_crc(request);
        // if (hid_write(handle, hid_report, usersize) == -1) {
        //     break;
        // }
        // memset(hid_report, 0x0, sizeof(unsigned char) * 0x40);
        // sleep(1);
        // //*****************************************************/
        // //Get Error
        // int result = hid_read_timeout(handle, ack, 0x40, -1);
        // if (result == -1) {
        //     break;
        // }

        // if (result > 0) {
        //     parse_ack((Ack *)ack, request->aim);
        // }

        // memset(hid_report, 0x0, sizeof(unsigned char) * 0x40);
        // memset(ack, 0x0, sizeof(unsigned char) * 0x40);
        // //*****************************************************/
        // //dynamic read diskcount
        // disk_count = get_all_disk_info(&disks);
        // if (disk_count <= 0) {
        //     printf("未找到物理磁盘设备\n");
        //     return 1;
        // }
        // for (int i = 0; i < disk_count; i++) {
        // printf("磁盘 %d: %s\n", i + 1, disks[i].device);
        // printf("总容量: ");
        // print_modify_disk_size(disks[i].total_size);
        // printf("\n");
        
        // if (strlen(disks[i].mountpoint) > 0) {
        //     printf("可用容量: ");
        //     print_modify_disk_size(disks[i].free_size);
        //     printf("\n");
        //     printf("已用容量: ");
        //     print_modify_disk_size(disks[i].used_size);
        //     printf("\n");
        //     printf("使用率: %.1f%%\n", disks[i].usage_percent);
        // } else {
        //     printf("状态: 未挂载\n");
        // }
        
        //*****************************************************/
        
        
        
        //scan_ec_ports();
        // // 获取 I/O 权限
        // unsigned char address,value;
        // address = 0xA0;
        // if (acquire_io_permissions() < 0) {
        //     fprintf(stderr, "需要 root 权限运行此程序\n");
        //     return 1;
        // }
        // print_ec_status();
        //     int result = 0;
        // if (ec_ram_read_byte(address, &value) == 0) {
        //     printf("Address 0x%02X: 0x%02X (%d)\n", address, value, value);
        // } else {
        //     fprintf(stderr, "Failed to read address 0x%02X\n", address);
        //     result = 1;
        // }
        // // 释放 I/O 权限
        // release_io_permissions();
        
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
        printf("User Online Count:%d/n",user_count);
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
    }
    
    return disk_count;
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
    if (iopl(3) < 0) {
        perror("iopl failed");
        return -1;
    }
    
    // 也可以使用 ioperm 获取特定端口的权限
    if (ioperm(ITE_EC_INDEX_PORT, 2, 1) < 0) {
        perror("ioperm failed");
        return -1;
    }
    
    return 0;
}

// 释放 I/O 端口权限
void release_io_permissions() {
    ioperm(ITE_EC_INDEX_PORT, 2, 0);
    iopl(0);
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
    outb(index, ITE_EC_INDEX_PORT);
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
    ec_write_data(address);
    
    if (ec_wait_ready() < 0) {
        return -1;
    }
    
    // 读取数据
    *value = ec_read_data();
    return 0;
}

// 写入 EC RAM 字节
int ec_ram_write_byte(unsigned char address, unsigned char value) {
    if (ec_wait_ready() < 0) {
        return -1;
    }
    
    // 发送写入命令、地址和数据
    ec_write_index(EC_CMD_WRITE_RAM);
    ec_write_data(address);
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

// 检查 EC 是否存活
int ec_check_alive() {
    unsigned char test_value;
    
    // 尝试读取一个已知的 RAM 地址（通常是版本信息区域）
    if (ec_ram_read_byte(0x00, &test_value) == 0) {
        return 1;
    }
    
    return 0;
}
// 打印 EC 状态信息
void print_ec_status() {
    unsigned char status = inb(ITE_EC_CMD_PORT);
    printf("EC Status: 0x%02X\n", status);
    printf("  OBF (Output Buffer Full): %d\n", (status & 0x01) ? 1 : 0);
    printf("  IBF (Input Buffer Full): %d\n", (status & 0x02) ? 1 : 0);
    printf("  CMD (Last was command): %d\n", (status & 0x08) ? 1 : 0);
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
void scan_ec_ports() {
    int ports[][2] = {
        {0x62, 0x66},  // 标准 ITE
        {0x68, 0x6C},  // 替代 ITE
        {0x2E, 0x2F},  // 超级 I/O
        {0x6E, 0x6F},  // 另一种常见组合
        //{0x164, 0x168}, // ACPI EC
        {0, 0}
    };
    
    printf("扫描EC端口...\n");
    for (int i = 0; ports[i][0] != 0; i++) {
        if (ioperm(ports[i][0], 2, 1) == 0) {
            unsigned char status = inb(ports[i][1]);
            printf("端口 %02X/%02X: 状态=0x%02X", 
                   ports[i][0], ports[i][1], status);
            
            if (status != 0 && status != 0xFF) {
                printf(" *可能有效*");
            }
            printf("\n");
            
            ioperm(ports[i][0], 2, 0);
            usleep(10000);
        }
    }
}
void diagnose_ec_issue() {
    printf("=== EC 68/6C 端口诊断 ===\n\n");
    
    // 获取端口权限
    if (ioperm(ITE_EC_DATA_PORT, 2, 1) < 0) {
        perror("获取端口权限失败");
        return;
    }
    
    // 测试直接读取端口状态
    printf("直接读取端口状态:\n");
    printf("CMD端口(0x6C): 0x%02X\n", inb(ITE_EC_CMD_PORT));
    printf("DATA端口(0x68): 0x%02X\n", inb(ITE_EC_DATA_PORT));
    
    // 测试写入后读取
    printf("\n测试写入后读取:\n");
    outb(0xAA, ITE_EC_DATA_PORT);
    printf("写入 0xAA 到 DATA端口，读取: 0x%02X\n", inb(ITE_EC_DATA_PORT));
    
    outb(0x55, ITE_EC_DATA_PORT);
    printf("写入 0x55 到 DATA端口，读取: 0x%02X\n", inb(ITE_EC_DATA_PORT));
    
    // 测试命令端口
    printf("\n测试命令端口:\n");
    outb(0x80, ITE_EC_CMD_PORT);
    printf("写入 0x80 到 CMD端口，状态: 0x%02X\n", inb(ITE_EC_CMD_PORT));
    
    ioperm(ITE_EC_DATA_PORT, 2, 0);
}
int try_alternative_protocols(unsigned char address, unsigned char *value) {
    printf("\n=== 尝试替代协议 ===\n");
    
    if (ioperm(ITE_EC_DATA_PORT, 2, 1) < 0) return -1;
    
    // 方法1: 直接索引协议
    // printf("方法1: 直接索引协议\n");
    // outb(address, ITE_EC_DATA_PORT);      // 地址写到数据端口
    // outb(0x80, ITE_EC_CMD_PORT);         // 命令写到命令端口
    // usleep(1000);
    // *value = inb(ITE_EC_DATA_PORT);
    // printf("结果: 0x%02X\n", *value);
    
    // 方法2: 延迟较长的协议
    // printf("方法2: 长延迟协议\n");
    // outb(0x80, ITE_EC_CMD_PORT);
    // usleep(5000);  // 5ms 延迟
    // outb(address, ITE_EC_DATA_PORT);
    // usleep(5000);
    // *value = inb(ITE_EC_DATA_PORT);
    // printf("结果: 0x%02X\n", *value);
    
    // 方法3: 带重试的协议
    printf("方法3: 重试协议\n");
    for (int retry = 0; retry < 3; retry++) {
        outb(0x80, ITE_EC_CMD_PORT);
        usleep(1000);
        outb(address, ITE_EC_DATA_PORT);
        usleep(1000);
        
        unsigned char status = inb(ITE_EC_CMD_PORT);
        if (status & 0x01) {  // OBF 置位
            *value = inb(ITE_EC_DATA_PORT);
            printf("重试 %d: 成功 0x%02X\n", retry + 1, *value);
            ioperm(ITE_EC_DATA_PORT, 2, 0);
            return 0;
        }
        printf("重试 %d: 状态=0x%02X\n", retry + 1, status);
    }
    
    ioperm(ITE_EC_DATA_PORT, 2, 0);
    return -1;
}