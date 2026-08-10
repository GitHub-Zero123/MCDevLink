#pragma once

#include "MCDevLink/Endpoint.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace MCDevLink {

using SessionId = std::uint64_t;

enum class LogLevel {
    unknown,
    trace,
    debug,
    info,
    warning,
    error,
    critical,
};

struct LogEvent {
    SessionId sessionId = 0;
    LogLevel level = LogLevel::unknown;
    std::string message;
    std::string source;
    std::chrono::system_clock::time_point time{};
};

enum class SessionState {
    connected,
    ready,
    disconnected,
};

struct SessionEvent {
    SessionId sessionId = 0;
    SessionState state = SessionState::disconnected;
    Endpoint remote;
    std::string clientName;
    std::string connectId;
};

enum class DiagnosticLevel {
    info,
    warning,
    error,
};

struct DiagnosticEvent {
    DiagnosticLevel level = DiagnosticLevel::info;
    std::string message;
};

struct ProtocolFrameEvent {
    SessionId sessionId = 0;
    std::int32_t protocolId = 0;
    std::vector<std::uint8_t> payload;
};

} // namespace MCDevLink
