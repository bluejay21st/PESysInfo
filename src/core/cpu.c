/*
 * cpu.c - 处理器信息采集（注册表 + CPUID + 拓扑 API）
 *
 * 约束：
 *  - 仅依赖 kernel32 / advapi32，不使用 WMI、PowerShell、.NET 或外部运行库；
 *  - 目标环境为 Windows PE 10.0.19041 x64/x86 / Windows 10、11 x64/x86；
 *  - 任何失败都降级处理，不向控制台输出，仅通过 psi_add_warning 上报中文提示。
 *
 * 数据来源：
 *  1) HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0
 *       ProcessorNameString / Identifier / VendorIdentifier / ~MHz
 *  2) CPUID：leaf 0（厂商串）、leaf 0x80000002..4（品牌串）、
 *     leaf 0x16（基础/最大频率）、leaf 1 / leaf 7 / leaf 0x80000001（特性位）
 *  3) GetActiveProcessorCount + GetLogicalProcessorInformationEx（逻辑处理器、
 *     物理核心、插槽、L1/L2/L3 缓存）
 */
#include "pesysinfo.h"

#include <intrin.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
} PsiProcessorPowerInformation;

typedef LONG (WINAPI *PsiCallNtPowerInformationFn)(int, PVOID, ULONG, PVOID, ULONG);

#define PSI_PROCESSOR_INFORMATION 11

#define CPU_REG_SUBKEY          L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0"

/* CPUID 叶子号（Intel / AMD 公开规范） */
#define CPU_LEAF_BASIC          0x00000000u
#define CPU_LEAF_FEATURES       0x00000001u
#define CPU_LEAF_FREQUENCY      0x00000016u
#define CPU_LEAF_STRUCT_EXT     0x00000007u
#define CPU_LEAF_EXT_MAX        0x80000000u
#define CPU_LEAF_EXT_FEATURES   0x80000001u
#define CPU_LEAF_BRAND_FIRST    0x80000002u
#define CPU_LEAF_BRAND_LAST     0x80000004u

#define CPU_COUNT_OF(array)     (sizeof(array) / sizeof((array)[0]))

#ifndef ALL_PROCESSOR_GROUPS
#define ALL_PROCESSOR_GROUPS    0xffffu
#endif

/* ------------------------------------------------------------------ */
/* 通用小工具                                                          */
/* ------------------------------------------------------------------ */

/* 注册表/CPUID 取到的文本统一 trim + 合并连续空白 */
static void cpu_text_clean(wchar_t *text)
{
    if (text == NULL) {
        return;
    }
    psi_str_trim(text);
    psi_str_collapse_spaces(text);
}

/* 读取 regs[index] 的指定 bit。regs 顺序固定为 EAX, EBX, ECX, EDX。 */
static BOOL cpu_reg_bit(const int regs[4], int index, unsigned bit)
{
    return ((((unsigned)regs[index]) >> bit) & 1u) ? TRUE : FALSE;
}

/* ------------------------------------------------------------------ */
/* 注册表                                                              */
/* ------------------------------------------------------------------ */

/* 读取 CentralProcessor\0 下的四个值；返回是否至少成功读取到一项。 */
static BOOL cpu_read_registry(CpuInfo *info, DWORD *reg_mhz)
{
    BOOL brand_ok;
    BOOL identifier_ok;
    BOOL vendor_ok;
    BOOL mhz_ok;

    *reg_mhz = 0;

    brand_ok = psi_reg_read_string(HKEY_LOCAL_MACHINE, CPU_REG_SUBKEY,
                                   L"ProcessorNameString", info->brand,
                                   CPU_COUNT_OF(info->brand));
    if (brand_ok) {
        cpu_text_clean(info->brand);
    }

    identifier_ok = psi_reg_read_string(HKEY_LOCAL_MACHINE, CPU_REG_SUBKEY,
                                        L"Identifier", info->identifier,
                                        CPU_COUNT_OF(info->identifier));
    if (identifier_ok) {
        cpu_text_clean(info->identifier);
    }

    vendor_ok = psi_reg_read_string(HKEY_LOCAL_MACHINE, CPU_REG_SUBKEY,
                                    L"VendorIdentifier", info->vendor,
                                    CPU_COUNT_OF(info->vendor));
    if (vendor_ok) {
        cpu_text_clean(info->vendor);
    }

    mhz_ok = psi_reg_read_dword(HKEY_LOCAL_MACHINE, CPU_REG_SUBKEY,
                                L"~MHz", reg_mhz);

    return (brand_ok || identifier_ok || vendor_ok || mhz_ok) ? TRUE : FALSE;
}

/* ------------------------------------------------------------------ */
/* CPUID                                                               */
/* ------------------------------------------------------------------ */

/* leaf 0：返回 12 字节厂商串（GenuineIntel / AuthenticAMD ...）。 */
static BOOL cpu_cpuid_vendor(wchar_t *vendor, size_t cch)
{
    int regs[4];
    unsigned char raw[13];

    if (vendor == NULL || cch == 0) {
        return FALSE;
    }
    vendor[0] = L'\0';

    __cpuid(regs, (int)CPU_LEAF_BASIC);
    memcpy(raw, &regs[1], 4);       /* EBX */
    memcpy(raw + 4, &regs[3], 4);   /* EDX */
    memcpy(raw + 8, &regs[2], 4);   /* ECX */
    raw[12] = '\0';

    /* 厂商串为纯 ASCII，按 ANSI 复制即可 */
    psi_str_copy_ansi(vendor, cch, (const char *)raw);
    cpu_text_clean(vendor);
    return (vendor[0] != L'\0') ? TRUE : FALSE;
}

/* leaf 0x80000002..0x80000004：拼接 48 字节品牌串。 */
static BOOL cpu_cpuid_brand(wchar_t *brand, size_t cch, unsigned max_extended)
{
    int regs[4];
    unsigned char raw[49];
    unsigned offset = 0;
    unsigned leaf;

    if (brand == NULL || cch == 0) {
        return FALSE;
    }
    brand[0] = L'\0';
    if (max_extended < CPU_LEAF_BRAND_LAST) {
        return FALSE;
    }

    for (leaf = CPU_LEAF_BRAND_FIRST; leaf <= CPU_LEAF_BRAND_LAST; ++leaf) {
        __cpuid(regs, (int)leaf);
        memcpy(raw + offset, &regs[0], 4);
        memcpy(raw + offset + 4, &regs[1], 4);
        memcpy(raw + offset + 8, &regs[2], 4);
        memcpy(raw + offset + 12, &regs[3], 4);
        offset += 16;
    }
    raw[48] = '\0';

    psi_str_copy_ansi(brand, cch, (const char *)raw);
    cpu_text_clean(brand);
    return (brand[0] != L'\0') ? TRUE : FALSE;
}

typedef struct {
    BOOL sse2;
    BOOL sse3;
    BOOL ssse3;
    BOOL sse41;
    BOOL sse42;
    BOOL avx;
    BOOL avx2;
    BOOL fma;
    BOOL aes;
    BOOL popcnt;
    BOOL bmi1;
    BOOL bmi2;
    BOOL rtm;
    BOOL nx;
    BOOL vmx;
    BOOL svm;
} CpuFeatureBits;

/* 按 Intel / AMD 公开的 CPUID 位布局汇总特性。 */
static void cpu_collect_feature_bits(CpuFeatureBits *bits, unsigned max_basic,
                                     unsigned max_extended)
{
    int regs[4];

    psi_zero_memory(bits, sizeof(*bits));
    psi_zero_memory(regs, sizeof(regs));

    /* leaf 1：基础指令集 / 虚拟化 / NX */
    if (max_basic >= CPU_LEAF_FEATURES) {
        __cpuid(regs, (int)CPU_LEAF_FEATURES);
        bits->sse2    = cpu_reg_bit(regs, 3, 26);   /* EDX bit26 */
        bits->sse3    = cpu_reg_bit(regs, 2, 0);    /* ECX bit0  */
        bits->ssse3   = cpu_reg_bit(regs, 2, 9);    /* ECX bit9  */
        bits->sse41   = cpu_reg_bit(regs, 2, 19);   /* ECX bit19 */
        bits->sse42   = cpu_reg_bit(regs, 2, 20);   /* ECX bit20 */
        bits->avx     = cpu_reg_bit(regs, 2, 28);   /* ECX bit28 */
        bits->fma     = cpu_reg_bit(regs, 2, 12);   /* ECX bit12 */
        bits->aes     = cpu_reg_bit(regs, 2, 25);   /* ECX bit25 */
        bits->popcnt  = cpu_reg_bit(regs, 2, 23);   /* ECX bit23 */
        bits->vmx     = cpu_reg_bit(regs, 2, 5);    /* ECX bit5  */
        bits->nx      = cpu_reg_bit(regs, 3, 20);   /* EDX bit20 (XD) */
    }

    /* leaf 7 subleaf 0：AVX2 / BMI / RTM */
    if (max_basic >= CPU_LEAF_STRUCT_EXT) {
        __cpuidex(regs, (int)CPU_LEAF_STRUCT_EXT, 0);
        bits->avx2 = cpu_reg_bit(regs, 1, 5);       /* EBX bit5  */
        bits->bmi1 = cpu_reg_bit(regs, 1, 3);       /* EBX bit3  */
        bits->bmi2 = cpu_reg_bit(regs, 1, 8);       /* EBX bit8  */
        bits->rtm  = cpu_reg_bit(regs, 1, 11);      /* EBX bit11 */
    }

    /* leaf 0x80000001：SVM（AMD-V）/ NX */
    if (max_extended >= CPU_LEAF_EXT_FEATURES) {
        __cpuid(regs, (int)CPU_LEAF_EXT_FEATURES);
        if (cpu_reg_bit(regs, 2, 2)) {              /* ECX bit2  */
            bits->svm = TRUE;
        }
        if (cpu_reg_bit(regs, 3, 20)) {             /* EDX bit20 */
            bits->nx = TRUE;
        }
    }
}

/* 追加一项特性名；空间不足时整项丢弃，保证最终串合法。 */
static void cpu_append_feature(wchar_t *buffer, size_t cch, size_t *length,
                               const wchar_t *name)
{
    size_t name_len;
    size_t separator;

    if (buffer == NULL || length == NULL || name == NULL || cch == 0) {
        return;
    }
    name_len = wcslen(name);
    separator = (*length > 0) ? 2u : 0u;
    if (*length + separator + name_len + 1u > cch) {
        return;
    }
    if (separator > 0) {
        buffer[(*length)++] = L',';
        buffer[(*length)++] = L' ';
    }
    memcpy(buffer + *length, name, name_len * sizeof(wchar_t));
    *length += name_len;
    buffer[*length] = L'\0';
}

/* 生成逗号分隔的英文特性名；顺序固定，便于输出稳定。 */
static void cpu_features_to_text(const CpuFeatureBits *bits, wchar_t *text,
                                 size_t cch)
{
    size_t length = 0;

    if (text == NULL || cch == 0) {
        return;
    }
    text[0] = L'\0';

    if (bits->sse2)   cpu_append_feature(text, cch, &length, L"SSE2");
    if (bits->sse3)   cpu_append_feature(text, cch, &length, L"SSE3");
    if (bits->ssse3)  cpu_append_feature(text, cch, &length, L"SSSE3");
    if (bits->sse41)  cpu_append_feature(text, cch, &length, L"SSE4.1");
    if (bits->sse42)  cpu_append_feature(text, cch, &length, L"SSE4.2");
    if (bits->avx)    cpu_append_feature(text, cch, &length, L"AVX");
    if (bits->avx2)   cpu_append_feature(text, cch, &length, L"AVX2");
    if (bits->fma)    cpu_append_feature(text, cch, &length, L"FMA");
    if (bits->bmi1)   cpu_append_feature(text, cch, &length, L"BMI1");
    if (bits->bmi2)   cpu_append_feature(text, cch, &length, L"BMI2");
    if (bits->aes)    cpu_append_feature(text, cch, &length, L"AES");
    if (bits->popcnt) cpu_append_feature(text, cch, &length, L"POPCNT");
    if (bits->rtm)    cpu_append_feature(text, cch, &length, L"RTM");
    if (bits->nx)     cpu_append_feature(text, cch, &length, L"NX");
    if (bits->vmx)    cpu_append_feature(text, cch, &length, L"VT-x");
    if (bits->svm)    cpu_append_feature(text, cch, &length, L"AMD-V");
}

/* 频率：优先 leaf 0x16，基础频率用注册表 ~MHz 兜底，最大频率拿不到则留 0。 */
static void cpu_collect_frequencies(CpuInfo *info, unsigned max_basic,
                                    DWORD reg_mhz)
{
    int regs[4];
    unsigned base = 0;
    unsigned max = 0;

    if (max_basic >= CPU_LEAF_FREQUENCY) {
        __cpuid(regs, (int)CPU_LEAF_FREQUENCY);
        base = (unsigned)regs[0];                   /* EAX：基础频率 MHz */
        max = (unsigned)regs[1];                    /* EBX：最大频率 MHz */
    }
    if (base == 0) {
        base = (unsigned)reg_mhz;
    }
    info->base_mhz = base;
    info->max_mhz = max;
}

/* SMBIOS type 4：CPU 频率/核心数/线程数的固件侧兜底。
 * 对 AMD 等没有 CPUID leaf 0x16 的平台，Max Speed 尤其有用。 */
static unsigned cpu_smbios_word(const BYTE *structure, size_t offset)
{
    WORD value = 0;

    if (structure == NULL || (size_t)structure[1] < offset + sizeof(WORD)) {
        return 0;
    }
    memcpy(&value, structure + offset, sizeof(value));
    return (unsigned)value;
}

static void cpu_apply_smbios(CpuInfo *info, const SmbiosData *smbios)
{
    const BYTE *processor;
    unsigned max_speed;
    unsigned current_speed;
    unsigned core_count2;
    unsigned thread_count2;

    if (info == NULL || smbios == NULL || !smbios->valid) {
        return;
    }
    processor = psi_smbios_find(smbios, 0x04, 0);
    if (processor == NULL) {
        return;
    }

    max_speed = cpu_smbios_word(processor, 0x14);
    current_speed = cpu_smbios_word(processor, 0x16);
    if (max_speed == 0xFFFFu) {
        max_speed = 0;
    }
    if (current_speed == 0xFFFFu) {
        current_speed = 0;
    }
    if (info->max_mhz == 0 && max_speed > 0) {
        info->max_mhz = max_speed;
    }
    if (info->base_mhz == 0 && current_speed > 0) {
        info->base_mhz = current_speed;
    }

    /* SMBIOS 2.5+ 的 Core Count / Thread Count 是 BYTE；SMBIOS 3.0+
     * 还有 WORD 扩展字段。只在 API 未得到结果时兜底。 */
    core_count2 = cpu_smbios_word(processor, 0x2A);
    thread_count2 = cpu_smbios_word(processor, 0x2E);
    if (info->cores == 0) {
        if (core_count2 > 0) {
            info->cores = core_count2;
        } else if (processor[1] >= 0x24) {
            info->cores = processor[0x23];
        }
    }
    if (info->logical == 0) {
        if (thread_count2 > 0) {
            info->logical = thread_count2;
        } else if (processor[1] >= 0x26) {
            info->logical = processor[0x25];
        }
    }
}

/* 当前频率快照：动态加载 powrprof.dll，避免增加硬导入依赖。
 * 返回 FALSE 时不修改 CpuInfo，界面显示“未知”。 */
static BOOL cpu_query_power_frequency(unsigned requested_count,
                                      unsigned *average_mhz,
                                      unsigned *max_mhz)
{
    HMODULE library;
    PsiCallNtPowerInformationFn call_nt_power_information;
    PsiProcessorPowerInformation *buffer;
    unsigned count;
    unsigned i;
    unsigned long long sum = 0;
    unsigned samples = 0;
    unsigned local_max = 0;
    LONG status;

    count = requested_count;
    if (count == 0) {
        count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    }
    if (count == 0 || count > 1024) {
        return FALSE;
    }
    library = LoadLibraryW(L"powrprof.dll");
    if (library == NULL) {
        return FALSE;
    }
    call_nt_power_information = (PsiCallNtPowerInformationFn)(void *)
        GetProcAddress(library, "CallNtPowerInformation");
    if (call_nt_power_information == NULL) {
        FreeLibrary(library);
        return FALSE;
    }
    buffer = (PsiProcessorPowerInformation *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY,
        (SIZE_T)count * sizeof(PsiProcessorPowerInformation));
    if (buffer == NULL) {
        FreeLibrary(library);
        return FALSE;
    }
    status = call_nt_power_information(
        PSI_PROCESSOR_INFORMATION, NULL, 0, buffer,
        count * (unsigned)sizeof(PsiProcessorPowerInformation));
    if (status == 0) {
        for (i = 0; i < count; ++i) {
            if (buffer[i].CurrentMhz > 0) {
                sum += buffer[i].CurrentMhz;
                ++samples;
            }
            if (buffer[i].MaxMhz > local_max) {
                local_max = buffer[i].MaxMhz;
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, buffer);
    FreeLibrary(library);
    if (status != 0 || samples == 0) {
        return FALSE;
    }
    if (average_mhz != NULL) {
        *average_mhz = (unsigned)(sum / samples);
    }
    if (max_mhz != NULL) {
        *max_mhz = local_max;
    }
    return TRUE;
}

/* 采集一次当前平均频率快照。 */
static BOOL cpu_collect_current_frequency(CpuInfo *info)
{
    unsigned average = 0;
    unsigned max_speed = 0;

    if (info == NULL) {
        return FALSE;
    }
    if (!cpu_query_power_frequency(info->logical, &average, &max_speed)) {
        return FALSE;
    }
    info->current_mhz = average;
    if (info->max_mhz == 0 && max_speed > 0) {
        info->max_mhz = max_speed;
    }
    return TRUE;
}

/* 先探测所需缓冲区长度，再分配并二次调用。 */
static BOOL cpu_query_topology(LOGICAL_PROCESSOR_RELATIONSHIP relationship,
                               PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *buffer,
                               DWORD *buffer_size)
{
    DWORD size = 0;
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX data;

    *buffer = NULL;
    *buffer_size = 0;

    if (GetLogicalProcessorInformationEx(relationship, NULL, &size)) {
        return FALSE;   /* 关系类型无数据时不会返回缓冲需求 */
    }
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return FALSE;
    }

    data = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)malloc((size_t)size);
    if (data == NULL) {
        return FALSE;
    }
    if (!GetLogicalProcessorInformationEx(relationship, data, &size)) {
        free(data);
        return FALSE;
    }

    *buffer = data;
    *buffer_size = size;
    return TRUE;
}

/* 统计指定关系（ProcessorCore / ProcessorPackage）的记录条数。 */
static BOOL cpu_count_relation(LOGICAL_PROCESSOR_RELATIONSHIP relationship,
                               unsigned *count)
{
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buffer = NULL;
    DWORD size = 0;
    DWORD offset = 0;
    unsigned total = 0;

    *count = 0;
    if (!cpu_query_topology(relationship, &buffer, &size)) {
        return FALSE;
    }

    while (offset < size) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX entry =
            (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)((BYTE *)buffer + offset);

        if (entry->Size == 0 || entry->Size > (size - offset)) {
            break;      /* 防御异常记录，避免越界 */
        }
        if (entry->Relationship == relationship) {
            ++total;
        }
        offset += entry->Size;
    }

    free(buffer);
    *count = total;
    return (total > 0) ? TRUE : FALSE;
}

typedef struct {
    unsigned long long l1_bytes;
    unsigned long long l2_bytes;
    unsigned long long l3_bytes;
} CpuCacheBytes;

/* 按 Level 累加 CacheSize（字节）。L1 会同时包含数据缓存与指令缓存记录。 */
static BOOL cpu_collect_cache_bytes(CpuCacheBytes *bytes)
{
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buffer = NULL;
    DWORD size = 0;
    DWORD offset = 0;
    BOOL ok;

    psi_zero_memory(bytes, sizeof(*bytes));
    ok = cpu_query_topology(RelationCache, &buffer, &size);
    if (!ok) {
        return FALSE;
    }

    while (offset < size) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX entry =
            (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)((BYTE *)buffer + offset);

        if (entry->Size == 0 || entry->Size > (size - offset)) {
            break;
        }
        if (entry->Relationship == RelationCache) {
            unsigned long long cache_size =
                (unsigned long long)entry->Cache.CacheSize;
            switch ((unsigned)entry->Cache.Level) {
            case 1:
                bytes->l1_bytes += cache_size;
                break;
            case 2:
                bytes->l2_bytes += cache_size;
                break;
            case 3:
                bytes->l3_bytes += cache_size;
                break;
            default:
                break;
            }
        }
        offset += entry->Size;
    }

    free(buffer);
    return (bytes->l1_bytes + bytes->l2_bytes + bytes->l3_bytes) > 0
               ? TRUE : FALSE;
}

/* 逻辑处理器数：优先 GetActiveProcessorCount（支持 >64 逻辑处理器/多处理器组），
 * 失败时退回 GetSystemInfo。返回 TRUE 表示使用了首选 API。 */
static BOOL cpu_logical_processors(unsigned *logical)
{
    DWORD count;
    SYSTEM_INFO system_info;

    count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (count > 0) {
        *logical = (unsigned)count;
        return TRUE;
    }

    psi_zero_memory(&system_info, sizeof(system_info));
    GetSystemInfo(&system_info);
    *logical = (unsigned)system_info.dwNumberOfProcessors;
    if (*logical == 0) {
        *logical = 1;
    }
    return FALSE;
}

/* 进程架构。32 位进程运行在 64 位系统时标注 WOW64，方便读取方判断。 */
static void cpu_set_arch(CpuInfo *info)
{
#if defined(_M_X64) || defined(_M_AMD64)
    psi_str_copy(info->arch, CPU_COUNT_OF(info->arch), L"x64");
#elif defined(_M_IX86)
    {
        BOOL wow64 = FALSE;

        if (IsWow64Process(GetCurrentProcess(), &wow64) && wow64) {
            psi_str_copy(info->arch, CPU_COUNT_OF(info->arch), L"x86 (WOW64)");
        } else {
            psi_str_copy(info->arch, CPU_COUNT_OF(info->arch), L"x86");
        }
    }
#else
    psi_str_copy(info->arch, CPU_COUNT_OF(info->arch), L"unknown");
#endif
}

/* ------------------------------------------------------------------ */
/* 对外入口                                                            */
/* ------------------------------------------------------------------ */

BOOL psi_cpu_collect(CpuInfo *info, const SmbiosData *smbios,
                     SystemReport *report)
{
    DWORD reg_mhz = 0;
    unsigned max_basic = 0;
    unsigned max_extended = 0;
    int regs[4];
    BOOL reg_ok;
    BOOL cpuid_ok;
    BOOL logical_api_ok;
    BOOL cores_ok;
    BOOL sockets_ok;
    BOOL cache_ok;
    BOOL got_identity;
    wchar_t cpuid_vendor[64];
    wchar_t cpuid_brand[PSI_TEXT_CCH];
    CpuFeatureBits features;
    CpuCacheBytes cache;

    if (info == NULL) {
        return FALSE;
    }
    psi_zero_memory(info, sizeof(*info));

    /* 1) 注册表：在 WinPE 中通常由 PnP 枚举写入，是最省事的型号来源。 */
    reg_ok = cpu_read_registry(info, &reg_mhz);
    if (!reg_ok && report != NULL) {
        psi_add_warning(report,
                        L"无法读取处理器注册表信息，已改用 CPUID 检测数据");
    }

    /* 2) CPUID：厂商串、品牌串、频率、特性位。 */
    psi_zero_memory(cpuid_vendor, sizeof(cpuid_vendor));
    psi_zero_memory(cpuid_brand, sizeof(cpuid_brand));
    cpuid_ok = cpu_cpuid_vendor(cpuid_vendor, CPU_COUNT_OF(cpuid_vendor));
    if (cpuid_ok) {
        psi_zero_memory(regs, sizeof(regs));
        __cpuid(regs, (int)CPU_LEAF_BASIC);
        max_basic = (unsigned)regs[0];
        __cpuid(regs, (int)CPU_LEAF_EXT_MAX);
        max_extended = (unsigned)regs[0];

        if (psi_str_is_empty(info->vendor)) {
            psi_str_copy(info->vendor, CPU_COUNT_OF(info->vendor),
                         cpuid_vendor);
        }

        /* 注册表品牌串缺失或为 "Generic" 时用 CPUID 品牌串覆盖 */
        if (psi_str_is_empty(info->brand) ||
            psi_str_contains_ci(info->brand, L"Generic")) {
            if (cpu_cpuid_brand(cpuid_brand, CPU_COUNT_OF(cpuid_brand),
                                max_extended)) {
                psi_str_copy(info->brand, CPU_COUNT_OF(info->brand),
                             cpuid_brand);
            }
        }

        cpu_collect_feature_bits(&features, max_basic, max_extended);
        cpu_features_to_text(&features, info->features,
                             CPU_COUNT_OF(info->features));
        cpu_collect_frequencies(info, max_basic, reg_mhz);
    } else {
        if (report != NULL) {
            psi_add_warning(report,
                            L"CPUID 不可用，处理器特性与频率信息可能缺失");
        }
        if (reg_mhz != 0) {
            info->base_mhz = (unsigned)reg_mhz;
        }
    }

    if (psi_str_is_empty(info->brand) && report != NULL) {
        psi_add_warning(report, L"无法获取处理器型号名称");
    }

    /* 3) 拓扑：逻辑处理器、物理核心、插槽。 */
    logical_api_ok = cpu_logical_processors(&info->logical);
    if (!logical_api_ok && report != NULL) {
        psi_add_warning(report,
                        L"获取活动逻辑处理器数失败，已回退到系统默认值");
    }

    cores_ok = cpu_count_relation(RelationProcessorCore, &info->cores);
    sockets_ok = cpu_count_relation(RelationProcessorPackage, &info->sockets);

    /* 固件兜底先执行，避免把逻辑线程直接当成物理核心。 */
    cpu_apply_smbios(info, smbios);

    if (!cores_ok || !sockets_ok) {
        if (!cores_ok && info->cores == 0) {
            info->cores = info->logical;
        }
        if (!sockets_ok && info->sockets == 0) {
            info->sockets = 1;
        }
        if (report != NULL && (info->cores == info->logical || info->sockets == 1)) {
            psi_add_warning(report,
                            L"处理器拓扑信息不完整，物理核心数/插槽数已降级估算");
        }
    }
    if (info->cores == 0) {
        info->cores = info->logical;
    }
    if (info->cores > info->logical) {
        info->cores = info->logical;    /* 物理核心不可能多于活动逻辑处理器 */
    }
    if (info->sockets == 0) {
        info->sockets = 1;
    }

    /* 4) 缓存：RelationCache 按 Level 累加，字节换算为 KB。 */
    cache_ok = cpu_collect_cache_bytes(&cache);
    if (cache_ok) {
        info->l1_kb = (unsigned)(cache.l1_bytes / 1024ULL);
        info->l2_kb = (unsigned)(cache.l2_bytes / 1024ULL);
        info->l3_kb = (unsigned)(cache.l3_bytes / 1024ULL);
    } else if (report != NULL) {
        psi_add_warning(report, L"获取处理器缓存信息失败，缓存容量留空");
    }

    /* 5) 当前频率快照。 */
    (void)cpu_collect_current_frequency(info);

    /* 6) 进程架构标记。 */
    cpu_set_arch(info);

    /* 只要拿到品牌、厂商或注册表标识之一，就视为采集成功（允许部分降级）。 */
    got_identity = (!psi_str_is_empty(info->brand) ||
                    !psi_str_is_empty(info->vendor) ||
                    !psi_str_is_empty(info->identifier)) ? TRUE : FALSE;
    if (!got_identity && report != NULL) {
        psi_add_warning(report, L"未能获取任何处理器标识信息");
    }
    return got_identity;
}
