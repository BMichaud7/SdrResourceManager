cmake_minimum_required(VERSION 3.20)

# ── Google Test ───────────────────────────────────────────────────────────────
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    message(STATUS "GTest not found via find_package; fetching from GitHub")
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# ── spdlog: CentOS uses imported target, Ubuntu uses pkg-config variables ─────
if(TARGET spdlog::spdlog)
    set(_SPDLOG_LIBS spdlog::spdlog fmt::fmt)
    set(_SPDLOG_DEFS SPDLOG_FMT_EXTERNAL)
    set(_SPDLOG_INCS "")
else()
    set(_SPDLOG_LIBS ${SPDLOG_LIBRARIES})
    set(_SPDLOG_DEFS "")
    set(_SPDLOG_INCS ${SPDLOG_INCLUDE_DIRS})
endif()

# ── Controller sources minus main.cpp ─────────────────────────────────────────
file(GLOB CTRL_SRCS ${CMAKE_SOURCE_DIR}/src/*.cpp)
list(FILTER CTRL_SRCS EXCLUDE REGEX ".*/main\\.cpp$")

# ── Test binary ───────────────────────────────────────────────────────────────
add_executable(sdr_tests
    fake_soapy/FakeSoapyDevice.cpp
    unit/test_spectrum_timeline.cpp
    unit/test_message_codec.cpp
    unit/test_config_parser.cpp
    unit/test_udp_port_pool.cpp
    unit/test_resource_manager.cpp
    unit/test_resource_manager_scale.cpp
    unit/test_iq_streamer.cpp
    unit/test_ddc.cpp
    unit/test_channelization.cpp
    unit/test_fft_engine.cpp
    unit/test_scan_executor.cpp
    unit/test_trigger_monitor.cpp
    ${CTRL_SRCS}
)

target_include_directories(sdr_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/fake_soapy
    ${SoapySDR_INCLUDE_DIRS}
    ${PROTON_INCLUDE_DIRS}
    ${TINYXML2_INCLUDE_DIRS}
    ${FFTW3_INCLUDE}
    ${UUID_INCLUDE}
    ${_SPDLOG_INCS}
)

target_link_libraries(sdr_tests PRIVATE
    sdr_task_api
    GTest::gtest_main
    SoapySDR
    ${PROTON_LIBRARIES}
    ${TINYXML2_LIBRARIES}
    ${_SPDLOG_LIBS}
    ${FFTW3F_LIB}
    ${UUID_LIB}
    Threads::Threads
)

target_compile_definitions(sdr_tests PRIVATE
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_WARN
    ${_SPDLOG_DEFS}
)

# ── CTest integration ─────────────────────────────────────────────────────────
include(CTest)
include(GoogleTest)
gtest_discover_tests(sdr_tests)
