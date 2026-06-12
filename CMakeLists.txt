# CMake build script for libzerotiercore.a

cmake_minimum_required(VERSION 3.15)
project(zerotier-one LANGUAGES CXX C)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(ZT1_CENTRAL_CONTROLLER "Build with ZeroTier Central Controller support" OFF)
option(ADDRESS_SANITIZER "Build with Address Sanitizer enabled (only for x86_64/arm64)" OFF)

set(PROJ_DIR ${PROJECT_SOURCE_DIR})

# ---------------------------------------------------------------------------
# CPU architecture detection and architecture-specific flags
# ---------------------------------------------------------------------------
execute_process(COMMAND uname -m COMMAND tr -d '\n' OUTPUT_VARIABLE CPU_ARCHITECTURE)
set(CPU_ARCHITECTURE ${CPU_ARCHITECTURE} CACHE STRING "Target CPU architecture (detected automatically)")
message(STATUS "Detected CPU architecture: ${CPU_ARCHITECTURE}")

if(CPU_ARCHITECTURE STREQUAL "x86_64")
    add_definitions(
        -DZT_ARCHITECTURE=2
        -DZT_USE_X64_ASM_SALSA=1
        -DZT_USE_X64_ASM_ED25519=1
        -DZT_SSO_SUPPORTED=1
        -DZT_USE_X64_ASM_SALSA2012=1
        -DZT_USE_FAST_X64_ED25519=1
    )
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -z noexecstack")
elseif(CPU_ARCHITECTURE STREQUAL "aarch64" OR CPU_ARCHITECTURE STREQUAL "arm64")
    add_definitions(
        -DZT_ARCHITECTURE=4
        -DZT_ARCH_ARM_HAS_NEON=1
        -DZT_SSO_SUPPORTED=1
        -DZT_AES_NEON=1
    )
    add_compile_options(-march=armv8-a+crypto -mtune=generic -mstrict-align)
endif()

# Address Sanitizer is only supported on x86_64/arm64.
if(ADDRESS_SANITIZER AND (CPU_ARCHITECTURE STREQUAL "x86_64"
        OR CPU_ARCHITECTURE STREQUAL "aarch64"
        OR CPU_ARCHITECTURE STREQUAL "arm64"))
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
endif()

# ---------------------------------------------------------------------------
# C++ language standard
# ---------------------------------------------------------------------------
if(ZT1_CENTRAL_CONTROLLER)
    set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard to conform to" FORCE)
    set(CMAKE_CXX_STANDARD_REQUIRED True CACHE BOOL "C++ standard required" FORCE)
    set(CMAKE_CXX_EXTENSIONS ON CACHE BOOL "Enable compiler-specific extensions" FORCE)
else()
    set(CMAKE_CXX_STANDARD 11 CACHE STRING "C++ standard to conform to")
    set(CMAKE_CXX_STANDARD_REQUIRED True CACHE BOOL "C++ standard required")
    set(CMAKE_CXX_EXTENSIONS ON CACHE BOOL "Enable compiler-specific extensions")
endif()

# ---------------------------------------------------------------------------
# Global compile definitions
# ---------------------------------------------------------------------------
add_definitions(-DCMAKE_BUILD)

if(ZT1_CENTRAL_CONTROLLER)
    add_definitions(
        -DZT_NONFREE_CONTROLLER=1
        -DZT_CONTROLLER_USE_LIBPQ=1
        -DZT1_CENTRAL_CONTROLLER=1
        -DZT_OPENTELEMETRY_ENABLED=1
        -DZT_NO_PEER_METRICS=1
    )
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
include(FetchContent)
include(ExternalProject)

find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(inja REQUIRED)

# Threads (prefer pthreads)
set(THREADS_PREFER_PTHREAD_FLAG TRUE CACHE INTERNAL "Use pthreads" FORCE)
find_package(Threads REQUIRED)
if(CMAKE_USE_PTHREADS_INIT)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pthread")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
endif()

# OpenTelemetry: the controller build needs the full SDK + exporters; everyone
# else only needs the API.
if(ZT1_CENTRAL_CONTROLLER)
    find_package(PostgreSQL REQUIRED)
    find_package(opentelemetry-cpp REQUIRED COMPONENTS api sdk exporters_otlp_grpc exporters_otlp_http exporters_prometheus)
    find_package(google_cloud_cpp_bigtable REQUIRED)
    find_package(google_cloud_cpp_pubsub REQUIRED)
else()
    find_package(opentelemetry-cpp REQUIRED COMPONENTS api)
endif()

# Dependencies not provided by conda.
include(cmake/cpp-httplib.cmake)
include(cmake/redis-plus-plus.cmake)
include(cmake/miniupnpc.cmake)

# ---------------------------------------------------------------------------
# rustybits (built via cargo, linked as a static library)
# ---------------------------------------------------------------------------
set(RUSTYBITS_LIB ${PROJ_DIR}/rustybits/target/release/librustybits.a)
set(RUSTYBITS_INCLUDE_DIR ${PROJ_DIR}/rustybits/target)

ExternalProject_Add(
    rustybits_build
    DOWNLOAD_COMMAND ""
    CONFIGURE_COMMAND ""
    BUILD_COMMAND cd ${PROJ_DIR}/rustybits && cargo build --release
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${RUSTYBITS_LIB} ${RUSTYBITS_INCLUDE_DIR}/rustybits.h
)

add_library(rustybits STATIC IMPORTED GLOBAL)
set_property(TARGET rustybits PROPERTY IMPORTED_LOCATION ${RUSTYBITS_LIB})
add_dependencies(rustybits rustybits_build)

# Make `make clean` also clean rustybits. CMake's clean target can only delete
# files (it can't run `cargo clean`), but removing the target dir is exactly
# what `cargo clean` does.
set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES ${PROJ_DIR}/rustybits/target)

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
add_subdirectory(ext)
add_subdirectory(node)
add_subdirectory(osdep)
add_subdirectory(service)
add_subdirectory(nonfree)

# ---------------------------------------------------------------------------
# Link libraries
# ---------------------------------------------------------------------------
set(LINKED_LIBRARIES
    prometheus-cpp-lite
    zerotier-service
    zerotier-osdep
    zerotier-core
    zerotier-controller
    Threads::Threads
    nlohmann_json::nlohmann_json
    opentelemetry-cpp::api
    rustybits
    OpenSSL::Crypto
    OpenSSL::SSL
)

# Controller-only OpenTelemetry exporters.
if(ZT1_CENTRAL_CONTROLLER)
    list(APPEND LINKED_LIBRARIES
        opentelemetry-cpp::sdk
        opentelemetry-cpp::trace
        opentelemetry-cpp::proto_grpc
        opentelemetry-cpp::otlp_grpc_client
        opentelemetry-cpp::otlp_grpc_exporter
        opentelemetry-cpp::otlp_grpc_log_record_exporter
        opentelemetry-cpp::otlp_grpc_metrics_exporter
        opentelemetry-cpp::otlp_http_exporter
        opentelemetry-cpp::otlp_http_log_record_exporter
        opentelemetry-cpp::otlp_http_metric_exporter
        opentelemetry-cpp::prometheus_exporter
    )
endif()

# Apple system frameworks.
if(APPLE)
    find_library(COREFOUNDATION_LIBRARY CoreFoundation)
    find_library(SECURITY_LIBRARY Security)
    find_library(SYSTEM_CONFIGURATION_LIBRARY SystemConfiguration)
    find_library(CARBON_LIBRARY Carbon)
    find_library(CORESERVICES_LIBRARY CoreServices)
    list(APPEND LINKED_LIBRARIES
        ${COREFOUNDATION_LIBRARY}
        ${SECURITY_LIBRARY}
        ${CARBON_LIBRARY}
        ${SYSTEM_CONFIGURATION_LIBRARY}
        ${CORESERVICES_LIBRARY}
    )
endif()

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
add_executable(zerotier-one
    one.cpp
    ext/http-parser/http_parser.c
)
target_link_libraries(zerotier-one ${LINKED_LIBRARIES})

add_executable(zerotier-selftest
    selftest.cpp
)
target_link_libraries(zerotier-selftest
    zerotier-core
    zerotier-osdep
    Threads::Threads
    nlohmann_json::nlohmann_json
    prometheus-cpp-lite
)
