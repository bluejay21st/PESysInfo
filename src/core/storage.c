/*
 * storage.c - 物理磁盘与卷采集
 *
 * 目标环境：Windows PE 10.0.19041 x64/x86 / Windows 10、11 x64/x86，
 * SYSTEM 权限，无 WMI / PowerShell / SetupAPI。仅依赖 kernel32。
 *
 * 实现要点：
 *  - 磁盘侧（\\.\PhysicalDrive0..31）只发送查询类 IOCTL：
 *      IOCTL_STORAGE_QUERY_PROPERTY / StorageDeviceProperty
 *          -> 厂商、型号、固件版本、序列号、总线类型、可移动标志
 *      IOCTL_DISK_GET_DRIVE_GEOMETRY_EX
 *          -> 容量（字节）、逻辑扇区大小
 *      IOCTL_DISK_GET_DRIVE_LAYOUT_EX
 *          -> 分区样式（MBR / GPT）与分区数量
 *      IOCTL_STORAGE_QUERY_PROPERTY / StorageDeviceSeekPenaltyProperty
 *          -> IncursSeekPenalty 为空则判为 SSD，否则 HDD
 *  - 卷侧（A: .. Z:）：
 *      GetDriveTypeW / GetVolumeInformationW / GetDiskFreeSpaceExW
 *      IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS -> 卷所属物理磁盘号
 *      GetSystemDirectoryW / GetWindowsDirectoryW -> 系统卷盘符
 *  - 单个磁盘/卷失败只记录一次中文警告（相同原因去重），绝不中断整体采集。
 *  - 全程不写磁盘、不修改分区表/卷、不调用 WMI 或 SetupAPI。
 */
#include "pesysinfo.h"

#include <winioctl.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* ------------------------------------------------------------------ */
/* 常量                                                                */
/* ------------------------------------------------------------------ */

/* STORAGE_DEVICE_DESCRIPTOR 缓冲（设备返回的字符串位于该缓冲内） */
#define PSI_DEVICE_PROPERTY_BYTES  1024u
/* DISK_GEOMETRY_EX 缓冲 */
#define PSI_GEOMETRY_BYTES         256u
/* DRIVE_LAYOUT_INFORMATION_EX 缓冲：默认 64KB，分区过多时放大一次到 1MB */
#define PSI_LAYOUT_BYTES           (64u * 1024u)
#define PSI_LAYOUT_BYTES_MAX       (1024u * 1024u)
/* IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS 一次可容纳的 extent 数 */
#define PSI_VOLUME_EXTENTS         16u
/* 设备属性查询失败时的总线类型占位值（不会与任何 STORAGE_BUS_TYPE 冲突） */
#define PSI_NO_BUS_TYPE            0xFFu

/*
 * STORAGE_BUS_TYPE 的数值（winioctl.h 的 BusTypeXxx 枚举）。
 * 这里使用数值而不是枚举名，避免旧版 SDK 缺少 BusTypeNvme 等成员。
 */
enum {
    PSI_BUS_SCSI           = 0x01,
    PSI_BUS_ATAPI          = 0x02,
    PSI_BUS_ATA            = 0x03,
    PSI_BUS_IEEE1394       = 0x04,
    PSI_BUS_SSA            = 0x05,
    PSI_BUS_FIBRE          = 0x06,
    PSI_BUS_USB            = 0x07,
    PSI_BUS_RAID           = 0x08,
    PSI_BUS_ISCSI          = 0x09,
    PSI_BUS_SAS            = 0x0A,
    PSI_BUS_SATA           = 0x0B,
    PSI_BUS_SD             = 0x0C,
    PSI_BUS_MMC            = 0x0D,
    PSI_BUS_VIRTUAL        = 0x0E,
    PSI_BUS_FILE_VIRTUAL   = 0x0F,
    PSI_BUS_SPACES         = 0x10,
    PSI_BUS_NVME           = 0x11,
    PSI_BUS_SCM            = 0x12,
    PSI_BUS_UFS            = 0x13
};

/* ------------------------------------------------------------------ */
/* 警告去重                                                            */
/* ------------------------------------------------------------------ */

/*
 * 采集过程中的警告状态：按“原因键”去重，同一原因最多提示一次。
 * 键都是静态字符串字面量，保存指针并按内容比较。
 */
typedef struct {
    const wchar_t *keys[PSI_MAX_WARNINGS];
    size_t         count;
} StorageWarnState;

static BOOL warning_should_emit(StorageWarnState *state, const wchar_t *key)
{
    size_t i;

    if (state == NULL) {
        return TRUE;
    }
    for (i = 0; i < state->count; ++i) {
        if (wcscmp(state->keys[i], key) == 0) {
            return FALSE;
        }
    }
    if (state->count < (size_t)PSI_MAX_WARNINGS) {
        state->keys[state->count] = key;
        ++state->count;
    }
    return TRUE;
}

static void storage_warn(StorageWarnState *state, SystemReport *report,
                         const wchar_t *key, const wchar_t *fmt, ...)
{
    wchar_t text[PSI_TEXT_CCH];
    va_list args;

    if (report == NULL || !warning_should_emit(state, key)) {
        return;
    }
    va_start(args, fmt);
    _vsnwprintf_s(text, ARRAYSIZE(text), _TRUNCATE, fmt, args);
    va_end(args);
    text[ARRAYSIZE(text) - 1] = L'\0';
    psi_add_warning(report, L"%ls", text);
}

/* ------------------------------------------------------------------ */
/* 基础辅助                                                            */
/* ------------------------------------------------------------------ */

/* 把 A-Z / a-z 统一成大写；其他字符返回 0。 */
static wchar_t upper_letter(wchar_t ch)
{
    if (ch >= L'a' && ch <= L'z') {
        return (wchar_t)(ch - L'a' + L'A');
    }
    if (ch >= L'A' && ch <= L'Z') {
        return ch;
    }
    return L'\0';
}

/* 物理磁盘号 -> 总线类型中文/英文名称。 */
static const wchar_t *storage_bus_name(BYTE bus_type)
{
    switch (bus_type) {
    case PSI_BUS_SCSI:         return L"SCSI";
    case PSI_BUS_ATAPI:        return L"ATAPI";
    case PSI_BUS_ATA:          return L"ATA";
    case PSI_BUS_IEEE1394:     return L"1394";
    case PSI_BUS_SSA:          return L"SSA";
    case PSI_BUS_FIBRE:        return L"FC";
    case PSI_BUS_USB:          return L"USB";
    case PSI_BUS_RAID:         return L"RAID";
    case PSI_BUS_ISCSI:        return L"iSCSI";
    case PSI_BUS_SAS:          return L"SAS";
    case PSI_BUS_SATA:         return L"SATA";
    case PSI_BUS_SD:           return L"SD";
    case PSI_BUS_MMC:          return L"MMC";
    case PSI_BUS_VIRTUAL:      return L"虚拟";
    case PSI_BUS_FILE_VIRTUAL: return L"虚拟";
    case PSI_BUS_SPACES:       return L"存储空间";
    case PSI_BUS_NVME:         return L"NVMe";
    case PSI_BUS_SCM:          return L"SCM";
    case PSI_BUS_UFS:          return L"UFS";
    default:                   return L"未知";
    }
}

/*
 * 从 STORAGE_DEVICE_DESCRIPTOR 缓冲中按偏移取 ANSI 字符串：
 * 只读取缓冲区范围内、以 NUL 结尾的内容，然后去空白、合并连续空格。
 * 偏移为 0、越界或空串都视为“没有该字段”。
 */
static BOOL descriptor_string(const BYTE *buffer, DWORD buffer_size, DWORD offset,
                              wchar_t *dst, size_t cch)
{
    char ansi[PSI_TEXT_CCH];
    size_t limit;
    size_t i;

    if (dst == NULL || cch == 0) {
        return FALSE;
    }
    dst[0] = L'\0';
    if (buffer == NULL || offset == 0 || offset >= buffer_size) {
        return FALSE;
    }
    limit = (size_t)(buffer_size - offset);
    if (limit >= sizeof(ansi)) {
        limit = sizeof(ansi) - 1;
    }
    for (i = 0; i < limit; ++i) {
        char ch = (char)buffer[offset + i];
        if (ch == '\0') {
            break;
        }
        ansi[i] = ch;
    }
    if (i == 0) {
        return FALSE;
    }
    ansi[i] = '\0';
    psi_str_copy_ansi(dst, cch, ansi);
    psi_str_trim(dst);
    psi_str_collapse_spaces(dst);
    return (dst[0] != L'\0') ? TRUE : FALSE;
}

/*
 * 合成型号名：厂商 + 型号；若型号字符串里已经包含厂商（不区分大小写），
 * 则只保留型号，避免出现 "Samsung Samsung SSD 860" 这类重复。
 */
static void compose_disk_model(DiskInfo *disk, const wchar_t *vendor,
                               const wchar_t *product)
{
    BOOL has_vendor = (vendor != NULL && vendor[0] != L'\0') ? TRUE : FALSE;
    BOOL has_product = (product != NULL && product[0] != L'\0') ? TRUE : FALSE;

    disk->model[0] = L'\0';
    if (has_product) {
        if (has_vendor && !psi_str_contains_ci(product, vendor)) {
            psi_str_copy(disk->model, ARRAYSIZE(disk->model), vendor);
            psi_str_append(disk->model, ARRAYSIZE(disk->model), L" ");
        }
        psi_str_append(disk->model, ARRAYSIZE(disk->model), product);
    } else if (has_vendor) {
        psi_str_copy(disk->model, ARRAYSIZE(disk->model), vendor);
    }
    psi_str_collapse_spaces(disk->model);
}

/* DRIVE_* -> 卷类型中文名称。 */
static void volume_type_text(UINT drive_type, wchar_t *buf, size_t cch)
{
    switch (drive_type) {
    case DRIVE_FIXED:     psi_str_copy(buf, cch, L"本地磁盘");   break;
    case DRIVE_REMOVABLE: psi_str_copy(buf, cch, L"可移动磁盘"); break;
    case DRIVE_CDROM:     psi_str_copy(buf, cch, L"光驱");       break;
    case DRIVE_RAMDISK:   psi_str_copy(buf, cch, L"内存盘");     break;
    case DRIVE_REMOTE:    psi_str_copy(buf, cch, L"网络驱动器"); break;
    default:              psi_str_copy(buf, cch, L"未知");       break;
    }
}

/* ------------------------------------------------------------------ */
/* 物理磁盘                                                            */
/* ------------------------------------------------------------------ */

/*
 * 打开 \\.\PhysicalDriveN：
 *   1) dwDesiredAccess = 0；查询类 IOCTL 不需要读写权限，也不受卷锁影响；
 *   2) 失败后再用 GENERIC_READ 尝试一次。
 * 成功返回句柄，失败返回 INVALID_HANDLE_VALUE 并带出 GetLastError。
 */
static HANDLE open_physical_drive(unsigned index, DWORD *out_error)
{
    wchar_t path[64];
    HANDLE handle;

    *out_error = ERROR_SUCCESS;
    swprintf_s(path, ARRAYSIZE(path), L"\\\\.\\PhysicalDrive%u", index);
    handle = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        *out_error = GetLastError();
        handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
        if (handle != INVALID_HANDLE_VALUE) {
            *out_error = ERROR_SUCCESS;
        } else {
            *out_error = GetLastError();
        }
    }
    return handle;
}

/*
 * StorageDeviceProperty：厂商 / 型号 / 固件 / 序列号 / 总线类型 / 可移动标志。
 * 输出总线数值与可移动标志，供介质类型判定使用。
 */
static void query_disk_properties(HANDLE handle, DiskInfo *disk, BYTE *out_bus,
                                  BOOL *out_removable, StorageWarnState *warn,
                                  SystemReport *report)
{
    BYTE buffer[PSI_DEVICE_PROPERTY_BYTES];
    DWORD bytes = 0;
    DWORD minimum_size;
    DWORD descriptor_size;
    STORAGE_PROPERTY_QUERY query;
    const STORAGE_DEVICE_DESCRIPTOR *descriptor;
    wchar_t vendor[PSI_TEXT_CCH];
    wchar_t product[PSI_TEXT_CCH];

    *out_bus = PSI_NO_BUS_TYPE;
    *out_removable = FALSE;

    minimum_size = (DWORD)(offsetof(STORAGE_DEVICE_DESCRIPTOR, RawPropertiesLength) +
                           sizeof(DWORD));

    psi_zero_memory(buffer, sizeof(buffer));
    psi_zero_memory(&query, sizeof(query));
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    if (!DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY,
                         &query, (DWORD)sizeof(query),
                         buffer, (DWORD)sizeof(buffer), &bytes, NULL) ||
        bytes < minimum_size) {
        storage_warn(warn, report, L"disk_property",
                     L"物理磁盘 %u 的设备属性读取失败，型号/序列号等信息记为未知。",
                     disk->index);
        return;
    }

    descriptor = (const STORAGE_DEVICE_DESCRIPTOR *)buffer;
    descriptor_size = descriptor->Size;
    if (descriptor_size < minimum_size || descriptor_size > bytes) {
        descriptor_size = bytes;
    }

    vendor[0] = L'\0';
    product[0] = L'\0';
    (void)descriptor_string(buffer, descriptor_size, descriptor->VendorIdOffset,
                            vendor, ARRAYSIZE(vendor));
    (void)descriptor_string(buffer, descriptor_size, descriptor->ProductIdOffset,
                            product, ARRAYSIZE(product));
    (void)descriptor_string(buffer, descriptor_size, descriptor->ProductRevisionOffset,
                            disk->firmware, ARRAYSIZE(disk->firmware));
    (void)descriptor_string(buffer, descriptor_size, descriptor->SerialNumberOffset,
                            disk->serial, ARRAYSIZE(disk->serial));

    compose_disk_model(disk, vendor, product);

    *out_bus = (BYTE)descriptor->BusType;
    *out_removable = descriptor->RemovableMedia ? TRUE : FALSE;
    psi_str_copy(disk->bus, ARRAYSIZE(disk->bus), storage_bus_name(*out_bus));
}

/* IOCTL_DISK_GET_DRIVE_GEOMETRY_EX：容量（字节）与逻辑扇区大小。 */
static void query_disk_geometry(HANDLE handle, DiskInfo *disk,
                                StorageWarnState *warn, SystemReport *report)
{
    BYTE buffer[PSI_GEOMETRY_BYTES];
    DWORD bytes = 0;
    DWORD minimum_size;

    minimum_size = (DWORD)offsetof(DISK_GEOMETRY_EX, Data);

    psi_zero_memory(buffer, sizeof(buffer));
    if (!DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0,
                         buffer, (DWORD)sizeof(buffer), &bytes, NULL) ||
        bytes < minimum_size) {
        disk->size_bytes = 0;
        disk->sector_size = 0;
        storage_warn(warn, report, L"disk_geometry",
                     L"物理磁盘 %u 的容量/扇区信息读取失败，已记为 0。",
                     disk->index);
        return;
    }

    {
        const DISK_GEOMETRY_EX *geometry = (const DISK_GEOMETRY_EX *)buffer;
        disk->size_bytes = (unsigned long long)geometry->DiskSize.QuadPart;
        disk->sector_size = geometry->Geometry.BytesPerSector;
    }
}

/*
 * IOCTL_DISK_GET_DRIVE_LAYOUT_EX：分区样式与分区数量。
 * 使用 HeapAlloc 的 64KB 缓冲；若返回 ERROR_INSUFFICIENT_BUFFER 再放大到 1MB。
 * 失败时分区样式记为“未知”、数量记为 0（不中断采集）。
 */
static void query_disk_layout(HANDLE handle, DiskInfo *disk,
                              StorageWarnState *warn, SystemReport *report)
{
    DWORD buffer_bytes = PSI_LAYOUT_BYTES;
    unsigned attempt;

    for (attempt = 0; attempt < 2; ++attempt) {
        BYTE *buffer;
        DWORD bytes = 0;

        buffer = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                   (SIZE_T)buffer_bytes);
        if (buffer == NULL) {
            storage_warn(warn, report, L"disk_layout_memory",
                         L"内存不足，无法读取物理磁盘分区布局信息。");
            return;
        }
        if (DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0,
                            buffer, buffer_bytes, &bytes, NULL)) {
            const DRIVE_LAYOUT_INFORMATION_EX *layout =
                (const DRIVE_LAYOUT_INFORMATION_EX *)buffer;
            DWORD need = (DWORD)(offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionCount) +
                                 sizeof(DWORD));
            if (bytes >= need) {
                switch (layout->PartitionStyle) {
                case PARTITION_STYLE_MBR:
                    psi_str_copy(disk->partition_style,
                                 ARRAYSIZE(disk->partition_style), L"MBR");
                    break;
                case PARTITION_STYLE_GPT:
                    psi_str_copy(disk->partition_style,
                                 ARRAYSIZE(disk->partition_style), L"GPT");
                    break;
                default:
                    psi_str_copy(disk->partition_style,
                                 ARRAYSIZE(disk->partition_style), L"未知");
                    break;
                }
                disk->partition_count = layout->PartitionCount;
            }
            HeapFree(GetProcessHeap(), 0, buffer);
            return;
        }
        {
            DWORD error = GetLastError();
            HeapFree(GetProcessHeap(), 0, buffer);
            if (error == ERROR_INSUFFICIENT_BUFFER && attempt == 0) {
                buffer_bytes = PSI_LAYOUT_BYTES_MAX;
                continue;
            }
        }
        break;
    }

    disk->partition_count = 0;
    psi_str_copy(disk->partition_style, ARRAYSIZE(disk->partition_style), L"未知");
    storage_warn(warn, report, L"disk_layout",
                 L"物理磁盘 %u 的分区布局读取失败，分区样式记为未知。",
                 disk->index);
}

/*
 * StorageDeviceSeekPenaltyProperty：
 *   返回  0 = 不产生寻道开销（SSD）
 *         1 = 产生寻道开销（HDD）
 *        -1 = 无法判定
 */
static int query_seek_penalty(HANDLE handle)
{
    STORAGE_PROPERTY_QUERY query;
    DEVICE_SEEK_PENALTY_DESCRIPTOR descriptor;
    DWORD bytes = 0;

    psi_zero_memory(&query, sizeof(query));
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;

    psi_zero_memory(&descriptor, sizeof(descriptor));
    if (!DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY,
                         &query, (DWORD)sizeof(query),
                         &descriptor, (DWORD)sizeof(descriptor), &bytes, NULL) ||
        bytes < (DWORD)sizeof(descriptor)) {
        return -1;
    }
    /* 成功返回时 Version/Size 由驱动填写；两者都为 0 说明设备其实并不支持
       该属性（个别 USB 桥接芯片），按“无法判定”处理，避免误判成 SSD。 */
    if (descriptor.Version == 0 && descriptor.Size == 0) {
        return -1;
    }
    return descriptor.IncursSeekPenalty ? 1 : 0;
}

/*
 * 介质类型：
 *   NVMe                       -> SSD（无需再查询寻道开销）
 *   IncursSeekPenalty = FALSE  -> SSD
 *   IncursSeekPenalty = TRUE   -> HDD
 *   无法判定且总线为 USB/可移动 -> 可移动
 *   其他                       -> 未知
 */
static void query_disk_media(HANDLE handle, DiskInfo *disk, BYTE bus_type,
                             BOOL removable)
{
    int penalty;

    if (bus_type == PSI_BUS_NVME) {
        psi_str_copy(disk->media, ARRAYSIZE(disk->media), L"SSD");
        return;
    }
    penalty = query_seek_penalty(handle);
    if (penalty == 0) {
        psi_str_copy(disk->media, ARRAYSIZE(disk->media), L"SSD");
        return;
    }
    if (penalty == 1) {
        psi_str_copy(disk->media, ARRAYSIZE(disk->media), L"HDD");
        return;
    }
    if (bus_type == PSI_BUS_USB || removable) {
        psi_str_copy(disk->media, ARRAYSIZE(disk->media), L"可移动");
        return;
    }
    psi_str_copy(disk->media, ARRAYSIZE(disk->media), L"未知");
}

/* 查询并填充一个物理磁盘；成功打开后才调用。 */
static void collect_disk(StorageInfo *info, unsigned index, HANDLE handle,
                         StorageWarnState *warn, SystemReport *report)
{
    DiskInfo *disk;
    BYTE bus_type;
    BOOL removable;

    disk = &info->disks[info->disk_count];
    psi_zero_memory(disk, sizeof(*disk));
    disk->index = index;
    disk->size_bytes = 0;
    disk->sector_size = 0;
    disk->partition_count = 0;
    psi_str_copy(disk->bus, ARRAYSIZE(disk->bus), L"未知");
    psi_str_copy(disk->media, ARRAYSIZE(disk->media), L"未知");
    psi_str_copy(disk->partition_style, ARRAYSIZE(disk->partition_style),
                 L"未知");

    bus_type = PSI_NO_BUS_TYPE;
    removable = FALSE;
    query_disk_properties(handle, disk, &bus_type, &removable, warn, report);
    query_disk_geometry(handle, disk, warn, report);
    query_disk_layout(handle, disk, warn, report);
    query_disk_media(handle, disk, bus_type, removable);

    ++info->disk_count;
}

/* 枚举 \\.\PhysicalDrive0..PSI_MAX_DISKS-1。 */
static void collect_disks(StorageInfo *info, StorageWarnState *warn,
                          SystemReport *report)
{
    unsigned index;

    for (index = 0; index < (unsigned)PSI_MAX_DISKS; ++index) {
        HANDLE handle;
        DWORD open_error = ERROR_SUCCESS;

        handle = open_physical_drive(index, &open_error);
        if (handle == INVALID_HANDLE_VALUE) {
            if (open_error == ERROR_ACCESS_DENIED) {
                storage_warn(warn, report, L"disk_access",
                             L"部分物理磁盘无法打开（拒绝访问），"
                             L"请确认以管理员/SYSTEM 身份运行。");
            }
            continue;
        }
        collect_disk(info, index, handle, warn, report);
        CloseHandle(handle);
    }

    if (info->disk_count == 0) {
        storage_warn(warn, report, L"no_disks",
                     L"未能枚举到任何物理磁盘，可能缺少存储驱动。");
    }
    if (info->disk_count >= (unsigned)PSI_MAX_DISKS) {
        storage_warn(warn, report, L"disk_limit",
                     L"物理磁盘数量达到上限 %u，可能还有磁盘未显示。",
                     (unsigned)PSI_MAX_DISKS);
    }
}

/* ------------------------------------------------------------------ */
/* 卷                                                                  */
/* ------------------------------------------------------------------ */

/* 用于 IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS 的输出缓冲（多 extent 安全）。 */
typedef struct {
    VOLUME_DISK_EXTENTS header;                       /* 含 Extents[1] */
    DISK_EXTENT         extra[PSI_VOLUME_EXTENTS - 1u];
} VolumeExtentBuffer;

/* 取卷所属的第一个物理磁盘号；失败返回 PSI_DISK_INDEX_UNKNOWN。 */
static unsigned query_volume_disk_index(wchar_t letter)
{
    wchar_t path[16];
    VolumeExtentBuffer buffer;
    HANDLE handle;
    DWORD bytes = 0;
    DWORD minimum;
    unsigned disk_index = PSI_DISK_INDEX_UNKNOWN;

    path[0] = L'\\';
    path[1] = L'\\';
    path[2] = L'.';
    path[3] = L'\\';
    path[4] = letter;
    path[5] = L':';
    path[6] = L'\0';

    handle = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        handle = CreateFileW(path, GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING, 0, NULL);
    }
    if (handle == INVALID_HANDLE_VALUE) {
        return PSI_DISK_INDEX_UNKNOWN;
    }

    minimum = (DWORD)(offsetof(VOLUME_DISK_EXTENTS, Extents) + sizeof(DISK_EXTENT));
    psi_zero_memory(&buffer, sizeof(buffer));
    if (DeviceIoControl(handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0,
                        &buffer, (DWORD)sizeof(buffer), &bytes, NULL) &&
        bytes >= minimum &&
        buffer.header.NumberOfDiskExtents > 0) {
        disk_index = (unsigned)buffer.header.Extents[0].DiskNumber;
    }

    CloseHandle(handle);
    return disk_index;
}

/* 把某个物理磁盘标记为“包含系统卷”。 */
static void mark_system_disk(StorageInfo *info, unsigned disk_index)
{
    unsigned i;

    if (disk_index == PSI_DISK_INDEX_UNKNOWN) {
        return;
    }
    for (i = 0; i < info->disk_count; ++i) {
        if (info->disks[i].index == disk_index) {
            info->disks[i].is_system = TRUE;
            return;
        }
    }
}

/* 系统目录所在盘符（优先 GetSystemDirectoryW，回退 GetWindowsDirectoryW）。 */
static wchar_t detect_system_letter(void)
{
    wchar_t path[MAX_PATH];
    wchar_t letter;

    path[0] = L'\0';
    if (GetSystemDirectoryW(path, (UINT)ARRAYSIZE(path)) > 0) {
        letter = upper_letter(path[0]);
        if (letter != L'\0') {
            return letter;
        }
    }
    path[0] = L'\0';
    if (GetWindowsDirectoryW(path, (UINT)ARRAYSIZE(path)) > 0) {
        letter = upper_letter(path[0]);
        if (letter != L'\0') {
            return letter;
        }
    }
    return L'\0';
}

/* 枚举一个卷；卷不存在（DRIVE_NO_ROOT_DIR）时直接跳过。 */
static void collect_volume(StorageInfo *info, wchar_t letter,
                           wchar_t system_letter, StorageWarnState *warn,
                           SystemReport *report)
{
    VolumeInfo *volume;
    wchar_t root[4];
    ULARGE_INTEGER free_for_caller;
    ULARGE_INTEGER total_bytes;
    ULARGE_INTEGER free_bytes;
    UINT drive_type;

    root[0] = letter;
    root[1] = L':';
    root[2] = L'\\';
    root[3] = L'\0';

    drive_type = GetDriveTypeW(root);
    if (drive_type == DRIVE_NO_ROOT_DIR) {
        return;
    }

    if (info->volume_count >= (unsigned)PSI_MAX_VOLUMES) {
        storage_warn(warn, report, L"volume_limit",
                     L"卷数量超过上限 %u，后续卷将被忽略。",
                     (unsigned)PSI_MAX_VOLUMES);
        return;
    }

    volume = &info->volumes[info->volume_count];
    psi_zero_memory(volume, sizeof(*volume));
    volume->letter[0] = letter;
    volume->letter[1] = L':';
    volume->letter[2] = L'\0';
    volume_type_text(drive_type, volume->type, ARRAYSIZE(volume->type));

    /* 卷标 / 文件系统：失败也保留卷，字段留空。 */
    volume->label[0] = L'\0';
    volume->filesystem[0] = L'\0';
    if (GetVolumeInformationW(root,
                              volume->label, (DWORD)ARRAYSIZE(volume->label),
                              NULL, NULL, NULL,
                              volume->filesystem,
                              (DWORD)ARRAYSIZE(volume->filesystem))) {
        volume->label[ARRAYSIZE(volume->label) - 1] = L'\0';
        volume->filesystem[ARRAYSIZE(volume->filesystem) - 1] = L'\0';
        psi_str_trim(volume->label);
        psi_str_trim(volume->filesystem);
    }

    /* 容量：光驱/网络驱动器可能失败，按契约记 0；固定卷失败给出一次警告。 */
    free_for_caller.QuadPart = 0;
    total_bytes.QuadPart = 0;
    free_bytes.QuadPart = 0;
    if (GetDiskFreeSpaceExW(root, &free_for_caller, &total_bytes, &free_bytes)) {
        volume->total_bytes = (unsigned long long)total_bytes.QuadPart;
        volume->free_bytes = (unsigned long long)free_bytes.QuadPart;
    } else {
        volume->total_bytes = 0;
        volume->free_bytes = 0;
        if (drive_type == DRIVE_FIXED) {
            storage_warn(warn, report, L"volume_space",
                         L"卷 %c: 的容量信息读取失败，已记为 0。", letter);
        }
    }

    /* 固定/可移动卷：关联到物理磁盘号，供 is_system 标记与报告展示。 */
    volume->disk_index = PSI_DISK_INDEX_UNKNOWN;
    if (drive_type == DRIVE_FIXED || drive_type == DRIVE_REMOVABLE) {
        unsigned disk_index = query_volume_disk_index(letter);
        volume->disk_index = disk_index;
        if (disk_index != PSI_DISK_INDEX_UNKNOWN) {
            if (upper_letter(letter) != L'\0' &&
                upper_letter(letter) == system_letter) {
                mark_system_disk(info, disk_index);
            }
        } else if (drive_type == DRIVE_FIXED) {
            storage_warn(warn, report, L"volume_disk",
                         L"卷 %c: 无法关联到物理磁盘号，磁盘关联信息未知。",
                         letter);
        }
    }

    ++info->volume_count;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

BOOL psi_storage_collect(StorageInfo *info, SystemReport *report)
{
    StorageWarnState warn_state;
    wchar_t system_letter;
    unsigned i;

    if (info == NULL) {
        return FALSE;
    }

    psi_zero_memory(info, sizeof(*info));
    psi_zero_memory(&warn_state, sizeof(warn_state));

    system_letter = detect_system_letter();

    collect_disks(info, &warn_state, report);

    for (i = 0; i < 26; ++i) {
        collect_volume(info, (wchar_t)(L'A' + (wchar_t)i), system_letter,
                       &warn_state, report);
    }
    if (info->volume_count == 0) {
        storage_warn(&warn_state, report, L"no_volumes",
                     L"未能枚举到任何卷。");
    }

    return TRUE;
}
