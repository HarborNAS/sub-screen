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
#define MAXLEN 0x40
#define PRODUCTID 0x0002
#define VENDORID 0x5448
#define DURATION 1
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


CPUData prev_data, curr_data;
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


    while (true) {
        Request* request = (Request *)hid_report;
        //Time
        int timereportsize = init_hidreport(request, SET, TIME_AIM);
        append_crc(request);
        if (hid_write(handle, hid_report, timereportsize) == -1) {
            break;
        }
        // if (hid_read(handle, hid_report, 0x40) == -1) {
        //     break;
        // }
        memset(hid_report, 0x0, sizeof(unsigned char) * 0x40);
        sleep(1);
        // //CPU
        int cpureportsize = init_hidreport(request, SET, CPU_AIM);
        append_crc(request);
        if (hid_write(handle, hid_report, cpureportsize) == -1) {
            break;
        }

        // // if (hid_read(handle, hid_report, 0x40) == -1) {
        // //     break;
        // // }
        memset(hid_report, 0x0, sizeof(unsigned char) * 0x40);
        sleep(1);
        // //Memory Usage
        int memusagesize = init_hidreport(request, SET, MEMORY_AIM);
        append_crc(request);
        if (hid_write(handle, hid_report, memusagesize) == -1) {
            break;
        }

        int result = hid_read_timeout(handle, ack, 0x40, -1);
        if (result == -1) {
            break;
        }

        if (result > 0) {
            parse_ack((Ack *)ack, request->aim);
        }

        memset(hid_report, 0x0, sizeof(unsigned char) * 0x40);
        memset(ack, 0x0, sizeof(unsigned char) * 0x40);




        //dynamic read diskcount
        disk_count = get_all_disk_info(&disks);
        if (disk_count <= 0) {
            printf("未找到物理磁盘设备\n");
            return 1;
        }
        for (int i = 0; i < disk_count; i++) {
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
            printf("使用率: %.1f%%\n", disks[i].usage_percent);
        } else {
            printf("状态: 未挂载\n");
        }
    
    }
        sleep(DURATION);
    }

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
