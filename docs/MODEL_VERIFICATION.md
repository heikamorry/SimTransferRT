# Windows 构建、部署与实测记录

工作区 `D:\SimTransferRT`，分支 `mid-term`，基线 `bcc2e98423bba8f72095c9355eba7e70e82e393d`。测试平台为 Windows 11 x64（build 26300）、16 个逻辑处理器、约 31.5 GiB 物理内存。系统同时有 Intel Graphics 与 NVIDIA GeForce RTX 5060 Laptop GPU，本次 OpenGL 实际渲染器为 Intel Graphics。

## 依赖与构建

本次使用 MSVC x64 v143 工具集 14.44.35207、Qt MSVC x64 6.12.0、CMake 4.4.5、Ninja 1.13.2、VTK 9.4.2、HDF5 C shared 1.14.6。VTK 在本机分别编译 Debug/Release，使用 Qt 6、OpenGL2、GUISupportQt、交互、文本、XML 和 legacy I/O 模块。HDF5 使用 HDF Group 的 vs2022_cl x64 预编译发行包。Python 用于生成独立夹具和运行回归。Qt 界面与读写器均由 C++ 编译。

| SDK 输入 | SHA-256 |
|---|---|
| VTK 官方 9.4.2 源码包 | `36c98e0da96bb12a30fe53708097aa9492e7b66d5c3b366e1c8dc251e2856a02` |
| HDF5 官方 1.14.6 Windows VS2022 包 | `976075012b8e395d3b89439f5d63b94481d0de79b69a0d17b99f587abf23100a` |

以下从仓库根目录的 CMD 执行。安装脚本校验下载包，首次构建 VTK 需要 Ninja、编译器和足够空间。SDK 安装在 `build\model-deps\sdk-release`、`sdk-debug` 和 `hdf-sdk`。Qt 如安装在其他位置，先按 README 设置 `QT_PREFIX`，并相应替换配置命令的路径。

```bat
cd /d D:\SimTransferRT
chcp 65001
call scripts\env.cmd
powershell -NoProfile -File scripts\setup-model-deps.ps1 -Configuration Release
powershell -NoProfile -File scripts\setup-model-deps.ps1 -Configuration Debug
cmake -S . -B build/verified-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.12.0/msvc2022_64 -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=D:/SimTransferRT/build/verified-release/bin
cmake --build build/verified-release
ctest --test-dir build/verified-release --output-on-failure
cmake -S . -B build/verified-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.12.0/msvc2022_64 -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=D:/SimTransferRT/build/verified-debug/bin
cmake --build build/verified-debug
ctest --test-dir build/verified-debug --output-on-failure
cmake --install build/verified-release --prefix D:/SimTransferRT/dist/model-preview
```

开发版可执行文件位于 `build\verified-release\bin` 或 `build\verified-debug\bin`。VTK、HDF5 和平台插件来自构建时选定的 SDK。构建后动作向测试程序和主程序所在目录部署运行库与 `platforms\qwindows[d].dll`、`qoffscreen[d].dll`。方括号内的 `d` 表示 Debug 后缀。Qt Test 需要 SDK 的 Test 组件。只构建现有网络桌面程序时可以配置 `-DSIM_MODEL_PREVIEW=OFF`。core-only 预设仍可独立运行。

本机中文版 MSVC 的包含文件输出前缀为“注意: 包含文件:”。在同一 UTF-8 代码页下配置和编译后，以 Ninja 的 `-t deps` 检查 `tests\CMakeFiles\SimModelTests.dir\test_model.cpp.obj`，实测识别 333 条头文件依赖（VALID）。

**完整交付目录为 `dist\model-preview\`，双击 `bin\SimTransfer.exe`。** 一并保留 `bin` 中的 Qt / VTK / HDF5 DLL、Qt 配置 `qt.conf`、`plugins`、翻译和许可证目录。分发版 Qt 按 `qt.conf` 寻找 `plugins\platforms\qwindows.dll`。把 EXE 单独复制会丢失依赖。开发测试使用与 Qt DLL 匹配的 Release 或 Debug 插件。分发版实机视图见[启动截图](model-verification/packaged-surface.png)。

下面的 PowerShell 检查从独立工作目录启动分发版，将进程 PATH 限于 Windows 系统目录，核对实际加载的 Qt、QPA、VTK 和 HDF5 模块，写出截图及运行证据，并检查 V1 启动。新电脑若缺少 MSVC 运行库，可从分发目录 `bin\vc_redist.x64.exe` 安装。另一台全新 Windows 电脑未验证。

```powershell
pwsh -NoProfile -File tests/verify_model_deployment.ps1 -Package D:\SimTransferRT\dist\model-preview -Model D:\SimTransferRT\build\verified-release\tests\model-fixtures\surface.ply -Evidence D:\SimTransferRT\docs\model-verification
```

实测分发版打开 `surface.ply` 并生成截图、V1 冒烟启动均返回 0。记录到 73 个从分发目录加载的 Qt、VTK、HDF5 相关模块及插件，其中包括 `plugins\platforms\qwindows.dll`。分发版 EXE 与验证用 Release EXE 的 SHA-256 相同，均为 `1F1D0DB06D2F59E0F3CFD6ECDCAFF99AF9E1C1EE5D2605A3C9DBD819C5B77DC7`。

## 功能验证

| 验证项 | 实测与范围 |
|---|---|
| Windows core-only | CTest 8/8 通过。 |
| 关闭预览的 Qt GUI / CLI | Ninja Release 配置 `-DSIM_MODEL_PREVIEW=OFF`，`build\model-off` 下 CTest 12/12 通过。 |
| Python 原有回归 | 188/188 通过。 |
| Windows Release / Debug 桌面及格式回归 | Release CTest 15/15 通过（24.4 秒），Debug CTest 15/15 通过（33.2 秒）。两配置各含 `local_model_formats`、`local_model_gui`、`gui_launch_options` 及原有 12 项；`local_model_gui` 增加 BX → HDF5 → 重新打开的回读断言，`gui_launch_options` 覆盖启动参数、截图与失败退出码。日志在 `build\model-deps\verified-tests-Release.log` 和 `verified-tests-Debug.log`。 |
| 复核截图中的 Qt 插件启动错误 | Release 和 Debug 的 `SimModelGuiTests.exe` 均从含中文、空格的临时工作目录无参数成功运行。子进程 PATH 仅有 Windows 系统目录，清除 Qt、VTK、HDF5 环境变量。测试根据 EXE 位置定位夹具。中文 TEMP 下最新的 22 组格式/归档用例全部通过，详见 `build\model-deps\unicode-formats.log`。直接启动日志在 `build\model-deps\direct-model-release.log`、`direct-model-debug.log`。 |
| 独立读写比对 | Windows 上扩展后的格式测试共 22 组通过，含原生 PLY 颜色/法向/UV 通道归属、VTP 活动属性冲突拒绝，以及标准 BX 固定样例、模型逐顶点导出、512 帧、重复/未引用点、帧号回绕和 BX→模型后端拒绝。Python `struct`、CRC、压缩器构造文件和预期数值。VTK 官方 XML/legacy 读写器交叉读取。HDF5 C API 核对 512×3 的 Float32 小端数组、块大小、shuffle、DEFLATE level 1，修改单个值后读回须报告字段校验失败。覆盖同名不同关联、64 位值、NaN 载荷、符号零、RGBA、UV、带非三角面的拓扑、多块、无面点云及拒绝条件。输入和预期常数见[夹具](../tests/model_fixtures.py)，实际断言见[格式测试](../tests/test_model.cpp)。 |
| 原生桌面交互 | 使用 Windows QPA 实际创建 OpenGL 视口，QTest 驱动相机操作及模式切换，采样屏幕读回。涉及接收表/分隔条、原始色/伪彩、卸载与重载及 BX 多工具显示。截图分别见[小模型](model-verification/local-surface.png)及[BX](model-verification/local-bx.png)。 |

```bat
cmake --preset core-only
cmake --build --preset core-only
ctest --preset core-only
python -m unittest discover -s tools -p "test_*.py"
```

## 大模型与界面响应

从用户 Downloads 读取 `Tiananmen_Gate_Detailed.ply`，212,485,676 字节，SHA-256 `f66ca4ce9076516ab5314bd942db084c958700ad20e3cf7b942b167f74bdab6c`。二进制小端 PLY，4,490,824 个顶点、7,017,756 个面，包含 Float32 位置/法向和 UInt8 顶点 RGB。文件声明以米为单位、X 朝东 Y 朝北 Z 朝上。独立解析与校验 1.751 秒，三轮实机数据如下。工作集是 Windows 进程采样值。峰值计数器在进程生命周期内累计。

| 轮次 | 打开到首帧读回 | 拖动及帧缓冲读回 | 最长 UI 心跳间隔 | 显示中工作集 | 卸载后工作集 |
|---|---:|---:|---:|---:|---:|
| 1 | 2.420 s | 42 ms | 300 ms | 1,834,483,712 B | 996,478,976 B |
| 2 | 2.441 s | 35 ms | 302 ms | 1,738,342,400 B | 838,483,968 B |
| 3 | 2.476 s | 42 ms | 315 ms | 1,739,186,176 B | 838,508,544 B |

首轮转换 VTP、目标回读验证、写入并复核 HDF5 合计 7.585 秒，记录到的进程最高工作集为 2,531,987,456 B。当次 Intel 适配器的 DXGI local 使用量在首轮显示时为 802,885,632 B，卸载后为 586,293,248 B。重复枚举项不相加。三轮不足以判定长期泄漏。第三轮卸载后仍有 Qt、VTK、图形驱动和分配器驻留内存。[拖动放大后的细节](model-verification/large-detail.png)。

分发仓库中仅附[大型模型显示截图](model-verification/large-model.png)。依据输入头部，2026-11-19 生成的模型按 [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/) 分发，模型截图中的模型部分沿用该许可。肖像颜色来自 Daniel Case（2014），经裁剪、透视校正、重采样并映射顶点颜色。[肖像原图](https://commons.wikimedia.org/wiki/File:Front_view_of_Tiananmen_gate_from_north_end_of_Tiananmen_Square.jpg)，[CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/)。彩绘取色来自 Lan Zhiyi（2024），经裁剪、透视校正、颜色归一化并映射顶点颜色。[彩绘原图](https://commons.wikimedia.org/wiki/File:%E5%A4%A9%E5%AE%89%E9%97%A8%E5%9F%8E%E6%A5%BC%E7%9A%84%E4%BE%A7%E9%9D%A2.jpg)，[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)。国徽浅浮雕改编自文件头注明的公有领域图形，经过采样和颜色调整。[国徽原图](https://commons.wikimedia.org/wiki/File:%E4%B8%AD%E8%8F%AF%E4%BA%BA%E6%B0%91%E5%85%B1%E5%92%8C%E5%9C%8B%E5%9C%8B%E5%BE%BD.svg)。

可从仓库根目录重复大模型检查。第二个参数为证据输出目录，第三个参数为本机模型路径。当前测试可再传入轮数（默认 3），每轮均执行转换和回读。逐轮内存数据和复测命令见[天安门模型 12 轮内存复测](MODEL_MEMORY_SOAK.md)。

```bat
build\verified-release\bin\SimModelGuiTests.exe build\verified-release\tests\model-fixtures docs\model-verification "%USERPROFILE%\Downloads\Tiananmen_Gate_Detailed.ply"
```

最终 Release 构建完成后又用同一大模型复测三轮。独立读取校验为 1.805 秒，从打开文件到首帧读回分别为 2.405、2.480、2.598 秒。三轮拖动至读回分别为 37、41、34 毫秒。首轮转换至 VTP、输出回读、HDF5 归档与回读合计 7.346 秒，进程峰值为 2,527,014,912 字节。

## 模型到 BX 与点云回归

| 验证项 | 实测与范围 |
|---|---|
| CRC 与固定样例 | `"123456789"` 的 CRC16 为 `0xBB3D`；单工具样例逐字节等于 53 字节固定序列，`HeaderCRC=0x4330`、`BodyCRC=0x3904`。`encodeBx` 与快速单工具编码器输出一致。 |
| 逐顶点导出 | `surface.ply` 4 点、单位 metres → 4 帧、212 字节；坐标换算为 mm 并舍入为 Float32，帧号 8/16/24/32。`bx-model.ply` 的 5 个点（含重复坐标和未被面引用点）保持顺序输出 5 帧。 |
| 大点数 | `compressed-cloud.ply` 512 点输出 512 帧、27,136 字节；回读 512 个有效位置。 |
| BX 读取 | `tools.bx` 单帧 5 工具 → 3 个 Valid 位置、1 Missing、1 Disabled；`concat.bx` 两帧 → 6 个位置，重复 Handle/坐标不合并；帧号 `0xFFFFFFF8 → 0 → 8` 回绕保留 3 个位置。 |
| 错误边界 | CRC、长度、尾随字节、非有限坐标、Float32 溢出、空点集分别给出文件偏移或点索引；BX→PLY/VTP/VTK 在能力检查和转换入口均拒绝。 |
| 本地与网络一致 | GUI 测试把同一组 3 帧 BX 字节经 V2 UDP 发送，接收重组后预览得到 3 个位置；把相同字节落盘再打开，点数、顺序和坐标一致。传输完成、BX 解码完成和点云显示分别记录。 |

## 原网络功能与预览并行

V2 GUI 的本地预览直接读取用户选择的文件。网络发送、接收、NACK、重传、时序和缓存仍由原链路处理，打开本地文件不会发出数据报。以下比较使用同机独立的 CLI 发送进程和 Python 接收桩，5 秒、1g 档、60 Hz NDI 和 50 Hz EP。对照组启用分发版 GUI 打开大型模型，GUI 在取到首帧截图后退出。覆盖加载与显示时段。分别观察同一组原有严格验收指标。

| 指标（NDI / EP） | 仅网络 | 并行打开大模型 |
|---|---:|---:|
| 构建、完整接收、消费帧数 | 每项 300 / 250 | 每项 300 / 250 |
| 跳槽、首发截止超期 | 每项 0 / 0 | 每项 0 / 0 |
| 采样偏差 P99.9（µs） | 1651 / 1121 | 1010 / 842 |
| 单模块包间隔最大偏差（µs） | 1772 / 2705 | 1113 / 2226 |
| 原严格验收（通过 / 失败 / 未测） | 9 / 5 / 13 | 10 / 4 / 13 |

并行组 NDI 采样偏差、两模块包间隔偏差和全局包间隔偏差仍超过原阈值。基线额外有 EP 采样偏差超限。两次 5 秒回环只覆盖本机调度和功能完整性。跨机物理网卡、硬件采样、同一进程长时间拖动与长期网络稳定性在本轮未执行。[复现实验脚本](../tests/model_network_comparison.py)。本机明细为 `build\model-deps\preview-network-final\{baseline,concurrent-preview}\acceptance.json` 和 `result.json`。

复现时指定新的输出目录，脚本会创建该目录并保留每次运行的参数和证据。

```bat
python tests\model_network_comparison.py --gui dist\model-preview\bin\SimTransfer.exe --sender dist\model-preview\bin\SimTransferCli.exe --model "%USERPROFILE%\Downloads\Tiananmen_Gate_Detailed.ply" --output build\model-deps\preview-network-repeat --seconds 5
```

最终分发版再次执行相同的 5 秒网络对照，两种情况下均构建、完整接收并消费 300 帧 NDI 和 250 帧 EP，跳槽和首发截止超期均为零。原严格指标在仅网络组为 10 项通过、4 项失败、13 项未测，在并行打开模型组为 13 项通过、1 项失败、13 项未测。并行组 EP 包间隔最大偏差为 1,021 µs，超过原定的 1,000 µs 上限。

## 本轮缺陷修复

| 位置 | 现象 | 处理 |
|---|---|---|
| `src/main.cpp` | `QFileInfo::operator==` 把不同路径判为同一文件，`--model` 与 `--screenshot` 的正常组合被误拒 | 改为比较规范化绝对路径（Windows 不区分大小写） |
| `src/main.cpp` | 截图路径没有扩展名时到写入阶段才失败，错误不明确 | 参数阶段返回明确错误 |
| `src/v2/Engine.cpp` | EP 已发完但仍标记重复补发时 `remaining==0`，整数除零 | 增加边界提前返回 |
| `src/v2/Runner.cpp` | 工作线程收尾的 `publish()` 抛异常会终止进程，且 `active` 不复位 | 捕获异常并无条件清除 `active`；生产者收尾同样保护 |
| `src/application/SessionManager.cpp` | 线程启动失败后已发布的快照仍为 `running=true` | 发布失败快照并复位状态；catch 内发布与空快照也做保护 |
| `src/ui/MainWindow.cpp` | 纯 `QWidget#card` 缺少 `WA_StyledBackground`，卡片背景不生效 | 卡片改用 `QFrame`，配置面板设置属性 |
| `src/model/PreviewPanel.cpp` | BX 由单帧工具表改为点云后，统计行和点大小缺少展示位置 | 增加帧数/有效/Missing/Disabled/点数/范围统计行与点大小控制，相机自动留边 |

## 变更定位

| 路径 | 用途 |
|---|---|
| `CMakeLists.txt`、`src/CMakeLists.txt`、`src/model/{CMakeLists.txt,Dependencies.cmake,StageRuntime.cmake}` | 功能开关、依赖锁定、模型模块链接、Windows DLL/插件部署、安装规则。 |
| `scripts/setup-model-deps.ps1`、`scripts/build-model-vtk.cmd` | 下载校验、解包 HDF5 并编译匹配配置的 VTK。 |
| `src/main.cpp`、`src/ui/{V2Window,MainWindow}.cpp`、`src/ui/MedicalBackdrop.h`、`resources/{styles/light.qss,app.ico,app.rc,icons/app-*.png,icons/chevron-*.png,icons/spin-*.png}`、`tools/{make_app_icon.py,make_ui_arrows.py}`、`src/model/{PreviewPanel,Scene}.*` | Qt/VTK 初始化、医疗主题背景与阿斯克勒庇俄斯之杖水印、同款 EXE/窗口图标、Qt 分隔条、后台加载/转换、进度取消、三维场景和交互。 |
| `src/model/{Model,Ply,Vtp,Legacy,Conversion,Archive}.*` | 类型化数据、拓扑约束、格式适配器、BX 协议桥接、能力检查、文件提交和 HDF5 校验。 |
| `tests/CMakeLists.txt`、`tests/{model_fixtures.py,test_model.cpp,test_model_gui.cpp,test_gui_launch.py,model_network_comparison.py,verify_model_deployment.ps1}` | 自动生成夹具，注册格式与桌面测试、GUI 启动参数回归、独立交叉验证、性能与原网络对照、独立分发路径验证。 |
| `README.md`、`docs/MODEL_*.md`、`docs/model-verification/*.png` | 使用入口、能力矩阵、HDF5 布局、实测结论与截图。 |
