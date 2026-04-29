cmake_minimum_required(VERSION 3.20)
project(sdr_acquisition VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Optional: ASAN/UBSan for Debug builds ───────────────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(SoapySDR REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(PROTON     REQUIRED IMPORTED_TARGET qpid-proton-cpp)
pkg_check_modules(TINYXML2   REQUIRED IMPORTED_TARGET tinyxml2)
pkg_check_modules(FFTW3F     REQUIRED IMPORTED_TARGET fftw3f)
pkg_check_modules(PQXX       REQUIRED IMPORTED_TARGET libpqxx)
pkg_check_modules(SPDLOG     REQUIRED IMPORTED_TARGET spdlog)
pkg_check_modules(FMT        REQUIRED IMPORTED_TARGET fmt)
pkg_check_modules(UUID       REQUIRED IMPORTED_TARGET uuid)

# ── Binary ───────────────────────────────────────────────────────────────────
add_executable(sdr_acquisition
    src/main.cpp
    src/SweepConfig.cpp
    src/FftProcessor.cpp
    src/SpectrumScanner.cpp
    src/DetectionDb.cpp
    src/AmqpPublisher.cpp
)

target_include_directories(sdr_acquisition PRIVATE include)

target_link_libraries(sdr_acquisition PRIVATE
    SoapySDR
    nlohmann_json::nlohmann_json
    PkgConfig::PROTON
    PkgConfig::TINYXML2
    PkgConfig::FFTW3F
    PkgConfig::PQXX
    PkgConfig::SPDLOG
    PkgConfig::FMT
    PkgConfig::UUID
)

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS sdr_acquisition DESTINATION bin)
install(FILES config/scanner.xml
    DESTINATION etc/sdr-acquisition
    RENAME scanner.xml.example)
install(FILES schema/init.sql DESTINATION share/sdr-acquisition)
