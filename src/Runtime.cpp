#include "MCDevLink/Runtime.hpp"

#include "Detail/RuntimeContext.hpp"

namespace MCDevLink {

class Runtime::Impl {
public:
    std::shared_ptr<Detail::RuntimeContext> context = std::make_shared<Detail::RuntimeContext>();
};

Runtime::Runtime()
    : impl_(std::make_unique<Impl>()) {}

Runtime::~Runtime() = default;

std::shared_ptr<Detail::RuntimeContext> Runtime::context() const {
    return impl_ ? impl_->context : nullptr;
}

PollResult Runtime::poll(const PollOptions& options) {
    PollResult result;
    if (!impl_ || options.maxEvents == 0 || options.timeBudget <= std::chrono::microseconds::zero()) {
        return result;
    }

    const auto startedAt = std::chrono::steady_clock::now();
    while (result.eventsProcessed < options.maxEvents) {
        if (std::chrono::steady_clock::now() - startedAt >= options.timeBudget) {
            result.timeLimitReached = true;
            break;
        }

        const auto processed = impl_->context->io.poll_one();
        if (processed == 0) {
            break;
        }
        result.eventsProcessed += processed;
    }

    result.eventLimitReached = result.eventsProcessed == options.maxEvents;
    return result;
}

} // namespace MCDevLink
