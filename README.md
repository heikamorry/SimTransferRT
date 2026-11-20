# SimTransferRT（asio 分支）

C++17 / Qt 6 / Boost.Asio 的多模块 UDP 传输模拟器。本分支只保留 V2 双模块链路，
GUI 的发送与接收均固定使用 Boost.Asio，不再链接 Qt Network；三维模型/BX 预览保持不变。

## 依赖

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

## 运行

```bat
build\windows-release\SimTransfer.exe
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
3. 右侧 **打开 ▾ → 打开模型** 可加载 PLY / VTP / VTK / BX / HDF5；三维视口支持旋转、缩放、点大小调整。
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

## 能力边界

- BX 只支持加载与点云预览，不反向导出为模型文件。
- 回环测试不能证明真实网卡出线、硬件采样时钟或 Aurora 设备时序。
- V1 已从本分支移除；历史 V1 文档仅作记录，不再对应当前可执行文件。

更多细节：[Asio 迁移](docs/ASIO_MIGRATION.md)、[Asio 实测](docs/ASIO_PERFORMANCE.md)、
[模型预览](docs/MODEL_PREVIEW.md)、[格式矩阵](docs/MODEL_FORMATS.md)、
[HDF5 布局](docs/MODEL_ARCHIVE.md)、[验证记录](docs/MODEL_VERIFICATION.md)。
