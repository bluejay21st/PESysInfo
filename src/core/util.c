/*
 * util.c - 通用字符串、格式化、编码、注册表辅助函数
 */
#include "pesysinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <limits.h>
#include <wctype.h>
#include <ctype.h>

void psi_zero_memory(void *ptr, size_t size)
{
    if (ptr != NULL && size > 0) {
        ZeroMemory(ptr, size);
    }
}

void psi_str_copy(wchar_t *dst, size_t cch, const wchar_t *src)
{
    size_t i;

    if (dst == NULL || cch == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = L'\0';
        return;
    }
    for (i = 0; i + 1 < cch && src[i] != L'\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = L'\0';
}

void psi_str_copy_ansi(wchar_t *dst, size_t cch, const char *src)
{
    if (dst == NULL || cch == 0) {
        return;
    }
    dst[0] = L'\0';
    if (src == NULL || src[0] == '\0' || cch > (size_t)INT_MAX) {
        return;
    }
    if (MultiByteToWideChar(CP_ACP, 0, src, -1, dst, (int)cch) == 0) {
        dst[0] = L'\0';
    }
}

void psi_str_trim(wchar_t *text)
{
    size_t len;
    size_t start = 0;

    if (text == NULL) {
        return;
    }
    len = wcslen(text);
    while (start < len && iswspace(text[start])) {
        ++start;
    }
    while (len > start && iswspace(text[len - 1])) {
        --len;
    }
    if (start > 0) {
        memmove(text, text + start, (len - start) * sizeof(wchar_t));
    }
    text[len - start] = L'\0';
}

void psi_str_collapse_spaces(wchar_t *text)
{
    size_t read_pos;
    size_t write_pos = 0;
    BOOL last_was_space = FALSE;

    if (text == NULL) {
        return;
    }
    psi_str_trim(text);
    for (read_pos = 0; text[read_pos] != L'\0'; ++read_pos) {
        if (iswspace(text[read_pos])) {
            if (!last_was_space) {
                text[write_pos++] = L' ';
                last_was_space = TRUE;
            }
        } else {
            text[write_pos++] = text[read_pos];
            last_was_space = FALSE;
        }
    }
    text[write_pos] = L'\0';
}

void psi_str_append(wchar_t *dst, size_t cch, const wchar_t *src)
{
    size_t len;

    if (dst == NULL || cch == 0 || src == NULL) {
        return;
    }
    len = wcslen(dst);
    if (len >= cch) {
        return;
    }
    psi_str_copy(dst + len, cch - len, src);
}

void psi_str_appendf(wchar_t *dst, size_t cch, const wchar_t *fmt, ...)
{
    size_t len;
    va_list args;

    if (dst == NULL || cch == 0 || fmt == NULL) {
        return;
    }
    len = wcslen(dst);
    if (len >= cch) {
        return;
    }
    va_start(args, fmt);
    _vsnwprintf_s(dst + len, cch - len, _TRUNCATE, fmt, args);
    va_end(args);
}

BOOL psi_str_contains_ci(const wchar_t *haystack, const wchar_t *needle)
{
    size_t hay_len;
    size_t needle_len;
    size_t i;

    if (haystack == NULL || needle == NULL) {
        return FALSE;
    }
    hay_len = wcslen(haystack);
    needle_len = wcslen(needle);
    if (needle_len == 0) {
        return TRUE;
    }
    if (needle_len > hay_len) {
        return FALSE;
    }
    for (i = 0; i + needle_len <= hay_len; ++i) {
        size_t j;
        for (j = 0; j < needle_len; ++j) {
            if (towlower(haystack[i + j]) != towlower(needle[j])) {
                break;
            }
        }
        if (j == needle_len) {
            return TRUE;
        }
    }
    return FALSE;
}

BOOL psi_str_is_empty(const wchar_t *text)
{
    return (text == NULL || text[0] == L'\0') ? TRUE : FALSE;
}

void psi_format_bytes(unsigned long long bytes, wchar_t *buf, size_t cch)
{
    if (buf == NULL || cch == 0) {
        return;
    }
    if (bytes < 1024ULL) {
        swprintf_s(buf, cch, L"%llu B", bytes);
    } else if (bytes < 1024ULL * 1024ULL) {
        swprintf_s(buf, cch, L"%.1f KB", (double)bytes / 1024.0);
    } else if (bytes < 1024ULL * 1024ULL * 1024ULL) {
        swprintf_s(buf, cch, L"%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes < 1024ULL * 1024ULL * 1024ULL * 1024ULL) {
        swprintf_s(buf, cch, L"%.1f GB",
                   (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else {
        swprintf_s(buf, cch, L"%.2f TB",
                   (double)bytes / (1024.0 * 1024.0 * 1024.0 * 1024.0));
    }
}

void psi_format_mhz(unsigned mhz, wchar_t *buf, size_t cch)
{
    if (buf == NULL || cch == 0) {
        return;
    }
    if (mhz == 0) {
        psi_str_copy(buf, cch, L"未知");
    } else if (mhz >= 1000) {
        swprintf_s(buf, cch, L"%.2f GHz", (double)mhz / 1000.0);
    } else {
        swprintf_s(buf, cch, L"%u MHz", mhz);
    }
}

void psi_format_datetime(const SYSTEMTIME *st, wchar_t *buf, size_t cch)
{
    if (buf == NULL || cch == 0) {
        return;
    }
    if (st == NULL) {
        psi_str_copy(buf, cch, L"未知");
        return;
    }
    swprintf_s(buf, cch, L"%04u-%02u-%02u %02u:%02u:%02u",
               st->wYear, st->wMonth, st->wDay,
               st->wHour, st->wMinute, st->wSecond);
}

void psi_format_duration(unsigned long long seconds, wchar_t *buf, size_t cch)
{
    unsigned long long days;
    unsigned long long hours;
    unsigned long long minutes;
    unsigned long long secs;

    if (buf == NULL || cch == 0) {
        return;
    }
    days = seconds / 86400ULL;
    hours = (seconds % 86400ULL) / 3600ULL;
    minutes = (seconds % 3600ULL) / 60ULL;
    secs = seconds % 60ULL;
    if (days > 0) {
        swprintf_s(buf, cch, L"%llu 天 %02llu:%02llu:%02llu",
                   days, hours, minutes, secs);
    } else {
        swprintf_s(buf, cch, L"%02llu:%02llu:%02llu", hours, minutes, secs);
    }
}

BOOL psi_reg_read_string(HKEY root, const wchar_t *subkey, const wchar_t *value,
                         wchar_t *buf, size_t cch)
{
    HKEY key = NULL;
    LONG status;
    DWORD type = 0;
    DWORD bytes;
    BOOL ok = FALSE;

    if (buf == NULL || cch == 0) {
        return FALSE;
    }
    buf[0] = L'\0';
    if (subkey == NULL || value == NULL || cch > (size_t)(0x7FFFFFFF / sizeof(wchar_t))) {
        return FALSE;
    }

    status = RegOpenKeyExW(root, subkey, 0, KEY_READ | KEY_WOW64_64KEY, &key);
    if (status != ERROR_SUCCESS) {
        status = RegOpenKeyExW(root, subkey, 0, KEY_READ, &key);
    }
    if (status != ERROR_SUCCESS) {
        return FALSE;
    }

    bytes = (DWORD)(cch * sizeof(wchar_t));
    status = RegQueryValueExW(key, value, NULL, &type, (LPBYTE)buf, &bytes);
    if (status == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) &&
        bytes >= sizeof(wchar_t)) {
        buf[cch - 1] = L'\0';
        ok = TRUE;
    }
    RegCloseKey(key);
    if (!ok) {
        buf[0] = L'\0';
    }
    return ok;
}

BOOL psi_reg_read_dword(HKEY root, const wchar_t *subkey, const wchar_t *value,
                        DWORD *out)
{
    HKEY key = NULL;
    LONG status;
    DWORD type = 0;
    DWORD data = 0;
    DWORD bytes = sizeof(data);
    BOOL ok = FALSE;

    if (out == NULL) {
        return FALSE;
    }
    *out = 0;
    if (subkey == NULL || value == NULL) {
        return FALSE;
    }

    status = RegOpenKeyExW(root, subkey, 0, KEY_READ | KEY_WOW64_64KEY, &key);
    if (status != ERROR_SUCCESS) {
        status = RegOpenKeyExW(root, subkey, 0, KEY_READ, &key);
    }
    if (status != ERROR_SUCCESS) {
        return FALSE;
    }

    status = RegQueryValueExW(key, value, NULL, &type, (LPBYTE)&data, &bytes);
    if (status == ERROR_SUCCESS && type == REG_DWORD && bytes == sizeof(data)) {
        *out = data;
        ok = TRUE;
    }
    RegCloseKey(key);
    return ok;
}

BOOL psi_file_exists(const wchar_t *path)
{
    DWORD attrs;

    if (path == NULL || path[0] == L'\0') {
        return FALSE;
    }
    attrs = GetFileAttributesW(path);
    return (attrs != INVALID_FILE_ATTRIBUTES) ? TRUE : FALSE;
}

void psi_get_system_dir(wchar_t *buf, size_t cch)
{
    if (buf == NULL || cch == 0) {
        return;
    }
    buf[0] = L'\0';
    if (cch > (size_t)INT_MAX) {
        return;
    }
    GetSystemDirectoryW(buf, (UINT)cch);
    buf[cch - 1] = L'\0';
}
