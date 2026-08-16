#include "MCDevLink/Protocol/Safaia.hpp"

#include "Detail/ProtocolFrame.hpp"
#include "Detail/RuntimeContext.hpp"
#include "MCDevLink/Runtime.hpp"

#include <utility>

#include <asio/async_result.hpp>
#include <asio/awaitable.hpp>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/error.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/read.hpp>
#include <asio/redirect_error.hpp>
#include <asio/socket_base.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MCDevLink::Protocol {

namespace {

using Tcp = asio::ip::tcp;
using Udp = asio::ip::udp;

std::optional<int> parsePort(const nlohmann::json& value) {
    if (value.is_number_integer()) {
        const auto port = value.get<std::int64_t>();
        if (port > 0 && port <= std::numeric_limits<std::uint16_t>::max()) {
            return static_cast<int>(port);
        }
        return std::nullopt;
    }
    if (!value.is_string()) {
        return std::nullopt;
    }

    const auto& text = value.get_ref<const std::string&>();
    int port = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), port);
    if (error != std::errc{} || end != text.data() + text.size() || port <= 0
        || port > std::numeric_limits<std::uint16_t>::max()) {
        return std::nullopt;
    }
    return port;
}

std::string jsonText(const nlohmann::json& value) {
    return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::span<const std::uint8_t> asBytes(const std::string_view value) {
    return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

void cancelTimer(asio::steady_timer& timer) noexcept {
    try {
        (void)timer.cancel();
    } catch (...) {
    }
}

std::string errorMessageUtf8(const asio::error_code& error) {
#if defined(_WIN32)
    const auto localMessage = error.message();
    if (!localMessage.empty()) {
        const auto wideSize = MultiByteToWideChar(
            CP_ACP, 0, localMessage.data(), static_cast<int>(localMessage.size()), nullptr, 0);
        if (wideSize > 0) {
            std::wstring wideMessage(static_cast<std::size_t>(wideSize), L'\0');
            if (MultiByteToWideChar(
                    CP_ACP, 0, localMessage.data(), static_cast<int>(localMessage.size()),
                    wideMessage.data(), wideSize) == wideSize) {
                const auto utf8Size = WideCharToMultiByte(
                    CP_UTF8, 0, wideMessage.data(), wideSize, nullptr, 0, nullptr, nullptr);
                if (utf8Size > 0) {
                    std::string utf8Message(static_cast<std::size_t>(utf8Size), '\0');
                    if (WideCharToMultiByte(
                            CP_UTF8, 0, wideMessage.data(), wideSize, utf8Message.data(), utf8Size,
                            nullptr, nullptr) == utf8Size) {
                        return utf8Message;
                    }
                }
            }
        }
    }
    return "Windows error " + std::to_string(error.value());
#else
    return error.message();
#endif
}

void appendUniqueAddress(std::vector<std::string>& addresses, const char* address) {
    if (address == nullptr || address[0] == '\0') {
        return;
    }
    const std::string_view value{address};
    if (value == "0.0.0.0") {
        return;
    }
    if (std::find(addresses.begin(), addresses.end(), address) == addresses.end()) {
        addresses.emplace_back(address);
    }
}

std::vector<std::string> enumerateLocalIPv4() {
    std::vector<std::string> addresses{"127.0.0.1"};

#if defined(_WIN32)
    ULONG bufferSize = 15U * 1024U;
    std::vector<unsigned char> buffer(bufferSize);
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        const auto result = GetAdaptersAddresses(
            AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, adapters, &bufferSize);
        if (result == ERROR_BUFFER_OVERFLOW) {
            buffer.resize(bufferSize);
            continue;
        }
        if (result != NO_ERROR) {
            return addresses;
        }

        for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp) {
                continue;
            }
            for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
                 unicast = unicast->Next) {
                if (unicast->Address.lpSockaddr == nullptr
                    || unicast->Address.lpSockaddr->sa_family != AF_INET
                    || unicast->DadState != IpDadStatePreferred) {
                    continue;
                }
                const auto* endpoint =
                    reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
                char text[INET_ADDRSTRLEN]{};
                appendUniqueAddress(
                    addresses, inet_ntop(AF_INET, &endpoint->sin_addr, text, sizeof(text)));
            }
        }
        return addresses;
    }
#else
    ifaddrs* rawInterfaces = nullptr;
    if (getifaddrs(&rawInterfaces) != 0) {
        return addresses;
    }
    const std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> interfaces(rawInterfaces, freeifaddrs);
    for (auto* interface = interfaces.get(); interface != nullptr; interface = interface->ifa_next) {
        if (interface->ifa_addr == nullptr || interface->ifa_addr->sa_family != AF_INET
            || (interface->ifa_flags & IFF_UP) == 0) {
            continue;
        }
        const auto* endpoint = reinterpret_cast<const sockaddr_in*>(interface->ifa_addr);
        char text[INET_ADDRSTRLEN]{};
        appendUniqueAddress(
            addresses, inet_ntop(AF_INET, &endpoint->sin_addr, text, sizeof(text)));
    }
#endif

    return addresses;
}

std::string joinAddresses(const std::vector<std::string>& addresses) {
    std::string result;
    for (const auto& address : addresses) {
        if (!result.empty()) {
            result += ", ";
        }
        result += address;
    }
    return result;
}

std::error_code findUdpPortsForProcess(
    const std::uint32_t processId, const std::uint16_t firstPort,
    const std::uint16_t portCount, std::vector<std::uint16_t>& ports) {
    ports.clear();
#if defined(_WIN32)
    ULONG bufferSize = 0;
    auto result = GetExtendedUdpTable(
        nullptr, &bufferSize, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    if (result != ERROR_INSUFFICIENT_BUFFER && result != NO_ERROR) {
        return {static_cast<int>(result), std::system_category()};
    }
    if (bufferSize == 0) {
        return {};
    }

    std::vector<std::uint64_t> buffer;
    for (int attempt = 0; attempt < 3; ++attempt) {
        buffer.resize(
            (static_cast<std::size_t>(bufferSize) + sizeof(std::uint64_t) - 1U)
            / sizeof(std::uint64_t));
        result = GetExtendedUdpTable(
            buffer.data(), &bufferSize, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
        if (result == ERROR_INSUFFICIENT_BUFFER) {
            continue;
        }
        if (result != NO_ERROR) {
            return {static_cast<int>(result), std::system_category()};
        }

        const auto* table = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(buffer.data());
        const auto portLimit = static_cast<std::uint32_t>(firstPort) + portCount;
        for (DWORD index = 0; index < table->dwNumEntries; ++index) {
            const auto& row = table->table[index];
            if (row.dwOwningPid != processId) {
                continue;
            }
            const auto port = ntohs(static_cast<u_short>(row.dwLocalPort));
            if (port >= firstPort && static_cast<std::uint32_t>(port) < portLimit) {
                ports.push_back(port);
            }
        }
        std::sort(ports.begin(), ports.end());
        ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
        return {};
    }
    return {static_cast<int>(ERROR_INSUFFICIENT_BUFFER), std::system_category()};
#else
    (void)processId;
    (void)firstPort;
    (void)portCount;
    return std::make_error_code(std::errc::operation_not_supported);
#endif
}

} // namespace

class SafaiaService::Impl : public std::enable_shared_from_this<SafaiaService::Impl> {
public:
    class Session;

    Impl(std::shared_ptr<Detail::RuntimeContext> runtimeContext, SafaiaOptions serviceOptions)
        : context(std::move(runtimeContext)),
          options(std::move(serviceOptions)),
          acceptor(context->io),
          discoverySocket(context->io),
          discoveryTimer(context->io) {}

    std::error_code start() {
        if (running) {
            return {};
        }
        if (startedOnce) {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
#if !defined(_WIN32)
        if (options.targetProcessId != 0) {
            return std::make_error_code(std::errc::operation_not_supported);
        }
#endif
        if (!context || !validateOptions()) {
            return std::make_error_code(std::errc::invalid_argument);
        }

        asio::error_code error;
        const auto bindAddress = asio::ip::make_address_v4(options.bindEndpoint.address, error);
        if (error) {
            return error;
        }

        acceptor.open(Tcp::v4(), error);
        if (error) {
            return error;
        }
        acceptor.set_option(Tcp::acceptor::reuse_address(true), error);
        if (error) {
            closeAcceptor();
            return error;
        }
        acceptor.bind(Tcp::endpoint(bindAddress, options.bindEndpoint.port), error);
        if (error) {
            closeAcceptor();
            return error;
        }
        acceptor.listen(asio::socket_base::max_listen_connections, error);
        if (error) {
            closeAcceptor();
            return error;
        }

        const auto bound = acceptor.local_endpoint(error);
        if (error) {
            closeAcceptor();
            return error;
        }
        local = {bound.address().to_string(), bound.port()};

        if (options.discoveryEnabled) {
            if (!prepareDiscovery(error)) {
                closeAcceptor();
                return error;
            }
        }

        running = true;
        startedOnce = true;
        auto self = shared_from_this();
        asio::co_spawn(context->io, [self]() { return self->acceptLoop(); }, asio::detached);
        if (options.discoveryEnabled) {
            asio::co_spawn(context->io, [self]() { return self->discoveryLoop(); }, asio::detached);
        }
        return {};
    }

    void stop();

    bool validateOptions() {
        if (options.maxFramePayload > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())
            || options.maxPendingWriteBytes < Detail::protocolFrameHeaderSize
            || options.handshakeTimeout <= std::chrono::milliseconds::zero()
            || options.discoveryInterval <= std::chrono::milliseconds::zero()) {
            return false;
        }
        if (options.discoveryEnabled || options.targetProcessId != 0) {
            if (options.discoveryPortCount == 0) {
                return false;
            }
            const auto lastPort = static_cast<std::uint32_t>(options.discoveryPortFirst)
                + static_cast<std::uint32_t>(options.discoveryPortCount) - 1U;
            if (lastPort > std::numeric_limits<std::uint16_t>::max()) {
                return false;
            }
        }
        return true;
    }

    bool prepareDiscovery(asio::error_code& error) {
        discoveryEndpoints.clear();
        (void)asio::ip::make_address_v4(options.advertiseAddress, error);
        if (error) {
            return false;
        }
        const auto targets = options.discoveryTargets.empty()
            ? enumerateLocalIPv4()
            : options.discoveryTargets;
        std::vector<std::string> uniqueTargets;
        for (const auto& target : targets) {
            const auto address = asio::ip::make_address_v4(target, error);
            if (error) {
                return false;
            }
            if (std::find(uniqueTargets.begin(), uniqueTargets.end(), target) != uniqueTargets.end()) {
                continue;
            }
            uniqueTargets.push_back(target);
            for (std::uint32_t offset = 0; offset < options.discoveryPortCount; ++offset) {
                discoveryEndpoints.emplace_back(
                    address, static_cast<std::uint16_t>(options.discoveryPortFirst + offset));
            }
        }

        discoverySocket.open(Udp::v4(), error);
        if (error) {
            return false;
        }
        discoverySocket.set_option(asio::socket_base::broadcast(true), error);
        if (error) {
            asio::error_code ignored;
            discoverySocket.close(ignored);
            return false;
        }

        discoveryPayload = jsonText({
            {"ip", options.advertiseAddress},
            {"port", local.port},
        });
        emitDiagnostic(
            DiagnosticLevel::info,
            "Safaia discovery targets: " + joinAddresses(uniqueTargets)
                + (options.targetProcessId == 0
                       ? std::string{}
                       : ", process ID " + std::to_string(options.targetProcessId)));
        return true;
    }

    asio::awaitable<void> acceptLoop() {
        while (running) {
            asio::error_code error;
            Tcp::socket socket(context->io);
            co_await acceptor.async_accept(socket, asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                if (running && error != asio::error::operation_aborted) {
                    emitDiagnostic(
                        DiagnosticLevel::error,
                        "Safaia TCP accept failed: " + errorMessageUtf8(error));
                    stop();
                }
                co_return;
            }
            addSession(std::move(socket));
        }
    }

    asio::awaitable<void> discoveryLoop() {
        while (running) {
            if (!discoveryPaused) {
                std::vector<std::uint16_t> processPorts;
                std::error_code processPortError;
                if (options.targetProcessId != 0) {
                    processPortError = findUdpPortsForProcess(
                        options.targetProcessId, options.discoveryPortFirst,
                        options.discoveryPortCount, processPorts);
                    if (processPortError && !reportedProcessPortLookupFailure) {
                        reportedProcessPortLookupFailure = true;
                        emitDiagnostic(
                            DiagnosticLevel::warning,
                            "Safaia could not query UDP ports for process "
                                + std::to_string(options.targetProcessId) + ": "
                                + errorMessageUtf8(processPortError));
                    } else if (!processPortError) {
                        reportedProcessPortLookupFailure = false;
                    }
                }
                for (const auto& endpoint : discoveryEndpoints) {
                    if (!running || discoveryPaused) {
                        break;
                    }
                    if (processPortError
                        || (options.targetProcessId != 0
                            && !std::binary_search(
                                processPorts.begin(), processPorts.end(), endpoint.port()))) {
                        continue;
                    }
                    asio::error_code error;
                    co_await discoverySocket.async_send_to(
                        asio::buffer(discoveryPayload), endpoint,
                        asio::redirect_error(asio::use_awaitable, error));
                    if (error && error != asio::error::operation_aborted && running) {
                        const auto address = endpoint.address().to_string();
                        if (reportedDiscoveryFailureAddresses.insert(address).second) {
                            emitDiagnostic(
                                DiagnosticLevel::warning,
                                "Safaia discovery send failed for " + address + ":"
                                    + std::to_string(endpoint.port()) + ": "
                                    + errorMessageUtf8(error));
                        }
                    }
                }
            }

            if (!running) {
                co_return;
            }

            discoveryTimer.expires_after(options.discoveryInterval);
            asio::error_code error;
            co_await discoveryTimer.async_wait(asio::redirect_error(asio::use_awaitable, error));
            if (!running) {
                co_return;
            }
        }
    }

    void addSession(Tcp::socket socket);
    void sessionReady(const std::shared_ptr<Session>& session);
    void sessionFinished(SessionId id, bool wasReady, const Endpoint& remote,
                         const std::string& clientName, const std::string& connectId);

    bool send(const SessionId id, const std::int32_t protocolId,
              const std::span<const std::uint8_t> payload);

    void emitLog(const LogEvent& event) {
        if (!logHandler) {
            return;
        }
        try {
            logHandler(event);
        } catch (...) {
            emitDiagnostic(DiagnosticLevel::error, "Safaia log handler threw an exception");
        }
    }

    void emitSession(const SessionEvent& event) {
        if (!sessionHandler) {
            return;
        }
        try {
            sessionHandler(event);
        } catch (...) {
            emitDiagnostic(DiagnosticLevel::error, "Safaia session handler threw an exception");
        }
    }

    void emitFrame(const SessionId id, const Detail::ProtocolFrame& frame) {
        if (!frameHandler) {
            return;
        }
        try {
            frameHandler(ProtocolFrameEvent{id, frame.protocolId, frame.payload});
        } catch (...) {
            emitDiagnostic(DiagnosticLevel::error, "Safaia frame handler threw an exception");
        }
    }

    void emitDiagnostic(const DiagnosticLevel level, std::string message) noexcept {
        if (!diagnosticHandler) {
            return;
        }
        try {
            diagnosticHandler(DiagnosticEvent{level, std::move(message)});
        } catch (...) {
        }
    }

    void closeAcceptor() noexcept {
        asio::error_code ignored;
        acceptor.cancel(ignored);
        acceptor.close(ignored);
    }

    std::shared_ptr<Detail::RuntimeContext> context;
    SafaiaOptions options;
    Tcp::acceptor acceptor;
    Udp::socket discoverySocket;
    asio::steady_timer discoveryTimer;
    std::vector<Udp::endpoint> discoveryEndpoints;
    std::unordered_set<std::string> reportedDiscoveryFailureAddresses;
    bool reportedProcessPortLookupFailure = false;
    std::string discoveryPayload;
    Endpoint local;
    bool running = false;
    bool startedOnce = false;
    bool discoveryPaused = false;
    std::size_t readySessionCount = 0;
    SessionId nextSessionId = 1;
    std::unordered_map<SessionId, std::shared_ptr<Session>> sessions;
    LogHandler logHandler;
    SessionHandler sessionHandler;
    FrameHandler frameHandler;
    DiagnosticHandler diagnosticHandler;
};

class SafaiaService::Impl::Session : public std::enable_shared_from_this<SafaiaService::Impl::Session> {
public:
    Session(std::weak_ptr<Impl> service, Tcp::socket acceptedSocket, const SessionId sessionId)
        : owner(std::move(service)),
          socket(std::move(acceptedSocket)),
          handshakeTimer(socket.get_executor()),
          idleTimer(socket.get_executor()),
          id(sessionId) {
        asio::error_code error;
        const auto endpoint = socket.remote_endpoint(error);
        if (!error) {
            remote = {endpoint.address().to_string(), endpoint.port()};
        }
    }

    asio::awaitable<void> run() {
        armHandshakeTimeout();
        armIdleTimeout();

        while (!closed) {
            auto frame = co_await readFrame();
            if (!frame) {
                break;
            }
            handleFrame(*frame);
        }

        close();
        if (auto service = owner.lock()) {
            service->sessionFinished(id, ready, remote, clientName, connectId);
        }
    }

    bool enqueue(const std::int32_t protocolId, const std::span<const std::uint8_t> payload,
                 const bool requireReady = true) {
        const auto service = owner.lock();
        if (!service || closed || (requireReady && !ready)
            || payload.size() > service->options.maxFramePayload) {
            return false;
        }

        const auto frameBytes = Detail::protocolFrameHeaderSize + payload.size();
        if (frameBytes > service->options.maxPendingWriteBytes
            || pendingWriteBytes > service->options.maxPendingWriteBytes - frameBytes) {
            service->emitDiagnostic(
                DiagnosticLevel::warning,
                "Safaia session " + std::to_string(id) + " write queue limit reached");
            return false;
        }

        writeQueue.push_back(Detail::encodeProtocolFrame(protocolId, payload));
        pendingWriteBytes += frameBytes;
        if (!writing) {
            writing = true;
            auto self = shared_from_this();
            asio::co_spawn(socket.get_executor(), [self]() { return self->writeLoop(); }, asio::detached);
        }
        return true;
    }

    void close() noexcept {
        if (closed) {
            return;
        }
        closed = true;
        asio::error_code ignored;
        cancelTimer(handshakeTimer);
        cancelTimer(idleTimer);
        socket.cancel(ignored);
        socket.shutdown(Tcp::socket::shutdown_both, ignored);
        socket.close(ignored);
    }

    asio::awaitable<std::optional<Detail::ProtocolFrame>> readFrame() {
        std::array<std::uint8_t, Detail::protocolFrameHeaderSize> header{};
        asio::error_code error;
        co_await asio::async_read(
            socket, asio::buffer(header), asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            co_return std::nullopt;
        }

        const auto rawSize = Detail::readInt32LE(header.data() + 4);
        const auto service = owner.lock();
        if (!service) {
            co_return std::nullopt;
        }
        if (rawSize < 0 || static_cast<std::size_t>(rawSize) > service->options.maxFramePayload) {
            service->emitDiagnostic(
                DiagnosticLevel::warning,
                "Safaia session " + std::to_string(id) + " sent an invalid frame length");
            co_return std::nullopt;
        }

        Detail::ProtocolFrame frame;
        frame.protocolId = Detail::readInt32LE(header.data());
        frame.payload.resize(static_cast<std::size_t>(rawSize));
        if (!frame.payload.empty()) {
            co_await asio::async_read(
                socket, asio::buffer(frame.payload), asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                co_return std::nullopt;
            }
        }
        armIdleTimeout();
        co_return frame;
    }

    void handleFrame(const Detail::ProtocolFrame& frame) {
        auto service = owner.lock();
        if (!service) {
            close();
            return;
        }
        if (rejected) {
            return;
        }

        if (frame.protocolId == SafaiaMessage::config) {
            const bool wasReady = ready;
            handleConfig(frame.payload);
            if (wasReady) {
                service->emitFrame(id, frame);
            }
            return;
        }
        if (!ready) {
            return;
        }

        switch (frame.protocolId) {
            case SafaiaMessage::heartbeat:
                return;
            case SafaiaMessage::message: {
                LogEvent event;
                event.sessionId = id;
                event.level = LogLevel::unknown;
                event.message.assign(
                    reinterpret_cast<const char*>(frame.payload.data()), frame.payload.size());
                event.source = clientName.empty() ? remote.address : clientName;
                event.time = std::chrono::system_clock::now();
                service->emitLog(event);
                service->emitFrame(id, frame);
                return;
            }
            case SafaiaMessage::leave:
                service->emitFrame(id, frame);
                close();
                return;
            default:
                service->emitFrame(id, frame);
                return;
        }
    }

    void handleConfig(const std::vector<std::uint8_t>& payload) {
        const auto document = nlohmann::json::parse(payload.begin(), payload.end(), nullptr, false);
        if (!document.is_object() || !document.contains("connect_port")) {
            // Safaia sends another full config without connect_port after the handshake.
            return;
        }
        if (ready) {
            return;
        }

        const auto port = parsePort(document["connect_port"]);
        if (!port) {
            return;
        }

        if (auto service = owner.lock(); service && service->options.targetProcessId != 0) {
            std::vector<std::uint16_t> processPorts;
            const auto error = findUdpPortsForProcess(
                service->options.targetProcessId, service->options.discoveryPortFirst,
                service->options.discoveryPortCount, processPorts);
            const auto owned = !error
                && std::binary_search(
                    processPorts.begin(), processPorts.end(), static_cast<std::uint16_t>(*port));
            if (!owned) {
                std::string message = "Safaia rejected session " + std::to_string(id)
                    + ": connect_port " + std::to_string(*port)
                    + " is not owned by process "
                    + std::to_string(service->options.targetProcessId);
                if (error) {
                    message += " (UDP port query failed: " + errorMessageUtf8(error) + ')';
                }
                service->emitDiagnostic(DiagnosticLevel::warning, std::move(message));

                rejected = true;
                closeAfterWrite = true;
                cancelTimer(handshakeTimer);
                cancelTimer(idleTimer);
                const auto response = jsonText({{"notify", "notify_block"}});
                if (!enqueue(SafaiaMessage::connectBlocked, asBytes(response), false)) {
                    close();
                }
                return;
            }
        }

        if (document.contains("name") && document["name"].is_string()) {
            clientName = document["name"].get<std::string>();
        }
        if (document.contains("connect_id") && document["connect_id"].is_string()) {
            connectId = document["connect_id"].get<std::string>();
        }

        const auto response = jsonText({{"notify", "pass"}});
        if (!enqueue(SafaiaMessage::connectSuccess, asBytes(response), false)) {
            close();
            return;
        }
        ready = true;
        cancelTimer(handshakeTimer);
        if (auto service = owner.lock()) {
            service->sessionReady(shared_from_this());
        }
    }

    void armHandshakeTimeout() {
        const auto service = owner.lock();
        if (!service) {
            return;
        }
        handshakeTimer.expires_after(service->options.handshakeTimeout);
        auto self = weak_from_this();
        handshakeTimer.async_wait([self](const asio::error_code& error) {
            if (error) {
                return;
            }
            if (const auto session = self.lock(); session && !session->ready) {
                if (const auto service = session->owner.lock()) {
                    service->emitDiagnostic(
                        DiagnosticLevel::warning,
                        "Safaia session " + std::to_string(session->id) + " handshake timed out");
                }
                session->close();
            }
        });
    }

    void armIdleTimeout() {
        const auto service = owner.lock();
        if (!service || service->options.idleTimeout <= std::chrono::milliseconds::zero()) {
            return;
        }
        idleTimer.expires_after(service->options.idleTimeout);
        auto self = weak_from_this();
        idleTimer.async_wait([self](const asio::error_code& error) {
            if (error) {
                return;
            }
            if (const auto session = self.lock()) {
                if (const auto service = session->owner.lock()) {
                    service->emitDiagnostic(
                        DiagnosticLevel::warning,
                        "Safaia session " + std::to_string(session->id) + " became idle");
                }
                session->close();
            }
        });
    }

    asio::awaitable<void> writeLoop() {
        while (!writeQueue.empty() && !closed) {
            asio::error_code error;
            co_await asio::async_write(
                socket, asio::buffer(writeQueue.front()),
                asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                close();
                break;
            }
            pendingWriteBytes -= writeQueue.front().size();
            writeQueue.pop_front();
        }
        if (closeAfterWrite && writeQueue.empty() && !closed) {
            close();
        }
        if (closed) {
            writeQueue.clear();
            pendingWriteBytes = 0;
        }
        writing = false;
    }

    std::weak_ptr<Impl> owner;
    Tcp::socket socket;
    asio::steady_timer handshakeTimer;
    asio::steady_timer idleTimer;
    SessionId id;
    Endpoint remote;
    std::string clientName;
    std::string connectId;
    std::deque<std::vector<std::uint8_t>> writeQueue;
    std::size_t pendingWriteBytes = 0;
    bool writing = false;
    bool ready = false;
    bool rejected = false;
    bool closeAfterWrite = false;
    bool closed = false;
};

void SafaiaService::Impl::stop() {
    if (!running && sessions.empty()) {
        return;
    }
    running = false;
    discoveryPaused = false;
    readySessionCount = 0;

    closeAcceptor();
    asio::error_code ignored;
    cancelTimer(discoveryTimer);
    discoverySocket.cancel(ignored);
    discoverySocket.close(ignored);

    auto activeSessions = std::move(sessions);
    sessions.clear();
    for (const auto& [id, session] : activeSessions) {
        (void)id;
        session->close();
    }
}

void SafaiaService::Impl::addSession(Tcp::socket socket) {
    const auto id = nextSessionId++;
    auto session = std::make_shared<Session>(weak_from_this(), std::move(socket), id);
    sessions.emplace(id, session);
    emitSession(SessionEvent{id, SessionState::connected, session->remote, {}, {}});
    asio::co_spawn(context->io, [session]() { return session->run(); }, asio::detached);
}

void SafaiaService::Impl::sessionReady(const std::shared_ptr<Session>& session) {
    ++readySessionCount;
    if (options.pauseDiscoveryWhileConnected) {
        discoveryPaused = true;
        cancelTimer(discoveryTimer);
    }
    emitSession(SessionEvent{
        session->id,
        SessionState::ready,
        session->remote,
        session->clientName,
        session->connectId,
    });
}

void SafaiaService::Impl::sessionFinished(
    const SessionId id, const bool wasReady, const Endpoint& remote, const std::string& clientName,
    const std::string& connectId) {
    if (sessions.erase(id) == 0) {
        return;
    }
    if (wasReady && readySessionCount != 0) {
        --readySessionCount;
    }
    if (running && options.pauseDiscoveryWhileConnected && readySessionCount == 0) {
        discoveryPaused = false;
        cancelTimer(discoveryTimer);
    }
    emitSession(SessionEvent{id, SessionState::disconnected, remote, clientName, connectId});
}

bool SafaiaService::Impl::send(
    const SessionId id, const std::int32_t protocolId,
    const std::span<const std::uint8_t> payload) {
    const auto found = sessions.find(id);
    return found != sessions.end() && found->second->enqueue(protocolId, payload);
}

SafaiaService::SafaiaService(Runtime& runtime, SafaiaOptions options)
    : impl_(std::make_shared<Impl>(runtime.context(), std::move(options))) {}

SafaiaService::~SafaiaService() {
    if (impl_) {
        impl_->stop();
    }
}

SafaiaService::SafaiaService(SafaiaService&&) noexcept = default;
SafaiaService& SafaiaService::operator=(SafaiaService&& other) noexcept {
    if (this != &other) {
        if (impl_) {
            impl_->stop();
        }
        impl_ = std::move(other.impl_);
    }
    return *this;
}

std::error_code SafaiaService::start() {
    return impl_ ? impl_->start() : std::make_error_code(std::errc::operation_not_permitted);
}

void SafaiaService::stop() {
    if (impl_) {
        impl_->stop();
    }
}

bool SafaiaService::isRunning() const noexcept {
    return impl_ && impl_->running;
}

Endpoint SafaiaService::localEndpoint() const {
    return impl_ ? impl_->local : Endpoint{};
}

std::size_t SafaiaService::sessionCount() const noexcept {
    return impl_ ? impl_->sessions.size() : 0;
}

void SafaiaService::setLogHandler(LogHandler handler) {
    if (impl_) {
        impl_->logHandler = std::move(handler);
    }
}

void SafaiaService::setSessionHandler(SessionHandler handler) {
    if (impl_) {
        impl_->sessionHandler = std::move(handler);
    }
}

void SafaiaService::setFrameHandler(FrameHandler handler) {
    if (impl_) {
        impl_->frameHandler = std::move(handler);
    }
}

void SafaiaService::setDiagnosticHandler(DiagnosticHandler handler) {
    if (impl_) {
        impl_->diagnosticHandler = std::move(handler);
    }
}

bool SafaiaService::send(
    const SessionId sessionId, const std::int32_t protocolId,
    const std::span<const std::uint8_t> payload) {
    return impl_ && impl_->send(sessionId, protocolId, payload);
}

bool SafaiaService::send(
    const SessionId sessionId, const std::int32_t protocolId, const std::string_view payload) {
    return send(sessionId, protocolId, asBytes(payload));
}

} // namespace MCDevLink::Protocol
