#include "Detail/ProtocolFrame.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace MCDevLink::Detail {

std::int32_t readInt32LE(const std::uint8_t* bytes) noexcept {
    const auto value = static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[3]) << 24U);
    return std::bit_cast<std::int32_t>(value);
}

void writeInt32LE(std::uint8_t* bytes, const std::int32_t value) noexcept {
    const auto unsignedValue = static_cast<std::uint32_t>(value);
    bytes[0] = static_cast<std::uint8_t>(unsignedValue & 0xffU);
    bytes[1] = static_cast<std::uint8_t>((unsignedValue >> 8U) & 0xffU);
    bytes[2] = static_cast<std::uint8_t>((unsignedValue >> 16U) & 0xffU);
    bytes[3] = static_cast<std::uint8_t>((unsignedValue >> 24U) & 0xffU);
}

std::vector<std::uint8_t> encodeProtocolFrame(
    const std::int32_t protocolId, const std::span<const std::uint8_t> payload) {
    if (payload.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::length_error("protocol frame payload is too large");
    }

    std::vector<std::uint8_t> result(protocolFrameHeaderSize + payload.size());
    writeInt32LE(result.data(), protocolId);
    writeInt32LE(result.data() + 4, static_cast<std::int32_t>(payload.size()));
    std::copy(payload.begin(), payload.end(), result.begin() + protocolFrameHeaderSize);
    return result;
}

ProtocolFrameDecoder::ProtocolFrameDecoder(const std::size_t maxPayload)
    : maxPayload_(maxPayload) {}

std::vector<ProtocolFrame> ProtocolFrameDecoder::push(const std::span<const std::uint8_t> bytes) {
    std::vector<ProtocolFrame> frames;
    if (failed_) {
        return frames;
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());

    std::size_t offset = 0;
    while (buffer_.size() - offset >= protocolFrameHeaderSize) {
        const auto* header = buffer_.data() + offset;
        const auto rawSize = readInt32LE(header + 4);
        if (rawSize < 0 || static_cast<std::size_t>(rawSize) > maxPayload_) {
            buffer_.clear();
            failed_ = true;
            return frames;
        }

        const auto payloadSize = static_cast<std::size_t>(rawSize);
        if (buffer_.size() - offset - protocolFrameHeaderSize < payloadSize) {
            break;
        }

        ProtocolFrame frame;
        frame.protocolId = readInt32LE(header);
        const auto payloadBegin = buffer_.begin() + static_cast<std::ptrdiff_t>(offset + protocolFrameHeaderSize);
        frame.payload.assign(payloadBegin, payloadBegin + static_cast<std::ptrdiff_t>(payloadSize));
        frames.push_back(std::move(frame));
        offset += protocolFrameHeaderSize + payloadSize;
    }

    if (offset != 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    return frames;
}

bool ProtocolFrameDecoder::failed() const noexcept {
    return failed_;
}

std::size_t ProtocolFrameDecoder::bufferedBytes() const noexcept {
    return buffer_.size();
}

void ProtocolFrameDecoder::reset() {
    buffer_.clear();
    failed_ = false;
}

} // namespace MCDevLink::Detail
