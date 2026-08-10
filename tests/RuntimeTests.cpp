#include <MCDevLink/Protocol/Safaia.hpp>
#include <MCDevLink/Runtime.hpp>

#include <chrono>
#include <iostream>

namespace {

int failures = 0;

void check(const bool condition, const char* expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

} // namespace

int main() {
    MCDevLink::Runtime runtime;

    auto result = runtime.poll({0, std::chrono::microseconds{500}});
    CHECK(result.eventsProcessed == 0);
    CHECK(!result.eventLimitReached);

    MCDevLink::Protocol::SafaiaOptions options;
    CHECK(options.bindEndpoint.address == "127.0.0.1");
    CHECK(options.bindEndpoint.port == 0);
    CHECK(options.discoveryTargets.empty());
    options.discoveryEnabled = false;

    MCDevLink::Protocol::SafaiaService service(runtime, options);
    const auto error = service.start();
    CHECK(!error);
    CHECK(service.isRunning());
    CHECK(service.localEndpoint().address == "127.0.0.1");
    CHECK(service.localEndpoint().port != 0);
    CHECK(service.sessionCount() == 0);
    service.stop();
    CHECK(!service.isRunning());
    CHECK(service.start() == std::make_error_code(std::errc::operation_not_permitted));

    MCDevLink::Protocol::SafaiaOptions emptyBind;
    emptyBind.bindEndpoint.address.clear();
    emptyBind.discoveryEnabled = false;
    MCDevLink::Protocol::SafaiaService emptyBindService(runtime, emptyBind);
    CHECK(emptyBindService.start() == std::make_error_code(std::errc::invalid_argument));

    MCDevLink::Protocol::SafaiaOptions invalid;
    invalid.discoveryEnabled = true;
    invalid.discoveryPortCount = 0;
    MCDevLink::Protocol::SafaiaService invalidService(runtime, invalid);
    CHECK(invalidService.start() == std::make_error_code(std::errc::invalid_argument));

    return failures == 0 ? 0 : 1;
}
