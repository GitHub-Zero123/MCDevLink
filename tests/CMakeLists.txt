add_executable(
    mcdevlink_protocol_frame_tests
    ProtocolFrameTests.cpp
    ${PROJECT_SOURCE_DIR}/src/Detail/ProtocolFrame.cpp
)
target_compile_features(mcdevlink_protocol_frame_tests PRIVATE cxx_std_20)
target_include_directories(mcdevlink_protocol_frame_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_target_properties(mcdevlink_protocol_frame_tests PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME mcdevlink.protocol_frame COMMAND mcdevlink_protocol_frame_tests)

add_executable(mcdevlink_runtime_tests RuntimeTests.cpp)
target_link_libraries(mcdevlink_runtime_tests PRIVATE MCDevLink::MCDevLink)
set_target_properties(mcdevlink_runtime_tests PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME mcdevlink.runtime COMMAND mcdevlink_runtime_tests)

add_executable(
    mcdevlink_safaia_loopback_tests
    SafaiaLoopbackTests.cpp
    ${PROJECT_SOURCE_DIR}/src/Detail/ProtocolFrame.cpp
)
target_compile_features(mcdevlink_safaia_loopback_tests PRIVATE cxx_std_20)
target_compile_definitions(mcdevlink_safaia_loopback_tests PRIVATE ASIO_STANDALONE)
target_include_directories(
    mcdevlink_safaia_loopback_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/third_party/asio/include
)
target_link_libraries(mcdevlink_safaia_loopback_tests PRIVATE MCDevLink::MCDevLink)
set_target_properties(mcdevlink_safaia_loopback_tests PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME mcdevlink.safaia_loopback COMMAND mcdevlink_safaia_loopback_tests)
