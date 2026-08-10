#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace MCDevLink::Detail {

struct ProtocolFrame {
    std::int32_t protocolId = 0;
    std::vector<std::uint8_t> payload;
};

inline constexpr std::size_t protocolFrameHeaderSize = 8;

[[nodiscard]] std::int32_t readInt32LE(const std::uint8_t* bytes) noexcept;
void writeInt32LE(std::uint8_t* bytes, std::int32_t value) noexcept;
[[nodiscard]] std::vector<std::uint8_t> encodeProtocolFrame(
    std::int32_t protocolId, std::span<const std::uint8_t> payload);

class ProtocolFrameDecoder {
public:
    explicit ProtocolFrameDecoder(std::size_t maxPayload);

    [[nodiscard]] std::vector<ProtocolFrame> push(std::span<const std::uint8_t> bytes);
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] std::size_t bufferedBytes() const noexcept;
    void reset();

private:
    std::size_t maxPayload_;
    std::vector<std::uint8_t> buffer_;
    bool failed_ = false;
};

} // namespace MCDevLink::Detail
