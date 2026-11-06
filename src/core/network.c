/*
 * network.c - 网络适配器采集（IP Helper API）
 *
 * 使用 GetAdaptersAddresses 枚举适配器（WinPE 无 WMI/PowerShell）。
 * 仅依赖 iphlpapi（GetAdaptersAddresses），不依赖 ws2_32：IPv4 地址按
 * sockaddr_in 的 4 个字节手工格式化，不调用 InetNtopW/ws2_32。
 *
 * 所有函数只做采集，不打印任何内容。
 */
#include "pesysinfo.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <winsock2.h>
#include <iphlpapi.h>

#define PSI_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define PSI_NET_FLAGS (GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | \
                       GAA_FLAG_SKIP_DNS_SERVER)

#ifndef IF_TYPE_SOFTWARE_LOOPBACK
#define IF_TYPE_SOFTWARE_LOOPBACK 24
#endif

/* 返回首个 IPv4 单播地址，没有则返回 NULL */
static const SOCKADDR *psi_network_first_ipv4(const IP_ADAPTER_ADDRESSES *adapter)
{
    const IP_ADAPTER_UNICAST_ADDRESS *unicast;

    for (unicast = adapter->FirstUnicastAddress; unicast != NULL;
         unicast = unicast->Next) {
        if (unicast->Address.lpSockaddr != NULL &&
            unicast->Address.lpSockaddr->sa_family == AF_INET) {
            return unicast->Address.lpSockaddr;
        }
    }
    return NULL;
}

/* MAC 格式化为 AA-BB-CC-DD-EE-FF */
static void psi_network_format_mac(const BYTE *address, ULONG length,
                                   wchar_t *buf, size_t cch)
{
    ULONG i;

    if (buf == NULL || cch == 0) {
        return;
    }
    buf[0] = L'\0';
    if (address == NULL || length == 0) {
        return;
    }
    if (length > (ULONG)MAX_ADAPTER_ADDRESS_LENGTH) {
        length = (ULONG)MAX_ADAPTER_ADDRESS_LENGTH;
    }
    for (i = 0; i < length; ++i) {
        psi_str_appendf(buf, cch, (i == 0) ? L"%02X" : L"-%02X",
                        (unsigned)address[i]);
    }
}

/* IPv4 地址按字节格式化，不依赖 InetNtopW/ws2_32 */
static void psi_network_format_ipv4(const SOCKADDR *address,
                                    wchar_t *buf, size_t cch)
{
    const struct sockaddr_in *ipv4;
    const BYTE *bytes;

    if (buf == NULL || cch == 0) {
        return;
    }
    buf[0] = L'\0';
    if (address == NULL || address->sa_family != AF_INET) {
        return;
    }
    ipv4 = (const struct sockaddr_in *)address;
    bytes = (const BYTE *)&ipv4->sin_addr;
    swprintf_s(buf, cch, L"%u.%u.%u.%u",
               (unsigned)bytes[0], (unsigned)bytes[1],
               (unsigned)bytes[2], (unsigned)bytes[3]);
}

/* TransmitLinkSpeed（bit/s）格式化为 100 Mbps / 1.0 Gbps / 2.5 Gbps / 10 Gbps */
static void psi_network_format_speed(ULONG64 bits_per_second,
                                     wchar_t *buf, size_t cch)
{
    double value;

    if (buf == NULL || cch == 0) {
        return;
    }
    buf[0] = L'\0';
    if (bits_per_second == 0 ||
        bits_per_second == (ULONG64)-1 ||
        bits_per_second > 1000000000000ULL) {
        return;
    }

    if (bits_per_second >= 1000000000ULL) {
        value = (double)bits_per_second / 1000000000.0;
        if (value >= 10.0) {
            swprintf_s(buf, cch, L"%.0f Gbps", value);
        } else {
            swprintf_s(buf, cch, L"%.1f Gbps", value);
        }
    } else if (bits_per_second >= 1000000ULL) {
        value = (double)bits_per_second / 1000000.0;
        if (value >= 10.0) {
            swprintf_s(buf, cch, L"%.0f Mbps", value);
        } else {
            swprintf_s(buf, cch, L"%.1f Mbps", value);
        }
    } else if (bits_per_second >= 1000ULL) {
        swprintf_s(buf, cch, L"%.0f Kbps",
                   (double)bits_per_second / 1000.0);
    } else {
        swprintf_s(buf, cch, L"%llu bps",
                   (unsigned long long)bits_per_second);
    }
}

static void psi_network_warn_result(SystemReport *report, ULONG result)
{
    if (report == NULL) {
        return;
    }
    if (result == ERROR_NO_DATA) {
        psi_add_warning(report,
                        L"未检测到网络适配器（网络可能尚未初始化，可运行 wpeinit 后重试）");
    } else if (result == ERROR_NOT_SUPPORTED) {
        psi_add_warning(report,
                        L"当前环境不支持网络适配器枚举（GetAdaptersAddresses 返回 ERROR_NOT_SUPPORTED）");
    } else {
        psi_add_warning(report, L"获取网络适配器信息失败（错误码 %lu）",
                        (unsigned long)result);
    }
}

BOOL psi_network_collect(NetworkInfo *info, SystemReport *report)
{
    ULONG size = 0;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    PIP_ADAPTER_ADDRESSES addresses = NULL;
    PIP_ADAPTER_ADDRESSES probe = NULL;
    PIP_ADAPTER_ADDRESSES current;
    unsigned retries = 0;

    if (info == NULL) {
        return FALSE;
    }
    info->count = 0;

    /* 第一步：用小缓冲区探测所需长度，正常情况下返回 ERROR_BUFFER_OVERFLOW，
     * 并在 size 中得到实际需要的字节数。 */
    size = (ULONG)sizeof(IP_ADAPTER_ADDRESSES);
    probe = (PIP_ADAPTER_ADDRESSES)HeapAlloc(GetProcessHeap(), 0, size);
    if (probe == NULL) {
        if (report != NULL) {
            psi_add_warning(report, L"内存不足，无法枚举网络适配器");
        }
        return TRUE;
    }
    result = GetAdaptersAddresses(AF_UNSPEC, PSI_NET_FLAGS, NULL, probe, &size);
    HeapFree(GetProcessHeap(), 0, probe);
    probe = NULL;

    if (result != ERROR_BUFFER_OVERFLOW && result != ERROR_SUCCESS) {
        /* ERROR_NO_DATA / ERROR_NOT_SUPPORTED 在 WinPE 未初始化网络时属正常 */
        psi_network_warn_result(report, result);
        return TRUE;
    }

    /* 第二步：按探测到的长度分配并正式调用；若两次调用之间适配器增多导致
     * 再次 ERROR_BUFFER_OVERFLOW，则重新分配，最多重试 2 次。 */
    for (;;) {
        addresses = (PIP_ADAPTER_ADDRESSES)HeapAlloc(GetProcessHeap(),
                                                     HEAP_ZERO_MEMORY, size);
        if (addresses == NULL) {
            if (report != NULL) {
                psi_add_warning(report, L"内存不足，无法枚举网络适配器");
            }
            return TRUE;
        }

        result = GetAdaptersAddresses(AF_UNSPEC, PSI_NET_FLAGS, NULL,
                                      addresses, &size);
        if (result != ERROR_BUFFER_OVERFLOW) {
            break;
        }

        HeapFree(GetProcessHeap(), 0, addresses);
        addresses = NULL;
        if (retries >= 2) {
            break;
        }
        retries++;
    }

    if (result != ERROR_SUCCESS || addresses == NULL) {
        psi_network_warn_result(report, result);
        if (addresses != NULL) {
            HeapFree(GetProcessHeap(), 0, addresses);
        }
        return TRUE;
    }

    for (current = addresses;
         current != NULL && info->count < PSI_MAX_NET;
         current = current->Next) {
        NetworkAdapter *item;
        const SOCKADDR *ipv4;
        const wchar_t *description;

        /* 跳过回环适配器 */
        if (current->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }

        ipv4 = psi_network_first_ipv4(current);
        /* 无 MAC 且无 IPv4 的适配器（Teredo、ISATAP 等无意义条目）跳过 */
        if (current->PhysicalAddressLength == 0 && ipv4 == NULL) {
            continue;
        }

        item = &info->items[info->count];
        psi_zero_memory(item, sizeof(*item));

        description = NULL;
        if (current->FriendlyName != NULL && current->FriendlyName[0] != L'\0') {
            description = current->FriendlyName;
        } else if (current->Description != NULL &&
                   current->Description[0] != L'\0') {
            description = current->Description;
        }
        psi_str_copy(item->description, PSI_ARRAY_COUNT(item->description),
                     description);

        if (current->PhysicalAddressLength > 0) {
            psi_network_format_mac(current->PhysicalAddress,
                                   current->PhysicalAddressLength,
                                   item->mac, PSI_ARRAY_COUNT(item->mac));
        }
        if (ipv4 != NULL) {
            psi_network_format_ipv4(ipv4, item->ipv4,
                                    PSI_ARRAY_COUNT(item->ipv4));
        }
        psi_network_format_speed(current->TransmitLinkSpeed, item->speed,
                                 PSI_ARRAY_COUNT(item->speed));

        if (current->OperStatus == IfOperStatusUp) {
            psi_str_copy(item->status, PSI_ARRAY_COUNT(item->status),
                         L"已连接");
        } else if (current->OperStatus == IfOperStatusDown) {
            psi_str_copy(item->status, PSI_ARRAY_COUNT(item->status),
                         L"已断开");
        } else {
            psi_str_copy(item->status, PSI_ARRAY_COUNT(item->status),
                         L"未知");
        }

        info->count++;
    }

    HeapFree(GetProcessHeap(), 0, addresses);

    if (info->count == 0 && report != NULL) {
        psi_add_warning(report,
                        L"未检测到可用的网络适配器（网络可能尚未初始化，可运行 wpeinit 后重试）");
    }

    /* 无网络/WinPE 未初始化网络均属可降级情况，不作为致命错误。 */
    return TRUE;
}
