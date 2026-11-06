/*
 * smbios.c - SMBIOS 原始固件表（RawSMBIOSData）读取与结构遍历
 *
 * 目标环境：Windows PE 10.0.19041，只依赖 kernel32（GetSystemFirmwareTable），
 * 不使用 WMI / PowerShell / 任何外部运行库。
 *
 * 数据布局（GetSystemFirmwareTable('RSMB', 0, ...) 返回的 RawSMBIOSData）：
 *   偏移 0  BYTE  Used20CallingMethod
 *   偏移 1  BYTE  MajorVersion
 *   偏移 2  BYTE  MinorVersion
 *   偏移 3  BYTE  DmiRevision
 *   偏移 4  DWORD Length          （小端，结构表字节数）
 *   偏移 8  BYTE  SMBIOSTableData[]（以 type 0x7F 结束）
 *
 * 安全策略：
 *   - 所有遍历都以 table_size 为上限做严格边界检查；
 *   - 结构长度 < 4、结构越过表尾、字符串区缺少结束双 NUL，
 *     都视为坏数据并返回 NULL；
 *   - 字符串遍历受 SMBIOS 规范限制（每条 ≤64 字符、≤255 条），
 *     编号 0 / 越界 / 找不到时返回 NULL。
 */
#include "pesysinfo.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define PSI_RSMB_HEADER_SIZE     8u
#define PSI_SMBIOS_MIN_STRUCT    4u
#define PSI_SMBIOS_END_OF_TABLE  0x7Fu
#define PSI_SMBIOS_STRING_MAX    64u
#define PSI_SMBIOS_MAX_STRINGS   255u

/*
 * 返回 structure 之后的下一条结构地址（跳过字符串区）。
 * 数据不自洽时返回 NULL，调用方据此停止遍历。
 */
static const BYTE *smbios_next_structure(const SmbiosData *smbios,
                                         const BYTE *structure)
{
    ULONG_PTR base;
    ULONG_PTR address;
    size_t offset;
    size_t remaining;
    size_t length;
    const BYTE *cursor;
    const BYTE *end;

    if (smbios == NULL || smbios->table == NULL || structure == NULL) {
        return NULL;
    }
    base = (ULONG_PTR)smbios->table;
    address = (ULONG_PTR)structure;
    if (address < base) {
        return NULL;
    }
    offset = (size_t)(address - base);
    if (offset >= (size_t)smbios->table_size) {
        return NULL;
    }
    remaining = (size_t)smbios->table_size - offset;
    if (remaining < PSI_SMBIOS_MIN_STRUCT) {
        return NULL;
    }
    length = (size_t)structure[1];
    if (length < PSI_SMBIOS_MIN_STRUCT || length > remaining) {
        return NULL;
    }
    cursor = structure + length;
    end = smbios->table + smbios->table_size;
    /* 字符串区以双 NUL 结束；没有字符串时就是紧接着的两个 NUL。 */
    while ((size_t)(end - cursor) >= 2) {
        if (cursor[0] == 0x00 && cursor[1] == 0x00) {
            return cursor + 2;
        }
        ++cursor;
    }
    return NULL;
}

BOOL psi_smbios_load(SmbiosData *smbios)
{
    UINT needed;
    UINT written;
    BYTE *buffer;
    DWORD table_length = 0;

    if (smbios == NULL) {
        return FALSE;
    }
    psi_zero_memory(smbios, sizeof(*smbios));

    needed = GetSystemFirmwareTable('RSMB', 0, NULL, 0);
    if (needed < PSI_RSMB_HEADER_SIZE) {
        return FALSE;
    }
    buffer = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)needed);
    if (buffer == NULL) {
        return FALSE;
    }
    written = GetSystemFirmwareTable('RSMB', 0, buffer, needed);
    if (written < PSI_RSMB_HEADER_SIZE || written > needed) {
        HeapFree(GetProcessHeap(), 0, buffer);
        return FALSE;
    }

    /* Length 是小端 DWORD；目标平台为小端，直接 memcpy。 */
    memcpy(&table_length, buffer + 4, sizeof(table_length));
    if (table_length == 0 ||
        (size_t)table_length > (size_t)written - PSI_RSMB_HEADER_SIZE) {
        HeapFree(GetProcessHeap(), 0, buffer);
        return FALSE;
    }

    smbios->raw = buffer;
    smbios->table = buffer + PSI_RSMB_HEADER_SIZE;
    smbios->table_size = table_length;
    smbios->valid = TRUE;
    return TRUE;
}

void psi_smbios_free(SmbiosData *smbios)
{
    if (smbios == NULL) {
        return;
    }
    if (smbios->raw != NULL) {
        HeapFree(GetProcessHeap(), 0, smbios->raw);
    }
    psi_zero_memory(smbios, sizeof(*smbios));
}

const BYTE *psi_smbios_find(const SmbiosData *smbios, BYTE type,
                            unsigned occurrence)
{
    const BYTE *cursor;
    unsigned seen = 0;

    if (smbios == NULL || !smbios->valid || smbios->table == NULL ||
        smbios->table_size < PSI_SMBIOS_MIN_STRUCT) {
        return NULL;
    }

    cursor = smbios->table;
    while (cursor != NULL) {
        const BYTE *next;
        DWORD offset;
        size_t remaining;

        offset = (DWORD)((ULONG_PTR)cursor - (ULONG_PTR)smbios->table);
        if ((size_t)offset >= (size_t)smbios->table_size) {
            return NULL;
        }
        remaining = (size_t)smbios->table_size - (size_t)offset;
        if (remaining < PSI_SMBIOS_MIN_STRUCT) {
            return NULL;
        }

        /* 先校验字符串区完整性：只有自洽的结构才会被返回。 */
        next = smbios_next_structure(smbios, cursor);
        if (next == NULL) {
            return NULL;
        }

        /* type 0x7F 是结构表结束标记，永不匹配、也不越过它继续扫描。 */
        if (cursor[0] == PSI_SMBIOS_END_OF_TABLE) {
            return NULL;
        }
        if (cursor[0] == type) {
            if (seen == occurrence) {
                return cursor;
            }
            ++seen;
        }
        cursor = next;
    }
    return NULL;
}

const char *psi_smbios_string(const BYTE *structure, BYTE offset)
{
    size_t length;
    const BYTE *cursor;
    unsigned index = 1;
    BYTE number;

    if (structure == NULL) {
        return NULL;
    }
    length = (size_t)structure[1];
    if (length < PSI_SMBIOS_MIN_STRUCT || (size_t)offset >= length) {
        return NULL;
    }
    number = structure[offset];
    if (number == 0) {
        return NULL;
    }

    cursor = structure + length;
    for (;;) {
        const BYTE *start;
        size_t text_length = 0;

        /* 空字符串表示字符串集合结束，编号比实际字符串多。 */
        if (cursor[0] == 0x00) {
            return NULL;
        }
        start = cursor;
        while (text_length <= PSI_SMBIOS_STRING_MAX &&
               cursor[text_length] != 0x00) {
            ++text_length;
        }
        if (text_length > PSI_SMBIOS_STRING_MAX) {
            return NULL; /* 单条字符串超过规范上限，判为坏数据 */
        }
        if (index == (unsigned)number) {
            return (const char *)start;
        }
        ++index;
        if (index > PSI_SMBIOS_MAX_STRINGS) {
            return NULL;
        }
        cursor += text_length + 1;
    }
}

void psi_smbios_to_wide(const char *text, wchar_t *dst, size_t cch)
{
    if (dst == NULL || cch == 0) {
        return;
    }
    dst[0] = L'\0';
    if (text == NULL || text[0] == '\0') {
        return;
    }
    /* SMBIOS 字符串为 CP_ACP 编码（OEM 厂商通常写 ASCII）。 */
    psi_str_copy_ansi(dst, cch, text);
    psi_str_trim(dst);
}

unsigned long long psi_smbios_memory_size(const BYTE *type17)
{
    WORD size = 0;
    size_t length;

    if (type17 == NULL || type17[0] != 0x11) { /* 必须是 Memory Device */
        return 0;
    }
    length = (size_t)type17[1];
    /* 0x0C WORD Size 需要结构长度至少到 0x0E。 */
    if (length < 0x0Eu) {
        return 0;
    }
    memcpy(&size, type17 + 0x0C, sizeof(size));

    if (size == 0x0000) {
        return 0; /* 空插槽 */
    }
    if (size == 0xFFFF) {
        return 0; /* 容量未知 */
    }
    if (size == 0x7FFF) {
        DWORD extended = 0;
        /* 0x1C DWORD Extended Size 需要结构长度至少到 0x20。 */
        if (length < 0x20u) {
            return 0;
        }
        memcpy(&extended, type17 + 0x1C, sizeof(extended));
        if (extended == 0 || extended == 0xFFFFFFFFu) {
            return 0;
        }
        return (unsigned long long)extended * 1024ULL * 1024ULL;
    }
    if ((size & 0x8000u) != 0) {
        /* 最高位为 1：单位为 KB，低 15 位有效。 */
        return (unsigned long long)(size & 0x7FFFu) * 1024ULL;
    }
    /* 最高位为 0：单位为 MB。 */
    return (unsigned long long)size * 1024ULL * 1024ULL;
}

unsigned psi_smbios_struct_count(const SmbiosData *smbios, BYTE type)
{
    const BYTE *cursor;
    unsigned count = 0;

    if (smbios == NULL || !smbios->valid || smbios->table == NULL ||
        smbios->table_size < PSI_SMBIOS_MIN_STRUCT) {
        return 0;
    }

    cursor = smbios->table;
    while (cursor != NULL) {
        DWORD offset;
        size_t remaining;
        const BYTE *next;

        offset = (DWORD)((ULONG_PTR)cursor - (ULONG_PTR)smbios->table);
        if ((size_t)offset >= (size_t)smbios->table_size) {
            break;
        }
        remaining = (size_t)smbios->table_size - (size_t)offset;
        if (remaining < PSI_SMBIOS_MIN_STRUCT) {
            break;
        }

        next = smbios_next_structure(smbios, cursor);
        if (next == NULL) {
            break;
        }
        if (cursor[0] == PSI_SMBIOS_END_OF_TABLE) {
            break;
        }
        if (cursor[0] == type) {
            ++count;
        }
        cursor = next;
    }
    return count;
}
