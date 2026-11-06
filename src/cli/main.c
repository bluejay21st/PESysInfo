/*
 * PESysInfo-cli - 控制台/Agent 入口
 *
 * 默认输出 JSON；不创建窗口、不等待回车、不支持交互。
 */
#include "probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#if defined(_M_X64)
#define PSI_ARCH L"x64"
#else
#define PSI_ARCH L"x86"
#endif

typedef struct {
    const wchar_t *get_key;
    const wchar_t *output;
    BOOL  help;
    BOOL  version;
    BOOL  display;
} CliOptions;

static BOOL stdout_is_console(void)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    return (h != NULL && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) ? TRUE : FALSE;
}

static BOOL write_console_w(const wchar_t *text)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    size_t length;
    size_t offset = 0;

    if (text == NULL) {
        return TRUE;
    }
    length = wcslen(text);
    while (offset < length) {
        DWORD chunk = (DWORD)((length - offset > 32767U) ? 32767U : (length - offset));
        if (!WriteConsoleW(h, text + offset, chunk, &written, NULL)) {
            return FALSE;
        }
        if (written == 0) {
            return FALSE;
        }
        offset += written;
    }
    return TRUE;
}

static BOOL write_stdout_bytes(const char *data, size_t length)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    size_t offset = 0;

    while (offset < length) {
        DWORD written = 0;
        DWORD chunk = (DWORD)((length - offset > 0x10000000U) ? 0x10000000U : (length - offset));
        if (!WriteFile(h, data + offset, chunk, &written, NULL)) {
            return FALSE;
        }
        if (written == 0) {
            return FALSE;
        }
        offset += written;
    }
    return TRUE;
}

static BOOL write_stdout_utf8(const char *text)
{
    if (text == NULL) {
        return TRUE;
    }
    if (!stdout_is_console()) {
        return write_stdout_bytes(text, strlen(text));
    }
    {
        int needed = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
        wchar_t *wide;
        BOOL ok;
        if (needed <= 0) {
            return FALSE;
        }
        wide = (wchar_t *)HeapAlloc(GetProcessHeap(), 0,
                                    (SIZE_T)needed * sizeof(wchar_t));
        if (wide == NULL) {
            return FALSE;
        }
        MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, needed);
        ok = write_console_w(wide);
        HeapFree(GetProcessHeap(), 0, wide);
        return ok;
    }
}

static BOOL write_stdout_wide(const wchar_t *text)
{
    if (text == NULL) {
        return TRUE;
    }
    if (stdout_is_console()) {
        return write_console_w(text);
    }
    {
        int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
        char *utf8;
        BOOL ok;
        if (needed <= 0) {
            return FALSE;
        }
        utf8 = (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)needed);
        if (utf8 == NULL) {
            return FALSE;
        }
        WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, needed, NULL, NULL);
        ok = write_stdout_bytes(utf8, (size_t)needed - 1);
        HeapFree(GetProcessHeap(), 0, utf8);
        return ok;
    }
}

static BOOL write_file_bytes(const wchar_t *path, const char *data, size_t length)
{
    HANDLE h;
    DWORD written = 0;
    BOOL ok = TRUE;

    if (path == NULL || path[0] == L'\0' || data == NULL) {
        return FALSE;
    }
    h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    if (length > 0 && (!WriteFile(h, data, (DWORD)length, &written, NULL) ||
                       written != (DWORD)length)) {
        ok = FALSE;
    }
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(path);
    }
    return ok;
}

static BOOL parse_options(int argc, wchar_t **argv, CliOptions *opt)
{
    int i;

    ZeroMemory(opt, sizeof(*opt));
    for (i = 1; i < argc; ++i) {
        const wchar_t *arg = argv[i];
        const wchar_t *name;
        const wchar_t *value = NULL;

        if (arg[0] != L'-' && arg[0] != L'/') {
            return FALSE;
        }
        name = arg;
        while (*name == L'-' || *name == L'/') {
            ++name;
        }
        if (wcschr(name, L'=') != NULL) {
            value = wcschr(name, L'=') + 1;
        }
        if (_wcsicmp(name, L"help") == 0 || _wcsicmp(name, L"h") == 0 ||
            _wcsicmp(name, L"?") == 0) {
            opt->help = TRUE;
        } else if (_wcsicmp(name, L"version") == 0 ||
                   _wcsicmp(name, L"v") == 0) {
            opt->version = TRUE;
        } else if (_wcsicmp(name, L"json") == 0) {
            /* 默认即 JSON */
        } else if (_wcsicmp(name, L"display") == 0) {
            opt->display = TRUE;
        } else if (_wcsicmp(name, L"get") == 0 ||
                   _wcsnicmp(name, L"get=", 4) == 0) {
            if (value == NULL) {
                if (i + 1 >= argc) {
                    return FALSE;
                }
                value = argv[++i];
                if (value[0] == L'-' || value[0] == L'/') {
                    return FALSE;
                }
            }
            if (value[0] == L'-' || value[0] == L'/') {
                return FALSE;
            }
            opt->get_key = value;
        } else if (_wcsicmp(name, L"output") == 0 ||
                   _wcsnicmp(name, L"output=", 7) == 0) {
            if (value == NULL) {
                if (i + 1 >= argc) {
                    return FALSE;
                }
                value = argv[++i];
                if (value[0] == L'-' || value[0] == L'/') {
                    return FALSE;
                }
            }
            if (value[0] == L'-' || value[0] == L'/') {
                return FALSE;
            }
            opt->output = value;
        } else {
            return FALSE;
        }
    }
    return TRUE;
}

static void print_help(void)
{
    write_stdout_wide(
        L"PESysInfo-cli " PSI_TOOL_VERSION L" - 硬件信息探测（Agent/脚本接口）\n"
        L"\n"
        L"用法:\n"
        L"  PESysInfo-cli.exe [选项]\n"
        L"\n"
        L"选项:\n"
        L"  --json                输出 JSON（默认）\n"
        L"  --get <key>           输出单个字段的原始值\n"
        L"  --display             配合 --get，输出显示值而不是原始值\n"
        L"  --output <文件>       将结果写入文件（UTF-8）\n"
        L"  -h, --help            显示帮助\n"
        L"  -v, --version         显示版本\n"
        L"\n"
        L"示例:\n"
        L"  PESysInfo-cli.exe --json > info.json\n"
        L"  PESysInfo-cli.exe --get cpu.brand\n"
        L"  PESysInfo-cli.exe --get memory.module.0.size_bytes\n"
        L"\n"
        L"字段 key 命名空间：system.* / bios.* / board.* / cpu.* / memory.* / storage.* / graphics.* / network.*\n"
        L"用 --json 获取全部 key；用 --get <key> 取单字段。\n");
}

static BOOL write_item_value(const PsiItem *item, BOOL display)
{
    wchar_t text[PSI_DISPLAY_CCH];

    if (item == NULL) {
        return FALSE;
    }
    if (display) {
        return write_stdout_wide(item->display);
    }
    switch (item->type) {
    case PSI_ITEM_STRING:
        return write_stdout_wide(item->value.string);
    case PSI_ITEM_U64:
        swprintf_s(text, PSI_ARRAY_COUNT(text), L"%llu", item->value.u64);
        return write_stdout_wide(text);
    case PSI_ITEM_I64:
        swprintf_s(text, PSI_ARRAY_COUNT(text), L"%lld", item->value.i64);
        return write_stdout_wide(text);
    case PSI_ITEM_BOOL:
        return write_stdout_wide(item->value.boolean ? L"true" : L"false");
    default:
        return write_stdout_wide(L"");
    }
}

int wmain(int argc, wchar_t **argv)
{
    CliOptions opt;
    PsiReport model;
    char *json = NULL;
    BOOL ok;

    if (!parse_options(argc, argv, &opt)) {
        write_stdout_wide(L"参数错误。使用 --help 查看用法。\n");
        return 2;
    }
    if (opt.help) {
        print_help();
        return 0;
    }
    if (opt.version) {
        write_stdout_wide(L"PESysInfo-cli " PSI_TOOL_VERSION L" (");
        write_stdout_wide(PSI_ARCH);
        write_stdout_wide(L")\n");
        return 0;
    }
    if (opt.get_key != NULL && opt.output != NULL) {
        write_stdout_wide(L"参数错误：--output 仅用于 JSON 输出。\n");
        return 2;
    }

    if (!psi_probe_collect(&model)) {
        write_stdout_wide(L"错误: 无法采集硬件信息。\n");
        return 1;
    }

    if (opt.get_key != NULL) {
        PsiItem *item = psi_model_find_item(&model, opt.get_key);
        if (item == NULL) {
            wchar_t message[512];
            swprintf_s(message, PSI_ARRAY_COUNT(message),
                       L"错误: 未知字段: %ls\n", opt.get_key);
            write_stdout_wide(message);
            psi_model_free(&model);
            return 2;
        }
        ok = write_item_value(item, opt.display);
        write_stdout_wide(L"\n");
        psi_model_free(&model);
        return ok ? 0 : 3;
    }

    if (!psi_model_json_utf8(&model, &json)) {
        psi_model_free(&model);
        write_stdout_wide(L"错误: 生成 JSON 失败。\n");
        return 1;
    }

    if (opt.output != NULL) {
        ok = write_file_bytes(opt.output, json, strlen(json));
        psi_model_free_text(json);
        psi_model_free(&model);
        if (!ok) {
            write_stdout_wide(L"错误: 无法写入输出文件。\n");
            return 3;
        }
        return 0;
    }

    ok = write_stdout_utf8(json);
    psi_model_free_text(json);
    psi_model_free(&model);
    return ok ? 0 : 3;
}
