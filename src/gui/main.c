/*
 * main.c - PESysInfo Win32 GUI 渲染层
 *
 * 纯 C + Win32 API（不使用 MFC/Qt/.NET）。GUI 只消费 core.lib 暴露的
 * PsiReport 模型：
 *   - psi_probe_collect()  采集一次并构造模型；
 *   - psi_model_json()     生成宽字符 JSON（原始数据页 / 复制 / 保存共用）；
 *   - psi_model_find_item() 按 key 读取概览字段；
 *   - psi_model_free()     释放模型。
 *
 * 页面：概览 / 处理器 / 内存 / 存储 / 图形 / 网络 / 原始数据 / 关于。
 * 底部按钮：刷新、复制 JSON、保存 JSON、退出；状态栏显示采集时间。
 * F5 作为刷新快捷键（不是退出热键；关闭窗口 / Alt+F4 退出）。
 */
#include "probe.h"

#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <wchar.h>

#if defined(_M_X64) || defined(_M_AMD64)
#define PSI_GUI_ARCH L"x64"
#elif defined(_M_IX86)
#define PSI_GUI_ARCH L"x86"
#else
#define PSI_GUI_ARCH L"unknown"
#endif

#define PSI_GUI_TITLE        L"PESysInfo - 硬件信息"
#define PSI_GUI_CLASS_NAME   L"PESysInfoGuiWindow"
#define PSI_GUI_MIN_WIDTH    760
#define PSI_GUI_MIN_HEIGHT   520

#define PSI_GUI_VALUE_CCH    (PSI_DISPLAY_CCH + 64)

/* 控件 ID */
#define IDC_TAB              1001
#define IDC_LIST_OVERVIEW    1002
#define IDC_LIST_CPU         1003
#define IDC_LIST_MEMORY      1004
#define IDC_LIST_STORAGE     1005
#define IDC_EDIT_RAW         1006
#define IDC_STATIC_ABOUT     1007
#define IDC_STATUSBAR        1008
#define IDC_LIST_GRAPHICS    1009
#define IDC_LIST_NETWORK     1014
#define IDC_BTN_REFRESH      1010
#define IDC_BTN_COPY         1011
#define IDC_BTN_SAVE         1012
#define IDC_BTN_EXIT         1013

typedef enum {
    PAGE_OVERVIEW = 0,
    PAGE_CPU,
    PAGE_MEMORY,
    PAGE_STORAGE,
    PAGE_GRAPHICS,
    PAGE_NETWORK,
    PAGE_RAW,
    PAGE_ABOUT,
    PAGE_COUNT
} GuiPage;

/* 全局状态：GUI 进程内单窗口，使用静态变量即可。 */
static HINSTANCE g_instance;
static HWND g_main_window;
static HWND g_tab_control;
static HWND g_list_overview;
static HWND g_list_cpu;
static HWND g_list_memory;
static HWND g_list_storage;
static HWND g_list_graphics;
static HWND g_list_network;
static HWND g_edit_raw;
static HWND g_static_about;
static HWND g_status_bar;
static HWND g_btn_refresh;
static HWND g_btn_copy;
static HWND g_btn_save;
static HWND g_btn_exit;

static HFONT g_ui_font;
static BOOL  g_ui_font_owned;   /* TRUE: CreateFont 创建，需要 DeleteObject */
static int   g_dpi = 96;

static PsiReport g_report;
static BOOL      g_has_report;
static wchar_t  *g_json;        /* psi_model_json 生成，psi_model_free_text 释放 */

/* ListView 折叠分组状态（刷新后保留用户展开/折叠选择）。 */
typedef struct {
    int  id;
    BOOL collapsed;
} GuiGroupState;

#define GUI_MAX_GROUP_STATES 64
#define GUI_GROUP_SUMMARY    1
#define GUI_GROUP_MEMORY     100
#define GUI_GROUP_DISK       100
#define GUI_GROUP_VOLUME     500
#define GUI_GROUP_NETWORK    100

static GuiGroupState g_memory_groups[GUI_MAX_GROUP_STATES];
static size_t        g_memory_group_count;
static GuiGroupState g_storage_groups[GUI_MAX_GROUP_STATES];
static size_t        g_storage_group_count;
static GuiGroupState g_network_groups[GUI_MAX_GROUP_STATES];
static size_t        g_network_group_count;

/* 当前页面内容区（由 layout_controls 计算，供切换页面时重排 ListView 用）。 */
static RECT g_page_rect;
static int  g_page_width;

static const wchar_t *const k_page_titles[PAGE_COUNT] = {
    L"概览", L"处理器", L"内存", L"存储", L"图形", L"网络", L"原始数据", L"关于"
};

static const wchar_t k_about_text[] =
    L"PESysInfo  版本 " PSI_TOOL_VERSION L"（" PSI_GUI_ARCH L"）\r\n"
    L"\r\n"
    L"面向 Windows PE 的硬件信息检测工具；\r\n"
    L"\r\n"
    L"纯 C + Win32 API 实现，静态链接 C 运行库。\r\n"
    L"数据来源：SMBIOS、CPUID、注册表与 Windows API。\r\n"
    L"\r\n"
    L"操作说明：\r\n"
    L"  · F5 重新采集并刷新全部页面；\r\n"
    L"  · “复制 JSON”写入剪贴板；\r\n"
    L"  · “保存 JSON”另存为 UTF-8（无 BOM）文件；\r\n"
    L"  · 关闭窗口或 Alt+F4 退出程序。\r\n";

/* ------------------------------------------------------------------ */
/* 通用小工具                                                          */
/* ------------------------------------------------------------------ */
static int dip(int value)
{
    return MulDiv(value, g_dpi, 96);
}

static int get_system_dpi(void)
{
    HDC screen = GetDC(NULL);
    int dpi = 96;

    if (screen != NULL) {
        dpi = GetDeviceCaps(screen, LOGPIXELSY);
        ReleaseDC(NULL, screen);
    }
    if (dpi < 96) {
        dpi = 96;
    }
    return dpi;
}

static void show_message(const wchar_t *text, UINT icon)
{
    MessageBoxW(g_main_window, text, PSI_GUI_TITLE, MB_OK | icon);
}

static void append_text(wchar_t *buffer, size_t cch, const wchar_t *text)
{
    size_t used;

    if (buffer == NULL || cch == 0 || text == NULL) {
        return;
    }
    used = wcslen(buffer);
    if (used + 1 >= cch) {
        return;
    }
    wcsncpy_s(buffer + used, cch - used, text, _TRUNCATE);
}

static void copy_text(wchar_t *buffer, size_t cch, const wchar_t *text)
{
    if (buffer == NULL || cch == 0) {
        return;
    }
    if (text == NULL) {
        buffer[0] = L'\0';
        return;
    }
    wcsncpy_s(buffer, cch, text, _TRUNCATE);
}

/* 把 PsiItem 的 display 加上 estimated/unavailable 标记。 */
static void format_item_value(const PsiItem *item, wchar_t *buffer, size_t cch)
{
    if (buffer == NULL || cch == 0) {
        return;
    }
    if (item == NULL || item->display[0] == L'\0') {
        copy_text(buffer, cch, L"未知");
        if (item != NULL && item->estimated) {
            append_text(buffer, cch, L"（推测）");
        }
        if (item != NULL && item->unavailable) {
            append_text(buffer, cch, L"（不可用）");
        }
        return;
    }
    copy_text(buffer, cch, item->display);
    if (item->estimated) {
        append_text(buffer, cch, L"（推测）");
    }
    if (item->unavailable) {
        append_text(buffer, cch, L"（不可用）");
    }
}

static BOOL item_has_display(const PsiItem *item)
{
    return (item != NULL && item->display[0] != L'\0') ? TRUE : FALSE;
}

static void format_bytes_local(unsigned long long bytes, wchar_t *buffer, size_t cch)
{
    static const wchar_t *const units[] = { L"B", L"KB", L"MB", L"GB", L"TB", L"PB" };
    double value;
    size_t unit = 0;

    if (buffer == NULL || cch == 0) {
        return;
    }
    if (bytes == 0) {
        copy_text(buffer, cch, L"0 B");
        return;
    }
    value = (double)bytes;
    while (value >= 1024.0 && (unit + 1) < PSI_ARRAY_COUNT(units)) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        swprintf_s(buffer, cch, L"%llu B", bytes);
    } else {
        swprintf_s(buffer, cch, L"%.2f %ls", value, units[unit]);
    }
}

/* ------------------------------------------------------------------ */
/* JSON 生命周期                                                       */
/* ------------------------------------------------------------------ */
static void free_json(void)
{
    if (g_json != NULL) {
        psi_model_free_text(g_json);
        g_json = NULL;
    }
}

static BOOL ensure_json(void)
{
    if (g_json != NULL) {
        return TRUE;
    }
    if (!g_has_report) {
        return FALSE;
    }
    free_json();
    if (!psi_model_json(&g_report, &g_json)) {
        g_json = NULL;
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* 状态栏                                                              */
/* ------------------------------------------------------------------ */
static void set_status_message(const wchar_t *text)
{
    if (g_status_bar == NULL) {
        return;
    }
    SendMessageW(g_status_bar, SB_SETTEXTW, (WPARAM)0, (LPARAM)text);
}

static void update_status_details(void)
{
    wchar_t time_text[128];

    if (g_status_bar == NULL) {
        return;
    }
    if (g_has_report) {
        const SYSTEMTIME *st = &g_report.generated;

        swprintf_s(time_text, PSI_ARRAY_COUNT(time_text),
                   L"采集时间: %04u-%02u-%02u %02u:%02u:%02u",
                   (unsigned)st->wYear, (unsigned)st->wMonth, (unsigned)st->wDay,
                   (unsigned)st->wHour, (unsigned)st->wMinute, (unsigned)st->wSecond);
    } else {
        copy_text(time_text, PSI_ARRAY_COUNT(time_text), L"采集时间: -");
    }
    SendMessageW(g_status_bar, SB_SETTEXTW, (WPARAM)1, (LPARAM)time_text);
}

static void set_status_parts(int width)
{
    int parts[2];

    if (g_status_bar == NULL) {
        return;
    }
    if (width < dip(480)) {
        width = dip(480);
    }
    parts[0] = width - dip(300);
    parts[1] = -1;
    if (parts[0] < dip(170)) {
        parts[0] = dip(170);
    }
    SendMessageW(g_status_bar, SB_SETPARTS, (WPARAM)2, (LPARAM)parts);
}

/* ------------------------------------------------------------------ */
/* ListView 辅助                                                       */
/* ------------------------------------------------------------------ */
static void list_clear(HWND list)
{
    if (list != NULL) {
        ListView_DeleteAllItems(list);
    }
}

static void list_add_row(HWND list, int row, const wchar_t *name, const wchar_t *value)
{
    LVITEMW item;

    if (list == NULL || name == NULL || value == NULL) {
        return;
    }
    ZeroMemory(&item, sizeof(item));
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.iSubItem = 0;
    item.pszText = (LPWSTR)(void *)name;
    if (ListView_InsertItem(list, &item) < 0) {
        return;
    }
    ListView_SetItemText(list, row, 1, (LPWSTR)(void *)value);
}

static void list_add_row_group(HWND list, int row, const wchar_t *name,
                               const wchar_t *value, int group_id)
{
    LVITEMW item;

    if (list == NULL || name == NULL || value == NULL) {
        return;
    }
    ZeroMemory(&item, sizeof(item));
    item.mask = LVIF_TEXT | LVIF_GROUPID;
    item.iItem = row;
    item.iSubItem = 0;
    item.pszText = (LPWSTR)(void *)name;
    item.iGroupId = group_id;
    if (ListView_InsertItem(list, &item) < 0) {
        return;
    }
    ListView_SetItemText(list, row, 1, (LPWSTR)(void *)value);
}

static void setup_list_columns(HWND list)
{
    LVCOLUMNW column;
    wchar_t first_title[] = L"项目";
    wchar_t second_title[] = L"值";

    if (list == NULL) {
        return;
    }
    ZeroMemory(&column, sizeof(column));
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.iSubItem = 0;
    column.pszText = first_title;
    column.cx = dip(260);
    ListView_InsertColumn(list, 0, &column);

    column.iSubItem = 1;
    column.pszText = second_title;
    column.cx = dip(560);
    ListView_InsertColumn(list, 1, &column);
}

static void resize_list_columns(HWND list, int total_width)
{
    int first;
    int second;

    if (list == NULL) {
        return;
    }
    first = dip(250);
    if (first > total_width - dip(160)) {
        first = total_width - dip(160);
    }
    if (first < dip(120)) {
        first = dip(120);
    }
    second = total_width - first - dip(28);
    if (second < dip(80)) {
        second = dip(80);
    }
    ListView_SetColumnWidth(list, 0, first);
    ListView_SetColumnWidth(list, 1, second);
}

static HWND create_list_control(HWND parent, int identifier)
{
    HWND list;

    list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                           WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
                               WS_TABSTOP | WS_CLIPSIBLINGS,
                           0, 0, 0, 0, parent, (HMENU)(INT_PTR)identifier,
                           g_instance, NULL);
    if (list == NULL) {
        return NULL;
    }
    ListView_SetExtendedListViewStyle(
        list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    setup_list_columns(list);
    return list;
}

/* ------------------------------------------------------------------ */
/* 页面数据填充                                                        */
/* ------------------------------------------------------------------ */
static void set_raw_text(const wchar_t *text)
{
    size_t length;
    size_t i;
    size_t out = 0;
    wchar_t *converted;

    if (g_edit_raw == NULL || text == NULL) {
        return;
    }
    length = wcslen(text);
    converted = (wchar_t *)HeapAlloc(GetProcessHeap(), 0,
                                     (length * 2 + 1) * sizeof(wchar_t));
    if (converted == NULL) {
        SetWindowTextW(g_edit_raw, text);
        return;
    }
    for (i = 0; i < length; ++i) {
        if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) {
            converted[out++] = L'\r';
        }
        converted[out++] = text[i];
    }
    converted[out] = L'\0';
    SetWindowTextW(g_edit_raw, converted);
    HeapFree(GetProcessHeap(), 0, converted);
    SendMessageW(g_edit_raw, EM_SETSEL, (WPARAM)0, (LPARAM)0);
    SendMessageW(g_edit_raw, EM_SCROLLCARET, (WPARAM)0, (LPARAM)0);
}

static void update_raw_text(void)
{
    if (g_edit_raw == NULL) {
        return;
    }
    if (g_json != NULL) {
        set_raw_text(g_json);
    } else if (!g_has_report) {
        SetWindowTextW(g_edit_raw, L"尚未采集数据。\r\n\r\n请点击底部“刷新”重新采集。");
    } else {
        SetWindowTextW(g_edit_raw, L"生成 JSON 失败。\r\n\r\n请点击底部“刷新”重试。");
    }
}

static BOOL disk_total_bytes(unsigned long long *total_out)
{
    PsiSection *section;
    size_t i;
    unsigned long long total = 0;
    BOOL found = FALSE;

    section = psi_model_find_section(&g_report, L"storage");
    if (section == NULL) {
        return FALSE;
    }
    for (i = 0; i < section->count; ++i) {
        const PsiItem *item = &section->items[i];
        size_t key_length;

        if (item->type != PSI_ITEM_U64 || item->unavailable) {
            continue;
        }
        if (_wcsnicmp(item->key, L"storage.disk.", 13) != 0) {
            continue;
        }
        key_length = wcslen(item->key);
        if (key_length < 11 ||
            _wcsicmp(item->key + key_length - 11, L".size_bytes") != 0) {
            continue;
        }
        total += item->value.u64;
        found = TRUE;
    }
    if (found && total_out != NULL) {
        *total_out = total;
    }
    return found;
}

static void overview_system_text(wchar_t *buffer, size_t cch)
{
    PsiItem *name;
    PsiItem *version;

    if (buffer == NULL || cch == 0) {
        return;
    }
    buffer[0] = L'\0';
    name = psi_model_find_item(&g_report, L"system.os_name");
    version = psi_model_find_item(&g_report, L"system.os_version");
    if (item_has_display(name)) {
        append_text(buffer, cch, name->display);
    }
    if (item_has_display(version)) {
        if (buffer[0] != L'\0') {
            append_text(buffer, cch, L" ");
        }
        append_text(buffer, cch, version->display);
    }
    if (buffer[0] == L'\0') {
        copy_text(buffer, cch, L"未知");
    }
}

static void overview_gpu_text(wchar_t *buffer, size_t cch)
{
    PsiItem *item;

    item = psi_model_find_item(&g_report, L"graphics.adapter.0.name");
    if (item_has_display(item)) {
        format_item_value(item, buffer, cch);
        return;
    }
    item = psi_model_find_item(&g_report, L"graphics.count");
    if (item != NULL && item->type == PSI_ITEM_U64 && item->value.u64 == 0) {
        copy_text(buffer, cch, L"未检测到显示适配器");
    } else {
        copy_text(buffer, cch, L"未知");
    }
}

static void overview_cache_text(wchar_t *buffer, size_t cch)
{
    static const wchar_t *const keys[] = {
        L"cpu.l1_kb", L"cpu.l2_kb", L"cpu.l3_kb"
    };
    static const wchar_t *const labels[] = {
        L"L1 ", L"L2 ", L"L3 "
    };
    size_t i;

    if (buffer == NULL || cch == 0) {
        return;
    }
    buffer[0] = L'\0';
    for (i = 0; i < PSI_ARRAY_COUNT(keys); ++i) {
        PsiItem *item = psi_model_find_item(&g_report, keys[i]);

        if (i > 0) {
            psi_str_append(buffer, cch, L" / ");
        }
        psi_str_append(buffer, cch, labels[i]);
        if (item != NULL && item->display[0] != L'\0') {
            psi_str_append(buffer, cch, item->display);
        } else {
            psi_str_append(buffer, cch, L"未知");
        }
    }
}

static void populate_overview(void)
{
    static const wchar_t *const labels[] = {
        L"处理器型号", L"物理核心", L"逻辑线程", L"基准频率", L"最大频率",
        L"当前频率", L"缓存", L"安装内存", L"当前可用内存", L"内存插槽总数",
        L"已安装内存条", L"物理磁盘数量"
    };
    static const wchar_t *const keys[] = {
        L"cpu.brand", L"cpu.cores", L"cpu.logical", L"cpu.base_mhz",
        L"cpu.max_mhz", L"cpu.current_mhz", NULL,
        L"memory.installed_bytes", L"memory.available_bytes",
        L"memory.slot_count", L"memory.module_count", L"storage.disk_count"
    };
    wchar_t value[PSI_GUI_VALUE_CCH];
    unsigned long long disk_total = 0;
    size_t i;
    int row = 0;

    list_clear(g_list_overview);
    for (i = 0; i < PSI_ARRAY_COUNT(labels); ++i) {
        if (keys[i] == NULL) {
            overview_cache_text(value, PSI_ARRAY_COUNT(value));
        } else {
            PsiItem *item = psi_model_find_item(&g_report, keys[i]);

            format_item_value(item, value, PSI_ARRAY_COUNT(value));
        }
        list_add_row(g_list_overview, row, labels[i], value);
        ++row;
    }

    if (disk_total_bytes(&disk_total)) {
        format_bytes_local(disk_total, value, PSI_ARRAY_COUNT(value));
    } else {
        copy_text(value, PSI_ARRAY_COUNT(value), L"未知");
    }
    list_add_row(g_list_overview, row, L"磁盘总容量", value);
    ++row;

    overview_system_text(value, PSI_ARRAY_COUNT(value));
    list_add_row(g_list_overview, row, L"系统版本", value);
    ++row;

    overview_gpu_text(value, PSI_ARRAY_COUNT(value));
    list_add_row(g_list_overview, row, L"显卡", value);
}

static void capture_group_states(HWND list, GuiGroupState *states, size_t *count)
{
    int group_count;
    int i;

    if (count != NULL) {
        *count = 0;
    }
    if (list == NULL || states == NULL || count == NULL) {
        return;
    }
    group_count = (int)ListView_GetGroupCount(list);
    for (i = 0; i < group_count && *count < GUI_MAX_GROUP_STATES; ++i) {
        LVGROUP group;

        ZeroMemory(&group, sizeof(group));
        group.cbSize = sizeof(group);
        group.mask = LVGF_GROUPID | LVGF_STATE;
        group.stateMask = LVGS_COLLAPSED;
        if (ListView_GetGroupInfoByIndex(list, i, &group) >= 0) {
            states[*count].id = group.iGroupId;
            states[*count].collapsed =
                (group.state & LVGS_COLLAPSED) ? TRUE : FALSE;
            ++*count;
        }
    }
}

static void insert_group(HWND list, const GuiGroupState *saved, size_t saved_count,
                         int id, const wchar_t *header, BOOL default_collapsed)
{
    LVGROUP group;
    BOOL collapsed = default_collapsed;

    if (list == NULL || header == NULL) {
        return;
    }
    if (saved != NULL) {
        size_t i;
        for (i = 0; i < saved_count; ++i) {
            if (saved[i].id == id) {
                collapsed = saved[i].collapsed;
                break;
            }
        }
    }
    ZeroMemory(&group, sizeof(group));
    group.cbSize = sizeof(group);
    group.mask = LVGF_HEADER | LVGF_GROUPID | LVGF_STATE | LVGF_ALIGN;
    group.iGroupId = id;
    group.pszHeader = (LPWSTR)(void *)header;
    group.cchHeader = (int)wcslen(header);
    group.stateMask = LVGS_COLLAPSIBLE | LVGS_COLLAPSED;
    group.state = LVGS_COLLAPSIBLE |
                  (collapsed ? LVGS_COLLAPSED : 0);
    group.uAlign = LVGA_HEADER_LEFT;
    ListView_InsertGroup(list, -1, &group);
}

static BOOL key_index(const wchar_t *key, const wchar_t *prefix, int *index)
{
    const wchar_t *p;
    wchar_t *end = NULL;
    unsigned long value;

    if (key == NULL || prefix == NULL || index == NULL) {
        return FALSE;
    }
    if (_wcsnicmp(key, prefix, (int)wcslen(prefix)) != 0) {
        return FALSE;
    }
    p = key + wcslen(prefix);
    if (*p == L'\0') {
        return FALSE;
    }
    value = wcstoul(p, &end, 10);
    if (end == p || end == NULL || *end != L'.' || value > 9999) {
        return FALSE;
    }
    *index = (int)value;
    return TRUE;
}

static void append_item_display(wchar_t *buffer, size_t cch, const wchar_t *key)
{
    PsiItem *item = psi_model_find_item(&g_report, key);

    if (item == NULL || item->display[0] == L'\0') {
        return;
    }
    if (buffer[0] != L'\0') {
        psi_str_append(buffer, cch, L" · ");
    }
    psi_str_append(buffer, cch, item->display);
}

static void memory_group_title(int index, wchar_t *buffer, size_t cch)
{
    wchar_t key[PSI_KEY_CCH];

    swprintf_s(buffer, cch, L"内存插槽 #%d", index + 1);
    swprintf_s(key, PSI_ARRAY_COUNT(key), L"memory.module.%d.locator", index);
    append_item_display(buffer, cch, key);
    swprintf_s(key, PSI_ARRAY_COUNT(key), L"memory.module.%d.bank", index);
    append_item_display(buffer, cch, key);
    swprintf_s(key, PSI_ARRAY_COUNT(key), L"memory.module.%d.size_bytes", index);
    append_item_display(buffer, cch, key);
}

static unsigned disk_display_index(int index)
{
    wchar_t key[PSI_KEY_CCH];
    PsiItem *item;

    swprintf_s(key, PSI_ARRAY_COUNT(key), L"storage.disk.%d.index", index);
    item = psi_model_find_item(&g_report, key);
    if (item != NULL && item->type == PSI_ITEM_U64) {
        return (unsigned)item->value.u64;
    }
    return (unsigned)index;
}

static void disk_group_title(int index, wchar_t *buffer, size_t cch)
{
    wchar_t key[PSI_KEY_CCH];
    unsigned display_index = disk_display_index(index);

    swprintf_s(buffer, cch, L"磁盘 #%u", display_index);
    swprintf_s(key, PSI_ARRAY_COUNT(key), L"storage.disk.%d.model", index);
    append_item_display(buffer, cch, key);
    swprintf_s(key, PSI_ARRAY_COUNT(key), L"storage.disk.%d.size_bytes", index);
    append_item_display(buffer, cch, key);
}

static void volume_group_title(int index, wchar_t *buffer, size_t cch)
{
    wchar_t key[PSI_KEY_CCH];

    swprintf_s(buffer, cch, L"卷 #%d", index);
    swprintf_s(key, PSI_ARRAY_COUNT(key), L"storage.volume.%d.letter", index);
    append_item_display(buffer, cch, key);
    swprintf_s(key, PSI_ARRAY_COUNT(key), L"storage.volume.%d.label", index);
    append_item_display(buffer, cch, key);
}

static BOOL item_is_memory_module(const PsiItem *item, int *index)
{
    return key_index(item->key, L"memory.module.", index);
}

static BOOL item_is_disk(const PsiItem *item, int *index)
{
    return key_index(item->key, L"storage.disk.", index);
}

static BOOL item_is_volume(const PsiItem *item, int *index)
{
    return key_index(item->key, L"storage.volume.", index);
}

static void populate_memory_groups(void)
{
    PsiSection *section;
    wchar_t value[PSI_GUI_VALUE_CCH];
    wchar_t title[160];
    size_t i;
    int row = 0;
    int module_index;
    unsigned module;
    BOOL has_item = FALSE;

    capture_group_states(g_list_memory, g_memory_groups, &g_memory_group_count);
    list_clear(g_list_memory);
    ListView_RemoveAllGroups(g_list_memory);
    section = psi_model_find_section(&g_report, L"memory");
    if (section == NULL) {
        list_add_row(g_list_memory, 0,
                     g_has_report ? L"（无数据）" : L"（尚未采集）",
                     g_has_report ? L"未知" : L"请点击“刷新”");
        return;
    }

    ListView_EnableGroupView(g_list_memory, TRUE);
    insert_group(g_list_memory, NULL, 0, GUI_GROUP_SUMMARY, L"内存总览", FALSE);
    for (i = 0; i < section->count; ++i) {
        const PsiItem *item = &section->items[i];

        if (item_is_memory_module(item, &module_index)) {
            continue;
        }
        format_item_value(item, value, PSI_ARRAY_COUNT(value));
        list_add_row_group(g_list_memory, row,
                           item->name[0] != L'\0' ? item->name : item->key,
                           value, GUI_GROUP_SUMMARY);
        ++row;
        has_item = TRUE;
    }

    for (module = 0; module < PSI_MAX_MEM_MODULES; ++module) {
        int count = 0;

        for (i = 0; i < section->count; ++i) {
            if (item_is_memory_module(&section->items[i], &module_index) &&
                module_index == (int)module) {
                ++count;
            }
        }
        if (count == 0) {
            continue;
        }
        memory_group_title((int)module, title, PSI_ARRAY_COUNT(title));
        insert_group(g_list_memory, g_memory_groups, g_memory_group_count,
                     GUI_GROUP_MEMORY + (int)module, title, TRUE);
        for (i = 0; i < section->count; ++i) {
            const PsiItem *item = &section->items[i];

            if (!item_is_memory_module(item, &module_index) ||
                module_index != (int)module) {
                continue;
            }
            format_item_value(item, value, PSI_ARRAY_COUNT(value));
            list_add_row_group(g_list_memory, row,
                               item->name[0] != L'\0' ? item->name : item->key,
                               value, GUI_GROUP_MEMORY + (int)module);
            ++row;
            has_item = TRUE;
        }
    }

    if (!has_item) {
        list_add_row_group(g_list_memory, row,
                           g_has_report ? L"（无数据）" : L"（尚未采集）",
                           g_has_report ? L"未知" : L"请点击“刷新”",
                           GUI_GROUP_SUMMARY);
    }
}

static void populate_storage_groups(void)
{
    PsiSection *section;
    wchar_t value[PSI_GUI_VALUE_CCH];
    wchar_t title[160];
    size_t i;
    int row = 0;
    int index;
    unsigned disk;
    unsigned volume;
    BOOL has_item = FALSE;

    capture_group_states(g_list_storage, g_storage_groups, &g_storage_group_count);
    list_clear(g_list_storage);
    ListView_RemoveAllGroups(g_list_storage);
    section = psi_model_find_section(&g_report, L"storage");
    if (section == NULL) {
        list_add_row(g_list_storage, 0,
                     g_has_report ? L"（无数据）" : L"（尚未采集）",
                     g_has_report ? L"未知" : L"请点击“刷新”");
        return;
    }

    ListView_EnableGroupView(g_list_storage, TRUE);
    insert_group(g_list_storage, NULL, 0, GUI_GROUP_SUMMARY, L"存储总览", FALSE);
    for (i = 0; i < section->count; ++i) {
        const PsiItem *item = &section->items[i];

        if (item_is_disk(item, &index) || item_is_volume(item, &index)) {
            continue;
        }
        format_item_value(item, value, PSI_ARRAY_COUNT(value));
        list_add_row_group(g_list_storage, row,
                           item->name[0] != L'\0' ? item->name : item->key,
                           value, GUI_GROUP_SUMMARY);
        ++row;
        has_item = TRUE;
    }

    for (disk = 0; disk < PSI_MAX_DISKS; ++disk) {
        int count = 0;

        for (i = 0; i < section->count; ++i) {
            if (item_is_disk(&section->items[i], &index) &&
                index == (int)disk) {
                ++count;
            }
        }
        if (count == 0) {
            continue;
        }
        disk_group_title((int)disk, title, PSI_ARRAY_COUNT(title));
        insert_group(g_list_storage, g_storage_groups, g_storage_group_count,
                     GUI_GROUP_DISK + (int)disk, title, TRUE);
        for (i = 0; i < section->count; ++i) {
            const PsiItem *item = &section->items[i];

            if (!item_is_disk(item, &index) || index != (int)disk) {
                continue;
            }
            format_item_value(item, value, PSI_ARRAY_COUNT(value));
            list_add_row_group(g_list_storage, row,
                               item->name[0] != L'\0' ? item->name : item->key,
                               value, GUI_GROUP_DISK + (int)disk);
            ++row;
            has_item = TRUE;
        }
    }

    for (volume = 0; volume < PSI_MAX_VOLUMES; ++volume) {
        int count = 0;

        for (i = 0; i < section->count; ++i) {
            if (item_is_volume(&section->items[i], &index) &&
                index == (int)volume) {
                ++count;
            }
        }
        if (count == 0) {
            continue;
        }
        volume_group_title((int)volume, title, PSI_ARRAY_COUNT(title));
        insert_group(g_list_storage, g_storage_groups, g_storage_group_count,
                     GUI_GROUP_VOLUME + (int)volume, title, TRUE);
        for (i = 0; i < section->count; ++i) {
            const PsiItem *item = &section->items[i];

            if (!item_is_volume(item, &index) || index != (int)volume) {
                continue;
            }
            format_item_value(item, value, PSI_ARRAY_COUNT(value));
            list_add_row_group(g_list_storage, row,
                               item->name[0] != L'\0' ? item->name : item->key,
                               value, GUI_GROUP_VOLUME + (int)volume);
            ++row;
            has_item = TRUE;
        }
    }

    if (!has_item) {
        list_add_row_group(g_list_storage, row,
                           g_has_report ? L"（无数据）" : L"（尚未采集）",
                           g_has_report ? L"未知" : L"请点击“刷新”",
                           GUI_GROUP_SUMMARY);
    }
}

static void network_group_title(int index, wchar_t *buffer, size_t cch)
{
    wchar_t key[PSI_KEY_CCH];

    swprintf_s(buffer, cch, L"网卡 #%d", index);
    swprintf_s(key, PSI_ARRAY_COUNT(key), L"network.adapter.%d.description", index);
    append_item_display(buffer, cch, key);
    swprintf_s(key, PSI_ARRAY_COUNT(key), L"network.adapter.%d.status", index);
    append_item_display(buffer, cch, key);
}

static BOOL item_is_network_adapter(const PsiItem *item, int *index)
{
    return key_index(item->key, L"network.adapter.", index);
}

static void populate_network_groups(void)
{
    PsiSection *section;
    wchar_t value[PSI_GUI_VALUE_CCH];
    wchar_t title[160];
    size_t i;
    int row = 0;
    int adapter_index;
    unsigned adapter;
    BOOL has_item = FALSE;

    capture_group_states(g_list_network, g_network_groups, &g_network_group_count);
    list_clear(g_list_network);
    ListView_RemoveAllGroups(g_list_network);
    section = psi_model_find_section(&g_report, L"network");
    if (section == NULL) {
        list_add_row(g_list_network, 0,
                     g_has_report ? L"（无数据）" : L"（尚未采集）",
                     g_has_report ? L"未知" : L"请点击“刷新”");
        return;
    }

    ListView_EnableGroupView(g_list_network, TRUE);
    insert_group(g_list_network, NULL, 0, GUI_GROUP_SUMMARY, L"网络总览", FALSE);
    for (i = 0; i < section->count; ++i) {
        const PsiItem *item = &section->items[i];

        if (item_is_network_adapter(item, &adapter_index)) {
            continue;
        }
        format_item_value(item, value, PSI_ARRAY_COUNT(value));
        list_add_row_group(g_list_network, row,
                           item->name[0] != L'\0' ? item->name : item->key,
                           value, GUI_GROUP_SUMMARY);
        ++row;
        has_item = TRUE;
    }

    for (adapter = 0; adapter < PSI_MAX_NET; ++adapter) {
        int count = 0;

        for (i = 0; i < section->count; ++i) {
            if (item_is_network_adapter(&section->items[i], &adapter_index) &&
                adapter_index == (int)adapter) {
                ++count;
            }
        }
        if (count == 0) {
            continue;
        }
        network_group_title((int)adapter, title, PSI_ARRAY_COUNT(title));
        insert_group(g_list_network, g_network_groups, g_network_group_count,
                     GUI_GROUP_NETWORK + (int)adapter, title, TRUE);
        for (i = 0; i < section->count; ++i) {
            const PsiItem *item = &section->items[i];

            if (!item_is_network_adapter(item, &adapter_index) ||
                adapter_index != (int)adapter) {
                continue;
            }
            format_item_value(item, value, PSI_ARRAY_COUNT(value));
            list_add_row_group(g_list_network, row,
                               item->name[0] != L'\0' ? item->name : item->key,
                               value, GUI_GROUP_NETWORK + (int)adapter);
            ++row;
            has_item = TRUE;
        }
    }

    if (!has_item) {
        list_add_row_group(g_list_network, row,
                           g_has_report ? L"（无数据）" : L"（尚未采集）",
                           g_has_report ? L"未知" : L"请点击“刷新”",
                           GUI_GROUP_SUMMARY);
    }
}

static void populate_section_list(HWND list, const wchar_t *section_key)
{
    PsiSection *section;
    wchar_t value[PSI_GUI_VALUE_CCH];
    size_t i;
    int row = 0;

    list_clear(list);
    section = psi_model_find_section(&g_report, section_key);
    if (section == NULL) {
        list_add_row(list, 0,
                     g_has_report ? L"（无数据）" : L"（尚未采集）",
                     g_has_report ? L"未知" : L"请点击“刷新”");
        return;
    }
    for (i = 0; i < section->count; ++i) {
        const PsiItem *item = &section->items[i];
        const wchar_t *name = (item->name[0] != L'\0') ? item->name : item->key;

        format_item_value(item, value, PSI_ARRAY_COUNT(value));
        list_add_row(list, row, name, value);
        ++row;
    }
    if (section->count == 0) {
        list_add_row(list, 0,
                     g_has_report ? L"（无数据）" : L"（尚未采集）",
                     g_has_report ? L"未知" : L"请点击“刷新”");
    }
}

static void populate_all(void)
{
    populate_overview();
    populate_section_list(g_list_cpu, L"cpu");
    populate_memory_groups();
    populate_storage_groups();
    populate_section_list(g_list_graphics, L"graphics");
    populate_network_groups();
    update_raw_text();
    update_status_details();
}

/* ------------------------------------------------------------------ */
/* 采集 / 刷新                                                          */
/* ------------------------------------------------------------------ */
static void refresh_report(BOOL user_initiated)
{
    PsiReport model;
    HCURSOR wait_cursor;
    HCURSOR previous_cursor = NULL;
    BOOL ok;

    wait_cursor = LoadCursorW(NULL, IDC_WAIT);
    if (wait_cursor != NULL) {
        previous_cursor = SetCursor(wait_cursor);
    }

    ZeroMemory(&model, sizeof(model));
    ok = psi_probe_collect(&model);
    if (ok) {
        psi_model_free(&g_report);
        g_report = model;
        g_has_report = TRUE;
        free_json();
        if (!psi_model_json(&g_report, &g_json)) {
            g_json = NULL;
        }
        populate_all();
        set_status_message(L"采集完成");
    } else {
        psi_model_free(&model);
        if (g_has_report) {
            if (user_initiated) {
                show_message(L"采集失败，已保留上一次的数据。\r\n\r\n可以稍后点击“刷新”重试。",
                             MB_ICONWARNING);
            }
            set_status_message(L"刷新失败，已保留上次数据");
        } else {
            if (user_initiated) {
                show_message(L"采集失败，暂时无法显示硬件信息。\r\n\r\n请点击“刷新”重试。",
                             MB_ICONERROR);
            }
            populate_all();
            set_status_message(L"采集失败，暂无可显示数据");
        }
    }

    if (previous_cursor != NULL) {
        SetCursor(previous_cursor);
    } else {
        SetCursor(LoadCursorW(NULL, IDC_ARROW));
    }
}

/* ------------------------------------------------------------------ */
/* 剪贴板 / 保存                                                        */
/* ------------------------------------------------------------------ */
static void copy_json_to_clipboard(void)
{
    size_t length;
    size_t bytes;
    HGLOBAL memory;
    void *target;

    if (!ensure_json()) {
        show_message(L"当前没有可复制的 JSON 数据。\r\n\r\n请先点击“刷新”采集硬件信息。",
                     MB_ICONWARNING);
        return;
    }
    length = wcslen(g_json);
    bytes = (length + 1) * sizeof(wchar_t);
    memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == NULL) {
        show_message(L"内存不足，复制 JSON 失败。", MB_ICONERROR);
        return;
    }
    target = GlobalLock(memory);
    if (target == NULL) {
        GlobalFree(memory);
        show_message(L"锁定剪贴板内存失败，复制 JSON 失败。", MB_ICONERROR);
        return;
    }
    memcpy(target, g_json, bytes);
    GlobalUnlock(memory);

    if (!OpenClipboard(g_main_window)) {
        GlobalFree(memory);
        show_message(L"无法打开剪贴板，复制 JSON 失败。", MB_ICONERROR);
        return;
    }
    if (!EmptyClipboard()) {
        CloseClipboard();
        GlobalFree(memory);
        show_message(L"清空剪贴板失败，复制 JSON 失败。", MB_ICONERROR);
        return;
    }
    if (SetClipboardData(CF_UNICODETEXT, memory) == NULL) {
        CloseClipboard();
        GlobalFree(memory);
        show_message(L"写入剪贴板失败，复制 JSON 失败。", MB_ICONERROR);
        return;
    }
    CloseClipboard();
    set_status_message(L"JSON 已复制到剪贴板");
}

static BOOL write_json_file(const wchar_t *path, DWORD *error_code)
{
    int needed;
    char *utf8;
    HANDLE file;
    DWORD written = 0;
    DWORD length;
    BOOL ok = TRUE;
    DWORD error = ERROR_SUCCESS;

    if (path == NULL || g_json == NULL) {
        if (error_code != NULL) {
            *error_code = ERROR_INVALID_PARAMETER;
        }
        return FALSE;
    }

    needed = WideCharToMultiByte(CP_UTF8, 0, g_json, -1, NULL, 0, NULL, NULL);
    if (needed <= 1) {
        if (error_code != NULL) {
            *error_code = GetLastError();
        }
        return FALSE;
    }
    utf8 = (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)needed);
    if (utf8 == NULL) {
        if (error_code != NULL) {
            *error_code = ERROR_NOT_ENOUGH_MEMORY;
        }
        return FALSE;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, g_json, -1, utf8, needed, NULL, NULL) <= 0) {
        error = GetLastError();
        HeapFree(GetProcessHeap(), 0, utf8);
        if (error_code != NULL) {
            *error_code = error;
        }
        return FALSE;
    }

    length = (DWORD)(needed - 1);
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        HeapFree(GetProcessHeap(), 0, utf8);
        if (error_code != NULL) {
            *error_code = error;
        }
        return FALSE;
    }
    if (!WriteFile(file, utf8, length, &written, NULL) || written != length) {
        error = GetLastError();
        if (error == ERROR_SUCCESS) {
            error = ERROR_WRITE_FAULT;
        }
        ok = FALSE;
    } else {
        FlushFileBuffers(file);
    }
    CloseHandle(file);
    HeapFree(GetProcessHeap(), 0, utf8);

    if (!ok) {
        DeleteFileW(path);
    }
    if (error_code != NULL) {
        *error_code = error;
    }
    return ok;
}

static void save_json_to_file(void)
{
    OPENFILENAMEW dialog;
    wchar_t path[MAX_PATH];
    wchar_t filter[] = L"JSON 文件 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0";
    DWORD error = ERROR_SUCCESS;
    HMODULE comdlg;
    BOOL (WINAPI *get_save_name)(LPOPENFILENAMEW);

    if (!ensure_json()) {
        show_message(L"当前没有可保存的 JSON 数据。\r\n\r\n请先点击“刷新”采集硬件信息。",
                     MB_ICONWARNING);
        return;
    }
    comdlg = LoadLibraryW(L"comdlg32.dll");
    get_save_name = (comdlg != NULL)
                        ? (BOOL (WINAPI *)(LPOPENFILENAMEW))GetProcAddress(
                              comdlg, "GetSaveFileNameW")
                        : NULL;
    if (get_save_name == NULL) {
        if (comdlg != NULL) {
            FreeLibrary(comdlg);
        }
        show_message(L"当前环境没有可用的保存对话框（comdlg32.dll）。\r\n"
                     L"可以使用 PESysInfo-cli.exe --output 保存 JSON。",
                     MB_ICONWARNING);
        return;
    }
    copy_text(path, PSI_ARRAY_COUNT(path), L"PESysInfo.json");
    ZeroMemory(&dialog, sizeof(dialog));
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_main_window;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path;
    dialog.nMaxFile = (DWORD)PSI_ARRAY_COUNT(path);
    dialog.lpstrDefExt = L"json";
    dialog.lpstrTitle = L"保存 JSON 数据";
    dialog.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST |
                   OFN_HIDEREADONLY;

    if (!get_save_name(&dialog)) {
        FreeLibrary(comdlg);
        return; /* 用户取消 */
    }
    FreeLibrary(comdlg);
    if (!write_json_file(path, &error)) {
        wchar_t message[512];

        swprintf_s(message, PSI_ARRAY_COUNT(message),
                   L"保存 JSON 文件失败。\r\n\r\n路径：%ls\r\n错误码：%lu\r\n\r\n"
                   L"请检查目标目录的写入权限和磁盘剩余空间。",
                   path, (unsigned long)error);
        show_message(message, MB_ICONERROR);
        return;
    }
    {
        wchar_t message[512];

        swprintf_s(message, PSI_ARRAY_COUNT(message), L"已保存到：%ls", path);
        set_status_message(message);
    }
}

/* ------------------------------------------------------------------ */
/* 字体                                                                */
/* ------------------------------------------------------------------ */
static HFONT create_font_named(const wchar_t *face, int point_size, int weight)
{
    HDC screen;
    LOGFONTW logfont;
    HFONT font;
    HFONT previous;
    wchar_t actual[LF_FACESIZE];

    screen = GetDC(NULL);
    if (screen == NULL) {
        return NULL;
    }
    ZeroMemory(&logfont, sizeof(logfont));
    logfont.lfHeight = -MulDiv(point_size, g_dpi, 72);
    logfont.lfWeight = weight;
    logfont.lfCharSet = DEFAULT_CHARSET;
    logfont.lfOutPrecision = OUT_DEFAULT_PRECIS;
    logfont.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    logfont.lfQuality = CLEARTYPE_QUALITY;
    logfont.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcsncpy_s(logfont.lfFaceName, LF_FACESIZE, face, _TRUNCATE);

    font = CreateFontIndirectW(&logfont);
    if (font == NULL) {
        ReleaseDC(NULL, screen);
        return NULL;
    }
    previous = (HFONT)SelectObject(screen, font);
    actual[0] = L'\0';
    if (GetTextFaceW(screen, (int)PSI_ARRAY_COUNT(actual), actual) <= 0) {
        actual[0] = L'\0';
    }
    SelectObject(screen, previous);
    ReleaseDC(NULL, screen);

    if (_wcsicmp(actual, face) == 0) {
        return font;
    }
    DeleteObject(font);
    return NULL;
}

static HFONT create_ui_font(void)
{
    HFONT font;

    font = create_font_named(L"Microsoft YaHei", 10, FW_NORMAL);
    if (font == NULL) {
        font = create_font_named(L"SimSun", 10, FW_NORMAL);
    }
    if (font != NULL) {
        g_ui_font_owned = TRUE;
        return font;
    }
    g_ui_font_owned = FALSE;
    return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

static void apply_font(HWND control)
{
    if (control != NULL && g_ui_font != NULL) {
        SendMessageW(control, WM_SETFONT, (WPARAM)g_ui_font, MAKELPARAM(TRUE, 0));
    }
}

/* ------------------------------------------------------------------ */
/* 布局 / 页面切换                                                      */
/* ------------------------------------------------------------------ */
static void show_page(int page)
{
    HWND shown = NULL;
    BOOL is_list = FALSE;

    ShowWindow(g_list_overview, (page == PAGE_OVERVIEW) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_list_cpu, (page == PAGE_CPU) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_list_memory, (page == PAGE_MEMORY) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_list_storage, (page == PAGE_STORAGE) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_list_graphics, (page == PAGE_GRAPHICS) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_list_network, (page == PAGE_NETWORK) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_edit_raw, (page == PAGE_RAW) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_static_about, (page == PAGE_ABOUT) ? SW_SHOW : SW_HIDE);

    switch (page) {
    case PAGE_OVERVIEW: shown = g_list_overview; is_list = TRUE; break;
    case PAGE_CPU: shown = g_list_cpu; is_list = TRUE; break;
    case PAGE_MEMORY: shown = g_list_memory; is_list = TRUE; break;
    case PAGE_STORAGE: shown = g_list_storage; is_list = TRUE; break;
    case PAGE_GRAPHICS: shown = g_list_graphics; is_list = TRUE; break;
    case PAGE_NETWORK: shown = g_list_network; is_list = TRUE; break;
    case PAGE_RAW: shown = g_edit_raw; break;
    case PAGE_ABOUT: shown = g_static_about; break;
    default: break;
    }
    if (shown == NULL) {
        return;
    }
    /* 页面控件创建时不可见，可能停留在 Tab 控件下方，显式置顶。 */
    SetWindowPos(shown, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    if (is_list && g_page_width > 0) {
        /* ListView 的表头宽度在 WM_SIZE 时才会跟随，切换页面后强制重排一次。 */
        MoveWindow(shown, g_page_rect.left, g_page_rect.top,
                   (int)(g_page_rect.right - g_page_rect.left),
                   (int)(g_page_rect.bottom - g_page_rect.top), TRUE);
        resize_list_columns(shown, g_page_width);
    }
    if (page == PAGE_RAW) {
        SendMessageW(g_edit_raw, EM_SCROLLCARET, (WPARAM)0, (LPARAM)0);
    }
}

static void layout_controls(HWND hwnd)
{
    RECT client;
    RECT status_rect;
    RECT page;
    int width;
    int height;
    int margin = dip(10);
    int gap = dip(8);
    int button_height = dip(30);
    int button_y;
    int status_height = 0;
    int tab_height;
    int buttons_width;
    int page_width;
    int page_height;
    int x;

    GetClientRect(hwnd, &client);
    width = (int)client.right;
    height = (int)client.bottom;
    if (width < 0) {
        width = 0;
    }
    if (height < 0) {
        height = 0;
    }

    if (g_status_bar != NULL) {
        SendMessageW(g_status_bar, WM_SIZE, (WPARAM)0, (LPARAM)0);
        if (GetWindowRect(g_status_bar, &status_rect)) {
            status_height = (int)(status_rect.bottom - status_rect.top);
        }
        set_status_parts(width);
    }

    button_y = height - status_height - gap - button_height;
    if (button_y < margin) {
        button_y = margin;
    }

    /* 底部按钮右对齐：刷新 | 复制 JSON | 保存 JSON | 退出 */
    buttons_width = dip(90) + gap + dip(120) + gap + dip(120) + gap + dip(90);
    x = width - margin - buttons_width;
    if (x < margin) {
        x = margin;
    }
    MoveWindow(g_btn_refresh, x, button_y, dip(90), button_height, TRUE);
    x += dip(90) + gap;
    MoveWindow(g_btn_copy, x, button_y, dip(120), button_height, TRUE);
    x += dip(120) + gap;
    MoveWindow(g_btn_save, x, button_y, dip(120), button_height, TRUE);
    x += dip(120) + gap;
    MoveWindow(g_btn_exit, x, button_y, dip(90), button_height, TRUE);

    tab_height = button_y - gap - margin;
    if (tab_height < dip(120)) {
        tab_height = dip(120);
    }
    MoveWindow(g_tab_control, margin, margin, width - margin * 2, tab_height, TRUE);

    ZeroMemory(&page, sizeof(page));
    page.right = width - margin * 2;
    page.bottom = tab_height;
    TabCtrl_AdjustRect(g_tab_control, FALSE, &page);
    OffsetRect(&page, margin, margin);
    InflateRect(&page, -dip(6), -dip(6));
    if (page.right < page.left) {
        page.right = page.left;
    }
    if (page.bottom < page.top) {
        page.bottom = page.top;
    }
    page_width = (int)(page.right - page.left);
    page_height = (int)(page.bottom - page.top);
    g_page_rect = page;
    g_page_width = page_width;

    MoveWindow(g_list_overview, page.left, page.top, page_width, page_height, TRUE);
    MoveWindow(g_list_cpu, page.left, page.top, page_width, page_height, TRUE);
    MoveWindow(g_list_memory, page.left, page.top, page_width, page_height, TRUE);
    MoveWindow(g_list_storage, page.left, page.top, page_width, page_height, TRUE);
    MoveWindow(g_list_graphics, page.left, page.top, page_width, page_height, TRUE);
    MoveWindow(g_list_network, page.left, page.top, page_width, page_height, TRUE);
    MoveWindow(g_edit_raw, page.left, page.top, page_width, page_height, TRUE);
    MoveWindow(g_static_about, page.left + dip(6), page.top + dip(6),
               page_width - dip(12), page_height - dip(12), TRUE);

    resize_list_columns(g_list_overview, page_width);
    resize_list_columns(g_list_cpu, page_width);
    resize_list_columns(g_list_memory, page_width);
    resize_list_columns(g_list_storage, page_width);
    resize_list_columns(g_list_graphics, page_width);
    resize_list_columns(g_list_network, page_width);

    update_status_details();
}

/* ------------------------------------------------------------------ */
/* 主窗口创建                                                          */
/* ------------------------------------------------------------------ */
static HWND create_button_control(HWND parent, int identifier, const wchar_t *text)
{
    return CreateWindowExW(0, WC_BUTTONW, text,
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                           0, 0, 0, 0, parent, (HMENU)(INT_PTR)identifier,
                           g_instance, NULL);
}

static void update_about_text(void)
{
    if (g_static_about != NULL) {
        SetWindowTextW(g_static_about, k_about_text);
    }
}

static BOOL create_controls(HWND hwnd)
{
    int i;

    g_main_window = hwnd;
    g_dpi = get_system_dpi();
    g_ui_font = create_ui_font();
    if (g_ui_font == NULL) {
        return FALSE;
    }

    g_tab_control = CreateWindowExW(0, WC_TABCONTROLW, L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS,
                                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_TAB,
                                    g_instance, NULL);

    g_list_overview = create_list_control(hwnd, IDC_LIST_OVERVIEW);
    g_list_cpu = create_list_control(hwnd, IDC_LIST_CPU);
    g_list_memory = create_list_control(hwnd, IDC_LIST_MEMORY);
    g_list_storage = create_list_control(hwnd, IDC_LIST_STORAGE);
    g_list_graphics = create_list_control(hwnd, IDC_LIST_GRAPHICS);
    g_list_network = create_list_control(hwnd, IDC_LIST_NETWORK);

    g_edit_raw = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"",
                                 WS_CHILD | WS_TABSTOP | ES_MULTILINE | ES_READONLY |
                                     ES_AUTOVSCROLL | ES_NOHIDESEL | WS_VSCROLL |
                                     WS_CLIPSIBLINGS,
                                 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_EDIT_RAW,
                                 g_instance, NULL);
    if (g_edit_raw != NULL) {
        SendMessageW(g_edit_raw, EM_SETLIMITTEXT, (WPARAM)0x7FFFFFFE, (LPARAM)0);
    }

    g_static_about = CreateWindowExW(0, L"STATIC", L"",
                                     WS_CHILD | SS_LEFT | SS_NOPREFIX | WS_CLIPSIBLINGS,
                                     0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_STATIC_ABOUT,
                                     g_instance, NULL);

    g_btn_refresh = create_button_control(hwnd, IDC_BTN_REFRESH, L"刷新");
    g_btn_copy = create_button_control(hwnd, IDC_BTN_COPY, L"复制 JSON");
    g_btn_save = create_button_control(hwnd, IDC_BTN_SAVE, L"保存 JSON");
    g_btn_exit = create_button_control(hwnd, IDC_BTN_EXIT, L"退出");

    g_status_bar = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                                   WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                   0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_STATUSBAR,
                                   g_instance, NULL);

    if (g_tab_control == NULL || g_list_overview == NULL || g_list_cpu == NULL ||
        g_list_memory == NULL || g_list_storage == NULL ||
        g_list_graphics == NULL || g_list_network == NULL ||
        g_edit_raw == NULL ||
        g_static_about == NULL || g_btn_refresh == NULL || g_btn_copy == NULL ||
        g_btn_save == NULL || g_btn_exit == NULL || g_status_bar == NULL) {
        return FALSE;
    }

    for (i = 0; i < PAGE_COUNT; ++i) {
        TCITEMW item;

        ZeroMemory(&item, sizeof(item));
        item.mask = TCIF_TEXT;
        item.pszText = (LPWSTR)(void *)k_page_titles[i];
        TabCtrl_InsertItem(g_tab_control, i, &item);
    }

    apply_font(g_tab_control);
    apply_font(g_list_overview);
    apply_font(g_list_cpu);
    apply_font(g_list_memory);
    apply_font(g_list_storage);
    apply_font(g_list_graphics);
    apply_font(g_list_network);
    apply_font(g_edit_raw);
    apply_font(g_static_about);
    apply_font(g_btn_refresh);
    apply_font(g_btn_copy);
    apply_font(g_btn_save);
    apply_font(g_btn_exit);
    apply_font(g_status_bar);

    update_about_text();
    layout_controls(hwnd);
    /* Tab 控件创建得最早，且子窗口创建后可能保持在兄弟窗口上层，
     * 这里显式沉底，保证页面内容（ListView/Edit/Static）始终显示在 Tab 之上。 */
    SetWindowPos(g_tab_control, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    show_page(PAGE_OVERVIEW);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* 窗口过程                                                            */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK main_window_proc(HWND hwnd, UINT message,
                                         WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_CREATE:
        if (!create_controls(hwnd)) {
            return -1;
        }
        return 0;

    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) {
            layout_controls(hwnd);
        }
        return 0;

    case WM_GETMINMAXINFO:
        {
            MINMAXINFO *info = (MINMAXINFO *)lparam;
            RECT min_rect;

            if (info != NULL) {
                SetRect(&min_rect, 0, 0, dip(PSI_GUI_MIN_WIDTH),
                        dip(PSI_GUI_MIN_HEIGHT));
                AdjustWindowRectEx(&min_rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
                info->ptMinTrackSize.x = min_rect.right - min_rect.left;
                info->ptMinTrackSize.y = min_rect.bottom - min_rect.top;
            }
        }
        return 0;

    case WM_NOTIFY:
        {
            const NMHDR *header = (const NMHDR *)lparam;

            if (header != NULL && header->idFrom == (UINT_PTR)IDC_TAB &&
                header->code == TCN_SELCHANGE) {
                int selected = TabCtrl_GetCurSel(g_tab_control);

                show_page(selected < 0 ? PAGE_OVERVIEW : selected);
                return 0;
            }
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_BTN_REFRESH:
            refresh_report(TRUE);
            return 0;
        case IDC_BTN_COPY:
            copy_json_to_clipboard();
            return 0;
        case IDC_BTN_SAVE:
            save_json_to_file();
            return 0;
        case IDC_BTN_EXIT:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        break;

    case WM_CTLCOLORSTATIC:
        {
            HDC device_context = (HDC)wparam;

            if (device_context != NULL) {
                SetBkColor(device_context, GetSysColor(COLOR_WINDOW));
                SetTextColor(device_context, GetSysColor(COLOR_WINDOWTEXT));
            }
        }
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);

    case WM_DESTROY:
        free_json();
        psi_model_free(&g_report);
        g_has_report = FALSE;
        if (g_ui_font_owned && g_ui_font != NULL) {
            DeleteObject(g_ui_font);
        }
        g_ui_font = NULL;
        g_ui_font_owned = FALSE;
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

/* ------------------------------------------------------------------ */
/* 入口                                                                */
/* ------------------------------------------------------------------ */
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous_instance,
                    PWSTR command_line, int show_command)
{
    INITCOMMONCONTROLSEX common_controls;
    WNDCLASSEXW window_class;
    ACCEL accelerator;
    HACCEL accelerators;
    MSG message;
    HWND window;
    RECT window_rect;
    int exit_code = 0;

    UNREFERENCED_PARAMETER(previous_instance);
    UNREFERENCED_PARAMETER(command_line);

    g_instance = instance;
    g_dpi = get_system_dpi();

    ZeroMemory(&common_controls, sizeof(common_controls));
    common_controls.dwSize = sizeof(common_controls);
    common_controls.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES |
                            ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    if (!InitCommonControlsEx(&common_controls)) {
        MessageBoxW(NULL,
                    L"初始化 Windows 通用控件失败，界面可能无法正常显示。",
                    PSI_GUI_TITLE, MB_OK | MB_ICONWARNING);
    }

    ZeroMemory(&window_class, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = main_window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    window_class.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    window_class.hIconSm = LoadIconW(NULL, IDI_APPLICATION);
    window_class.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    window_class.lpszClassName = PSI_GUI_CLASS_NAME;

    if (RegisterClassExW(&window_class) == 0) {
        MessageBoxW(NULL, L"注册窗口类失败，无法启动界面。",
                    PSI_GUI_TITLE, MB_OK | MB_ICONERROR);
        return 1;
    }

    SetRect(&window_rect, 0, 0, dip(PSI_GUI_MIN_WIDTH), dip(PSI_GUI_MIN_HEIGHT));
    AdjustWindowRectEx(&window_rect, WS_OVERLAPPEDWINDOW, FALSE, 0);

    window = CreateWindowExW(0, PSI_GUI_CLASS_NAME, PSI_GUI_TITLE,
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             (int)(window_rect.right - window_rect.left),
                             (int)(window_rect.bottom - window_rect.top),
                             NULL, NULL, instance, NULL);
    if (window == NULL) {
        MessageBoxW(NULL, L"创建主窗口失败，无法启动界面。",
                    PSI_GUI_TITLE, MB_OK | MB_ICONERROR);
        return 1;
    }

    /* 启动时先同步采集一次，避免窗口闪一下空白。 */
    refresh_report(FALSE);
    layout_controls(window);

    ShowWindow(window, show_command);
    UpdateWindow(window);

    ZeroMemory(&accelerator, sizeof(accelerator));
    accelerator.fVirt = FVIRTKEY;
    accelerator.key = VK_F5;
    accelerator.cmd = IDC_BTN_REFRESH;
    accelerators = CreateAcceleratorTableW(&accelerator, 1);

    ZeroMemory(&message, sizeof(message));
    for (;;) {
        const BOOL result = GetMessageW(&message, NULL, 0, 0);

        if (result == 0) {
            break;
        }
        if (result == (BOOL)-1) {
            MessageBoxW(window,
                        L"消息循环异常，程序将退出。",
                        PSI_GUI_TITLE, MB_OK | MB_ICONERROR);
            break;
        }
        if (accelerators == NULL ||
            !TranslateAcceleratorW(window, accelerators, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    if (accelerators != NULL) {
        DestroyAcceleratorTable(accelerators);
    }
    if (message.message == WM_QUIT) {
        exit_code = (int)message.wParam;
    }
    return exit_code;
}
