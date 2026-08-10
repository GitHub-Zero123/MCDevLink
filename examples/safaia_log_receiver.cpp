#include <MCDevLink/Protocol/Safaia.hpp>
#include <MCDevLink/Runtime.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

std::atomic<bool> running = true;

void stop(int) {
    running.store(false);
}

const char* stateName(const MCDevLink::SessionState state) {
    switch (state) {
        case MCDevLink::SessionState::connected: return "connected";
        case MCDevLink::SessionState::ready: return "ready";
        case MCDevLink::SessionState::disconnected: return "disconnected";
    }
    return "unknown";
}

#if defined(_WIN32)
class ConsoleUtf8Scope {
public:
    ConsoleUtf8Scope() = default;
    ~ConsoleUtf8Scope() {
        restore();
    }

    ConsoleUtf8Scope(const ConsoleUtf8Scope&) = delete;
    ConsoleUtf8Scope& operator=(const ConsoleUtf8Scope&) = delete;

    bool configure() {
        inputCodePage_ = GetConsoleCP();
        outputCodePage_ = GetConsoleOutputCP();

        DWORD inputError = ERROR_SUCCESS;
        if (isConsoleHandle(STD_INPUT_HANDLE) && inputCodePage_ != CP_UTF8) {
            if (SetConsoleCP(CP_UTF8)) {
                inputChanged_ = true;
            } else {
                inputError = GetLastError();
            }
        }

        DWORD outputError = ERROR_SUCCESS;
        if ((isConsoleHandle(STD_OUTPUT_HANDLE) || isConsoleHandle(STD_ERROR_HANDLE))
            && outputCodePage_ != CP_UTF8) {
            if (SetConsoleOutputCP(CP_UTF8)) {
                outputChanged_ = true;
            } else {
                outputError = GetLastError();
            }
        }

        if (inputError == ERROR_SUCCESS && outputError == ERROR_SUCCESS) {
            return true;
        }

        restore();
        std::cerr << "Failed to configure the Windows console for UTF-8"
                  << " (input error=" << inputError << ", output error=" << outputError << ")\n";
        return false;
    }

private:
    static bool isConsoleHandle(const DWORD standardHandle) {
        const auto handle = GetStdHandle(standardHandle);
        DWORD mode = 0;
        return handle != nullptr && handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode);
    }

    void restore() noexcept {
        if (inputChanged_ && GetConsoleCP() == CP_UTF8) {
            (void)SetConsoleCP(inputCodePage_);
        }
        if (outputChanged_ && GetConsoleOutputCP() == CP_UTF8) {
            (void)SetConsoleOutputCP(outputCodePage_);
        }
        inputChanged_ = false;
        outputChanged_ = false;
    }

    UINT inputCodePage_ = 0;
    UINT outputCodePage_ = 0;
    bool inputChanged_ = false;
    bool outputChanged_ = false;
};
#endif

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    ConsoleUtf8Scope consoleUtf8;
    if (!consoleUtf8.configure()) {
        return 1;
    }
#endif

    const std::string advertiseAddress = argc > 1 ? argv[1] : "127.0.0.1";
    const std::string discoveryTarget = argc > 2 ? argv[2] : "";

    MCDevLink::Runtime runtime;
    MCDevLink::Protocol::SafaiaOptions options;
    options.bindEndpoint.address = advertiseAddress;
    options.advertiseAddress = advertiseAddress;
    if (!discoveryTarget.empty()) {
        options.discoveryTargets = {discoveryTarget};
    }

    MCDevLink::Protocol::SafaiaService service(runtime, std::move(options));
    service.setLogHandler([](const MCDevLink::LogEvent& event) {
        std::cout << "[log session=" << event.sessionId << " source=" << event.source << "] "
                  << event.message;
        if (event.message.empty() || event.message.back() != '\n') {
            std::cout << '\n';
        }
        std::cout.flush();
    });
    service.setSessionHandler([](const MCDevLink::SessionEvent& event) {
        std::cout << "[session " << event.sessionId << "] " << stateName(event.state) << " "
                  << event.remote.address << ':' << event.remote.port;
        if (!event.clientName.empty()) {
            std::cout << " name=" << event.clientName;
        }
        std::cout << '\n';
    });
    service.setDiagnosticHandler([](const MCDevLink::DiagnosticEvent& event) {
        std::cerr << "[diagnostic] " << event.message << '\n';
    });

    if (const auto error = service.start()) {
        std::cerr << "Failed to start Safaia receiver: " << error.category().name() << ':'
                  << error.value() << '\n';
        return 1;
    }

    const auto endpoint = service.localEndpoint();
    std::cout << "Safaia receiver listening on " << endpoint.address << ':' << endpoint.port
              << ", advertising " << advertiseAddress << " to "
              << (discoveryTarget.empty() ? "auto-detected local IPv4 addresses" : discoveryTarget)
              << ":26613..26622\n";

    std::signal(SIGINT, stop);
    while (running.load()) {
        (void)runtime.poll({256, std::chrono::microseconds{1000}});
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    service.stop();
}
