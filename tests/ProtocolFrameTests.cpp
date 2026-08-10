#include "Detail/ProtocolFrame.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
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

} // namespace

int main() {
    using MCDevLink::Detail::ProtocolFrameDecoder;
    using MCDevLink::Detail::encodeProtocolFrame;
    using MCDevLink::Detail::writeInt32LE;

    const std::string firstPayload = "first";
    const std::string secondPayload = "second";
    auto first = encodeProtocolFrame(3, bytes(firstPayload));
    auto second = encodeProtocolFrame(4, bytes(secondPayload));

    ProtocolFrameDecoder fragmented(1024);
    auto frames = fragmented.push(std::span(first).first(3));
    CHECK(frames.empty());
    CHECK(fragmented.bufferedBytes() == 3);
    frames = fragmented.push(std::span(first).subspan(3));
    CHECK(frames.size() == 1);
    CHECK(frames[0].protocolId == 3);
    CHECK(std::string(frames[0].payload.begin(), frames[0].payload.end()) == firstPayload);

    std::vector<std::uint8_t> combined = first;
    combined.insert(combined.end(), second.begin(), second.end());
    ProtocolFrameDecoder coalesced(1024);
    frames = coalesced.push(combined);
    CHECK(frames.size() == 2);
    CHECK(frames[1].protocolId == 4);
    CHECK(std::string(frames[1].payload.begin(), frames[1].payload.end()) == secondPayload);

    std::array<std::uint8_t, 8> invalid{};
    writeInt32LE(invalid.data(), 4);
    writeInt32LE(invalid.data() + 4, -1);
    ProtocolFrameDecoder rejected(1024);
    CHECK(rejected.push(invalid).empty());
    CHECK(rejected.failed());
    CHECK(rejected.push(first).empty());
    rejected.reset();
    CHECK(!rejected.failed());
    CHECK(rejected.push(first).size() == 1);

    std::array<std::uint8_t, 8> oversized{};
    writeInt32LE(oversized.data(), 4);
    writeInt32LE(oversized.data() + 4, 1025);
    ProtocolFrameDecoder limited(1024);
    CHECK(limited.push(oversized).empty());
    CHECK(limited.failed());

    return failures == 0 ? 0 : 1;
}
