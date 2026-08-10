#include <MCDevLink/Protocol/Safaia.hpp>
#include <MCDevLink/Runtime.hpp>

#include "Detail/AsioConfig.hpp"
#include "Detail/ProtocolFrame.hpp"

#include <utility>

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const char* expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

std::span<const std::uint8_t> bytes(const std::string& value) {
    return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

bool readExact(
    asio::ip::tcp::socket& socket, const std::span<std::uint8_t> destination,
    const std::chrono::steady_clock::time_point deadline) {
    std::size_t received = 0;
    while (received < destination.size() && std::chrono::steady_clock::now() < deadline) {
        asio::error_code error;
        const auto count = socket.read_some(
            asio::buffer(destination.data() + received, destination.size() - received), error);
        if (!error) {
            received += count;
            continue;
        }
        if (error == asio::error::would_block || error == asio::error::try_again) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
            continue;
        }
        return false;
    }
    return received == destination.size();
}

} // namespace

int main() {
    MCDevLink::Runtime runtime;
    MCDevLink::Protocol::SafaiaOptions options;
    options.bindEndpoint = {"127.0.0.1", 0};
    options.discoveryEnabled = false;
    options.handshakeTimeout = std::chrono::milliseconds{1000};
    options.idleTimeout = std::chrono::milliseconds{2000};

    MCDevLink::Protocol::SafaiaService service(runtime, options);
    bool logReceived = false;
    bool readyReceived = false;
    std::string logMessage;
    service.setLogHandler([&](const MCDevLink::LogEvent& event) {
        logReceived = true;
        logMessage = event.message;
        CHECK(event.level == MCDevLink::LogLevel::unknown);
        CHECK(event.source == "fake-mc");
    });
    service.setSessionHandler([&](const MCDevLink::SessionEvent& event) {
        if (event.state == MCDevLink::SessionState::ready) {
            readyReceived = true;
            CHECK(event.clientName == "fake-mc");
            CHECK(event.connectId == "loopback");
        }
    });

    CHECK(!service.start());
    const auto port = service.localEndpoint().port;
    CHECK(port != 0);

    std::atomic<bool> clientDone = false;
    std::atomic<bool> clientPassed = false;
    std::thread client([port, &clientDone, &clientPassed] {
        asio::io_context io;
        asio::ip::tcp::socket socket(io);
        asio::error_code error;
        socket.connect(
            {asio::ip::address_v4::loopback(), port}, error);
        if (error) {
            clientDone.store(true);
            return;
        }

        const std::string config =
            R"({"connect_id":"loopback","connect_port":26613,"name":"fake-mc"})";
        const auto configFrame = MCDevLink::Detail::encodeProtocolFrame(
            MCDevLink::Protocol::SafaiaMessage::config, bytes(config));
        asio::write(socket, asio::buffer(configFrame), error);
        if (error) {
            clientDone.store(true);
            return;
        }

        socket.non_blocking(true, error);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        std::array<std::uint8_t, MCDevLink::Detail::protocolFrameHeaderSize> header{};
        if (error || !readExact(socket, header, deadline)) {
            clientDone.store(true);
            return;
        }
        const auto responseId = MCDevLink::Detail::readInt32LE(header.data());
        const auto responseSize = MCDevLink::Detail::readInt32LE(header.data() + 4);
        if (responseId != MCDevLink::Protocol::SafaiaMessage::connectSuccess
            || responseSize <= 0 || responseSize > 1024) {
            clientDone.store(true);
            return;
        }

        std::vector<std::uint8_t> response(static_cast<std::size_t>(responseSize));
        if (!readExact(socket, response, deadline)
            || std::string(response.begin(), response.end()) != R"({"notify":"pass"})") {
            clientDone.store(true);
            return;
        }

        const std::string secondaryConfig = R"({"name":"fake-mc","platform":0})";
        const std::string log = "[Python] LOOPBACK_LOG\n";
        auto secondaryFrame = MCDevLink::Detail::encodeProtocolFrame(
            MCDevLink::Protocol::SafaiaMessage::config, bytes(secondaryConfig));
        auto logFrame = MCDevLink::Detail::encodeProtocolFrame(
            MCDevLink::Protocol::SafaiaMessage::message, bytes(log));
        secondaryFrame.insert(secondaryFrame.end(), logFrame.begin(), logFrame.end());
        socket.non_blocking(false, error);
        asio::write(socket, asio::buffer(secondaryFrame), error);
        clientPassed.store(!error);
        clientDone.store(true);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline
           && (!clientDone.load() || !logReceived)) {
        (void)runtime.poll({256, std::chrono::microseconds{1000}});
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    client.join();
    for (int attempt = 0; attempt < 10 && !logReceived; ++attempt) {
        (void)runtime.poll({256, std::chrono::microseconds{1000}});
    }

    CHECK(clientPassed.load());
    CHECK(readyReceived);
    CHECK(logReceived);
    CHECK(logMessage == "[Python] LOOPBACK_LOG\n");
    service.stop();

    return failures == 0 ? 0 : 1;
}
