# Safaia 真机日志测试

该测试需要人工启动 Minecraft，不属于 CTest。自动化阶段不得启动此程序或 Minecraft 真机实例。

## 前置条件

- 使用与宿主工程相同的 MSVC 运行库配置完成构建。
- Windows 防火墙允许测试程序接收 TCP 回连并发送 UDP discovery。
- 本机测试使用 `127.0.0.1`；跨设备测试确认两端位于可互通的可信开发网络。

## 启动

本机：

```powershell
.\build\x64-msvc-debug\examples\mcdevlink_safaia_log_receiver.exe 127.0.0.1 127.0.0.1
```

跨设备：

```powershell
.\build\x64-msvc-debug\examples\mcdevlink_safaia_log_receiver.exe <宿主局域网IPv4> <游戏设备IPv4>
```

程序应先输出随机 TCP 监听端口和 discovery 目标 `26613..26622`。保持程序运行，再启动 Minecraft 并进入能产生脚本日志的环境。

## 验收项

1. 冷启动发现：Minecraft 启动后，接收端出现 `connected`，随后出现 `ready`。
2. 握手兼容：进入世界后连接不因第二个不含 `connect_port` 的 config 帧中断。
3. 日志接收：游戏产生 Python 日志后，接收端持续输出 `[log session=...]`，正文与游戏日志一致。
4. 断线恢复：关闭当前游戏连接后出现 `disconnected`；重新进入后同一接收端能再次完成 `connected -> ready` 并继续收到日志。
5. 宿主响应：持续日志期间游戏主循环无明显卡顿；接收端仍能响应 `Ctrl+C`。

全部满足才视为 Windows 真机链路通过。请记录 Minecraft/Safaia 版本、是否本机或跨设备、关键输出和失败时的 `[diagnostic]` 内容。

## 已知边界

- 协议 4 不携带可靠日志等级，当前 `LogEvent::level` 为 `unknown`。
- 当前 discovery 按配置的 IPv4 和端口范围发送，不提供参考项目中的 Windows PID 多实例过滤。
- 当前协议没有认证或加密，不应在不可信网络测试，更不能据此开放未来的代码执行能力。
- Android NDK 构建与 Android 真机网络行为需要单独验证，不能由 Windows 结果推断通过。
