#include <MCDevLink/Protocol/Safaia.hpp>
#include <MCDevLink/Runtime.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <iostream>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
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

enum class ConsoleColor {
    defaultColor,
    green,
    red,
    yellow,
    cyan,
    darkGray,
};

void printColoredLine(
    const std::string& text, const ConsoleColor color, const bool useErrorStream = false) {
    auto& stream = useErrorStream ? std::cerr : std::cout;

#if defined(_WIN32)
    const auto handle = GetStdHandle(useErrorStream ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO original{};
    const bool canColor = color != ConsoleColor::defaultColor && handle != nullptr
        && handle != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(handle, &original);
    if (canColor) {
        WORD attributes = original.wAttributes;
        constexpr WORD foregroundMask = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE
            | FOREGROUND_INTENSITY;
        attributes &= static_cast<WORD>(~foregroundMask);
        switch (color) {
            case ConsoleColor::green:
                attributes |= FOREGROUND_GREEN | FOREGROUND_INTENSITY;
                break;
            case ConsoleColor::red:
                attributes |= FOREGROUND_RED | FOREGROUND_INTENSITY;
                break;
            case ConsoleColor::yellow:
                attributes |= FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
                break;
            case ConsoleColor::cyan:
                attributes |= FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
                break;
            case ConsoleColor::darkGray:
                attributes |= FOREGROUND_INTENSITY;
                break;
            case ConsoleColor::defaultColor:
                break;
        }
        (void)SetConsoleTextAttribute(handle, attributes);
    }

    stream << text << '\n';
    stream.flush();
    if (canColor) {
        (void)SetConsoleTextAttribute(handle, original.wAttributes);
    }
#else
    const int descriptor = useErrorStream ? STDERR_FILENO : STDOUT_FILENO;
    const bool canColor = color != ConsoleColor::defaultColor && isatty(descriptor) != 0;
    const char* escape = "";
    if (canColor) {
        switch (color) {
            case ConsoleColor::green: escape = "\x1b[92m"; break;
            case ConsoleColor::red: escape = "\x1b[91m"; break;
            case ConsoleColor::yellow: escape = "\x1b[93m"; break;
            case ConsoleColor::cyan: escape = "\x1b[96m"; break;
            case ConsoleColor::darkGray: escape = "\x1b[90m"; break;
            case ConsoleColor::defaultColor: break;
        }
        stream << escape;
    }
    stream << text;
    if (canColor) {
        stream << "\x1b[0m";
    }
    stream << '\n';
    stream.flush();
#endif
}

bool containsIgnoreCase(const std::string_view value, const std::string_view needle) {
    return std::search(
               value.begin(), value.end(), needle.begin(), needle.end(),
               [](const char left, const char right) {
                   return std::tolower(static_cast<unsigned char>(left))
                       == std::tolower(static_cast<unsigned char>(right));
               }) != value.end();
}

std::string_view stripPythonPrefix(const std::string_view line) {
    constexpr std::string_view prefix = "[Python] ";
    return line.starts_with(prefix) ? line.substr(prefix.size()) : line;
}

bool isBlank(const std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](const char character) {
        return character == ' ' || character == '\t' || character == '\r';
    });
}

bool isTracebackTerminator(std::string_view line) {
    line = stripPythonPrefix(line);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
    }
    static const std::regex pattern(
        R"(^[A-Za-z_][A-Za-z0-9_]*(\.[A-Za-z_][A-Za-z0-9_]*)*(Error|Exception|Interrupt)(:.*)?$)");
    return std::regex_match(line.begin(), line.end(), pattern);
}

ConsoleColor classifyLogColor(const std::string_view line, const bool inTraceback) {
    if (line.find("[INFO][Developer]") != std::string_view::npos) {
        return ConsoleColor::darkGray;
    }
    if (containsIgnoreCase(line, "SUC")) {
        return ConsoleColor::green;
    }
    if (inTraceback || containsIgnoreCase(line, "ERROR") || containsIgnoreCase(line, "FATAL")) {
        return ConsoleColor::red;
    }
    if (containsIgnoreCase(line, "WARN")) {
        return ConsoleColor::yellow;
    }
    if (containsIgnoreCase(line, "DEBUG")) {
        return ConsoleColor::cyan;
    }
    return ConsoleColor::defaultColor;
}

class SafaiaLogPrinter {
public:
    void consume(const MCDevLink::LogEvent& event) {
        auto& state = streams_[event.sessionId];
        state.source = event.source;
        state.residual += event.message;

        std::size_t newline = 0;
        while ((newline = state.residual.find('\n')) != std::string::npos) {
            auto line = state.residual.substr(0, newline);
            state.residual.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            printLine(event.sessionId, state, line);
        }
    }

    void flush(const MCDevLink::SessionId sessionId) {
        const auto found = streams_.find(sessionId);
        if (found == streams_.end()) {
            return;
        }
        if (!found->second.residual.empty()) {
            auto line = std::move(found->second.residual);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            printLine(sessionId, found->second, line);
        }
        streams_.erase(found);
    }

    void flushAll() {
        while (!streams_.empty()) {
            flush(streams_.begin()->first);
        }
    }

private:
    struct StreamState {
        std::string residual;
        std::string source;
        bool inTraceback = false;
    };

    static void printLine(
        const MCDevLink::SessionId sessionId, StreamState& state, const std::string& line) {
        const bool startsTraceback =
            line.find("Traceback (most recent call last):") != std::string::npos;
        const bool tracebackLine = state.inTraceback || startsTraceback;
        const auto color = classifyLogColor(line, tracebackLine);

        if (startsTraceback) {
            state.inTraceback = true;
        } else if (state.inTraceback) {
            const auto content = stripPythonPrefix(line);
            if (isBlank(content) || isTracebackTerminator(line)) {
                state.inTraceback = false;
            }
        }

        std::string output = "[log session=" + std::to_string(sessionId) + " source=" + state.source
            + "] " + line;
        printColoredLine(output, color);
    }

    std::unordered_map<MCDevLink::SessionId, StreamState> streams_;
};

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
    SafaiaLogPrinter logPrinter;
    service.setLogHandler([&logPrinter](const MCDevLink::LogEvent& event) {
        logPrinter.consume(event);
    });
    service.setSessionHandler([&logPrinter](const MCDevLink::SessionEvent& event) {
        if (event.state == MCDevLink::SessionState::disconnected) {
            logPrinter.flush(event.sessionId);
        }
        std::string output = "[session " + std::to_string(event.sessionId) + "] "
            + stateName(event.state) + " " + event.remote.address + ":"
            + std::to_string(event.remote.port);
        if (!event.clientName.empty()) {
            output += " name=" + event.clientName;
        }
        printColoredLine(output, ConsoleColor::cyan);
    });
    service.setDiagnosticHandler([](const MCDevLink::DiagnosticEvent& event) {
        ConsoleColor color = ConsoleColor::cyan;
        if (event.level == MCDevLink::DiagnosticLevel::warning) {
            color = ConsoleColor::yellow;
        } else if (event.level == MCDevLink::DiagnosticLevel::error) {
            color = ConsoleColor::red;
        }
        printColoredLine("[diagnostic] " + event.message, color, true);
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
    constexpr auto idlePollInterval = std::chrono::milliseconds{33};
    while (running.load()) {
        const auto result = runtime.poll({256, std::chrono::microseconds{1000}});
        if (!result.eventLimitReached && !result.timeLimitReached) {
            std::this_thread::sleep_for(idlePollInterval);
        }
    }
    service.stop();
    logPrinter.flushAll();
}
