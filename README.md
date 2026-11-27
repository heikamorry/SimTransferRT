# SimTransferRT（asio 分支）

C++17 / Qt 6 / Boost.Asio 的多模块 UDP 传输模拟器。本分支只保留 V2 双模块链路，
GUI 的发送与接收均固定使用 Boost.Asio，不再链接 Qt Network；三维模型/BX 预览保持不变。

## Raspberry Pi OS / Debian 13（64 位）

在树莓派 5 的桌面版系统或 Debian 13 的项目目录内运行以下命令。首次构建会校验 Boost 1.88.0 头文件包，并从经过校验的源码编译 VTK 9.4.2 和 HDF5 1.14.6。Qt 6 来自系统。树莓派默认用两个并行任务，依赖缓存位于 Linux 用户目录。

```bash
bash scripts/linux-release.sh prerequisites
bash scripts/linux-release.sh release
```

Release 步骤构建中文 GUI、两个命令行程序和正式环回测试驱动，运行 CTest、生成本机架构的 Debian 安装包和 SHA-256 清单，然后解包启动程序做冒烟测试。无桌面的构建机可用脚本自带的 Xvfb 测试路径。可用 `bash scripts/linux-release.sh core` 单独构建和测试 Asio 收发核心。设置 `SIM_LINUX_BUILD_DIR` 可更改应用构建目录，设置 `SIM_ARCHIVE_DIR` 可使用本机已有的同名源码压缩包。

在树莓派本机构建后，用以下命令校验并安装 ARM64 包。安装后可从桌面应用菜单或终端启动 `SimTransfer`。笔记本的 x86-64 Debian 构建会生成 `amd64` 包。

```bash
cd build/linux-release/packages
sha256sum -c simtransferrt_0.3.2_arm64.deb.sha256
sudo apt install ./simtransferrt_0.3.2_arm64.deb
```

做真机时序验收时，在树莓派已登录的桌面终端回到项目根目录，执行 `bash scripts/linux-release.sh soak`，保持接收页可见。测试通过同一套应用控件在 `127.0.0.1` 收发 600 秒，日志会给出 `joint.json` 和 `run.json` 路径。前者保留每项验收的实测值和判定，后者记录架构、程序哈希和运行条件。物理网口收发需要另做两机测试。

原生 ARM64 构建默认让 VTK 和 Qt 请求匹配的 OpenGL ES 3 上下文，x86-64 默认使用桌面 OpenGL。可用 `SIM_VTK_GLES=OFF` 或 `ON` 构建对应的 SDK，应用会读取该 SDK 的实际图形配置。

在 x86-64 Debian 上验证 ARM64 指令和 Asio 协议行为，可在准备好上述 Boost 头文件后交叉编译并用 QEMU 运行核心测试。树莓派图形驱动和实时时序仍需真机测量。

```bash
sudo apt install g++-aarch64-linux-gnu qemu-user
cmake --preset linux-arm64-core
cmake --build --preset linux-arm64-core --parallel 2
ctest --preset linux-arm64-core
```

树莓派离线时，可在 Debian 13 的 x86-64 笔记本上运行 `bash scripts/linux-arm64-package.sh`。脚本在 `/var/cache/simtransferrt-arm64` 建立独立的 ARM64 Debian 构建环境，使用目标架构的 Qt、VTK 和 HDF5。它在 QEMU 下运行同一套测试并验证包内程序，成品与校验文件位于 `build/linux-release-arm64/packages/`。模拟环境中的时序只用于诊断。

## Windows 依赖

| 依赖 | 版本 / 获取方式 |
|---|---|
| MSVC | VS Build Tools 2022（v143，14.44） |
| Qt | 6.12 `msvc2022_64`（Core、Widgets、OpenGLWidgets、Test） |
| Boost.Asio | 1.88.0 / Asio 1.34.2，`scripts/setup-asio-deps.ps1` 下载并校验 SHA-256 |
| VTK / HDF5 | `scripts/setup-model-deps.ps1`（VTK 9.4.2、HDF5 1.14.6） |
| CMake / Ninja / Python 3 | CMake ≥ 3.21；Ninja 用于编译 VTK；Python 用于夹具与接收桩 |

## 构建

```bat
cd /d D:\SimTransferRT
call scripts\env.cmd
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\setup-asio-deps.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\setup-model-deps.ps1 -Configuration Release

scripts\build.cmd Release
```

`build.cmd` 依次配置、编译并运行 CTest，产物在 `build\windows-release\SimTransfer.exe`。
只需要网络与命令行时，可关闭三维模块：

```bat
cmake -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DSIM_MODEL_PREVIEW=OFF ^
  -DSIM_WITH_ASIO=ON -DCMAKE_PREFIX_PATH=%QT_PREFIX% -S . -B build\asio-release
cmake --build build\asio-release
ctest --test-dir build\asio-release --output-on-failure
```

本分支要求 `SIM_WITH_ASIO=ON`；关闭时 GUI 配置会直接报错。分发目录：

```bat
cmake --install build\windows-release --prefix dist\model-preview
```

### Windows x64 Release

在配好上述依赖的 x64 Windows 构建机上安装 Python 3.11 或更新版本及 .NET SDK 9 或更新版本，运行：

```bat
scripts\package.cmd
```

脚本构建 Release、运行 CTest 并在 `dist\` 生成以下文件。安装包与 ZIP 使用同一次构建的程序、Qt 插件和模型库。

| 文件 | 用途 |
|---|---|
| `SimTransferRT-<版本>-x64-Setup.exe` | 离线安装向导，可指定安装位置、命令行工具和快捷方式。重开同版安装程序可修改、修复或卸载。 |
| `SimTransferRT-<版本>-x64-Portable.zip` | 将整个压缩包解压至独立目录，运行 `bin\SimTransfer.exe`。CLI 和 x64 MSVC 运行库随包提供。 |
| `SimTransferRT-<版本>-x64-SHA256SUMS.txt` | 使用 PowerShell 的 `Get-FileHash <文件> -Algorithm SHA256` 与清单逐项比对。 |

首次打包从 NuGet 获取 WiX 5.0.2 及其扩展。可用 `SIM_SIGN_THUMBPRINT` 和 `SIM_TIMESTAMP_URL` 配置安装包签名及时间戳服务。省略证书将生成未签名测试包。安装与维护验证见[安装包说明](docs/WINDOWS_INSTALLER_PLAN.md)。

## 运行

从工作区启动 Release 时，`run.cmd` 优先使用 `build.cmd Release` 的 `build\windows-release`。如果只配置过 `build-v2.cmd`，它会复用 `build\windows-v2-release`。脚本按当前源码增量构建 GUI，成功后打印并启动对应的 EXE。

```bat
scripts\run.cmd Release
build\windows-release\SimTransfer.exe --model D:\models\example.vtp
build\windows-release\SimTransferCli.exe --protocol v2 --transport asio-async ^
  --target 127.0.0.1 --port 5650 --command-port 5500 --duration 30 --report tx.json
build\windows-release\SimRtProbe.exe --protocol v2 --transport native --duration 10
```

`--transport` 只接受 `native`、`asio-sync`、`asio-async`；GUI 没有后端开关，固定 Asio 异步。

## GUI 功能与手工测试

### 1. 发送与网络

1. 目标填 `127.0.0.1`，数据端口 `5650`，命令端口 `5500`；网络档位选 `1GbE`。
2. 默认 EP 204800 字节 @50 Hz、NDI 4 tools @60 Hz；需要压力测试时改 EP 为 1048576。
3. 点 **开始 V2 发送**；日志应出现“传输后端：Boost.Asio 异步”。
4. 运行结束后看 **逐项验收**：NDI ≤15 ms、EP ≤18 ms、抖动目标 <1 ms 分别判定。

### 2. 接收与显示

1. 绑定地址 `127.0.0.1`，端口 `5650`，点 **启动接收**；状态应显示“接收已启动（Boost.Asio）”。
2. 发送端使用同一端口启动，NDI 工具表应出现有效位置；电生理计数持续增长。
3. 右侧点 **打开文件 ▾**，按文件类型选择 **打开模型**、**打开 NDI/BX 文件** 或 **打开过程档案**。三维视口支持旋转、缩放和调整点大小。
4. 点 **停止接收**，状态应变为“接收已停止”，NDI/电生理标记为过期。

### 3. 联合验收

同一窗口内先启动接收再启动发送，**逐项验收** 会给出 `gui_local_joint_delivery`；
需要独立证据时用 `tools/v2_receiver.py` 与发送端 `--report` 生成配对报告。

## 自动化测试

```bat
ctest --test-dir build\windows-release --output-on-failure
```

当前 11 项：协议、Aurora、接收器、引擎、Runner、Asio 传输、V2 界面、GUI 冒烟、
模型格式、模型 GUI、GUI 启动参数。Asio 传输测试覆盖同步、异步、背压、取消回收、
独占绑定与反馈回环；模型测试覆盖 PLY/VTP/VTK/BX/HDF5 与三维显示。

## 代码结构与排版

- `src/ui/`：窗口、收发控制、医疗主题背景；`src/model/PreviewPanel.*`：模型预览交互。
- `src/model/`：模型数据、PLY/VTK/BX/HDF5 读写、转换与场景渲染。
- `src/v2/`：配置、生产调度、发送引擎、接收重组和运行报告。
- `src/network/`、`src/protocol/`、`src/sources/`：UDP 适配器、报文编解码、模拟/回放数据源。
- `src/support/`：命令行参数与两个命令行程序共用的运行流程；`src/main.cpp`、`src/cli_main.cpp`、`src/native_cli.cpp`：三个程序入口。
- `tests/`：C++ 和 GUI 回归；`tools/`：Python 接收器、验收及诊断工具。

C++ 每条独立语句结束后换行，每个变量单独声明。条件和循环的主体另起一行，
保留 `for (...; ...; ...)` 的循环头写法。长语句按根目录 `.clang-format` 的
100 列目标宽度换行，使用 4 个空格缩进。
从项目目录运行以下命令（需 PATH 中有 `clang-format`，也可用 `-ClangFormat` 指定路径）：

```powershell
powershell -NoProfile -File scripts/format.ps1        # 整理 src/、tests/、tools/ 和 installer/ 的 C++ 文件
powershell -NoProfile -File scripts/format.ps1 -Check # 仅检查，不改写文件
```

格式脚本不进入构建产物或第三方依赖目录。编辑器格式化时也使用此规则文件。

## 能力边界

- BX 只支持加载与点云预览，不反向导出为模型文件。
- 回环测试不能证明真实网卡出线、硬件采样时钟或 Aurora 设备时序。
- V1 已从本分支移除；历史 V1 文档仅作记录，不再对应当前可执行文件。

更多细节：[Asio 迁移](docs/ASIO_MIGRATION.md)、[Asio 实测](docs/ASIO_PERFORMANCE.md)、
[模型预览](docs/MODEL_PREVIEW.md)、[格式矩阵](docs/MODEL_FORMATS.md)、
[HDF5 布局](docs/MODEL_ARCHIVE.md)、[验证记录](docs/MODEL_VERIFICATION.md)。
