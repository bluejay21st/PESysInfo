/*
 * model.c - 把 SystemReport 转换为 UI/CLI 共用的 PsiReport 数据模型
 *
 * 原则：
 *  - data 保存原始值（字符串 / 整数 / 布尔 / null）；
 *  - display 保存格式化后的显示文本；
 *  - name 是中文显示名称，key 是稳定 ASCII 标识；
 *  - 空值显式标记 unavailable，不伪造 0。
 */
#include "model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static void make_indexed(wchar_t *buf, size_t cch, const wchar_t *fmt,
                         unsigned index, const wchar_t *suffix)
{
    swprintf_s(buf, cch, fmt, index);
    if (suffix != NULL && suffix[0] != L'\0') {
        psi_str_append(buf, cch, suffix);
    }
}

static void format_cache(unsigned kb, wchar_t *buf, size_t cch)
{
    if (kb == 0) {
        psi_str_copy(buf, cch, L"未知");
    } else if (kb >= 1024U) {
        swprintf_s(buf, cch, L"%.1f MB", (double)kb / 1024.0);
    } else {
        swprintf_s(buf, cch, L"%u KB", kb);
    }
}

static void format_speed(unsigned speed, wchar_t *buf, size_t cch)
{
    if (speed == 0) {
        psi_str_copy(buf, cch, L"未知");
    } else {
        swprintf_s(buf, cch, L"%u MT/s", speed);
    }
}

/* ------------------------------------------------------------------ */
/* 模型容器                                                            */
/* ------------------------------------------------------------------ */
BOOL psi_model_add_section(PsiReport *model, const wchar_t *key,
                           const wchar_t *name, PsiSection **out_section)
{
    PsiSection *sections;
    size_t new_capacity;

    if (model == NULL || key == NULL || name == NULL) {
        return FALSE;
    }
    if (model->count >= PSI_MODEL_MAX_SECTIONS) {
        return FALSE;
    }
    if (model->count >= model->capacity) {
        new_capacity = (model->capacity == 0) ? 8 : model->capacity * 2;
        if (model->sections == NULL) {
            sections = (PsiSection *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                               new_capacity * sizeof(PsiSection));
        } else {
            sections = (PsiSection *)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                 model->sections,
                                                 new_capacity * sizeof(PsiSection));
        }
        if (sections == NULL) {
            return FALSE;
        }
        model->sections = sections;
        model->capacity = new_capacity;
    }
    ZeroMemory(&model->sections[model->count], sizeof(PsiSection));
    psi_str_copy(model->sections[model->count].key,
                 PSI_ARRAY_COUNT(model->sections[model->count].key), key);
    psi_str_copy(model->sections[model->count].name,
                 PSI_ARRAY_COUNT(model->sections[model->count].name), name);
    if (out_section != NULL) {
        *out_section = &model->sections[model->count];
    }
    ++model->count;
    return TRUE;
}

PsiItem *psi_model_add_item(PsiSection *section, const wchar_t *key,
                            const wchar_t *name)
{
    PsiItem *items;
    PsiItem *item;
    size_t new_capacity;

    if (section == NULL || key == NULL || name == NULL) {
        return NULL;
    }
    if (section->count >= PSI_MODEL_MAX_ITEMS) {
        return NULL;
    }
    if (section->count >= section->capacity) {
        new_capacity = (section->capacity == 0) ? 32 : section->capacity * 2;
        if (section->items == NULL) {
            items = (PsiItem *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                         new_capacity * sizeof(PsiItem));
        } else {
            items = (PsiItem *)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                           section->items,
                                           new_capacity * sizeof(PsiItem));
        }
        if (items == NULL) {
            return NULL;
        }
        section->items = items;
        section->capacity = new_capacity;
    }
    item = &section->items[section->count];
    ZeroMemory(item, sizeof(*item));
    psi_str_copy(item->key, PSI_ARRAY_COUNT(item->key), key);
    psi_str_copy(item->name, PSI_ARRAY_COUNT(item->name), name);
    item->type = PSI_ITEM_NULL;
    psi_str_copy(item->display, PSI_ARRAY_COUNT(item->display), L"未知");
    ++section->count;
    return item;
}

PsiSection *psi_model_find_section(const PsiReport *model, const wchar_t *key)
{
    size_t i;

    if (model == NULL || key == NULL) {
        return NULL;
    }
    for (i = 0; i < model->count; ++i) {
        if (_wcsicmp(model->sections[i].key, key) == 0) {
            return &model->sections[i];
        }
    }
    return NULL;
}

PsiItem *psi_model_find_item(const PsiReport *model, const wchar_t *key)
{
    size_t i;
    size_t j;

    if (model == NULL || key == NULL) {
        return NULL;
    }
    for (i = 0; i < model->count; ++i) {
        for (j = 0; j < model->sections[i].count; ++j) {
            if (_wcsicmp(model->sections[i].items[j].key, key) == 0) {
                return &model->sections[i].items[j];
            }
        }
    }
    return NULL;
}

void psi_item_set_string(PsiItem *item, const wchar_t *data,
                         const wchar_t *display)
{
    if (item == NULL) {
        return;
    }
    item->type = PSI_ITEM_STRING;
    item->unavailable = FALSE;
    psi_str_copy(item->value.string, PSI_ARRAY_COUNT(item->value.string), data);
    psi_str_copy(item->display, PSI_ARRAY_COUNT(item->display),
                 psi_str_is_empty(display) ? data : display);
}

void psi_item_set_u64(PsiItem *item, unsigned long long data,
                      const wchar_t *display, const wchar_t *unit)
{
    if (item == NULL) {
        return;
    }
    item->type = PSI_ITEM_U64;
    item->unavailable = FALSE;
    item->value.u64 = data;
    if (psi_str_is_empty(display)) {
        swprintf_s(item->display, PSI_ARRAY_COUNT(item->display),
                   L"%llu", data);
    } else {
        psi_str_copy(item->display, PSI_ARRAY_COUNT(item->display), display);
    }
    psi_str_copy(item->unit, PSI_ARRAY_COUNT(item->unit), unit);
}

void psi_item_set_i64(PsiItem *item, long long data,
                      const wchar_t *display, const wchar_t *unit)
{
    if (item == NULL) {
        return;
    }
    item->type = PSI_ITEM_I64;
    item->unavailable = FALSE;
    item->value.i64 = data;
    if (psi_str_is_empty(display)) {
        swprintf_s(item->display, PSI_ARRAY_COUNT(item->display),
                   L"%lld", data);
    } else {
        psi_str_copy(item->display, PSI_ARRAY_COUNT(item->display), display);
    }
    psi_str_copy(item->unit, PSI_ARRAY_COUNT(item->unit), unit);
}

void psi_item_set_bool(PsiItem *item, BOOL data, const wchar_t *display)
{
    if (item == NULL) {
        return;
    }
    item->type = PSI_ITEM_BOOL;
    item->unavailable = FALSE;
    item->value.boolean = data;
    psi_str_copy(item->display, PSI_ARRAY_COUNT(item->display),
                 psi_str_is_empty(display) ? (data ? L"是" : L"否") : display);
}

void psi_item_set_null(PsiItem *item, const wchar_t *display)
{
    if (item == NULL) {
        return;
    }
    item->type = PSI_ITEM_NULL;
    item->unavailable = TRUE;
    ZeroMemory(&item->value, sizeof(item->value));
    psi_str_copy(item->display, PSI_ARRAY_COUNT(item->display),
                 psi_str_is_empty(display) ? L"未知" : display);
}

void psi_model_free(PsiReport *model)
{
    size_t i;

    if (model == NULL) {
        return;
    }
    for (i = 0; i < model->count; ++i) {
        if (model->sections[i].items != NULL) {
            HeapFree(GetProcessHeap(), 0, model->sections[i].items);
        }
    }
    if (model->sections != NULL) {
        HeapFree(GetProcessHeap(), 0, model->sections);
    }
    ZeroMemory(model, sizeof(*model));
}

void psi_model_free_text(void *text)
{
    if (text != NULL) {
        HeapFree(GetProcessHeap(), 0, text);
    }
}

/* ------------------------------------------------------------------ */
/* 构造辅助                                                            */
/* ------------------------------------------------------------------ */
static void add_str(PsiSection *section, const wchar_t *key,
                    const wchar_t *name, const wchar_t *value,
                    const wchar_t *display)
{
    PsiItem *item = psi_model_add_item(section, key, name);
    if (item == NULL) {
        return;
    }
    if (psi_str_is_empty(value)) {
        psi_item_set_null(item, display);
    } else {
        psi_item_set_string(item, value, display);
    }
}

static void add_u64(PsiSection *section, const wchar_t *key,
                    const wchar_t *name, unsigned long long value,
                    const wchar_t *display, const wchar_t *unit)
{
    PsiItem *item = psi_model_add_item(section, key, name);
    if (item != NULL) {
        psi_item_set_u64(item, value, display, unit);
    }
}

static void add_bool(PsiSection *section, const wchar_t *key,
                     const wchar_t *name, BOOL value)
{
    PsiItem *item = psi_model_add_item(section, key, name);
    if (item != NULL) {
        psi_item_set_bool(item, value, value ? L"是" : L"否");
    }
}

/* 测量值 0 表示未知时，输出 null，不把 0 当成真实测量值。 */
static void add_measure_u64(PsiSection *section, const wchar_t *key,
                            const wchar_t *name, unsigned long long value,
                            const wchar_t *display, const wchar_t *unit)
{
    PsiItem *item = psi_model_add_item(section, key, name);
    if (item == NULL) {
        return;
    }
    if (value == 0) {
        psi_item_set_null(item, display);
    } else {
        psi_item_set_u64(item, value, display, unit);
    }
}

/* ------------------------------------------------------------------ */
/* 各 section                                                          */
/* ------------------------------------------------------------------ */
static void build_system(PsiReport *model, const SystemReport *source)
{
    PsiSection *section = NULL;
    const SystemInfo *sys = &source->system;
    wchar_t timebuf[64];
    wchar_t uptime[64];

    if (!psi_model_add_section(model, L"system", L"系统概况", &section)) {
        return;
    }
    add_str(section, L"system.computer_name", L"计算机名", sys->computer_name, NULL);
    add_str(section, L"system.domain", L"所属域", sys->domain, NULL);
    add_str(section, L"system.user_name", L"当前用户", sys->user_name, NULL);
    add_str(section, L"system.os_name", L"操作系统", sys->os_name, NULL);
    add_str(section, L"system.os_version", L"系统版本", sys->os_version, NULL);
    add_str(section, L"system.os_build", L"系统构建", sys->os_build, NULL);
    add_str(section, L"system.os_arch", L"系统架构", sys->os_arch, NULL);
    add_str(section, L"system.os_edition", L"系统版本类型", sys->os_edition, NULL);
    add_bool(section, L"system.is_pe", L"是否 WinPE", sys->is_pe);
    add_str(section, L"system.pe_version", L"PE 版本", sys->pe_version, NULL);
    add_str(section, L"system.firmware_mode", L"引导模式", sys->firmware_mode, NULL);
    add_str(section, L"system.manufacturer", L"系统厂商", sys->manufacturer, NULL);
    add_str(section, L"system.product", L"系统型号", sys->product, NULL);
    add_str(section, L"system.system_version", L"系统版本号", sys->system_version, NULL);
    add_str(section, L"system.serial", L"系统序列号", sys->serial, NULL);
    add_str(section, L"system.uuid", L"系统 UUID", sys->uuid, NULL);
    add_str(section, L"system.sku", L"系统 SKU", sys->sku, NULL);
    add_str(section, L"system.chassis", L"机箱类型", sys->chassis, NULL);

    psi_format_duration(sys->uptime_seconds, uptime, PSI_ARRAY_COUNT(uptime));
    add_u64(section, L"system.uptime_seconds", L"运行时间",
            sys->uptime_seconds, uptime, L"seconds");

    psi_format_datetime(&sys->local_time, timebuf, PSI_ARRAY_COUNT(timebuf));
    add_str(section, L"system.local_time", L"当前时间", timebuf, NULL);

    add_str(section, L"bios.vendor", L"BIOS 厂商", sys->bios_vendor, NULL);
    add_str(section, L"bios.version", L"BIOS 版本", sys->bios_version, NULL);
    add_str(section, L"bios.release_date", L"BIOS 日期", sys->bios_date, NULL);
    add_str(section, L"board.manufacturer", L"主板厂商", sys->board_manufacturer, NULL);
    add_str(section, L"board.product", L"主板型号", sys->board_product, NULL);
    add_str(section, L"board.version", L"主板版本", sys->board_version, NULL);
    add_str(section, L"board.serial", L"主板序列号", sys->board_serial, NULL);
}

static void build_cpu(PsiReport *model, const SystemReport *source)
{
    PsiSection *section = NULL;
    const CpuInfo *cpu = &source->cpu;
    wchar_t buf[PSI_DISPLAY_CCH];
    wchar_t c1[64];
    wchar_t c2[64];
    wchar_t c3[64];

    if (!psi_model_add_section(model, L"cpu", L"处理器", &section)) {
        return;
    }
    add_str(section, L"cpu.brand", L"型号", cpu->brand, NULL);
    add_str(section, L"cpu.vendor", L"厂商", cpu->vendor, NULL);
    add_str(section, L"cpu.identifier", L"标识", cpu->identifier, NULL);
    add_str(section, L"cpu.arch", L"架构", cpu->arch, NULL);
    add_u64(section, L"cpu.sockets", L"物理插槽", cpu->sockets, NULL, L"count");
    add_u64(section, L"cpu.cores", L"物理核心", cpu->cores, NULL, L"count");
    add_u64(section, L"cpu.logical", L"逻辑线程", cpu->logical, NULL, L"count");

    if (cpu->base_mhz == 0) {
        PsiItem *item = psi_model_add_item(section, L"cpu.base_mhz", L"基准频率");
        psi_item_set_null(item, L"未知");
    } else {
        psi_format_mhz(cpu->base_mhz, buf, PSI_ARRAY_COUNT(buf));
        add_u64(section, L"cpu.base_mhz", L"基准频率", cpu->base_mhz, buf, L"MHz");
    }
    if (cpu->max_mhz == 0) {
        PsiItem *item = psi_model_add_item(section, L"cpu.max_mhz", L"最大频率");
        psi_item_set_null(item, L"未知");
    } else {
        psi_format_mhz(cpu->max_mhz, buf, PSI_ARRAY_COUNT(buf));
        add_u64(section, L"cpu.max_mhz", L"最大频率", cpu->max_mhz, buf, L"MHz");
    }
    if (cpu->current_mhz == 0) {
        PsiItem *item = psi_model_add_item(section, L"cpu.current_mhz",
                                           L"当前频率");
        psi_item_set_null(item, L"未知（电源 API 不可用）");
    } else {
        psi_format_mhz(cpu->current_mhz, buf, PSI_ARRAY_COUNT(buf));
        add_u64(section, L"cpu.current_mhz", L"当前频率",
                cpu->current_mhz, buf, L"MHz");
    }

    format_cache(cpu->l1_kb, c1, PSI_ARRAY_COUNT(c1));
    format_cache(cpu->l2_kb, c2, PSI_ARRAY_COUNT(c2));
    format_cache(cpu->l3_kb, c3, PSI_ARRAY_COUNT(c3));
    add_measure_u64(section, L"cpu.l1_kb", L"L1 缓存", cpu->l1_kb, c1, L"KB");
    add_measure_u64(section, L"cpu.l2_kb", L"L2 缓存", cpu->l2_kb, c2, L"KB");
    add_measure_u64(section, L"cpu.l3_kb", L"L3 缓存", cpu->l3_kb, c3, L"KB");
    add_str(section, L"cpu.features", L"指令集", cpu->features, NULL);
}

static void build_memory(PsiReport *model, const SystemReport *source)
{
    PsiSection *section = NULL;
    const MemoryInfo *mem = &source->memory;
    wchar_t buf[PSI_DISPLAY_CCH];
    wchar_t display[128];
    unsigned i;

    if (!psi_model_add_section(model, L"memory", L"内存", &section)) {
        return;
    }
    psi_format_bytes(mem->installed_bytes, buf, PSI_ARRAY_COUNT(buf));
    add_u64(section, L"memory.installed_bytes", L"安装容量",
            mem->installed_bytes, buf, L"bytes");
    psi_format_bytes(mem->total_bytes, buf, PSI_ARRAY_COUNT(buf));
    add_u64(section, L"memory.total_bytes", L"系统可见容量",
            mem->total_bytes, buf, L"bytes");
    psi_format_bytes(mem->available_bytes, buf, PSI_ARRAY_COUNT(buf));
    add_u64(section, L"memory.available_bytes", L"当前可用",
            mem->available_bytes, buf, L"bytes");

    if (mem->max_capacity_bytes > 0) {
        psi_format_bytes(mem->max_capacity_bytes, buf, PSI_ARRAY_COUNT(buf));
        add_u64(section, L"memory.max_capacity_bytes", L"最大支持容量",
                mem->max_capacity_bytes, buf, L"bytes");
    } else {
        PsiItem *item = psi_model_add_item(section, L"memory.max_capacity_bytes",
                                           L"最大支持容量");
        psi_item_set_null(item, L"未知");
    }
    add_str(section, L"memory.ecc_type", L"内存纠错类型",
            mem->ecc_type, NULL);

    add_u64(section, L"memory.slot_count", L"插槽总数",
            mem->slot_count, NULL, L"count");
    add_u64(section, L"memory.module_count", L"已安装内存条",
            mem->module_count, NULL, L"count");
    if (mem->slot_count >= mem->module_count) {
        swprintf_s(buf, PSI_ARRAY_COUNT(buf), L"%u",
                   (unsigned)(mem->slot_count - mem->module_count));
    } else {
        psi_str_copy(buf, PSI_ARRAY_COUNT(buf), L"0");
    }
    add_u64(section, L"memory.empty_slot_count", L"空余插槽",
            mem->slot_count >= mem->module_count ?
            (unsigned long long)(mem->slot_count - mem->module_count) : 0,
            buf, L"count");

    for (i = 0; i < mem->module_count && i < PSI_MAX_MEM_MODULES; ++i) {
        const MemoryModule *m = &mem->modules[i];
        wchar_t key[PSI_KEY_CCH];
        wchar_t name[PSI_NAME_CCH];
        wchar_t speed[64];

        psi_format_bytes(m->size_bytes, display, PSI_ARRAY_COUNT(display));
        make_indexed(name, PSI_ARRAY_COUNT(name), L"内存条 #%u 容量", i + 1, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"memory.module.%u.size_bytes", i);
        add_u64(section, key, name, m->size_bytes, display, L"bytes");

        make_indexed(name, PSI_ARRAY_COUNT(name), L"内存条 #%u 固件槽位", i + 1, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"memory.module.%u.locator", i);
        add_str(section, key, name, m->locator, NULL);

        make_indexed(name, PSI_ARRAY_COUNT(name), L"内存条 #%u Bank/通道", i + 1, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"memory.module.%u.bank", i);
        add_str(section, key, name, m->bank, NULL);

        make_indexed(name, PSI_ARRAY_COUNT(name), L"内存条 #%u 类型", i + 1, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"memory.module.%u.type", i);
        add_str(section, key, name, m->type, NULL);

        make_indexed(name, PSI_ARRAY_COUNT(name), L"内存条 #%u 形态", i + 1, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"memory.module.%u.form_factor", i);
        add_str(section, key, name, m->form_factor, NULL);

        format_speed(m->speed_mts, speed, PSI_ARRAY_COUNT(speed));
        make_indexed(name, PSI_ARRAY_COUNT(name), L"内存条 #%u 标称速度", i + 1, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"memory.module.%u.speed_mts", i);
        add_measure_u64(section, key, name, m->speed_mts, speed, L"MT/s");

        format_speed(m->configured_speed_mts, speed, PSI_ARRAY_COUNT(speed));
        make_indexed(name, PSI_ARRAY_COUNT(name), L"内存条 #%u 配置速度", i + 1, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"memory.module.%u.configured_speed_mts", i);
        add_measure_u64(section, key, name, m->configured_speed_mts, speed, L"MT/s");

        make_indexed(name, PSI_ARRAY_COUNT(name), L"内存条 #%u Rank", i + 1, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"memory.module.%u.rank", i);
        if (m->rank > 0) {
            swprintf_s(speed, PSI_ARRAY_COUNT(speed), L"%u", m->rank);
            add_u64(section, key, name, m->rank, speed, L"rank");
        } else {
            PsiItem *item = psi_model_add_item(section, key, name);
            psi_item_set_null(item, L"未知");
        }

        make_indexed(name, PSI_ARRAY_COUNT(name), L"内存条 #%u 数据位宽", i + 1, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"memory.module.%u.data_width_bits", i);
        if (m->data_width_bits > 0) {
            swprintf_s(speed, PSI_ARRAY_COUNT(speed), L"%u bit", m->data_width_bits);
            add_u64(section, key, name, m->data_width_bits, speed, L"bits");
        } else {
            PsiItem *item = psi_model_add_item(section, key, name);
            psi_item_set_null(item, L"未知");
        }

        make_indexed(name, PSI_ARRAY_COUNT(name), L"内存条 #%u 电压", i + 1, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"memory.module.%u.voltage", i);
        if (m->voltage_configured_mv > 0) {
            swprintf_s(speed, PSI_ARRAY_COUNT(speed), L"%.2f V",
                       (double)m->voltage_configured_mv / 1000.0);
            add_u64(section, key, name, m->voltage_configured_mv, speed, L"mV");
        } else if (m->voltage_min_mv > 0 && m->voltage_min_mv == m->voltage_max_mv) {
            swprintf_s(speed, PSI_ARRAY_COUNT(speed), L"%.2f V",
                       (double)m->voltage_min_mv / 1000.0);
            add_u64(section, key, name, m->voltage_min_mv, speed, L"mV");
        } else {
            PsiItem *item = psi_model_add_item(section, key, name);
            psi_item_set_null(item, L"未知");
        }

        make_indexed(name, PSI_ARRAY_COUNT(name), L"内存条 #%u Registered", i + 1, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"memory.module.%u.registered", i);
        add_bool(section, key, name, m->registered);

        make_indexed(name, PSI_ARRAY_COUNT(name), L"内存条 #%u 厂商", i + 1, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"memory.module.%u.manufacturer", i);
        add_str(section, key, name, m->manufacturer, NULL);

        make_indexed(name, PSI_ARRAY_COUNT(name), L"内存条 #%u 型号", i + 1, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"memory.module.%u.part_number", i);
        add_str(section, key, name, m->part_number, NULL);

        make_indexed(name, PSI_ARRAY_COUNT(name), L"内存条 #%u 序列号", i + 1, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"memory.module.%u.serial", i);
        add_str(section, key, name, m->serial, NULL);
    }
}

static void build_storage(PsiReport *model, const SystemReport *source)
{
    PsiSection *section = NULL;
    const StorageInfo *st = &source->storage;
    wchar_t buf[PSI_DISPLAY_CCH];
    wchar_t name[PSI_NAME_CCH];
    wchar_t key[PSI_KEY_CCH];
    unsigned i;

    if (!psi_model_add_section(model, L"storage", L"存储设备", &section)) {
        return;
    }
    add_u64(section, L"storage.disk_count", L"物理磁盘数量",
            st->disk_count, NULL, L"count");
    add_u64(section, L"storage.volume_count", L"卷数量",
            st->volume_count, NULL, L"count");

    for (i = 0; i < st->disk_count && i < PSI_MAX_DISKS; ++i) {
        const DiskInfo *d = &st->disks[i];

        psi_format_bytes(d->size_bytes, buf, PSI_ARRAY_COUNT(buf));
        make_indexed(name, PSI_ARRAY_COUNT(name), L"磁盘 #%u 容量", d->index, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"storage.disk.%u.size_bytes", i);
        add_u64(section, key, name, d->size_bytes, buf, L"bytes");

        swprintf_s(buf, PSI_DISPLAY_CCH, L"%u", d->index);
        make_indexed(name, PSI_ARRAY_COUNT(name), L"磁盘 #%u PhysicalDrive 序号", d->index, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"storage.disk.%u.index", i);
        add_u64(section, key, name, d->index, buf, NULL);

        make_indexed(name, PSI_ARRAY_COUNT(name), L"磁盘 #%u 型号", d->index, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"storage.disk.%u.model", i);
        add_str(section, key, name, d->model, NULL);

        make_indexed(name, PSI_ARRAY_COUNT(name), L"磁盘 #%u 接口", d->index, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"storage.disk.%u.bus", i);
        add_str(section, key, name, d->bus, NULL);

        make_indexed(name, PSI_ARRAY_COUNT(name), L"磁盘 #%u 介质", d->index, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"storage.disk.%u.media", i);
        add_str(section, key, name, d->media, NULL);

        make_indexed(name, PSI_ARRAY_COUNT(name), L"磁盘 #%u 分区表", d->index, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"storage.disk.%u.partition_style", i);
        add_str(section, key, name, d->partition_style, NULL);

        make_indexed(name, PSI_ARRAY_COUNT(name), L"磁盘 #%u 序列号", d->index, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"storage.disk.%u.serial", i);
        add_str(section, key, name, d->serial, NULL);

        make_indexed(name, PSI_ARRAY_COUNT(name), L"磁盘 #%u 固件", d->index, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"storage.disk.%u.firmware", i);
        add_str(section, key, name, d->firmware, NULL);

        swprintf_s(buf, PSI_DISPLAY_CCH, L"%u", d->partition_count);
        make_indexed(name, PSI_ARRAY_COUNT(name), L"磁盘 #%u 分区数", d->index, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"storage.disk.%u.partition_count", i);
        add_u64(section, key, name, d->partition_count, buf, L"count");

        swprintf_s(buf, PSI_DISPLAY_CCH, L"%u 字节", d->sector_size);
        make_indexed(name, PSI_ARRAY_COUNT(name), L"磁盘 #%u 扇区大小", d->index, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"storage.disk.%u.sector_size", i);
        add_u64(section, key, name, d->sector_size, buf, L"bytes");

        make_indexed(name, PSI_ARRAY_COUNT(name), L"磁盘 #%u 是否系统盘", d->index, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"storage.disk.%u.is_system", i);
        add_bool(section, key, name, d->is_system);
    }

    for (i = 0; i < st->volume_count && i < PSI_MAX_VOLUMES; ++i) {
        const VolumeInfo *v = &st->volumes[i];

        psi_format_bytes(v->total_bytes, buf, PSI_ARRAY_COUNT(buf));
        make_indexed(name, PSI_ARRAY_COUNT(name), L"卷 #%u 容量", i, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"storage.volume.%u.total_bytes", i);
        add_u64(section, key, name, v->total_bytes, buf, L"bytes");

        make_indexed(name, PSI_ARRAY_COUNT(name), L"卷 #%u 盘符", i, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"storage.volume.%u.letter", i);
        add_str(section, key, name, v->letter, NULL);

        make_indexed(name, PSI_ARRAY_COUNT(name), L"卷 #%u 卷标", i, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"storage.volume.%u.label", i);
        add_str(section, key, name, v->label, NULL);

        make_indexed(name, PSI_ARRAY_COUNT(name), L"卷 #%u 文件系统", i, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"storage.volume.%u.filesystem", i);
        add_str(section, key, name, v->filesystem, NULL);

        make_indexed(name, PSI_ARRAY_COUNT(name), L"卷 #%u 类型", i, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"storage.volume.%u.type", i);
        add_str(section, key, name, v->type, NULL);

        psi_format_bytes(v->free_bytes, buf, PSI_ARRAY_COUNT(buf));
        make_indexed(name, PSI_ARRAY_COUNT(name), L"卷 #%u 可用", i, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"storage.volume.%u.free_bytes", i);
        add_u64(section, key, name, v->free_bytes, buf, L"bytes");

        if (v->disk_index == PSI_DISK_INDEX_UNKNOWN) {
            make_indexed(name, PSI_ARRAY_COUNT(name), L"卷 #%u 物理磁盘", i, NULL);
            swprintf_s(key, PSI_ARRAY_COUNT(key), L"storage.volume.%u.disk_index", i);
            {
                PsiItem *item = psi_model_add_item(section, key, name);
                psi_item_set_null(item, L"未知");
            }
        } else {
            swprintf_s(buf, PSI_DISPLAY_CCH, L"%u", v->disk_index);
            make_indexed(name, PSI_ARRAY_COUNT(name), L"卷 #%u 物理磁盘", i, NULL);
            swprintf_s(key, PSI_ARRAY_COUNT(key), L"storage.volume.%u.disk_index", i);
            add_u64(section, key, name, v->disk_index, buf, NULL);
        }
    }
}

static void build_graphics(PsiReport *model, const SystemReport *source)
{
    PsiSection *section = NULL;
    wchar_t name[PSI_NAME_CCH];
    wchar_t key[PSI_KEY_CCH];
    wchar_t buf[PSI_DISPLAY_CCH];
    unsigned i;

    if (!psi_model_add_section(model, L"graphics", L"图形", &section)) {
        return;
    }
    add_u64(section, L"graphics.count", L"显示适配器数量",
            source->graphics.count, NULL, L"count");
    for (i = 0; i < source->graphics.count && i < PSI_MAX_GRAPHICS; ++i) {
        const GraphicsAdapter *g = &source->graphics.items[i];

        make_indexed(name, PSI_ARRAY_COUNT(name), L"适配器 #%u 名称", i, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"graphics.adapter.%u.name", i);
        add_str(section, key, name, g->adapter, NULL);

        make_indexed(name, PSI_ARRAY_COUNT(name), L"适配器 #%u 分辨率", i, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"graphics.adapter.%u.resolution", i);
        if (g->width > 0 && g->height > 0) {
            PsiItem *item = psi_model_add_item(section, key, name);
            swprintf_s(buf, PSI_ARRAY_COUNT(buf), L"%u x %u", g->width, g->height);
            psi_item_set_string(item, buf, NULL);
        } else {
            PsiItem *item = psi_model_add_item(section, key, name);
            psi_item_set_null(item, L"未知");
        }

        make_indexed(name, PSI_ARRAY_COUNT(name), L"适配器 #%u 主显示器", i, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"graphics.adapter.%u.primary", i);
        add_bool(section, key, name, g->primary);
    }

    add_u64(section, L"graphics.monitor_count", L"显示器数量",
            source->graphics.monitor_count, NULL, L"count");
    for (i = 0; i < source->graphics.monitor_count && i < PSI_MAX_MONITORS; ++i) {
        const DisplayMonitor *m = &source->graphics.monitors[i];

        make_indexed(name, PSI_ARRAY_COUNT(name), L"显示器 #%u 名称", i, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"graphics.monitor.%u.name", i);
        add_str(section, key, name, m->name, NULL);

        make_indexed(name, PSI_ARRAY_COUNT(name), L"显示器 #%u 所属适配器", i, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"graphics.monitor.%u.adapter", i);
        add_str(section, key, name, m->adapter, NULL);

        make_indexed(name, PSI_ARRAY_COUNT(name), L"显示器 #%u 设备名", i, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"graphics.monitor.%u.device", i);
        add_str(section, key, name, m->device, NULL);

        make_indexed(name, PSI_ARRAY_COUNT(name), L"显示器 #%u 分辨率", i, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"graphics.monitor.%u.resolution", i);
        if (m->width > 0 && m->height > 0) {
            PsiItem *item = psi_model_add_item(section, key, name);
            swprintf_s(buf, PSI_ARRAY_COUNT(buf), L"%u x %u", m->width, m->height);
            psi_item_set_string(item, buf, NULL);
        } else {
            PsiItem *item = psi_model_add_item(section, key, name);
            psi_item_set_null(item, L"未知");
        }

        make_indexed(name, PSI_ARRAY_COUNT(name), L"显示器 #%u 刷新率", i, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"graphics.monitor.%u.refresh_hz", i);
        if (m->refresh_hz > 1) {
            swprintf_s(buf, PSI_ARRAY_COUNT(buf), L"%u Hz", m->refresh_hz);
            add_u64(section, key, name, m->refresh_hz, buf, L"Hz");
        } else {
            PsiItem *item = psi_model_add_item(section, key, name);
            psi_item_set_null(item, L"未知");
        }

        make_indexed(name, PSI_ARRAY_COUNT(name), L"显示器 #%u 主显示器", i, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"graphics.monitor.%u.primary", i);
        add_bool(section, key, name, m->primary);
    }
}

static void build_network(PsiReport *model, const SystemReport *source)
{
    PsiSection *section = NULL;
    wchar_t name[PSI_NAME_CCH];
    wchar_t key[PSI_KEY_CCH];
    unsigned i;

    if (!psi_model_add_section(model, L"network", L"网络适配器", &section)) {
        return;
    }
    add_u64(section, L"network.count", L"网络适配器数量",
            source->network.count, NULL, L"count");
    for (i = 0; i < source->network.count && i < PSI_MAX_NET; ++i) {
        const NetworkAdapter *n = &source->network.items[i];

        make_indexed(name, PSI_ARRAY_COUNT(name), L"网卡 #%u 描述", i, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"network.adapter.%u.description", i);
        add_str(section, key, name, n->description, NULL);

        make_indexed(name, PSI_ARRAY_COUNT(name), L"网卡 #%u MAC", i, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"network.adapter.%u.mac", i);
        add_str(section, key, name, n->mac, NULL);

        make_indexed(name, PSI_ARRAY_COUNT(name), L"网卡 #%u IPv4", i, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"network.adapter.%u.ipv4", i);
        add_str(section, key, name, n->ipv4, NULL);

        make_indexed(name, PSI_ARRAY_COUNT(name), L"网卡 #%u 速率", i, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"network.adapter.%u.speed", i);
        add_str(section, key, name, n->speed, NULL);

        make_indexed(name, PSI_ARRAY_COUNT(name), L"网卡 #%u 状态", i, NULL);
        swprintf_s(key, PSI_ARRAY_COUNT(key), L"network.adapter.%u.status", i);
        add_str(section, key, name, n->status, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* 构建                                                                */
/* ------------------------------------------------------------------ */
BOOL psi_model_build(const SystemReport *source, PsiReport *model)
{
    size_t i;

    if (source == NULL || model == NULL) {
        return FALSE;
    }
    ZeroMemory(model, sizeof(*model));
    if (source->system.local_time.wYear >= 1601) {
        model->generated = source->system.local_time;
    } else {
        GetLocalTime(&model->generated);
    }

    for (i = 0; i < source->warning_count && i < PSI_MODEL_MAX_WARNINGS; ++i) {
        psi_str_copy(model->warnings[i], PSI_DISPLAY_CCH, source->warnings[i]);
    }
    model->warning_count = source->warning_count;
    if (model->warning_count > PSI_MODEL_MAX_WARNINGS) {
        model->warning_count = PSI_MODEL_MAX_WARNINGS;
    }

    build_system(model, source);
    build_cpu(model, source);
    build_memory(model, source);
    build_storage(model, source);
    build_graphics(model, source);
    build_network(model, source);
    return TRUE;
}
