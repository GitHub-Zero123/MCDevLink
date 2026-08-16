# Safaia 真机日志测试

该测试需要人工启动 Minecraft，不属于 CTest。自动化阶段不得启动此程序或 Minecraft 真机实例。

## 前置条件

- 使用与宿主工程相同的 MSVC 运行库配置完成构建。
- Windows 防火墙允许测试程序接收 TCP 回连并发送 UDP discovery。
- 本机测试使用 `127.0.0.1`；跨设备测试确认两端位于可互通的可信开发网络。

## 启动

本机：

```powershell
.\build\x64-msvc-debug\examples\mcdevlink_safaia_log_receiver.exe
```

跨设备：

```powershell
.\build\x64-msvc-debug\examples\mcdevlink_safaia_log_receiver.exe <宿主局域网IPv4> <游戏设备IPv4>
```

Windows 本机 PID 定向：

```powershell
.\build\x64-msvc-debug\examples\mcdevlink_safaia_log_receiver.exe 127.0.0.1 - <Minecraft PID>
```

程序在 Windows 启动时会保存原控制台代码页，将输入、输出切换为 UTF-8，并在正常退出时恢复；设置失败会报告 Win32 错误码并退出，避免把 socket 收到的 UTF-8 日志按系统代码页误显示或污染后续 CMake 配置。随后程序应输出 TCP 监听地址、随机监听端口和实际枚举到的 discovery 目标。程序会过滤未启用接口和非 Preferred 地址，再向其余本机 IPv4 的 `26613..26622` 单播；这包括游戏可能用于 Safaia UDP socket 的虚拟网卡地址。本机模式仍只监听 `127.0.0.1`；跨设备模式监听命令行传入的宿主局域网 IPv4，而不是默认暴露全部网卡。保持程序运行，再启动 Minecraft 并进入能产生脚本日志的环境。

示例程序仅在展示层按文本启发式着色，不改变核心 `LogEvent`：`[INFO][Developer]` 深灰、含 `SUC` 的行绿色、`ERROR`/`FATAL`/Python traceback 红色、`WARN` 黄色、`DEBUG` 青色，其余使用控制台默认色。程序会流式处理跨 payload 残行和多行 payload；协议 4 原始 payload 仍可通过核心帧回调获取。

接收循环使用非阻塞 `Runtime::poll()` 批量处理就绪 handler；未触及事件数或时间预算上限时休眠 33ms，避免空闲忙等。触及任一上限表示可能仍有事件积压，此时不休眠并继续分批处理。

## 验收项

1. 冷启动发现：Minecraft 启动后，接收端出现 `connected`，随后出现 `ready`。
2. 握手兼容：进入世界后连接不因第二个不含 `connect_port` 的 config 帧中断。
3. 日志接收：游戏产生 Python 日志后，接收端持续输出 `[log session=...]`，正文与游戏日志一致。
4. 断线恢复：关闭当前游戏连接后出现 `disconnected`；重新进入后同一接收端能再次完成 `connected -> ready` 并继续收到日志。
5. 宿主响应：持续日志期间游戏主循环无明显卡顿；接收端仍能响应 `Ctrl+C`。
6. PID 隔离：同时运行两个 Minecraft 实例时，PID 定向接收端只允许目标 PID 占用的 Safaia UDP 端口完成握手；另一实例的 `connect_port` 会收到 `connect_block` 并断开。

全部满足才视为 Windows 真机链路通过。请记录 Minecraft/Safaia 版本、是否本机或跨设备、关键输出和失败时的 `[diagnostic]` 内容。

## 已知边界

- 协议 4 不携带可靠日志等级，当前 `LogEvent::level` 为 `unknown`。
- discovery 目标为空时自动枚举本机 IPv4，非空时按配置地址发送；PID 定向仅支持 Windows 本机进程，不适用于跨设备目标。
- 当前协议没有认证或加密，不应在不可信网络测试，更不能据此开放未来的代码执行能力。
- 核心库默认绑定 `127.0.0.1`。只有调用方显式配置 `0.0.0.0` 才会监听全部 IPv4 网卡；跨设备测试优先绑定具体的宿主局域网 IPv4。
- Android NDK 构建与 Android 真机网络行为需要单独验证，不能由 Windows 结果推断通过。
