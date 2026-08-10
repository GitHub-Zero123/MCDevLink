#pragma once

#include <chrono>
#include <cstddef>
#include <memory>

namespace MCDevLink {

namespace Detail {
struct RuntimeContext;
}

namespace Protocol {
class SafaiaService;
}

struct PollOptions {
    std::size_t maxEvents = 256;
    std::chrono::microseconds timeBudget{500};
};

struct PollResult {
    std::size_t eventsProcessed = 0;
    bool eventLimitReached = false;
    bool timeLimitReached = false;
};

class Runtime {
public:
    Runtime();
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) noexcept = delete;
    Runtime& operator=(Runtime&&) noexcept = delete;

    // Executes ready handlers on the calling thread and never waits for I/O.
    // A Runtime must only be polled from one thread at a time.
    [[nodiscard]] PollResult poll(const PollOptions& options = {});

private:
    class Impl;
    std::unique_ptr<Impl> impl_;

    [[nodiscard]] std::shared_ptr<Detail::RuntimeContext> context() const;

    friend class Protocol::SafaiaService;
};

} // namespace MCDevLink
