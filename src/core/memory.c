/*
 * memory.c - 物理内存 + SMBIOS 内存条信息采集
 *
 * 数据来源：
 *   - GlobalMemoryStatusEx            ：物理内存总量与可用量
 *   - GetPhysicallyInstalledSystemMemory：已安装物理内存（KB），失败时用
 *                                       SMBIOS type 17 容量求和兜底
 *   - SMBIOS type 16                  ：内存阵列最大容量/ECC
 *   - SMBIOS type 17（Memory Device） ：插槽数、容量、类型、形态、Rank、
 *                                       位宽、电压、频率、厂商、型号、序列号
 *
 * WinPE 10.0.19041 下没有 WMI / PowerShell，全部失败路径都必须安全降级，
 * 输出结构在使用前已整体清零，保证不会出现未初始化字段。
 */
#include "pesysinfo.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* SMBIOS type 17 字段偏移 */
#define PSI_MEM_DATA_WIDTH         0x0A
#define PSI_MEM_DEVICE_LOCATOR     0x10
#define PSI_MEM_BANK_LOCATOR       0x11
#define PSI_MEM_TYPE               0x12
#define PSI_MEM_TYPE_DETAIL        0x13
#define PSI_MEM_SPEED              0x15
#define PSI_MEM_MANUFACTURER       0x17
#define PSI_MEM_SERIAL             0x18
#define PSI_MEM_PART_NUMBER        0x1A
#define PSI_MEM_ATTRIBUTES         0x1B
#define PSI_MEM_CONFIGURED_SPEED   0x20
#define PSI_MEM_MIN_VOLTAGE        0x22
#define PSI_MEM_MAX_VOLTAGE        0x24
#define PSI_MEM_CONFIGURED_VOLTAGE 0x26
#define PSI_MEM_FORM_FACTOR        0x0E

/* 按结构长度安全读取一个字段；结构不够长时返回 0。 */
static unsigned read_word_field(const BYTE *structure, size_t offset)
{
    WORD value = 0;

    if (structure == NULL) {
        return 0;
    }
    if ((size_t)structure[1] < offset + sizeof(WORD)) {
        return 0;
    }
    memcpy(&value, structure + offset, sizeof(value));
    return (unsigned)value;
}

static unsigned read_byte_field(const BYTE *structure, size_t offset)
{
    if (structure == NULL || (size_t)structure[1] <= offset) {
        return 0;
    }
    return (unsigned)structure[offset];
}

static DWORD read_dword_field(const BYTE *structure, size_t offset)
{
    DWORD value = 0;

    if (structure == NULL || (size_t)structure[1] < offset + sizeof(DWORD)) {
        return 0;
    }
    memcpy(&value, structure + offset, sizeof(value));
    return value;
}

static ULONGLONG read_qword_field(const BYTE *structure, size_t offset)
{
    ULONGLONG value = 0;

    if (structure == NULL || (size_t)structure[1] < offset + sizeof(ULONGLONG)) {
        return 0;
    }
    memcpy(&value, structure + offset, sizeof(value));
    return value;
}

/* SMBIOS 内存类型编码（DSP0134 3.x 规范表）到显示名称。 */
static const wchar_t *memory_type_name(BYTE type)
{
    switch (type) {
    case 0x01: return L"其它";
    case 0x02: return L"未知";
    case 0x03: return L"DRAM";
    case 0x04: return L"EDRAM";
    case 0x05: return L"VRAM";
    case 0x06: return L"SRAM";
    case 0x07: return L"RAM";
    case 0x08: return L"ROM";
    case 0x09: return L"Flash";
    case 0x0A: return L"EEPROM";
    case 0x0B: return L"FEPROM";
    case 0x0C: return L"EPROM";
    case 0x0D: return L"CDRAM";
    case 0x0E: return L"3DRAM";
    case 0x0F: return L"SDRAM";
    case 0x10: return L"SGRAM";
    case 0x11: return L"RDRAM";
    case 0x12: return L"DDR";
    case 0x13: return L"DDR2";
    case 0x14: return L"DDR2 FB-DIMM";
    case 0x18: return L"DDR3";
    case 0x19: return L"FBD2";
    case 0x1A: return L"DDR4";
    case 0x1B: return L"LPDDR";
    case 0x1C: return L"LPDDR2";
    case 0x1D: return L"LPDDR3";
    case 0x1E: return L"LPDDR4";
    case 0x1F: return L"LPDDR4X";
    case 0x20: return L"HBM";
    case 0x21: return L"HBM2";
    case 0x22: return L"DDR5";
    case 0x23: return L"LPDDR5";
    case 0x24: return L"HBM3";
    default:   return NULL;
    }
}

static const wchar_t *memory_form_factor_name(BYTE value)
{
    switch (value) {
    case 0x01: return L"其它";
    case 0x02: return L"未知";
    case 0x03: return L"SIMM";
    case 0x04: return L"SIP";
    case 0x05: return L"Chip";
    case 0x06: return L"DIP";
    case 0x07: return L"ZIP";
    case 0x08: return L"Proprietary Card";
    case 0x09: return L"DIMM";
    case 0x0A: return L"TSOP";
    case 0x0B: return L"Row of chips";
    case 0x0C: return L"RIMM";
    case 0x0D: return L"SODIMM";
    case 0x0E: return L"SRIMM";
    case 0x0F: return L"FB-DIMM";
    case 0x10: return L"Die";
    default:   return NULL;
    }
}

static const wchar_t *memory_ecc_type_name(BYTE value)
{
    switch (value) {
    case 0x01: return L"其它";
    case 0x02: return L"未知";
    case 0x03: return L"无";
    case 0x04: return L"Parity";
    case 0x05: return L"单比特 ECC";
    case 0x06: return L"多比特 ECC";
    case 0x07: return L"CRC";
    default:   return NULL;
    }
}

/* 填充一条已安装内存条；structure 必须来自 psi_smbios_find(type 17)。 */
static void fill_module(MemoryModule *module, const BYTE *structure,
                        unsigned long long size_bytes)
{
    BYTE type_code = 0;
    BYTE form_code = 0;
    const wchar_t *type_name;
    const wchar_t *form_name;
    unsigned speed;
    unsigned configured_speed;
    unsigned type_detail;
    unsigned voltage;

    if (module == NULL || structure == NULL) {
        return;
    }

    psi_smbios_to_wide(psi_smbios_string(structure, PSI_MEM_DEVICE_LOCATOR),
                       module->locator, ARRAYSIZE(module->locator));
    psi_smbios_to_wide(psi_smbios_string(structure, PSI_MEM_BANK_LOCATOR),
                       module->bank, ARRAYSIZE(module->bank));

    if ((size_t)structure[1] > PSI_MEM_TYPE) {
        type_code = structure[PSI_MEM_TYPE];
    }
    type_name = memory_type_name(type_code);
    if (type_name != NULL) {
        psi_str_copy(module->type, ARRAYSIZE(module->type), type_name);
    } else {
        swprintf_s(module->type, ARRAYSIZE(module->type),
                   L"未知(0x%02X)", (unsigned)type_code);
    }

    form_code = (BYTE)read_byte_field(structure, PSI_MEM_FORM_FACTOR);
    form_name = memory_form_factor_name(form_code);
    if (form_name != NULL) {
        psi_str_copy(module->form_factor, ARRAYSIZE(module->form_factor),
                     form_name);
    } else if (form_code != 0) {
        swprintf_s(module->form_factor, ARRAYSIZE(module->form_factor),
                   L"未知(0x%02X)", (unsigned)form_code);
    }

    module->size_bytes = size_bytes;

    /* Speed 为 0 / 0xFFFF（未知）时，退回到 Configured Memory Speed。 */
    speed = read_word_field(structure, PSI_MEM_SPEED);
    configured_speed = read_word_field(structure, PSI_MEM_CONFIGURED_SPEED);
    if (configured_speed == 0xFFFFu) {
        configured_speed = 0;
    }
    if (speed == 0 || speed == 0xFFFFu) {
        speed = configured_speed;
    }
    module->speed_mts = speed;
    module->configured_speed_mts = configured_speed;

    module->rank = read_byte_field(structure, PSI_MEM_ATTRIBUTES) & 0x0Fu;
    module->data_width_bits = read_word_field(structure, PSI_MEM_DATA_WIDTH);
    if (module->data_width_bits == 0xFFFFu) {
        module->data_width_bits = 0;
    }

    voltage = read_word_field(structure, PSI_MEM_MIN_VOLTAGE);
    module->voltage_min_mv = (voltage == 0xFFFFu) ? 0 : voltage;
    voltage = read_word_field(structure, PSI_MEM_MAX_VOLTAGE);
    module->voltage_max_mv = (voltage == 0xFFFFu) ? 0 : voltage;
    voltage = read_word_field(structure, PSI_MEM_CONFIGURED_VOLTAGE);
    module->voltage_configured_mv = (voltage == 0xFFFFu) ? 0 : voltage;

    type_detail = read_word_field(structure, PSI_MEM_TYPE_DETAIL);
    /* Type Detail bit 12: Registered (Buffered). LRDIMM has its own bit and
     * is not used here to avoid mislabeling LPDDR/firmware quirks. */
    module->registered = ((type_detail & 0x1000u) != 0) ? TRUE : FALSE;

    psi_smbios_to_wide(psi_smbios_string(structure, PSI_MEM_MANUFACTURER),
                       module->manufacturer, ARRAYSIZE(module->manufacturer));
    psi_smbios_to_wide(psi_smbios_string(structure, PSI_MEM_PART_NUMBER),
                       module->part_number, ARRAYSIZE(module->part_number));
    psi_smbios_to_wide(psi_smbios_string(structure, PSI_MEM_SERIAL),
                       module->serial, ARRAYSIZE(module->serial));
}

BOOL psi_memory_collect(MemoryInfo *info, const SmbiosData *smbios,
                        SystemReport *report)
{
    MEMORYSTATUSEX status;
    ULONGLONG installed_kb = 0;
    BOOL api_ok;
    BOOL have_smbios;
    BOOL module_limit_warned = FALSE;
    unsigned long long smbios_total = 0;
    unsigned slot_total;
    unsigned i;

    if (info == NULL) {
        return FALSE;
    }
    psi_zero_memory(info, sizeof(*info));

    /* 1) 物理内存：API 失败时保持 0（未知）。 */
    status.dwLength = (DWORD)sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        info->total_bytes = status.ullTotalPhys;
        info->available_bytes = status.ullAvailPhys;
    }

    /* 2) 已安装物理内存：优先内核 API（单位 KB）。 */
    api_ok = GetPhysicallyInstalledSystemMemory(&installed_kb);
    if (api_ok) {
        info->installed_bytes = (unsigned long long)installed_kb * 1024ULL;
    }

    /* 3) SMBIOS type 16：内存阵列最大容量/ECC。 */
    have_smbios = (smbios != NULL && smbios->valid == TRUE) ? TRUE : FALSE;

    if (!have_smbios) {
        if (report != NULL) {
            psi_add_warning(report, L"未能读取 SMBIOS，内存条详情不可用");
        }
    } else {
        const BYTE *array = psi_smbios_find(smbios, 0x10, 0);

        if (array != NULL && array[1] >= 0x0F) {
            DWORD max_kb = read_dword_field(array, 0x07);
            unsigned long long max_bytes = 0;
            const wchar_t *ecc_name = memory_ecc_type_name(
                (BYTE)read_byte_field(array, 0x06));

            if (max_kb == 0x80000000u) {
                max_bytes = read_qword_field(array, 0x0F);
            } else if (max_kb != 0 && max_kb != 0xFFFFFFFFu) {
                max_bytes = (unsigned long long)max_kb * 1024ULL;
            }
            info->max_capacity_bytes = max_bytes;
            if (ecc_name != NULL) {
                psi_str_copy(info->ecc_type, ARRAYSIZE(info->ecc_type),
                             ecc_name);
            }
        }

        slot_total = psi_smbios_struct_count(smbios, 0x11);
        info->slot_count = slot_total;

        for (i = 0; i < slot_total; ++i) {
            const BYTE *structure = psi_smbios_find(smbios, 0x11, i);
            unsigned long long size_bytes;

            if (structure == NULL) {
                continue;
            }
            size_bytes = psi_smbios_memory_size(structure);
            if (size_bytes > 0) {
                smbios_total += size_bytes;
            }
            /* Size = 0 是空插槽：计入 slot_count，但不进 modules[]。 */
            if (size_bytes == 0) {
                continue;
            }
            if (info->module_count >= PSI_MAX_MEM_MODULES) {
                if (!module_limit_warned && report != NULL) {
                    psi_add_warning(report,
                                    L"内存条数量超过 %u 条，报告仅显示前 %u 条",
                                    (unsigned)PSI_MAX_MEM_MODULES,
                                    (unsigned)PSI_MAX_MEM_MODULES);
                    module_limit_warned = TRUE;
                }
                continue; /* modules[] 已满，容量求和仍在继续 */
            }
            fill_module(&info->modules[info->module_count], structure,
                        size_bytes);
            ++info->module_count;
        }
    }

    /* 4) GetPhysicallyInstalledSystemMemory 失败（或返回 0）时用 SMBIOS 求和兜底。 */
    if ((!api_ok || info->installed_bytes == 0) && smbios_total > 0) {
        info->installed_bytes = smbios_total;
    }

    return TRUE;
}
