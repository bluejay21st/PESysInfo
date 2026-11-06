/*
 * model.h - 采集结果的数据模型
 *
 * 探测层只负责把数据写成 PsiReport；GUI 和 CLI 都读取这一个模型。
 * 每一项同时保留原始值（data）和显示值（display），UI 使用 display，
 * Agent/CLI 使用 data。
 */
#ifndef PSI_MODEL_H
#define PSI_MODEL_H

#include "pesysinfo.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PSI_MODEL_MAX_SECTIONS 12
#define PSI_MODEL_MAX_ITEMS    1024
#define PSI_MODEL_MAX_WARNINGS PSI_MAX_WARNINGS
#define PSI_KEY_CCH            96
#define PSI_NAME_CCH           96
#define PSI_DISPLAY_CCH        256
#define PSI_UNIT_CCH           24
#define PSI_STRING_CCH         256

#ifndef PSI_ARRAY_COUNT
#define PSI_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#endif

typedef enum {
    PSI_ITEM_NULL = 0,
    PSI_ITEM_STRING,
    PSI_ITEM_U64,
    PSI_ITEM_I64,
    PSI_ITEM_BOOL
} PsiItemType;

typedef struct {
    wchar_t      key[PSI_KEY_CCH];
    wchar_t      name[PSI_NAME_CCH];
    wchar_t      display[PSI_DISPLAY_CCH];
    wchar_t      unit[PSI_UNIT_CCH];
    PsiItemType  type;
    union {
        wchar_t             string[PSI_STRING_CCH];
        unsigned long long  u64;
        long long           i64;
        BOOL                boolean;
    } value;
    BOOL         estimated;
    BOOL         unavailable;
} PsiItem;

typedef struct {
    wchar_t   key[32];
    wchar_t   name[64];
    PsiItem  *items;
    size_t    count;
    size_t    capacity;
} PsiSection;

typedef struct {
    PsiSection  *sections;
    size_t       count;
    size_t       capacity;
    wchar_t      warnings[PSI_MODEL_MAX_WARNINGS][PSI_DISPLAY_CCH];
    size_t       warning_count;
    SYSTEMTIME   generated;
} PsiReport;

/* 生命周期 */
BOOL  psi_model_build(const SystemReport *source, PsiReport *model);
void  psi_model_free(PsiReport *model);
BOOL  psi_model_add_section(PsiReport *model, const wchar_t *key,
                            const wchar_t *name, PsiSection **out_section);
PsiItem *psi_model_add_item(PsiSection *section, const wchar_t *key,
                            const wchar_t *name);
PsiSection *psi_model_find_section(const PsiReport *model, const wchar_t *key);
PsiItem *psi_model_find_item(const PsiReport *model, const wchar_t *key);

/* 写入辅助 */
void psi_item_set_string(PsiItem *item, const wchar_t *data,
                         const wchar_t *display);
void psi_item_set_u64(PsiItem *item, unsigned long long data,
                      const wchar_t *display, const wchar_t *unit);
void psi_item_set_i64(PsiItem *item, long long data,
                      const wchar_t *display, const wchar_t *unit);
void psi_item_set_bool(PsiItem *item, BOOL data, const wchar_t *display);
void psi_item_set_null(PsiItem *item, const wchar_t *display);

/* JSON 序列化（返回 HeapAlloc 分配的缓冲区，用 psi_model_free_text 释放） */
BOOL psi_model_json(const PsiReport *model, wchar_t **out_text);
BOOL psi_model_json_utf8(const PsiReport *model, char **out_text);
void psi_model_free_text(void *text);

#ifdef __cplusplus
}
#endif

#endif /* PSI_MODEL_H */
