#include <MCDevLink/Protocol/Safaia.hpp>
#include <MCDevLink/Runtime.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

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

} // namespace

int main(int argc, char** argv) {
    const std::string advertiseAddress = argc > 1 ? argv[1] : "127.0.0.1";
    const std::string discoveryTarget = argc > 2 ? argv[2] : advertiseAddress;

    MCDevLink::Runtime runtime;
    MCDevLink::Protocol::SafaiaOptions options;
    options.advertiseAddress = advertiseAddress;
    options.discoveryTargets = {discoveryTarget};

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
        std::cerr << "Failed to start Safaia receiver: " << error.message() << '\n';
        return 1;
    }

    const auto endpoint = service.localEndpoint();
    std::cout << "Safaia receiver listening on " << endpoint.address << ':' << endpoint.port
              << ", advertising " << advertiseAddress << " to " << discoveryTarget
              << ":26613..26622\n";

    std::signal(SIGINT, stop);
    while (running.load()) {
        (void)runtime.poll({256, std::chrono::microseconds{1000}});
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    service.stop();
}
