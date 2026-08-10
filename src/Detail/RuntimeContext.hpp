#pragma once

#include "Detail/AsioConfig.hpp"

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>

namespace MCDevLink::Detail {

struct RuntimeContext {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work{asio::make_work_guard(io)};
};

} // namespace MCDevLink::Detail
