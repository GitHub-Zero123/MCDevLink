# RuntimeLink 设计计划

## 1. 项目定位

`RuntimeLink` 是一个轻量、跨平台的 C++20 通信后端库，用于在运行中的应用与开发工具之间建立通信。

目标平台：

* Windows
* Android
* 后续可扩展 Linux / macOS / iOS

典型用途：

* 实时日志
* 调试通信
* 命令/RPC
* 文件传输
* 远程控制
* Safaia 等现有协议适配

核心库不绑定 Minecraft、Safaia 或具体业务。

---

## 2. 核心原则

### 无内部线程

RuntimeLink **不得主动创建线程**。

禁止核心库内部依赖：

```cpp
std::thread
std::jthread
CreateThread
pthread_create
```

线程模型完全由宿主决定。

游戏、GUI、Android 等环境通过：

```cpp
runtime.poll();
```

主动驱动网络事件循环。

CLI / Worker 类程序后续可额外支持：

```cpp
runtime.run();
```

阻塞执行事件循环。

---

## 3. 网络后端

使用：

**Standalone Asio + C++20**

不依赖 Boost。

主要使用：

* `asio::io_context`
* TCP
* UDP
* Timer
* C++20 Coroutine
* `asio::awaitable`
* `asio::co_spawn`
* `asio::use_awaitable`

平台底层由 Asio 负责适配：

```text
Windows  -> IOCP
Android  -> epoll
Linux    -> epoll
macOS    -> kqueue
```

RuntimeLink 不自行封装 IOCP/epoll。

---

## 4. 事件循环

宿主主动驱动：

```cpp
Runtime runtime;

while (running)
{
    update();
    runtime.poll();
    render();
}
```

`poll()` 必须非阻塞。

同时提供事件预算，防止网络事件过多导致主线程卡顿：

```cpp
struct PollOptions
{
    std::size_t maxEvents = 256;
    std::chrono::microseconds timeBudget = 500us;
};
```

内部优先使用：

```cpp
io_context::poll_one()
```

根据：

* 最大事件数量
* 最大执行时间

限制单次处理量。

---

## 5. 异步模型

内部优先使用 C++20 Coroutine，而不是 callback 状态机。

例如：

```cpp
asio::awaitable<void> Session::run()
{
    co_await handshake();

    while (connected())
    {
        auto packet = co_await readPacket();
        dispatch(packet);
    }
}
```

协程只负责描述异步状态机。

**Coroutine 不代表创建线程。**

协程恢复仍由：

```cpp
runtime.poll();
```

驱动。

---

## 6. API 隔离

Public API 不允许暴露 Asio。

禁止：

```cpp
asio::io_context&
asio::ip::tcp::socket&
asio::awaitable<>
```

出现在 RuntimeLink 公共接口中。

使用 PImpl 隔离：

```cpp
class Runtime
{
public:
    Runtime();
    ~Runtime();

    void poll();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
```

Asio 仅存在于内部 `.cpp` / private headers。

这样可以：

* 降低编译开销
* 隔离第三方依赖
* 保持稳定 API / ABI
* 未来允许替换网络实现

---

## 7. 模块划分

```text
RuntimeLink
│
├─ Core
│  ├─ Runtime
│  ├─ Session
│  ├─ Endpoint
│  ├─ Event
│  └─ Buffer
│
├─ Net
│  ├─ TCP
│  ├─ UDP
│  ├─ Timer
│  └─ Discovery
│
├─ Protocol
│  ├─ Safaia
│  └─ Future...
│
└─ Services
   ├─ Logger
   ├─ Command
   ├─ FileTransfer
   └─ Future...
```

---

## 8. Transport 与 Protocol 分离

网络传输层与协议层不得强耦合。

例如：

```text
TCP
UDP
    ↓
Transport
    ↓
Protocol
    ↓
Service
```

Safaia 只是：

```text
UDP Discovery
+
TCP Transport
+
Safaia Protocol
```

未来可以自由组合：

```text
TCP + Safaia
TCP + RuntimeLink Native Protocol
WebSocket + RuntimeLink Protocol
```

---

## 9. Safaia 第一阶段实现

Safaia 作为第一个协议适配器。

需要支持：

### UDP Discovery

扫描：

```text
26613 ~ 26622
```

向目标设备发送：

```json
{
    "ip": "HostIP",
    "port": 35000
}
```

Minecraft / Safaia Client 收到后主动建立 TCP 回连。

### TCP 协议

第一阶段仅实现必要协议：

```text
2   Heartbeat
3   Config / Handshake
4   Message / Log
32  Leave
48  Connect Success
```

第一阶段目标：

```text
PC / Android Host
       ↓
发送 UDP Discovery
       ↓
Minecraft 回连 TCP
       ↓
完成 Safaia 握手
       ↓
接收 Protocol 4
       ↓
实时日志
```

暂不实现：

* Python 远程执行
* 文件系统
* UI Debugger
* FTP
* 高危远程控制功能

---

## 10. 日志抽象

不要直接把协议消息暴露成：

```cpp
std::function<void(std::string)>
```

统一转换成结构化事件：

```cpp
struct LogEvent
{
    LogLevel level;
    std::string_view message;
    std::string_view source;
    std::chrono::system_clock::time_point time;
};
```

协议缺失的字段允许为空。

上层只依赖 RuntimeLink 的 `LogEvent`，不依赖 Safaia。

---

## 11. 依赖策略

第三方依赖尽可能少。

第一阶段：

```text
C++20
Standalone Asio
```

尽量避免：

```text
Boost
Qt
libcurl
OpenSSL
libuv
大型 RPC Framework
```

Asio 内部也避免直接：

```cpp
#include <asio.hpp>
```

优先按需 include：

```cpp
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/use_awaitable.hpp>
```

---

## 12. 第一阶段目录

```text
RuntimeLink/
├─ CMakeLists.txt
├─ include/
│  └─ RuntimeLink/
│     ├─ Runtime.hpp
│     ├─ Endpoint.hpp
│     ├─ Session.hpp
│     ├─ Event.hpp
│     └─ Protocol/
│        └─ Safaia.hpp
│
├─ src/
│  ├─ Runtime.cpp
│  │
│  ├─ Net/
│  │  ├─ TcpServer.cpp
│  │  ├─ TcpSession.cpp
│  │  ├─ UdpSocket.cpp
│  │  └─ Timer.cpp
│  │
│  └─ Protocol/
│     └─ Safaia/
│        ├─ SafaiaService.cpp
│        ├─ SafaiaSession.cpp
│        └─ SafaiaDiscovery.cpp
│
└─ third_party/
   └─ asio/
```

---

## 13. 第一阶段开发顺序

```text
1. CMake + C++20 + Standalone Asio

2. Runtime
   └─ io_context + poll/poll_one

3. TCP Server / Session

4. UDP Socket

5. C++20 Coroutine 基础封装

6. Safaia UDP Discovery

7. Safaia TCP Handshake

8. Protocol 4 日志接收

9. LogEvent 抽象

10. Windows 测试

11. Android NDK 测试

12. 完善异常、断线、重连和生命周期
```

---

## 14. 设计目标

RuntimeLink 最终应满足：

```text
轻量
跨平台
无内部线程
无阻塞网络操作
宿主驱动事件循环
C++20 Coroutine
协议与网络解耦
Asio 不泄漏到公共 API
低依赖
可嵌入游戏 / APK / EXE / DLL
```

核心设计理念：

> RuntimeLink 不拥有线程，只拥有事件循环。
> 宿主决定 RuntimeLink 在哪个线程、什么时候运行。
