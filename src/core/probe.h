/*
 * probe.h - 探测层对 UI/CLI 暴露的唯一接口
 */
#ifndef PSI_PROBE_H
#define PSI_PROBE_H

#include "model.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 采集一次完整硬件信息并构造 PsiReport。
 * 成功返回 TRUE；返回值不代表所有子项都采集成功，子项失败在 warnings 中体现。
 * 调用方负责用 psi_model_free 释放 model。
 */
BOOL psi_probe_collect(PsiReport *model);

#ifdef __cplusplus
}
#endif

#endif /* PSI_PROBE_H */
