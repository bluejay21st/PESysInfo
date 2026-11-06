# PESysInfo 开发文档

本文给出从零复现 PESysInfo 所需的架构、接口和实现要点。目标版本为 **v0.9**。

## 1. 目标与约束

- 平台：Windows 10/11 x64/x86、Windows PE 10.0.19041 x64/x86。
- 形态：Win32 GUI `PESysInfo.exe` + 控制台/Agent `PESysInfo-cli.exe`。
- 语言与工具链：C11 风格 C、MSVC x64/x86、Windows SDK；静态 CRT `/MT`。
- 不加载内核驱动，不使用 WMI、PowerShell、.NET、MFC、Qt、COM。
- GUI 与 CLI 共用 `src/core` 的采集和模型代码。
- 硬件数据只读采集；仅按用户操作生成 JSON 输出/保存文件，不修改磁盘或系统设置。

## 2. 目录结构

```text
src/core/      采集层 + 数据模型 + JSON 序列化（core.lib）
  pesysinfo.h  采集结构、常量、函数声明
  probe.c      唯一采集入口 psi_probe_collect()
  model.c/.h   SystemReport -> PsiReport，显示名/格式化
  json.c       PsiReport -> JSON
  cpu.c        注册表、CPUID、拓扑、频率
  memory.c     GlobalMemoryStatusEx、SMBIOS type 16/17
  storage.c    PhysicalDrive/卷 IOCTL
  system.c     OS/PE、计算机名、用户、启动模式
  firmware.c   SMBIOS 固件/整机/主板
  smbios.c     GetSystemFirmwareTable('RSMB')
  graphics.c   显示适配器与显示器
  network.c    GetAdaptersAddresses
  util.c       字符串、格式化、编码、注册表辅助

src/gui/       GUI，生成 PESysInfo.exe
  main.c       窗口、Tab、ListView、按钮、剪贴板、保存
  gui.rc       版本资源
  gui.manifest asInvoker、comctl32 v6、DPI aware

src/cli/       CLI，生成 PESysInfo-cli.exe
  main.c       参数解析、JSON/字段输出
  cli.rc       版本资源
```

依赖方向：`gui -> core`、`cli -> core`。GUI/CLI 不直接调用 CPUID、SMBIOS 或磁盘 IOCTL。

## 3. 构建

环境：Visual Studio（含 C++ 桌面开发工作负载）和 Windows SDK。

```bat
build.bat          :: 构建 x64
build.bat x86      :: 构建 x86
build.bat all      :: 构建 x64 + x86
build.bat clean    :: 删除 build/ 和 dist/
```

编译参数：

```text
/nologo /c /W4 /utf-8 /MT /O2 /GS
/DUNICODE /D_UNICODE
/DWINVER=0x0A00 /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A000000
```

链接：

- GUI：`/SUBSYSTEM:WINDOWS`，`kernel32 advapi32 user32 gdi32 comctl32 iphlpapi`。
- CLI：`/SUBSYSTEM:CONSOLE`，`kernel32 advapi32 user32 iphlpapi`。
- 链接机类型按构建参数选择 `/MACHINE:X64` 或 `/MACHINE:X86`。
- `comdlg32` 由 GUI 运行时动态加载，不进入导入表。
- 产物：`dist\x64\PESysInfo.exe`、`dist\x64\PESysInfo-cli.exe`、`dist\x86\PESysInfo.exe`、`dist\x86\PESysInfo-cli.exe`。
- 版本资源：`0.9.0.0`；界面/CLI 显示 `v0.9`。

## 4. 数据流

```text
psi_probe_collect(PsiReport *out)
  -> psi_collect_all(SystemReport *raw)      各采集器写入原始结构
  -> psi_model_build(&raw, out)               生成 sections/items
  -> GUI: 读取 section/item 的 name/display
  -> CLI: psi_model_json_utf8() 输出 sections/flat/warnings
```

一次 GUI 刷新只采集一次；复制/保存 JSON 使用同一份快照。CLI 每次启动采集一次。

## 5. 数据模型

`PsiReport`：

```text
sections[]  { key, name, items[] }
items[]     { key, name, data, display, type, unit, estimated, unavailable }
warnings[]
generated
```

- `key`：稳定、全局唯一的 ASCII key；数组元素用 `.0`、`.1` 索引。
- `type`：`string | u64 | i64 | bool | null`。
- `data`：原始值；`display`：界面/CLI `--display` 使用的值。
- 无数据项：`type=null`、`unavailable=true`，JSON `data:null`。

JSON 顶层：

```json
{
  "schema": "pesysinfo/1",
  "sections": [],
  "flat": {},
  "warnings": []
}
```

`flat` 是全部 key 的平铺映射，key 与 `sections[].items[].key` 一致。

## 6. 采集实现

### CPU

- 注册表：
  - `HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0`
  - `ProcessorNameString`、`Identifier`、`VendorIdentifier`、`~MHz`
- CPUID：
  - leaf 0：厂商与最大基础 leaf
  - leaf 1 / 7 / 0x80000001：特性位
  - leaf 0x80000002..4：品牌串
  - leaf 0x16：基础/最大频率（Intel 常见）
- `GetLogicalProcessorInformationEx`：核心数、插槽数、L1/L2/L3。
- `GetActiveProcessorCount`：逻辑线程数。
- SMBIOS type 4：最大/当前速度、核心/线程数兜底。
- `CallNtPowerInformation(ProcessorInformation)`：刷新时读取所有逻辑核当前频率平均值；`powrprof.dll` 动态加载，失败显示未知。

### 内存

- `GlobalMemoryStatusEx`：物理内存总量/可用量。
- `GetPhysicallyInstalledSystemMemory`：安装容量；失败时用 SMBIOS 求和兜底。
- SMBIOS type 16：最大支持容量、ECC 类型。
- SMBIOS type 17：插槽、容量、类型、形态、Rank、位宽、速度、电压、Registered、厂商、型号、序列号。
- 不读取 SPD 时序/XMP/EXPO；这些需要 SMBus/SPD 驱动。

### 存储

- 枚举 `\\.\PhysicalDrive0..N`。
- 只读 IOCTL 获取型号、固件、序列号、总线、介质、容量、扇区、分区表、分区数。
- 卷：枚举 A:..Z:，使用 `GetDriveTypeW`、`GetVolumeInformationW`、`GetDiskFreeSpaceExW`。
- `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS` 关联卷与物理磁盘；磁盘条目同时保留 `PhysicalDriveN` 序号。

### 图形

- `EnumDisplayDevicesW` 枚举显示适配器，按 `DeviceString` 去重。
- `EnumDisplaySettingsW` 读取分辨率。
- 对每个适配器继续调用 `EnumDisplayDevicesW` 枚举显示器，读取名称、分辨率、刷新率、主显示器标记。

### 网络

- `GetAdaptersAddresses`：描述、MAC、IPv4、链路速度、连接状态。
- 枚举所有适配器，用状态字段区分已连接/已断开，不做物理端口识别。

### 系统与固件

- `GetComputerNameExW`、`GetUserNameW`、`RtlGetVersion`/版本 API、`GetTickCount64`、`GetLocalTime`。
- WinPE 判定、固件模式（BIOS/UEFI）。
- SMBIOS type 0/1/2/3：BIOS、整机、主板、机箱。

## 7. GUI 实现

- Tab 页：概览、处理器、内存、存储、图形、网络、原始数据、关于。
- 列表：`SysListView32`，两列“项目/值”；内存、存储、网络使用原生 `LVGROUP` 分组。
- 分组：
  - 概览不分组；
  - 内存：总览 + 每个插槽；
  - 存储：总览 + 每个磁盘/卷；
  - 网络：总览 + 每个网卡；
  - 详情组默认折叠，摘要组展开；
  - 刷新时按 group ID 恢复 `LVGS_COLLAPSED` 状态。
- 原始数据：只读多行 `Edit`，内容为完整 JSON。
- 状态栏：左侧状态文本，右侧采集时间；不显示警告数量。
- 字体：Microsoft YaHei -> SimSun -> 默认 GUI 字体。
- 保存 JSON：动态加载 `comdlg32.dll` 的 `GetSaveFileNameW`，避免静态依赖。
- 最小内容区：760 × 520；支持 DPI 缩放。

## 8. CLI 契约

```bat
PESysInfo-cli.exe --json
PESysInfo-cli.exe --get cpu.brand
PESysInfo-cli.exe --display --get cpu.brand
PESysInfo-cli.exe --output info.json
PESysInfo-cli.exe --help
PESysInfo-cli.exe --version
```

- 默认输出 JSON；重定向时 UTF-8、无 BOM。
- `--get` 输出原始值，`--display --get` 输出显示值。
- `--output` 仅用于 JSON 输出；与 `--get` 同时使用会返回参数错误。
- 退出码：0 成功，1 采集失败，2 参数/字段错误，3 输出失败。
- 未知 key 返回 2；输出文件写入失败返回 3。

## 9. WinPE 兼容

- 目标 PE：Windows PE 10.0.19041 x64/x86；GUI/CLI 都以静态 CRT 链接。
- 部署时选择与 PE 架构一致的 `dist\x64` 或 `dist\x86`。
- 依赖：GUI `user32/gdi32/comctl32/iphlpapi`，CLI `user32/iphlpapi`；`comdlg32` 可选且动态加载。
- 中文：PE 需集成 `WinPE-FontSupport-ZH-CN` 和 `zh-cn` 语言包，否则中文可能显示方框。
- 部署：将两个 exe 复制到 `boot.wim` 的 `Windows\System32\`，启动 `wpeinit` 后运行。
- 无驱动设计意味着 CPU 温度/电压、风扇、SPD 时序、接口/端口等无法可靠获取。

## 10. 复现顺序

1. 建立 `src/core` 的 `SystemReport`/`PsiReport` 数据结构和 `util.c`。
2. 实现 SMBIOS 读取、系统/固件/CPU/内存采集。
3. 实现存储、图形、网络采集。
4. 实现 `model.c` 的 section/item 映射和显示格式化。
5. 实现 `json.c`，保证 `sections`、`flat`、`warnings` 一致。
6. 实现 CLI：参数、UTF-8 输出、`--get`、`--output`。
7. 实现 GUI：Tab、ListView、分组、原始数据、保存/复制、About。
8. 按本文的构建参数分别生成 x64/x86 两个架构的 exe，在 Windows 与 WinPE 中做启动和采集冒烟。

## 11. 明确不做

- 接口/端口检测（M.2/SATA/PCIe/USB/显示接口）；
- 能否加硬盘的自动结论；
- SPD 时序、XMP/EXPO、CPU 电压/温度/功耗、风扇转速；
- 精确实时 BCLK/倍频、时钟发生器；
- 性能跑分、磁盘测速；
- ARM64。
