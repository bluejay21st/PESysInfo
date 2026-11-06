/*
 * json.c - PsiReport -> UTF-8/UTF-16 JSON
 */
#include "model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <limits.h>

typedef struct {
    wchar_t *data;
    size_t   length;
    size_t   capacity;
    BOOL     failed;
} WideBuffer;

static BOOL wb_reserve(WideBuffer *buf, size_t extra)
{
    size_t needed;
    size_t capacity;
    wchar_t *data;

    if (buf->failed) {
        return FALSE;
    }
    needed = buf->length + extra + 1;
    if (needed <= buf->capacity) {
        return TRUE;
    }
    capacity = (buf->capacity == 0) ? 4096 : buf->capacity;
    while (capacity < needed) {
        capacity *= 2;
    }
    if (buf->data == NULL) {
        data = (wchar_t *)HeapAlloc(GetProcessHeap(), 0,
                                    capacity * sizeof(wchar_t));
    } else {
        data = (wchar_t *)HeapReAlloc(GetProcessHeap(), 0, buf->data,
                                      capacity * sizeof(wchar_t));
    }
    if (data == NULL) {
        buf->failed = TRUE;
        return FALSE;
    }
    buf->data = data;
    buf->capacity = capacity;
    return TRUE;
}

static void wb_append_n(WideBuffer *buf, const wchar_t *text, size_t length)
{
    if (text == NULL || length == 0) {
        return;
    }
    if (!wb_reserve(buf, length)) {
        return;
    }
    memcpy(buf->data + buf->length, text, length * sizeof(wchar_t));
    buf->length += length;
    buf->data[buf->length] = L'\0';
}

static void wb_append(WideBuffer *buf, const wchar_t *text)
{
    if (text != NULL) {
        wb_append_n(buf, text, wcslen(text));
    }
}

static void wb_appendf(WideBuffer *buf, const wchar_t *fmt, ...)
{
    va_list args;
    va_list copy;
    int needed;
    wchar_t *temp;

    if (buf->failed || fmt == NULL) {
        return;
    }
    va_start(args, fmt);
    va_copy(copy, args);
    needed = _vscwprintf(fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        buf->failed = TRUE;
        return;
    }
    if (!wb_reserve(buf, (size_t)needed)) {
        va_end(args);
        return;
    }
    temp = buf->data + buf->length;
    _vsnwprintf_s(temp, buf->capacity - buf->length, _TRUNCATE, fmt, args);
    va_end(args);
    buf->length += (size_t)needed;
    buf->data[buf->length] = L'\0';
}

static void wb_escape(WideBuffer *buf, const wchar_t *text)
{
    static const wchar_t hex[] = L"0123456789abcdef";
    size_t i;

    if (text == NULL) {
        text = L"";
    }
    for (i = 0; text[i] != L'\0'; ++i) {
        wchar_t ch = text[i];
        if (ch == L'"' || ch == L'\\') {
            wchar_t pair[2];
            pair[0] = L'\\';
            pair[1] = ch;
            wb_append_n(buf, pair, 2);
        } else if (ch == L'\b') {
            wb_append(buf, L"\\b");
        } else if (ch == L'\f') {
            wb_append(buf, L"\\f");
        } else if (ch == L'\n') {
            wb_append(buf, L"\\n");
        } else if (ch == L'\r') {
            wb_append(buf, L"\\r");
        } else if (ch == L'\t') {
            wb_append(buf, L"\\t");
        } else if (ch < 0x20) {
            wchar_t esc[6];
            esc[0] = L'\\';
            esc[1] = L'u';
            esc[2] = L'0';
            esc[3] = L'0';
            esc[4] = hex[(ch >> 4) & 0xF];
            esc[5] = hex[ch & 0xF];
            wb_append_n(buf, esc, 6);
        } else {
            wb_append_n(buf, &ch, 1);
        }
    }
}

static const wchar_t *item_type_name(PsiItemType type)
{
    switch (type) {
    case PSI_ITEM_STRING: return L"string";
    case PSI_ITEM_U64:    return L"u64";
    case PSI_ITEM_I64:    return L"i64";
    case PSI_ITEM_BOOL:   return L"bool";
    default:              return L"null";
    }
}

static void append_item_body(WideBuffer *buf, const PsiItem *item,
                             BOOL include_key)
{
    wb_append(buf, L"{");
    if (include_key) {
        wb_append(buf, L"\"key\":\"");
        wb_escape(buf, item->key);
        wb_append(buf, L"\",");
    }
    wb_append(buf, L"\"name\":\"");
    wb_escape(buf, item->name);
    wb_append(buf, L"\",\"data\":");

    switch (item->type) {
    case PSI_ITEM_STRING:
        wb_append(buf, L"\"");
        wb_escape(buf, item->value.string);
        wb_append(buf, L"\"");
        break;
    case PSI_ITEM_U64:
        wb_appendf(buf, L"%llu", item->value.u64);
        break;
    case PSI_ITEM_I64:
        wb_appendf(buf, L"%lld", item->value.i64);
        break;
    case PSI_ITEM_BOOL:
        wb_append(buf, item->value.boolean ? L"true" : L"false");
        break;
    default:
        wb_append(buf, L"null");
        break;
    }

    wb_append(buf, L",\"display\":\"");
    wb_escape(buf, item->display);
    wb_append(buf, L"\",\"type\":\"");
    wb_append(buf, item_type_name(item->type));
    wb_append(buf, L"\",\"unit\":\"");
    wb_escape(buf, item->unit);
    wb_append(buf, L"\",\"estimated\":");
    wb_append(buf, item->estimated ? L"true" : L"false");
    wb_append(buf, L",\"unavailable\":");
    wb_append(buf, item->unavailable ? L"true" : L"false");
    wb_append(buf, L"}");
}

static void append_generated_at(WideBuffer *buf, const SYSTEMTIME *st)
{
    TIME_ZONE_INFORMATION tz;
    DWORD tz_id;
    LONG bias;
    LONG offset;
    LONG abs_offset;

    ZeroMemory(&tz, sizeof(tz));
    tz_id = GetTimeZoneInformation(&tz);
    bias = (tz_id == TIME_ZONE_ID_INVALID) ? 0 : tz.Bias;
    if (tz_id == TIME_ZONE_ID_DAYLIGHT) {
        bias += tz.DaylightBias;
    }
    offset = -bias;
    abs_offset = (offset >= 0) ? offset : -offset;

    wb_appendf(buf,
               L"\"generated_at\":\"%04u-%02u-%02uT%02u:%02u:%02u%ls%02ld:%02ld\"",
               st->wYear, st->wMonth, st->wDay,
               st->wHour, st->wMinute, st->wSecond,
               (offset >= 0) ? L"+" : L"-",
               abs_offset / 60, abs_offset % 60);
}

BOOL psi_model_json(const PsiReport *model, wchar_t **out_text)
{
    WideBuffer buf;
    size_t i;
    size_t j;
    BOOL first;

    if (out_text == NULL) {
        return FALSE;
    }
    *out_text = NULL;
    if (model == NULL) {
        return FALSE;
    }
    ZeroMemory(&buf, sizeof(buf));

    wb_append(&buf, L"{\n  \"schema\":\"" PSI_JSON_SCHEMA L"\",\n  ");
    append_generated_at(&buf, &model->generated);
    wb_append(&buf, L",\n  \"sections\":[");
    for (i = 0; i < model->count; ++i) {
        const PsiSection *section = &model->sections[i];
        if (i > 0) {
            wb_append(&buf, L",");
        }
        wb_append(&buf, L"\n    {\"key\":\"");
        wb_escape(&buf, section->key);
        wb_append(&buf, L"\",\"name\":\"");
        wb_escape(&buf, section->name);
        wb_append(&buf, L"\",\"items\":[");
        for (j = 0; j < section->count; ++j) {
            if (j > 0) {
                wb_append(&buf, L",");
            }
            wb_append(&buf, L"\n      ");
            append_item_body(&buf, &section->items[j], TRUE);
        }
        wb_append(&buf, L"\n    ]}");
    }
    wb_append(&buf, L"\n  ],\n  \"flat\":{");
    first = TRUE;
    for (i = 0; i < model->count; ++i) {
        for (j = 0; j < model->sections[i].count; ++j) {
            const PsiItem *item = &model->sections[i].items[j];
            if (!first) {
                wb_append(&buf, L",");
            }
            first = FALSE;
            wb_append(&buf, L"\n    \"");
            wb_escape(&buf, item->key);
            wb_append(&buf, L"\":");
            append_item_body(&buf, item, FALSE);
        }
    }
    wb_append(&buf, L"\n  },\n  \"warnings\":[");
    for (i = 0; i < model->warning_count; ++i) {
        if (i > 0) {
            wb_append(&buf, L",");
        }
        wb_append(&buf, L"\n    \"");
        wb_escape(&buf, model->warnings[i]);
        wb_append(&buf, L"\"");
    }
    wb_append(&buf, L"\n  ]\n}\n");

    if (buf.failed) {
        if (buf.data != NULL) {
            HeapFree(GetProcessHeap(), 0, buf.data);
        }
        return FALSE;
    }
    *out_text = buf.data;
    return TRUE;
}

BOOL psi_model_json_utf8(const PsiReport *model, char **out_text)
{
    wchar_t *wide = NULL;
    char *utf8;
    int needed;

    if (out_text == NULL) {
        return FALSE;
    }
    *out_text = NULL;
    if (!psi_model_json(model, &wide)) {
        return FALSE;
    }
    needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (needed <= 0) {
        psi_model_free_text(wide);
        return FALSE;
    }
    utf8 = (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)needed);
    if (utf8 == NULL) {
        psi_model_free_text(wide);
        return FALSE;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, needed, NULL, NULL) <= 0) {
        HeapFree(GetProcessHeap(), 0, utf8);
        psi_model_free_text(wide);
        return FALSE;
    }
    psi_model_free_text(wide);
    *out_text = utf8;
    return TRUE;
}
