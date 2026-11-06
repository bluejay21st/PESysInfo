/*
 * firmware.c - 固件（BIOS）/ 整机 / 主板 / 机箱信息采集
 *
 * 数据来源优先级：
 *   1. SMBIOS（由 probe.c 统一加载后传入）：
 *        type 0  BIOS Information
 *        type 1  System Information
 *        type 2  Baseboard Information
 *        type 3  System Enclosure / Chassis
 *   2. 注册表 HKLM\HARDWARE\DESCRIPTION\System\BIOS（WinPE 下同样存在）
 *
 * 只填写 SystemInfo 中本模块负责的字段，绝不动 system.c 负责的
 * OS / 计算机名 / 域 / 用户 / 固件模式等字段。所有字符串统一 UTF-16，
 * 中文仅出现在 L"..." 字面量中。
 */
#include "pesysinfo.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define PSI_BIOS_REG_KEY L"HARDWARE\\DESCRIPTION\\System\\BIOS"

/* 结构长度是否至少覆盖到 needed 字节（needed 为该字段之后的下一个偏移）。 */
static BOOL structure_has(const BYTE *structure, size_t needed)
{
    return (structure != NULL && (size_t)structure[1] >= needed) ? TRUE : FALSE;
}

/* 按结构内字符串编号字段 offset 取出 OEM 字符串并转为宽字符。 */
static void copy_structure_string(wchar_t *dst, size_t cch,
                                  const BYTE *structure, BYTE offset)
{
    if (dst == NULL || cch == 0) {
        return;
    }
    dst[0] = L'\0';
    if (structure == NULL) {
        return;
    }
    psi_smbios_to_wide(psi_smbios_string(structure, offset), dst, cch);
}

/* SMBIOS 字段为空时，用注册表 HKLM\HARDWARE\DESCRIPTION\System\BIOS 兜底。 */
static void registry_fallback(wchar_t *field, size_t cch,
                              const wchar_t *value_name)
{
    wchar_t buffer[PSI_TEXT_CCH];

    if (field == NULL || cch == 0 || value_name == NULL) {
        return;
    }
    if (!psi_str_is_empty(field)) {
        return;
    }
    buffer[0] = L'\0';
    if (!psi_reg_read_string(HKEY_LOCAL_MACHINE, PSI_BIOS_REG_KEY, value_name,
                             buffer, ARRAYSIZE(buffer))) {
        return;
    }
    psi_str_trim(buffer);
    if (!psi_str_is_empty(buffer)) {
        psi_str_copy(field, cch, buffer);
    }
}

/*
 * SMBIOS type 1 UUID 按规范格式化：前三个字段小端，后两个字段大端。
 * 全 0 / 全 0xFF 视为未写入。
 */
static void fill_uuid(wchar_t *dst, size_t cch, const BYTE *system_structure)
{
    const BYTE *uuid;
    BOOL all_zero = TRUE;
    BOOL all_ff = TRUE;
    unsigned i;
    unsigned data1;
    unsigned data2;
    unsigned data3;

    if (dst == NULL || cch == 0) {
        return;
    }
    dst[0] = L'\0';
    if (!structure_has(system_structure, 0x18)) {
        return;
    }
    uuid = system_structure + 0x08;
    for (i = 0; i < 16; ++i) {
        if (uuid[i] != 0x00) {
            all_zero = FALSE;
        }
        if (uuid[i] != 0xFF) {
            all_ff = FALSE;
        }
    }
    if (all_zero || all_ff) {
        return;
    }
    data1 = (unsigned)uuid[0] | ((unsigned)uuid[1] << 8) |
            ((unsigned)uuid[2] << 16) | ((unsigned)uuid[3] << 24);
    data2 = (unsigned)uuid[4] | ((unsigned)uuid[5] << 8);
    data3 = (unsigned)uuid[6] | ((unsigned)uuid[7] << 8);
    swprintf_s(dst, cch,
               L"%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
               data1, data2, data3,
               (unsigned)uuid[8], (unsigned)uuid[9],
               (unsigned)uuid[10], (unsigned)uuid[11],
               (unsigned)uuid[12], (unsigned)uuid[13],
               (unsigned)uuid[14], (unsigned)uuid[15]);
}

/* SMBIOS type 3 机箱类型（已去掉 lock 标志位）到中文名称。 */
static const wchar_t *chassis_type_name(BYTE type)
{
    switch (type) {
    case 0x01: return L"其它";
    case 0x02: return L"未知";
    case 0x03: return L"台式机";
    case 0x04: return L"薄型台式机";
    case 0x05: return L"披萨盒式";
    case 0x06: return L"迷你塔式";
    case 0x07: return L"塔式";
    case 0x08: return L"便携式";
    case 0x09: return L"膝上型";
    case 0x0A: return L"笔记本";
    case 0x0B: return L"手持式";
    case 0x0C: return L"扩展坞";
    case 0x0D: return L"一体机";
    case 0x0E: return L"超便携笔记本";
    case 0x0F: return L"节省空间型";
    case 0x10: return L"饭盒式";
    case 0x11: return L"主服务器机箱";
    case 0x12: return L"扩展机箱";
    case 0x13: return L"子机箱";
    case 0x14: return L"总线扩展机箱";
    case 0x15: return L"外设机箱";
    case 0x16: return L"RAID 机箱";
    case 0x17: return L"机架式";
    case 0x18: return L"密封机箱式 PC";
    case 0x19: return L"多系统机箱";
    case 0x1A: return L"CompactPCI";
    case 0x1B: return L"AdvancedTCA";
    case 0x1C: return L"刀片服务器";
    case 0x1D: return L"刀片机箱";
    case 0x1E: return L"平板电脑";
    case 0x1F: return L"可转换式";
    case 0x20: return L"可拆卸式";
    case 0x21: return L"IoT 网关";
    case 0x22: return L"嵌入式 PC";
    case 0x23: return L"迷你 PC";
    case 0x24: return L"棒式 PC";
    default:   return NULL;
    }
}

static BOOL has_any_firmware_info(const SystemInfo *info)
{
    return (!psi_str_is_empty(info->bios_vendor) ||
            !psi_str_is_empty(info->bios_version) ||
            !psi_str_is_empty(info->bios_date) ||
            !psi_str_is_empty(info->manufacturer) ||
            !psi_str_is_empty(info->product) ||
            !psi_str_is_empty(info->system_version) ||
            !psi_str_is_empty(info->serial) ||
            !psi_str_is_empty(info->uuid) ||
            !psi_str_is_empty(info->sku) ||
            !psi_str_is_empty(info->chassis) ||
            !psi_str_is_empty(info->board_manufacturer) ||
            !psi_str_is_empty(info->board_product) ||
            !psi_str_is_empty(info->board_version) ||
            !psi_str_is_empty(info->board_serial)) ? TRUE : FALSE;
}

BOOL psi_firmware_collect(SystemInfo *info, const SmbiosData *smbios,
                          SystemReport *report)
{
    const BYTE *bios = NULL;
    const BYTE *system = NULL;
    const BYTE *board = NULL;
    const BYTE *chassis = NULL;
    BOOL have_smbios;

    if (info == NULL) {
        return FALSE;
    }

    /* 只清空本模块负责的字段；OS / 计算机名等由 system.c 填写。 */
    info->manufacturer[0] = L'\0';
    info->product[0] = L'\0';
    info->system_version[0] = L'\0';
    info->serial[0] = L'\0';
    info->uuid[0] = L'\0';
    info->sku[0] = L'\0';
    info->chassis[0] = L'\0';
    info->bios_vendor[0] = L'\0';
    info->bios_version[0] = L'\0';
    info->bios_date[0] = L'\0';
    info->board_manufacturer[0] = L'\0';
    info->board_product[0] = L'\0';
    info->board_version[0] = L'\0';
    info->board_serial[0] = L'\0';

    have_smbios = (smbios != NULL && smbios->valid == TRUE) ? TRUE : FALSE;
    if (have_smbios) {
        bios = psi_smbios_find(smbios, 0x00, 0);
        system = psi_smbios_find(smbios, 0x01, 0);
        board = psi_smbios_find(smbios, 0x02, 0);
        chassis = psi_smbios_find(smbios, 0x03, 0);
    }

    /* type 0: BIOS Information */
    copy_structure_string(info->bios_vendor,
                          ARRAYSIZE(info->bios_vendor), bios, 0x04);
    copy_structure_string(info->bios_version,
                          ARRAYSIZE(info->bios_version), bios, 0x05);
    copy_structure_string(info->bios_date,
                          ARRAYSIZE(info->bios_date), bios, 0x08);

    /* type 1: System Information */
    copy_structure_string(info->manufacturer,
                          ARRAYSIZE(info->manufacturer), system, 0x04);
    copy_structure_string(info->product,
                          ARRAYSIZE(info->product), system, 0x05);
    copy_structure_string(info->system_version,
                          ARRAYSIZE(info->system_version), system, 0x06);
    copy_structure_string(info->serial,
                          ARRAYSIZE(info->serial), system, 0x07);
    fill_uuid(info->uuid, ARRAYSIZE(info->uuid), system);
    /* 0x18 是 Wake-up Type（数值），SKU Number 字符串编号在 0x19。 */
    copy_structure_string(info->sku, ARRAYSIZE(info->sku), system, 0x19);

    /* type 2: Baseboard Information */
    copy_structure_string(info->board_manufacturer,
                          ARRAYSIZE(info->board_manufacturer), board, 0x04);
    copy_structure_string(info->board_product,
                          ARRAYSIZE(info->board_product), board, 0x05);
    copy_structure_string(info->board_version,
                          ARRAYSIZE(info->board_version), board, 0x06);
    copy_structure_string(info->board_serial,
                          ARRAYSIZE(info->board_serial), board, 0x07);

    /* type 3: System Enclosure / Chassis（0x05 低 7 位为类型，最高位为 lock 标志） */
    if (structure_has(chassis, 0x06)) {
        BYTE chassis_code = (BYTE)(chassis[0x05] & 0x7Fu);
        const wchar_t *chassis_name = chassis_type_name(chassis_code);
        if (chassis_name != NULL) {
            psi_str_copy(info->chassis, ARRAYSIZE(info->chassis), chassis_name);
        } else {
            swprintf_s(info->chassis, ARRAYSIZE(info->chassis),
                       L"未知(0x%02X)", (unsigned)chassis_code);
        }
    }

    /* SMBIOS 缺失或字段为空时，用注册表兜底（键值名与固件类型一一对应）。 */
    registry_fallback(info->bios_vendor,
                      ARRAYSIZE(info->bios_vendor), L"BIOSVendor");
    registry_fallback(info->bios_version,
                      ARRAYSIZE(info->bios_version), L"BIOSVersion");
    registry_fallback(info->bios_date,
                      ARRAYSIZE(info->bios_date), L"BIOSReleaseDate");
    registry_fallback(info->manufacturer,
                      ARRAYSIZE(info->manufacturer), L"SystemManufacturer");
    registry_fallback(info->product,
                      ARRAYSIZE(info->product), L"SystemProductName");
    registry_fallback(info->system_version,
                      ARRAYSIZE(info->system_version), L"SystemVersion");
    registry_fallback(info->serial,
                      ARRAYSIZE(info->serial), L"SystemSerialNumber");
    registry_fallback(info->board_manufacturer,
                      ARRAYSIZE(info->board_manufacturer),
                      L"BaseBoardManufacturer");
    registry_fallback(info->board_product,
                      ARRAYSIZE(info->board_product), L"BaseBoardProduct");
    registry_fallback(info->board_version,
                      ARRAYSIZE(info->board_version), L"BaseBoardVersion");

    if (!has_any_firmware_info(info)) {
        if (report != NULL) {
            psi_add_warning(report, L"未能从 SMBIOS 或注册表读取固件与主板信息");
        }
        return FALSE;
    }
    return TRUE;
}
