cmake_minimum_required(VERSION 3.24)

project(
    MCDevLink
    VERSION 0.1.0
    DESCRIPTION "Host-driven C++20 backend for Minecraft developer protocols"
    LANGUAGES CXX
)

option(MCDEVLINK_BUILD_TESTS "Build MCDevLink unit tests" ${PROJECT_IS_TOP_LEVEL})
option(MCDEVLINK_BUILD_EXAMPLES "Build MCDevLink examples and manual MC probes" ${PROJECT_IS_TOP_LEVEL})

add_library(MCDevLink STATIC)
add_library(MCDevLink::MCDevLink ALIAS MCDevLink)

target_sources(
    MCDevLink
    PRIVATE
        src/Runtime.cpp
        src/Detail/ProtocolFrame.cpp
        src/Protocol/Safaia/SafaiaService.cpp
)

target_compile_features(MCDevLink PUBLIC cxx_std_20)
target_compile_definitions(
    MCDevLink
    PRIVATE
        ASIO_STANDALONE
        ASIO_NO_DEPRECATED
)

target_include_directories(
    MCDevLink
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann/include
)

if(WIN32)
    target_compile_definitions(MCDevLink PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(MCDevLink PRIVATE ws2_32)
endif()

set_target_properties(
    MCDevLink
    PROPERTIES
        CXX_EXTENSIONS OFF
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        VISIBILITY_INLINES_HIDDEN YES
)

if(MSVC)
    target_compile_options(MCDevLink PRIVATE /W4 /permissive-)
else()
    target_compile_options(MCDevLink PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MCDEVLINK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MCDEVLINK_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
