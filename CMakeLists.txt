cmake_minimum_required(VERSION 3.20)
project(sdr_controller VERSION 2.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Compiler flags ────────────────────────────────────────────────────────────
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Debug>:-g>
    $<$<CONFIG:Debug>:-fsanitize=address>
    $<$<CONFIG:Debug>:-fsanitize=undefined>
)
add_link_options(
    $<$<CONFIG:Debug>:-fsanitize=address>
    $<$<CONFIG:Debug>:-fsanitize=undefined>
)

# ── PkgConfig ─────────────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)

# ── SdrSdk (provides sdr_task_api types via its subdirectory) ─────────────────
# Expected as a sibling directory: ../SdrSdk  (same parent as this repo).
# SdrSdk itself expects ../SdrTaskApi as its own sibling.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../SdrSdk/CMakeLists.txt")
    message(STATUS "SdrResourceManager: using sibling SdrSdk at ../SdrSdk")
    set(SDRSDK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDRSDK_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    add_subdirectory(../SdrSdk ${CMAKE_BINARY_DIR}/SdrSdk)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../SdrTaskApi/CMakeLists.txt")
    message(STATUS "SdrResourceManager: using sibling SdrTaskApi at ../SdrTaskApi")
    add_subdirectory(../SdrTaskApi ${CMAKE_BINARY_DIR}/SdrTaskApi)
else()
    find_package(sdr_task_api QUIET)
    if(NOT sdr_task_api_FOUND)
        message(FATAL_ERROR
            "\n[SdrResourceManager] SdrSdk not found.\n"
            "  Clone as sibling directories:\n"
            "    git clone https://github.com/BMichaud7/SdrSdk.git ../SdrSdk\n"
            "    git clone https://github.com/BMichaud7/SdrTaskApi.git ../SdrTaskApi\n")
    endif()
    message(STATUS "SdrResourceManager: using installed sdr_task_api ${sdr_task_api_VERSION}")
endif()

# ── SoapySDR ──────────────────────────────────────────────────────────────────
find_package(SoapySDR QUIET)
if(NOT SoapySDR_FOUND)
    message(FATAL_ERROR
        "\n[SdrResourceManager] SoapySDR not found.\n"
        "  Ubuntu/Debian : sudo apt-get install libsoapysdr-dev soapysdr-module-remote\n"
        "  CentOS/RHEL   : build from source — see Containerfile.centos10 for the exact steps\n"
        "  Or set SoapySDR_DIR to the directory containing SoapySDRConfig.cmake\n")
endif()

# ── qpid-proton C++ (AMQP 1.0) ───────────────────────────────────────────────
pkg_check_modules(PROTON QUIET libqpid-proton-cpp)
if(NOT PROTON_FOUND)
    find_library(PROTON_CPP_LIB NAMES qpid-proton-cpp)
    find_path(PROTON_INCLUDE_DIR NAMES proton/connection.hpp)
    if(NOT PROTON_CPP_LIB OR NOT PROTON_INCLUDE_DIR)
        message(FATAL_ERROR
            "\n[SdrResourceManager] qpid-proton C++ not found.\n"
            "  Ubuntu/Debian : sudo apt-get install libqpid-proton-cpp12-dev\n"
            "  CentOS/RHEL   : sudo dnf install qpid-proton-cpp-devel\n")
    endif()
    set(PROTON_LIBRARIES    ${PROTON_CPP_LIB})
    set(PROTON_INCLUDE_DIRS ${PROTON_INCLUDE_DIR})
    message(STATUS "qpid-proton-cpp found via find_library: ${PROTON_CPP_LIB}")
endif()

# ── TinyXML2 ──────────────────────────────────────────────────────────────────
pkg_check_modules(TINYXML2 QUIET tinyxml2)
if(NOT TINYXML2_FOUND)
    message(FATAL_ERROR
        "\n[SdrResourceManager] tinyxml2 not found.\n"
        "  Ubuntu/Debian : sudo apt-get install libtinyxml2-dev\n"
        "  CentOS/RHEL   : sudo dnf install tinyxml2-devel\n")
endif()

# ── spdlog ────────────────────────────────────────────────────────────────────
find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
    pkg_check_modules(SPDLOG QUIET spdlog)
    if(NOT SPDLOG_FOUND)
        message(FATAL_ERROR
            "\n[SdrResourceManager] spdlog not found.\n"
            "  Ubuntu/Debian : sudo apt-get install libspdlog-dev\n"
            "  CentOS/RHEL   : build from source — see Containerfile.centos10\n")
    endif()
    add_library(spdlog::spdlog INTERFACE IMPORTED)
    target_include_directories(spdlog::spdlog INTERFACE ${SPDLOG_INCLUDE_DIRS})
    target_link_libraries(spdlog::spdlog INTERFACE ${SPDLOG_LIBRARIES})
endif()

# ── fmt ───────────────────────────────────────────────────────────────────────
find_package(fmt QUIET)
if(NOT fmt_FOUND)
    pkg_check_modules(FMT QUIET fmt)
    if(NOT FMT_FOUND)
        message(FATAL_ERROR
            "\n[SdrResourceManager] {fmt} not found.\n"
            "  Ubuntu/Debian : sudo apt-get install libfmt-dev\n"
            "  CentOS/RHEL   : sudo dnf install fmt-devel\n")
    endif()
    add_library(fmt::fmt INTERFACE IMPORTED)
    target_include_directories(fmt::fmt INTERFACE ${FMT_INCLUDE_DIRS})
    target_link_libraries(fmt::fmt INTERFACE ${FMT_LIBRARIES})
endif()

# ── FFTW3 single-precision ────────────────────────────────────────────────────
find_library(FFTW3F_LIB NAMES fftw3f)
find_path(FFTW3_INCLUDE NAMES fftw3.h)
if(NOT FFTW3F_LIB OR NOT FFTW3_INCLUDE)
    message(FATAL_ERROR
        "\n[SdrResourceManager] FFTW3 (single-precision) not found.\n"
        "  Ubuntu/Debian : sudo apt-get install libfftw3-dev\n"
        "  CentOS/RHEL   : sudo dnf install fftw-devel\n")
endif()

# ── libuuid ───────────────────────────────────────────────────────────────────
find_library(UUID_LIB NAMES uuid)
find_path(UUID_INCLUDE NAMES uuid/uuid.h)
if(NOT UUID_LIB OR NOT UUID_INCLUDE)
    message(FATAL_ERROR
        "\n[SdrResourceManager] libuuid not found.\n"
        "  Ubuntu/Debian : sudo apt-get install uuid-dev\n"
        "  CentOS/RHEL   : sudo dnf install libuuid-devel\n")
endif()

# ── Threads ───────────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)

# ── Controller executable ─────────────────────────────────────────────────────
file(GLOB CONTROLLER_SOURCES src/*.cpp)
add_executable(sdr_controller ${CONTROLLER_SOURCES})

target_include_directories(sdr_controller PRIVATE
    include
    ${SoapySDR_INCLUDE_DIRS}
    ${PROTON_INCLUDE_DIRS}
    ${TINYXML2_INCLUDE_DIRS}
    ${FFTW3_INCLUDE}
    ${UUID_INCLUDE}
)

target_link_libraries(sdr_controller PRIVATE
    sdr_task_api
    SoapySDR
    ${PROTON_LIBRARIES}
    ${TINYXML2_LIBRARIES}
    spdlog::spdlog
    fmt::fmt
    ${FFTW3F_LIB}
    ${UUID_LIB}
    Threads::Threads
)

target_compile_definitions(sdr_controller PRIVATE
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
    SPDLOG_FMT_EXTERNAL
)

# ── Client ────────────────────────────────────────────────────────────────────
add_subdirectory(client)

# ── Tests ─────────────────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS sdr_controller DESTINATION bin)
install(FILES config/devices.xml DESTINATION etc/sdr-controller)

# ── Configure summary ─────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "┌─ SdrResourceManager (sdr_controller) ${PROJECT_VERSION} ───────┐")
message(STATUS "│  C++ standard : C++20                                  │")
message(STATUS "│  Build type   : ${CMAKE_BUILD_TYPE}                           ")
message(STATUS "│  Dependencies :                                        │")
message(STATUS "│    SdrTaskApi    : FOUND (sibling or installed)        │")
message(STATUS "│    SoapySDR      : FOUND ${SoapySDR_VERSION}                    ")
message(STATUS "│    qpid-proton   : FOUND                               │")
message(STATUS "│    tinyxml2      : FOUND ${TINYXML2_VERSION}                    ")
message(STATUS "│    spdlog        : FOUND                               │")
message(STATUS "│    fftw3f        : FOUND ${FFTW3F_LIB}                          ")
message(STATUS "│    libuuid       : FOUND                               │")
message(STATUS "│    googletest    : test-only (system or FetchContent)  │")
message(STATUS "└────────────────────────────────────────────────────────┘")
message(STATUS "")
