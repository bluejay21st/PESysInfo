/*
 * system.c - 系统信息采集
 *
 * 覆盖 SystemInfo 中由本模块负责的字段：
 *   computer_name / domain / user_name
 *   os_name / os_version / os_build / os_edition / os_arch
 *   is_pe / pe_version / firmware_mode
 *   uptime_seconds / local_time
 *
 * 整机厂商、型号、序列号、UUID、BIOS、主板等字段由 firmware.c 负责，
 * 本模块不得覆盖。所有函数只做采集，不打印任何内容。
 */
#include "pesysinfo.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define PSI_CURRENT_VERSION_KEY L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"
#define PSI_CONTROL_KEY         L"SYSTEM\\CurrentControlSet\\Control"
#define PSI_PE_OS_NAME          L"Windows Preinstallation Environment"
#define PSI_FIRMWARE_UNKNOWN    L"未知"
#define PSI_ARRAY_COUNT(a)      (sizeof(a) / sizeof((a)[0]))

/*
 * RtlGetVersion 原型：ntdll.dll 导出，返回 NTSTATUS(0 表示成功)。
 * 运行时动态解析，避免使用在 Win8.1+ 会撒谎的已弃用 GetVersionExW。
 */
typedef LONG (WINAPI *PsiRtlGetVersionFn)(OSVERSIONINFOEXW *);

/* 判断 %SystemRoot%\System32\wpeinit.exe 是否存在 */
static BOOL psi_wpeinit_exists(void)
{
    wchar_t directory[MAX_PATH];
    wchar_t path[MAX_PATH + 16];

    directory[0] = L'\0';
    psi_get_system_dir(directory, PSI_ARRAY_COUNT(directory));
    if (directory[0] == L'\0') {
        wchar_t windows_dir[MAX_PATH];

        windows_dir[0] = L'\0';
        if (GetWindowsDirectoryW(windows_dir, (UINT)PSI_ARRAY_COUNT(windows_dir)) > 0) {
            psi_str_copy(directory, PSI_ARRAY_COUNT(directory), windows_dir);
            psi_str_append(directory, PSI_ARRAY_COUNT(directory), L"\\System32");
        }
    }
    if (directory[0] == L'\0') {
        return FALSE;
    }

    psi_str_copy(path, PSI_ARRAY_COUNT(path), directory);
    psi_str_append(path, PSI_ARRAY_COUNT(path), L"\\wpeinit.exe");
    return psi_file_exists(path);
}

/* WinPE 注册表标志：HKLM\SYSTEM\CurrentControlSet\Control\MiniNT */
static BOOL psi_minint_key_exists(void)
{
    HKEY key = NULL;
    LONG status;

    status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, PSI_CONTROL_KEY L"\\MiniNT",
                           0, KEY_READ | KEY_WOW64_64KEY, &key);
    if (status != ERROR_SUCCESS) {
        status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, PSI_CONTROL_KEY L"\\MiniNT",
                               0, KEY_READ, &key);
    }
    if (status == ERROR_SUCCESS && key != NULL) {
        RegCloseKey(key);
        return TRUE;
    }
    return FALSE;
}

/* 读取 OS 版本：优先 RtlGetVersion，失败回退 GetVersionExW */
static BOOL psi_read_os_version(OSVERSIONINFOEXW *version)
{
    union {
        FARPROC             proc;
        PsiRtlGetVersionFn  fn;
    } resolve;
    HMODULE ntdll = NULL;
    BOOL loaded = FALSE;
    BOOL ok = FALSE;

    if (version == NULL) {
        return FALSE;
    }

    ZeroMemory(version, sizeof(*version));
    version->dwOSVersionInfoSize = (DWORD)sizeof(*version);

    ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == NULL) {
        ntdll = LoadLibraryW(L"ntdll.dll");
        loaded = (ntdll != NULL) ? TRUE : FALSE;
    }
    if (ntdll != NULL) {
        resolve.proc = GetProcAddress(ntdll, "RtlGetVersion");
        if (resolve.proc != NULL) {
            ok = (resolve.fn(version) == 0) ? TRUE : FALSE;
        }
    }
    if (loaded) {
        FreeLibrary(ntdll);
    }

    if (!ok) {
        /* 极老系统/非常规环境兜底；该 API 在 Win8.1+ 无清单时会返回 6.3。 */
        ZeroMemory(version, sizeof(*version));
        version->dwOSVersionInfoSize = (DWORD)sizeof(*version);
#pragma warning(push)
#pragma warning(disable : 4996)
        ok = GetVersionExW((LPOSVERSIONINFOW)version);
#pragma warning(pop)
        version->dwOSVersionInfoSize = (DWORD)sizeof(*version);
    }
    return ok;
}

/* 引导模式：GetFirmwareType 优先，失败回退注册表 PEFirmwareType */
static void psi_read_firmware_mode(wchar_t *buf, size_t cch)
{
    FIRMWARE_TYPE type = FirmwareTypeUnknown;
    DWORD pe_type = 0;

    if (buf == NULL || cch == 0) {
        return;
    }
    buf[0] = L'\0';

    if (GetFirmwareType(&type)) {
        if (type == FirmwareTypeBios) {
            psi_str_copy(buf, cch, L"BIOS");
            return;
        }
        if (type == FirmwareTypeUefi) {
            psi_str_copy(buf, cch, L"UEFI");
            return;
        }
    }

    if (psi_reg_read_dword(HKEY_LOCAL_MACHINE, PSI_CONTROL_KEY,
                           L"PEFirmwareType", &pe_type)) {
        if (pe_type == 1) {
            psi_str_copy(buf, cch, L"BIOS");
            return;
        }
        if (pe_type == 2) {
            psi_str_copy(buf, cch, L"UEFI");
            return;
        }
    }

    psi_str_copy(buf, cch, PSI_FIRMWARE_UNKNOWN);
}

/* 操作系统体系结构（并非进程体系结构） */
static void psi_read_os_arch(wchar_t *buf, size_t cch)
{
    SYSTEM_INFO sys_info;

    if (buf == NULL || cch == 0) {
        return;
    }
    buf[0] = L'\0';

    ZeroMemory(&sys_info, sizeof(sys_info));
    GetNativeSystemInfo(&sys_info);

    switch (sys_info.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
        psi_str_copy(buf, cch, L"x64");
        break;
    case PROCESSOR_ARCHITECTURE_INTEL:
        psi_str_copy(buf, cch, L"x86");
        break;
#ifdef PROCESSOR_ARCHITECTURE_ARM
    case PROCESSOR_ARCHITECTURE_ARM:
        psi_str_copy(buf, cch, L"ARM");
        break;
#endif
#ifdef PROCESSOR_ARCHITECTURE_ARM64
    case PROCESSOR_ARCHITECTURE_ARM64:
        psi_str_copy(buf, cch, L"ARM64");
        break;
#endif
#ifdef PROCESSOR_ARCHITECTURE_IA64
    case PROCESSOR_ARCHITECTURE_IA64:
        psi_str_copy(buf, cch, L"IA64");
        break;
#endif
    default:
        psi_str_copy(buf, cch, L"未知");
        break;
    }
}

BOOL psi_system_collect(SystemInfo *info, SystemReport *report)
{
    wchar_t computer_name[128];
    wchar_t domain[128];
    wchar_t user_name[128];
    wchar_t product_name[PSI_TEXT_CCH];
    wchar_t edition_id[PSI_TEXT_CCH];
    wchar_t display_version[64];
    wchar_t os_name[PSI_TEXT_CCH];
    wchar_t system_drive[16];
    OSVERSIONINFOEXW version;
    DWORD size;
    DWORD build_number = 0;
    DWORD ubr = 0;
    BOOL version_ok;
    BOOL has_ubr;
    BOOL is_pe;
    BOOL x_drive;
    BOOL minint_name;

    if (info == NULL) {
        return FALSE;
    }

    /* ---------------- 计算机名 / 域 / 用户名 ---------------- */
    computer_name[0] = L'\0';
    size = (DWORD)PSI_ARRAY_COUNT(computer_name);
    if (!GetComputerNameExW(ComputerNameDnsHostname, computer_name, &size)) {
        size = (DWORD)PSI_ARRAY_COUNT(computer_name);
        if (!GetComputerNameExW(ComputerNameNetBIOS, computer_name, &size)) {
            computer_name[0] = L'\0';
            if (GetEnvironmentVariableW(L"COMPUTERNAME", computer_name,
                                        (DWORD)PSI_ARRAY_COUNT(computer_name)) == 0) {
                computer_name[0] = L'\0';
            }
        }
    }
    computer_name[PSI_ARRAY_COUNT(computer_name) - 1] = L'\0';
    psi_str_copy(info->computer_name, PSI_ARRAY_COUNT(info->computer_name),
                 computer_name);

    domain[0] = L'\0';
    size = (DWORD)PSI_ARRAY_COUNT(domain);
    if (!GetComputerNameExW(ComputerNameDnsDomain, domain, &size)) {
        domain[0] = L'\0';
    }
    domain[PSI_ARRAY_COUNT(domain) - 1] = L'\0';
    psi_str_copy(info->domain, PSI_ARRAY_COUNT(info->domain), domain);

    user_name[0] = L'\0';
    size = (DWORD)PSI_ARRAY_COUNT(user_name);
    if (!GetUserNameW(user_name, &size)) {
        user_name[0] = L'\0';
        if (GetEnvironmentVariableW(L"USERNAME", user_name,
                                    (DWORD)PSI_ARRAY_COUNT(user_name)) == 0) {
            user_name[0] = L'\0';
        }
    }
    user_name[PSI_ARRAY_COUNT(user_name) - 1] = L'\0';
    psi_str_copy(info->user_name, PSI_ARRAY_COUNT(info->user_name), user_name);

    /* ---------------- OS 版本 / 注册表补充信息 ---------------- */
    version_ok = psi_read_os_version(&version);
    if (version_ok) {
        build_number = version.dwBuildNumber;
    }

    product_name[0] = L'\0';
    edition_id[0] = L'\0';
    display_version[0] = L'\0';
    psi_reg_read_string(HKEY_LOCAL_MACHINE, PSI_CURRENT_VERSION_KEY,
                        L"ProductName", product_name,
                        PSI_ARRAY_COUNT(product_name));
    psi_str_trim(product_name);
    psi_reg_read_string(HKEY_LOCAL_MACHINE, PSI_CURRENT_VERSION_KEY,
                        L"EditionID", edition_id, PSI_ARRAY_COUNT(edition_id));
    psi_str_trim(edition_id);
    psi_reg_read_string(HKEY_LOCAL_MACHINE, PSI_CURRENT_VERSION_KEY,
                        L"DisplayVersion", display_version,
                        PSI_ARRAY_COUNT(display_version));
    psi_str_trim(display_version);
    has_ubr = psi_reg_read_dword(HKEY_LOCAL_MACHINE, PSI_CURRENT_VERSION_KEY,
                                 L"UBR", &ubr);

    info->os_version[0] = L'\0';
    if (version_ok) {
        swprintf_s(info->os_version, PSI_ARRAY_COUNT(info->os_version),
                   L"%lu.%lu.%lu",
                   (unsigned long)version.dwMajorVersion,
                   (unsigned long)version.dwMinorVersion,
                   (unsigned long)version.dwBuildNumber);
    }

    info->os_build[0] = L'\0';
    if (build_number != 0) {
        if (has_ubr) {
            swprintf_s(info->os_build, PSI_ARRAY_COUNT(info->os_build),
                       L"%lu.%lu", (unsigned long)build_number,
                       (unsigned long)ubr);
        } else {
            swprintf_s(info->os_build, PSI_ARRAY_COUNT(info->os_build),
                       L"%lu", (unsigned long)build_number);
        }
    }

    psi_read_os_arch(info->os_arch, PSI_ARRAY_COUNT(info->os_arch));

    /* os_name：ProductName（追加 DisplayVersion，如 22H2）；
     * os_edition：EditionID（如 Professional），为空时回退 ProductName。 */
    os_name[0] = L'\0';
    psi_str_copy(os_name, PSI_ARRAY_COUNT(os_name), product_name);
    if (!psi_str_is_empty(os_name) && !psi_str_is_empty(display_version)) {
        psi_str_append(os_name, PSI_ARRAY_COUNT(os_name), L" ");
        psi_str_append(os_name, PSI_ARRAY_COUNT(os_name), display_version);
    }
    if (!psi_str_is_empty(edition_id)) {
        psi_str_copy(info->os_edition, PSI_ARRAY_COUNT(info->os_edition),
                     edition_id);
    } else {
        psi_str_copy(info->os_edition, PSI_ARRAY_COUNT(info->os_edition),
                     product_name);
    }

    /* ---------------- PE 判定 ---------------- */
    system_drive[0] = L'\0';
    GetEnvironmentVariableW(L"SystemDrive", system_drive,
                            (DWORD)PSI_ARRAY_COUNT(system_drive));
    system_drive[PSI_ARRAY_COUNT(system_drive) - 1] = L'\0';

    x_drive = ((system_drive[0] == L'X' || system_drive[0] == L'x') &&
               system_drive[1] == L':') ? TRUE : FALSE;
    minint_name = (_wcsnicmp(computer_name, L"MININT-", 7) == 0) ? TRUE : FALSE;
    is_pe = ((psi_wpeinit_exists() && (x_drive || minint_name)) ||
             psi_minint_key_exists()) ? TRUE : FALSE;

    info->is_pe = is_pe;
    info->pe_version[0] = L'\0';
    if (is_pe) {
        psi_str_copy(info->pe_version, PSI_ARRAY_COUNT(info->pe_version),
                     info->os_version);
    }

    if (is_pe || psi_str_is_empty(os_name)) {
        psi_str_copy(info->os_name, PSI_ARRAY_COUNT(info->os_name),
                     PSI_PE_OS_NAME);
    } else {
        psi_str_copy(info->os_name, PSI_ARRAY_COUNT(info->os_name), os_name);
    }

    /* ---------------- 引导模式 / 运行时长 / 本地时间 ---------------- */
    psi_read_firmware_mode(info->firmware_mode,
                           PSI_ARRAY_COUNT(info->firmware_mode));

    info->uptime_seconds = (unsigned long long)(GetTickCount64() / 1000ULL);
    GetLocalTime(&info->local_time);

    if (!version_ok && report != NULL) {
        psi_add_warning(report,
                        L"无法获取 Windows 版本信息（RtlGetVersion 与 GetVersionEx 均失败）");
    }

    return TRUE;
}
