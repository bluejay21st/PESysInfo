/*
 * probe.c - 探测层入口：采集、降级、警告
 */
#include "probe.h"

#include <wchar.h>

void psi_add_warning(SystemReport *report, const wchar_t *fmt, ...)
{
    wchar_t message[PSI_TEXT_CCH];
    va_list args;
    unsigned i;

    if (report == NULL || fmt == NULL) {
        return;
    }
    if (report->warning_count >= PSI_MAX_WARNINGS) {
        return;
    }
    va_start(args, fmt);
    _vsnwprintf_s(message, PSI_ARRAY_COUNT(message), _TRUNCATE, fmt, args);
    va_end(args);
    message[PSI_ARRAY_COUNT(message) - 1] = L'\0';
    psi_str_trim(message);
    if (message[0] == L'\0') {
        return;
    }
    for (i = 0; i < report->warning_count; ++i) {
        if (wcscmp(report->warnings[i], message) == 0) {
            return;
        }
    }
    psi_str_copy(report->warnings[report->warning_count],
                 PSI_ARRAY_COUNT(report->warnings[report->warning_count]), message);
    ++report->warning_count;
}

void psi_collect_all(SystemReport *report)
{
    SmbiosData smbios;

    if (report == NULL) {
        return;
    }
    ZeroMemory(report, sizeof(*report));
    ZeroMemory(&smbios, sizeof(smbios));

    if (!psi_smbios_load(&smbios)) {
        psi_add_warning(report,
                        L"未能读取 SMBIOS 固件表，固件、主板和内存条详情可能缺失。");
    }

    psi_system_collect(&report->system, report);
    psi_firmware_collect(&report->system, &smbios, report);
    psi_cpu_collect(&report->cpu, &smbios, report);
    psi_memory_collect(&report->memory, &smbios, report);
    psi_storage_collect(&report->storage, report);
    psi_graphics_collect(&report->graphics, report);
    psi_network_collect(&report->network, report);

    psi_smbios_free(&smbios);
}

BOOL psi_probe_collect(PsiReport *model)
{
    SystemReport source;
    BOOL ok;

    if (model == NULL) {
        return FALSE;
    }
    ZeroMemory(model, sizeof(*model));
    psi_collect_all(&source);
    ok = psi_model_build(&source, model);
    if (!ok) {
        psi_model_free(model);
    }
    return ok;
}
