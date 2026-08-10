# MCDevLink

MCDevLink 是宿主主动驱动的 C++20 调试协议后端。当前实现 Safaia 的 UDP discovery、TCP 回连、握手、心跳、断线处理和协议 4 日志接收；库内部不创建线程，所有异步推进和用户回调均发生在调用 `Runtime::poll()` 的线程。

## 接入

MCDevLink 以源码形式加入上层 C++ CMake 工程，并在同一次配置中构建为静态 target：

```cmake
add_subdirectory(path/to/MCDevLink)
target_link_libraries(your_target PRIVATE MCDevLink::MCDevLink)
```

最小日志接收代码：

```cpp
#include <MCDevLink/Protocol/Safaia.hpp>
#include <MCDevLink/Runtime.hpp>

MCDevLink::Runtime runtime;

MCDevLink::Protocol::SafaiaOptions options;
options.advertiseAddress = "127.0.0.1";
options.discoveryTargets = {"127.0.0.1"};

MCDevLink::Protocol::SafaiaService safaia(runtime, options);
safaia.setLogHandler([](const MCDevLink::LogEvent& event) {
    consume(event.message);
});

if (const auto error = safaia.start()) {
    report(error.message());
}

while (applicationRunning) {
    update();
    (void)runtime.poll();
    render();
}
```

`LogEvent` 拥有 `message` 和 `source` 字符串，回调中可安全复制或转移到上层队列。Safaia 协议 4 没有可靠的等级字段，因此当前 `level` 为 `LogLevel::unknown`，上层可按自身日志格式二次分类。

`bindEndpoint` 是 TCP 回连服务的监听端点，默认 `127.0.0.1:0`；端口 `0` 表示由操作系统分配临时端口。空地址是无效配置。只有明确需要从其他设备回连时才修改监听地址，优先绑定宿主的具体局域网 IPv4；只有确实需要监听全部 IPv4 网卡时才显式使用 `0.0.0.0`。

`advertiseAddress` 是通过 discovery 告知 MC 的宿主 IPv4，`discoveryTargets` 是 discovery 数据报的目标 IPv4。本机运行三者都使用 `127.0.0.1`；跨设备时，将 `bindEndpoint.address` 和 `advertiseAddress` 设置为宿主局域网地址，将 `discoveryTargets` 设置为游戏设备地址。监听地址与广播地址相互独立。当前这些地址只接受 IPv4 字面量，不做 DNS 解析。

## 线程和生命周期

- MCDevLink 核心不调用 `std::thread`、`CreateThread` 或 `pthread_create`。
- 一个 `Runtime` 同一时刻只能由一个线程调用 `poll()`；回调、`send()` 和 handler 修改也应在该线程进行。
- `SafaiaService` 必须先于关联的 `Runtime` 析构。服务实例一次启动；停止后如需重启，重新构造服务。
- `poll()` 默认限制事件数和处理时间，避免宿主主线程被网络事件长期占用。
- `MCDevLink` 是 C++ 静态 CMake target，不生成需要部署的 DLL。CMake 不设置 `/MT`、`/MD` 或 `CMAKE_MSVC_RUNTIME_LIBRARY`，在 `add_subdirectory` 模式下继承上层工程的运行库选择。

## 协议扩展

`setFrameHandler()` 可接收握手完成后的原始协议帧，`send(sessionId, protocolId, payload)` 可向指定会话排队发送帧。这两个入口用于后续命令/RPC、文件传输等协议服务，业务层无需接触 Asio。

当前 Safaia 链路没有认证、授权和加密，只适合可信开发网络。未来接入代码执行前必须先定义身份校验、能力授权、请求关联、超时和输出上限，不能直接把现有原始帧入口暴露到不可信网络。

## 构建与测试

```powershell
cmake --preset x64-msvc-debug
cmake --build --preset x64-msvc-debug
ctest --test-dir build/x64-msvc-debug --output-on-failure
```

自动测试只覆盖纯帧逻辑、Runtime 本地绑定和本机假客户端握手/日志链路，不启动 Minecraft。

真机日志接收程序为：

```powershell
.\build\x64-msvc-debug\examples\mcdevlink_safaia_log_receiver.exe 127.0.0.1 127.0.0.1
```

跨设备参数依次为 `宿主可达IPv4` 和 `游戏设备IPv4`。该程序仅构建，不注册为自动测试；需要人工启动 Minecraft 完成验证。

完整验收步骤见 [`docs/testing/SafaiaManualTest.md`](docs/testing/SafaiaManualTest.md)。
