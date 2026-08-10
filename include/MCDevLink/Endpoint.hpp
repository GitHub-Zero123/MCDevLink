#pragma once

#include <cstdint>
#include <string>

namespace MCDevLink {

struct Endpoint {
    std::string address;
    std::uint16_t port = 0;

    friend bool operator==(const Endpoint&, const Endpoint&) = default;
};

} // namespace MCDevLink
