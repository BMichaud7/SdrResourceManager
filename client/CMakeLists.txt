cmake_minimum_required(VERSION 3.20)

find_package(PkgConfig REQUIRED)
# Try both pkg-config names; fall back to find_library when .pc files are incomplete.
pkg_check_modules(PROTON libqpid-proton-cpp)
if(NOT PROTON_FOUND)
    pkg_check_modules(PROTON qpid-proton-cpp)
endif()
if(NOT PROTON_FOUND)
    find_library(PROTON_CPP_LIB NAMES qpid-proton-cpp REQUIRED)
    find_path(PROTON_INCLUDE_DIR NAMES proton/connection.hpp REQUIRED)
    set(PROTON_LIBRARIES    ${PROTON_CPP_LIB})
    set(PROTON_INCLUDE_DIRS ${PROTON_INCLUDE_DIR})
endif()

find_package(nlohmann_json 3.11 QUIET)

find_library(UUID_LIB uuid REQUIRED)
find_path(UUID_INCLUDE uuid/uuid.h REQUIRED)

add_executable(sdr_client sdr_client.cpp)

target_include_directories(sdr_client PRIVATE
    ${PROTON_INCLUDE_DIRS}
    ${UUID_INCLUDE}
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(sdr_client PRIVATE
    ${PROTON_LIBRARIES}
    ${UUID_LIB}
    Threads::Threads
    nlohmann_json::nlohmann_json
)

target_compile_features(sdr_client PRIVATE cxx_std_20)

install(TARGETS sdr_client DESTINATION bin)
