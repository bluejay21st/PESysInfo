/*
 * PESysInfo - 共享数据模型与模块契约
 *
 * 目标环境：Windows PE（10.0.19041）x64 / x86、Windows 10、11 x64 / x86。
 * 仅依赖 Win32 API，不使用 WMI、PowerShell、.NET 或任何外部运行库。
 *
 * 所有字符串在内部统一使用 UTF-16（wchar_t），输出层负责转换为
 * 控制台宽字符或 UTF-8/GBK 字节流。
 */
#ifndef PESYSINFO_H
#define PESYSINFO_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSI_TOOL_VERSION    L"v0.9"
#define PSI_JSON_SCHEMA     L"pesysinfo/1"

#define PSI_TEXT_CCH        256
#define PSI_MAX_MEM_MODULES 16
#define PSI_MAX_DISKS       32
#define PSI_MAX_VOLUMES     26
#define PSI_MAX_GRAPHICS    8
#define PSI_MAX_MONITORS    16
#define PSI_MAX_NET         16
#define PSI_MAX_WARNINGS    32
#define PSI_FEATURES_CCH    512

#define PSI_DISK_INDEX_UNKNOWN ((unsigned)-1)

/* ------------------------------------------------------------------ */
/* SMBIOS 原始表（GetSystemFirmwareTable('RSMB')）                     */
/* ------------------------------------------------------------------ */
typedef struct {
    BYTE        *raw;         /* HeapAlloc 分配的原始数据，供释放使用     */
    const BYTE  *table;       /* 指向第一条 SMBIOS 结构                  */
    DWORD        table_size;
    BOOL         valid;
} SmbiosData;

/* ------------------------------------------------------------------ */
/* 系统 / 固件 / 主板                                                  */
/* ------------------------------------------------------------------ */
typedef struct {
    wchar_t  computer_name[128];
    wchar_t  domain[128];
    wchar_t  user_name[128];

    wchar_t  os_name[PSI_TEXT_CCH];      /* 例如 Windows Preinstallation Environment */
    wchar_t  os_version[64];             /* 10.0.19041                                  */
    wchar_t  os_build[64];               /* 19041.1                                     */
    wchar_t  os_arch[32];                /* x64 / x86                                   */
    wchar_t  os_edition[PSI_TEXT_CCH];   /* 例如 Windows 10 Pro                         */

    BOOL     is_pe;
    wchar_t  pe_version[64];             /* WinPE 版本，如 10.0.19041                   */
    wchar_t  firmware_mode[32];          /* UEFI / BIOS / 未知                          */

    wchar_t  manufacturer[PSI_TEXT_CCH]; /* 整机厂商                                    */
    wchar_t  product[PSI_TEXT_CCH];      /* 整机型号                                    */
    wchar_t  system_version[PSI_TEXT_CCH];/* 整机版本                                   */
    wchar_t  serial[128];                /* 整机序列号                                  */
    wchar_t  uuid[64];
    wchar_t  sku[128];
    wchar_t  chassis[64];                /* 机箱类型                                    */

    wchar_t  bios_vendor[128];
    wchar_t  bios_version[PSI_TEXT_CCH];
    wchar_t  bios_date[32];

    wchar_t  board_manufacturer[128];
    wchar_t  board_product[PSI_TEXT_CCH];
    wchar_t  board_version[64];
    wchar_t  board_serial[128];

    unsigned long long uptime_seconds;
    SYSTEMTIME local_time;
} SystemInfo;

/* ------------------------------------------------------------------ */
/* 处理器                                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    wchar_t  brand[PSI_TEXT_CCH];
    wchar_t  vendor[64];
    wchar_t  identifier[PSI_TEXT_CCH];   /* 注册表 Identifier，如 Intel64 Family ... */
    wchar_t  arch[32];
    unsigned sockets;                    /* 物理插槽（路）数                         */
    unsigned cores;                      /* 物理核心数                               */
    unsigned logical;                    /* 逻辑处理器数                             */
    unsigned base_mhz;
    unsigned max_mhz;
    unsigned current_mhz;                /* 当前平均频率（快照，可能为 0/未知） */
    unsigned l1_kb;
    unsigned l2_kb;
    unsigned l3_kb;
    wchar_t  features[PSI_FEATURES_CCH]; /* 逗号分隔的指令集/虚拟化特性              */
} CpuInfo;

/* ------------------------------------------------------------------ */
/* 内存                                                                */
/* ------------------------------------------------------------------ */
typedef struct {
    wchar_t  locator[128];               /* 插槽位置，如 DIMM_A1                     */
    wchar_t  bank[64];
    wchar_t  type[32];                   /* DDR4 / DDR5 / LPDDR4 ...                 */
    wchar_t  form_factor[32];            /* DIMM / SODIMM ...                        */
    wchar_t  manufacturer[128];
    wchar_t  part_number[128];
    wchar_t  serial[128];
    unsigned long long size_bytes;
    unsigned speed_mts;
    unsigned configured_speed_mts;
    unsigned rank;                       /* 0 = 未知                                  */
    unsigned data_width_bits;
    unsigned voltage_min_mv;
    unsigned voltage_max_mv;
    unsigned voltage_configured_mv;
    BOOL     registered;                 /* Registered/Buffered                      */
} MemoryModule;

typedef struct {
    unsigned long long total_bytes;          /* 物理内存总量                          */
    unsigned long long available_bytes;
    unsigned long long installed_bytes;      /* 已安装物理内存（SMBIOS/API）          */
    unsigned long long max_capacity_bytes;   /* SMBIOS type 16 最大容量               */
    wchar_t  ecc_type[32];                   /* SMBIOS type 16 纠错类型                */

    unsigned slot_count;                     /* 检测到的内存插槽总数（含空槽）        */
    unsigned module_count;                   /* 已安装内存条数量                      */
    MemoryModule modules[PSI_MAX_MEM_MODULES];
} MemoryInfo;

/* ------------------------------------------------------------------ */
/* 存储                                                                */
/* ------------------------------------------------------------------ */
typedef struct {
    unsigned index;                          /* PhysicalDriveN                        */
    wchar_t  model[PSI_TEXT_CCH];
    wchar_t  serial[128];
    wchar_t  firmware[64];
    wchar_t  bus[32];                        /* SATA / NVMe / USB / SAS ...           */
    wchar_t  media[32];                      /* SSD / HDD / 可移动 / 未知             */
    wchar_t  partition_style[16];            /* MBR / GPT / 未知                      */
    unsigned long long size_bytes;
    unsigned sector_size;
    unsigned partition_count;
    BOOL     is_system;                      /* 是否包含固定/可移动系统卷              */
} DiskInfo;

typedef struct {
    wchar_t  letter[4];                      /* "C:"                                  */
    wchar_t  label[128];
    wchar_t  filesystem[32];                 /* NTFS / FAT32 / exFAT ...              */
    wchar_t  type[32];                       /* 本地磁盘 / 可移动磁盘 / 光驱 ...      */
    unsigned long long total_bytes;
    unsigned long long free_bytes;
    unsigned disk_index;                     /* 对应物理磁盘；未知为 UNKNOWN          */
} VolumeInfo;

typedef struct {
    unsigned disk_count;
    unsigned volume_count;
    DiskInfo disks[PSI_MAX_DISKS];
    VolumeInfo volumes[PSI_MAX_VOLUMES];
} StorageInfo;

/* ------------------------------------------------------------------ */
/* 显示适配器 / 网络适配器                                             */
/* ------------------------------------------------------------------ */
typedef struct {
    wchar_t  adapter[PSI_TEXT_CCH];          /* 适配器名称                            */
    wchar_t  device[PSI_TEXT_CCH];           /* 设备描述                              */
    unsigned width;
    unsigned height;
    BOOL     primary;
} GraphicsAdapter;

typedef struct {
    wchar_t  name[PSI_TEXT_CCH];             /* 显示器名称                            */
    wchar_t  adapter[PSI_TEXT_CCH];          /* 所属适配器名称                        */
    wchar_t  device[PSI_TEXT_CCH];           /* \\.\DISPLAYn                          */
    unsigned width;
    unsigned height;
    unsigned refresh_hz;
    BOOL     primary;
} DisplayMonitor;

typedef struct {
    unsigned count;
    GraphicsAdapter items[PSI_MAX_GRAPHICS];
    unsigned monitor_count;
    DisplayMonitor monitors[PSI_MAX_MONITORS];
} GraphicsInfo;

typedef struct {
    wchar_t  description[PSI_TEXT_CCH];
    wchar_t  mac[32];
    wchar_t  ipv4[64];
    wchar_t  speed[32];                      /* 例如 1.0 Gbps                          */
    wchar_t  status[32];                     /* 已连接 / 已断开 / 未知                 */
} NetworkAdapter;

typedef struct {
    unsigned count;
    NetworkAdapter items[PSI_MAX_NET];
} NetworkInfo;

/* ------------------------------------------------------------------ */
/* 顶层报告对象                                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    SystemInfo  system;
    CpuInfo     cpu;
    MemoryInfo  memory;
    StorageInfo storage;
    GraphicsInfo graphics;
    NetworkInfo network;

    wchar_t warnings[PSI_MAX_WARNINGS][PSI_TEXT_CCH];
    unsigned warning_count;
} SystemReport;

/* ------------------------------------------------------------------ */
/* util.c                                                              */
/* ------------------------------------------------------------------ */
void  psi_zero_memory(void *ptr, size_t size);
void  psi_str_copy(wchar_t *dst, size_t cch, const wchar_t *src);
void  psi_str_copy_ansi(wchar_t *dst, size_t cch, const char *src);
void  psi_str_trim(wchar_t *text);
void  psi_str_collapse_spaces(wchar_t *text);
void  psi_str_append(wchar_t *dst, size_t cch, const wchar_t *src);
void  psi_str_appendf(wchar_t *dst, size_t cch, const wchar_t *fmt, ...);
BOOL  psi_str_contains_ci(const wchar_t *haystack, const wchar_t *needle);
BOOL  psi_str_is_empty(const wchar_t *text);

void  psi_format_bytes(unsigned long long bytes, wchar_t *buf, size_t cch);
void  psi_format_mhz(unsigned mhz, wchar_t *buf, size_t cch);
void  psi_format_datetime(const SYSTEMTIME *st, wchar_t *buf, size_t cch);
void  psi_format_duration(unsigned long long seconds, wchar_t *buf, size_t cch);

BOOL  psi_reg_read_string(HKEY root, const wchar_t *subkey, const wchar_t *value,
                          wchar_t *buf, size_t cch);
BOOL  psi_reg_read_dword(HKEY root, const wchar_t *subkey, const wchar_t *value,
                         DWORD *out);
BOOL  psi_file_exists(const wchar_t *path);
void  psi_get_system_dir(wchar_t *buf, size_t cch);

/* ------------------------------------------------------------------ */
/* smbios.c                                                            */
/* ------------------------------------------------------------------ */
BOOL  psi_smbios_load(SmbiosData *smbios);
void  psi_smbios_free(SmbiosData *smbios);
const BYTE *psi_smbios_find(const SmbiosData *smbios, BYTE type, unsigned occurrence);
const char *psi_smbios_string(const BYTE *structure, BYTE offset);
void  psi_smbios_to_wide(const char *text, wchar_t *dst, size_t cch);
unsigned long long psi_smbios_memory_size(const BYTE *type17);
unsigned psi_smbios_struct_count(const SmbiosData *smbios, BYTE type);

/* ------------------------------------------------------------------ */
/* 采集模块                                                            */
/* ------------------------------------------------------------------ */
BOOL psi_system_collect(SystemInfo *info, SystemReport *report);
BOOL psi_firmware_collect(SystemInfo *info, const SmbiosData *smbios,
                          SystemReport *report);
BOOL psi_cpu_collect(CpuInfo *info, const SmbiosData *smbios,
                     SystemReport *report);
BOOL psi_memory_collect(MemoryInfo *info, const SmbiosData *smbios,
                        SystemReport *report);
BOOL psi_storage_collect(StorageInfo *info, SystemReport *report);
BOOL psi_graphics_collect(GraphicsInfo *info, SystemReport *report);
BOOL psi_network_collect(NetworkInfo *info, SystemReport *report);

/* 由 probe.c 实现：一次性完成全部采集。 */
void psi_collect_all(SystemReport *report);
void psi_add_warning(SystemReport *report, const wchar_t *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* PESYSINFO_H */
