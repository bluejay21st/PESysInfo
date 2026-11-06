/*
 * graphics.c - 显示适配器采集
 *
 * 使用 EnumDisplayDevicesW 枚举显示适配器，用 EnumDisplaySettingsW 读取
 * 当前显示模式（分辨率/刷新率/色深）。WinPE 无 WMI/PowerShell，此为
 * user32 提供的唯一稳定途径。
 *
 * 去重策略：以适配器名 DeviceString 为主键——同一物理显卡在多个显示器上
 * 在多个显示器上会以不同 DeviceName（\\.\DISPLAYn）出现，但 DeviceString
 * 相同，应合并为一条；DeviceString 为空时退化为按 DeviceName 去重。
 *
 * 所有函数只做采集，不打印任何内容。
 */
#include "pesysinfo.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define PSI_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

typedef struct {
    DISPLAY_DEVICEW device;
    BOOL            attached;   /* DISPLAY_DEVICE_ATTACHED_TO_DESKTOP */
    BOOL            primary;    /* DISPLAY_DEVICE_PRIMARY_DEVICE       */
    BOOL            mirroring;  /* DISPLAY_DEVICE_MIRRORING_DRIVER     */
} PsiDisplayCandidate;

/* 返回已收录的重复适配器下标，未找到返回 -1 */
static int psi_display_find_duplicate(const GraphicsInfo *info,
                                      const wchar_t *device_name,
                                      const wchar_t *device_string)
{
    unsigned i;

    for (i = 0; i < info->count; ++i) {
        if (device_string != NULL && device_string[0] != L'\0') {
            if (_wcsicmp(info->items[i].adapter, device_string) == 0) {
                return (int)i;
            }
        } else if (device_name != NULL && device_name[0] != L'\0' &&
                   info->items[i].device[0] != L'\0') {
            if (_wcsicmp(info->items[i].device, device_name) == 0) {
                return (int)i;
            }
        }
    }
    return -1;
}

static void psi_display_append(GraphicsInfo *info, const PsiDisplayCandidate *cand)
{
    GraphicsAdapter *item;
    DEVMODEW mode;
    BOOL mode_ok;

    if (info->count >= PSI_MAX_GRAPHICS) {
        return;
    }
    item = &info->items[info->count];
    psi_zero_memory(item, sizeof(*item));

    /* adapter 存适配器名，device 存设备名（\\.\DISPLAYn） */
    psi_str_copy(item->adapter, PSI_ARRAY_COUNT(item->adapter),
                 (cand->device.DeviceString[0] != L'\0')
                     ? cand->device.DeviceString
                     : cand->device.DeviceName);
    psi_str_copy(item->device, PSI_ARRAY_COUNT(item->device),
                 cand->device.DeviceName);
    item->primary = cand->primary;

    ZeroMemory(&mode, sizeof(mode));
    mode.dmSize = (WORD)sizeof(mode);
    mode_ok = FALSE;
    if (cand->device.DeviceName[0] != L'\0') {
        mode_ok = EnumDisplaySettingsW(cand->device.DeviceName,
                                       ENUM_CURRENT_SETTINGS, &mode);
    }
    if (!mode_ok && (cand->primary || cand->device.DeviceName[0] == L'\0')) {
        ZeroMemory(&mode, sizeof(mode));
        mode.dmSize = (WORD)sizeof(mode);
        mode_ok = EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &mode);
    }
    if (mode_ok) {
        item->width = mode.dmPelsWidth;
        item->height = mode.dmPelsHeight;
    }

    info->count++;
}

static int psi_monitor_find(const GraphicsInfo *info, const wchar_t *device_name)
{
    unsigned i;

    if (info == NULL || device_name == NULL || device_name[0] == L'\0') {
        return -1;
    }
    for (i = 0; i < info->monitor_count; ++i) {
        if (_wcsicmp(info->monitors[i].device, device_name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void psi_monitor_append(GraphicsInfo *info,
                               const PsiDisplayCandidate *cand,
                               const DISPLAY_DEVICEW *monitor)
{
    DisplayMonitor *item;
    DEVMODEW mode;
    BOOL mode_ok;
    BOOL primary;

    if (info->monitor_count >= PSI_MAX_MONITORS) {
        return;
    }
    item = &info->monitors[info->monitor_count];
    psi_zero_memory(item, sizeof(*item));

    psi_str_copy(item->name, PSI_ARRAY_COUNT(item->name),
                 (monitor->DeviceString[0] != L'\0')
                     ? monitor->DeviceString
                     : monitor->DeviceName);
    psi_str_copy(item->adapter, PSI_ARRAY_COUNT(item->adapter),
                 (cand->device.DeviceString[0] != L'\0')
                     ? cand->device.DeviceString
                     : cand->device.DeviceName);
    psi_str_copy(item->device, PSI_ARRAY_COUNT(item->device),
                 monitor->DeviceName);
    primary = (monitor->StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? TRUE : FALSE;
    if (!primary && info->monitor_count == 0 && cand->primary) {
        primary = TRUE;
    }
    item->primary = primary;

    ZeroMemory(&mode, sizeof(mode));
    mode.dmSize = (WORD)sizeof(mode);
    mode_ok = FALSE;
    if (monitor->DeviceName[0] != L'\0') {
        mode_ok = EnumDisplaySettingsW(monitor->DeviceName,
                                       ENUM_CURRENT_SETTINGS, &mode);
    }
    if (!mode_ok && cand->device.DeviceName[0] != L'\0') {
        ZeroMemory(&mode, sizeof(mode));
        mode.dmSize = (WORD)sizeof(mode);
        mode_ok = EnumDisplaySettingsW(cand->device.DeviceName,
                                       ENUM_CURRENT_SETTINGS, &mode);
    }
    if (mode_ok) {
        item->width = mode.dmPelsWidth;
        item->height = mode.dmPelsHeight;
        item->refresh_hz = mode.dmDisplayFrequency;
    }

    info->monitor_count++;
}

static void psi_collect_monitors(GraphicsInfo *info,
                                 const PsiDisplayCandidate *candidates,
                                 unsigned candidate_count)
{
    unsigned pass;
    unsigned i;
    unsigned j;

    for (pass = 0; pass < 2 && info->monitor_count == 0; ++pass) {
        for (i = 0; i < candidate_count; ++i) {
            const PsiDisplayCandidate *cand = &candidates[i];

            for (j = 0; j < PSI_MAX_MONITORS; ++j) {
                DISPLAY_DEVICEW monitor;

                ZeroMemory(&monitor, sizeof(monitor));
                monitor.cb = (DWORD)sizeof(monitor);
                if (!EnumDisplayDevicesW(cand->device.DeviceName, j,
                                         &monitor, 0)) {
                    break;
                }
                if (pass == 0 &&
                    ((monitor.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0 ||
                     (monitor.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) != 0)) {
                    continue;
                }
                if (monitor.DeviceName[0] == L'\0' ||
                    psi_monitor_find(info, monitor.DeviceName) >= 0) {
                    continue;
                }
                psi_monitor_append(info, cand, &monitor);
                if (info->monitor_count >= PSI_MAX_MONITORS) {
                    return;
                }
            }
        }
    }
}

BOOL psi_graphics_collect(GraphicsInfo *info, SystemReport *report)
{
    PsiDisplayCandidate candidates[PSI_MAX_GRAPHICS];
    unsigned candidate_count = 0;
    unsigned i;
    unsigned pass;

    if (info == NULL) {
        return FALSE;
    }
    info->count = 0;
    info->monitor_count = 0;
    psi_zero_memory(info->monitors, sizeof(info->monitors));
    psi_zero_memory(candidates, sizeof(candidates));

    for (i = 0; i < PSI_MAX_GRAPHICS; ++i) {
        DISPLAY_DEVICEW device;

        ZeroMemory(&device, sizeof(device));
        device.cb = (DWORD)sizeof(device);
        if (!EnumDisplayDevicesW(NULL, i, &device, 0)) {
            break;
        }
        candidates[candidate_count].device = device;
        candidates[candidate_count].attached =
            (device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) ? TRUE : FALSE;
        candidates[candidate_count].primary =
            (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? TRUE : FALSE;
        candidates[candidate_count].mirroring =
            (device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) ? TRUE : FALSE;
        candidate_count++;
    }

    psi_collect_monitors(info, candidates, candidate_count);

    /* 三轮优先级：已连接桌面的非镜像适配器 -> 已连接桌面的适配器 ->
     * 全部适配器（无桌面/远程会话等环境）。任一轮有结果即停止。 */
    for (pass = 0; pass < 3 && info->count == 0; ++pass) {
        for (i = 0; i < candidate_count && info->count < PSI_MAX_GRAPHICS; ++i) {
            const PsiDisplayCandidate *cand = &candidates[i];
            int duplicate = -1;

            if (pass == 0) {
                if (!cand->attached || cand->mirroring) {
                    continue;
                }
            } else if (pass == 1) {
                if (!cand->attached) {
                    continue;
                }
            }

            duplicate = psi_display_find_duplicate(info, cand->device.DeviceName,
                                                   cand->device.DeviceString);
            if (duplicate >= 0) {
                /* 合并同一显卡的多显示器条目时保留主显示器标记 */
                if (cand->primary) {
                    info->items[duplicate].primary = TRUE;
                }
                continue;
            }
            psi_display_append(info, cand);
        }
    }

    if (info->count == 0 && report != NULL) {
        psi_add_warning(report,
                        L"未检测到显示适配器（可能为无显示输出或远程会话环境）");
    }

    /* 无显示适配器不算致命错误，PE/服务器环境属于正常情况。 */
    return TRUE;
}
