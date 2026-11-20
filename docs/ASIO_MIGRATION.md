# V2 Boost.Asio 迁移（asio 分支）

## 基线与依赖

- 基线提交：`84c9843f7430076367b2e735c2b46b93ba123ccc`（本地与 `origin/mid-term` 一致）。
- 分支：`asio`，从上述提交创建，未改写 `mid-term`。
- Boost：**1.88.0**，Asio **1.34.2**，仅头文件，不引入 standalone Asio，不要求 C++20。
- 获取方式：运行 `scripts/setup-asio-deps.ps1`（PowerShell）。
  下载 `boost_1_88_0.tar.bz2`，校验 SHA-256
  `46d9d2c06637b219270877c9e16155cbd015b6dc84349af064c088e9b5b12f7b`，
  只解压 `boost/` 与许可证到 `%LOCALAPPDATA%\SimTransferRT\deps\boost_1_88_0`。
  离线时用 `-ArchivePath <已下载文件>`；也可 `-DSIM_BOOST_ROOT=<含 boost/asio.hpp 的目录>`。
- CMake：默认 `SIM_WITH_ASIO=OFF`，既有构建不受影响；`-DSIM_WITH_ASIO=ON` 时由
  `cmake/AsioDependencies.cmake` 校验版本并生成 `sim_asio_headers`。

## 已迁移路径

| 路径 | 状态 |
|---|---|
| V2 CLI 发送（`SimTransferCli --protocol v2`） | `--transport qt`（默认）、`native`、`asio-sync`、`asio-async` 均可选 |
| V2 原生探针（`SimRtProbe`） | `--transport native`（默认）、`asio-sync`、`asio-async` |
| V2 GUI 接收 socket | 已加入 `AsioReceiverSocket`，但当前界面仍使用 `QUdpSocket`；尚未提供界面切换 |
| V2 GUI 发送后端选择 | 未完成；界面仍固定 Qt 适配器 |
| V1、模型转换、BX 点云、VTK 预览 | 未改动，仍用原实现 |

## 线程与所有权

- 一个 `io_context`，只在调用线程（V2 唯一的发送线程）上 `poll()`；没有 io_context 线程池。
- 固定 64 个发送槽，每槽 1472 字节，另有 256 KiB 在途字节上限；槽满返回 `Backpressure`。
- 缓冲区归槽所有，完成处理前不释放；完成事件进入固定 64 项环，`inFlight()` 在完成被引擎取走后减一。
- `cancelOutstanding()` 只调用 `socket.cancel()`；析构时先取消并回收全部完成，再关闭 socket。
- 未连接 UDP：保留 `send_to` 目标与同一本地端口的反馈语义，不改成 connected UDP。
- 保留 IPv4 单播校验、DF、`SIO_UDP_CONNRESET`、4 MiB 收发缓冲与回读、超长报文拒绝。

## 发送语义

`IUdpTransport` 新增：

```cpp
enum class SendDisposition { Complete, Failed, Backpressure, Pending, Cancelled };
struct SendCompletion { std::uint64_t token; SendDisposition disposition; std::uint64_t observedUs; };
virtual SendDisposition submitDatagram(const std::uint8_t*, std::size_t, std::uint64_t token);
virtual bool pollCompletion(SendCompletion&);
virtual std::size_t inFlight() const noexcept;
virtual void cancelOutstanding() noexcept;
```

- 同步后端：`Complete/Failed/Backpressure` 立即返回，`step()` 的原有语义不变。
- 异步后端：`Pending` 只表示已提交；引擎在同一次 `stepTransport()` 内观察该 token 的完成后才记账。
  因此当前实现没有把多包流水线化，也没有把 `async_send_to` 的返回当作发送成功。
- 背压：`stepInternal(..., allowDefer=true)` 在 `Backpressure` 时直接返回，不推进分片、不计成功、不移动原始计划。
- 首发完成时间：异步模式取完成处理被观察到的时刻，报告中标注
  `first_send_endpoint=completion_handler_observed`；它仍不是网卡出线时间。

## 构建与运行

```bat
call scripts\env.cmd
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\setup-asio-deps.ps1

cmake -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DSIM_MODEL_PREVIEW=OFF ^
  -DSIM_BUILD_TESTS=ON -DSIM_WITH_ASIO=ON -DCMAKE_PREFIX_PATH=%QT_PREFIX% ^
  -S . -B build\asio-enabled
cmake --build build\asio-enabled
ctest --test-dir build\asio-enabled --output-on-failure
```

对照：

```bat
build\asio-enabled\SimTransferCli.exe --protocol v2 --transport asio-sync  ^
  --target 127.0.0.1 --port 5802 --command-port 5902 --duration 8 ^
  --network-profile 1g --ep-bytes 204800 --ndi-tools 4 --ndi-hz 60 --ep-hz 50 ^
  --status-interval-us 0 --report tx.json
```

`--transport qt` 在 `SimTransferCli` 中可用；`SimRtProbe` 不链接 Qt，只接受 `native` 与 Asio。

## 未完成 / 边界

- GUI 接收器尚未切到 Asio，也没有界面选择器；`AsioReceiverSocket` 只是可用组件。
- 异步路径一次只观察一个完成，未实现多包在途流水线；因此本轮不宣称异步优于同步。
- 未做 30 分钟长测、真实网卡双机、ETW 逐片定位；这些需要在本机 Windows 环境继续执行。
