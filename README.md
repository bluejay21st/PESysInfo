# PESysInfo v0.9

面向 Windows PE 的硬件信息检测工具，提供 x64 和 x86 版本，重点展示 CPU、内存、存储、图形和网络信息，同时提供 GUI 与 CLI/Agent 接口。

- 原生 Win32 GUI：`dist\x64\PESysInfo.exe`、`dist\x86\PESysInfo.exe`
- 控制台/Agent：`dist\x64\PESysInfo-cli.exe`、`dist\x86\PESysInfo-cli.exe`
- 无驱动、无 WMI、无 PowerShell、无 .NET、无 VC++ 运行库依赖

## GUI

| 页面 | 内容 |
|---|---|
| 概览 | CPU、内存、磁盘、系统版本、显卡摘要 |
| 处理器 | 型号、厂商、架构、核心/线程、频率、L1/L2/L3、指令集 |
| 内存 | 容量、插槽、ECC、每条内存的规格； |
| 存储 | 磁盘、卷信息；按磁盘/卷折叠 |
| 图形 | 显示适配器与显示器信息 |
| 网络 | 网卡信息；按网卡折叠 |
| 原始数据 | 完整 JSON，可复制或保存 |
| 关于 | 版本和平台说明 |

详情分组默认收起，摘要组展开；点击组标题切换，刷新后保留展开状态。
窗口最小内容区为 760 × 520，F5 重新采集。

## CLI

```bat
PESysInfo-cli.exe --json
PESysInfo-cli.exe --get cpu.brand
PESysInfo-cli.exe --display --get cpu.brand
PESysInfo-cli.exe --output info.json
PESysInfo-cli.exe --help
PESysInfo-cli.exe --version
```

- JSON 固定 UTF-8、无 BOM；
- 同时提供 `sections`、`flat` 和 `warnings`；
- 退出码：0 成功，1 采集失败，2 参数/字段错误，3 输出失败。

## 构建

需要 Visual Studio（C++ 桌面开发）和 Windows SDK：

```bat
build.bat          :: 构建 x64
build.bat x86      :: 构建 x86
build.bat all      :: 构建 x64 + x86
build.bat clean    :: 删除 build/ 和 dist/
```

产物位于 `dist\x64\` 和 `dist\x86\`。编译参数和实现细节见 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)。

## WinPE

- 目标：Windows PE 10.0.19041 x64/x86，同时兼容 Windows 10/11 x64/x86。
- 将对应架构的两个 exe 复制到 `Windows\System32\` 后运行。
- 中文显示需集成 `WinPE-FontSupport-ZH-CN` 字体包和 `zh-cn` 语言包。

## 限制

- 不读取 SPD 时序、CPU 电压/温度/风扇、精确 BCLK；
- 不检测接口/端口，也不判断能否升级硬盘；
- 不加载内核驱动，因此部分传感器和低层硬件信息不可用；
- 提供 x64 与 x86 两个构建。

## 文档

开发与复现说明：[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)
