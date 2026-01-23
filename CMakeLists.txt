# CMake build script for libzerotiercore.a

cmake_minimum_required (VERSION 3.13)
project (zerotier-one LANGUAGES CXX C)

option(ZT1_CENTRAL_CONTROLLER "Build with ZeroTier Central Controller support" OFF)
option(ADDRESS_SANITIZER "Build with Address Sanitizer enabled (only for x86_64/arm64)" OFF
)
set (PROJ_DIR ${PROJECT_SOURCE_DIR})

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

    if(ADDRESS_SANITIZER)
        add_compile_options(-fsanitize=address)
        add_link_options(-fsanitize=address)
    endif()
elseif(CPU_ARCHITECTURE STREQUAL "aarch64" OR CPU_ARCHITECTURE STREQUAL "arm64")
    add_definitions(-DZT_ARCHITECTURE=4 -DZT_ARCH_ARM_HAS_NEON=1 -DZT_SSO_SUPPORTED=1 -DZT_AES_NEON=1)
    add_compile_options(-march=armv8-a+crypto -mtune=generic -mstrict-align)
    
    if(ADDRESS_SANITIZER)
        add_compile_options(-fsanitize=address)
        add_link_options(-fsanitize=address)
    endif()
endif()

include(FetchContent)
include(ExternalProject)

add_definitions(-DCMAKE_BUILD)

find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(inja REQUIRED)

set(CMAKE_THREDAD_PREFER_PTHREAD TRUE CACHE INTERNAL "Use pthreads" FORCE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE CACHE INTERNAL "Use pthreads" FORCE)
find_package(Threads REQUIRED)
if(CMAKE_USE_PTHREADS_INIT)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pthread")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
endif()

if(ZT1_CENTRAL_CONTROLLER)
    find_package(PostgreSQL REQUIRED)
    find_package(opentelemetry-cpp REQUIRED COMPONENTS api sdk exporters_otlp_grpc exporters_otlp_http exporters_prometheus )
    find_package(google_cloud_cpp_bigtable REQUIRED)
    find_package(google_cloud_cpp_pubsub REQUIRED)
else()
    find_package(opentelemetry-cpp REQUIRED COMPONENTS api)
endif()

if(ZT1_CENTRAL_CONTROLLER)
    set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard to conform to" FORCE)
    set(CMAKE_CXX_STANDARD_REQUIRED True CACHE BOOL "C++ standard required" FORCE)
    set(CMAKE_CXX_EXTENSIONS ON CACHE BOOL "Enable compiler-specific extensions" FORCE)
    add_definitions(-DZT_NONFREE_CONTROLLER=1 -DZT_CONTROLLER_USE_LIBPQ=1 -DZT1_CENTRAL_CONTROLLER=1 -DZT_OPENTELEMETRY_ENABLED=1 -DZT_NO_PEER_METRICS=1)
    set(CONTROLLER_RUST_FEATURES ztcontroller)
    set(RUST_BUILD_COMMAND cargo build --release -F ${CONTROLLER_RUST_FEATURES})
else()
    set(CMAKE_CXX_STANDARD 11 CACHE STRING "C++ standard to conform to")
    set(CMAKE_CXX_STANDARD_REQUIRED True CACHE BOOL "C++ standard required")
    set(CMAKE_CXX_EXTENSIONS ON CACHE BOOL "Enable compiler-specific extensions")
    set(RUST_BUILD_COMMAND "cargo build --release")
endif()

# build rustybits
ExternalProject_Add(
    rustybits_build
    DOWNLOAD_COMMAND ""
    CONFIGURE_COMMAND ""
    # PREFIX ${PROJ_DIR}/rustybits
    BUILD_COMMAND cd ${PROJ_DIR}/rustybits && ${RUST_BUILD_COMMAND}
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${PROJ_DIR}/rustybits/target/release/librustybits.a ${PROJ_DIR}/rustybits/target/rustybits.h
  )
set(RUSTYBITS_LIB ${PROJ_DIR}/rustybits/target/release/librustybits.a)
set(RUSTYBITS_INCLUDE_DIR ${PROJ_DIR}/rustybits/target)
add_library(rustybits STATIC IMPORTED GLOBAL)
set_property(TARGET rustybits PROPERTY IMPORTED_LOCATION ${RUSTYBITS_LIB})
add_dependencies(rustybits rustybits_build)



# Get & build dependencies not in conda
include (cmake/cpp-httplib.cmake)
include (cmake/redis-plus-plus.cmake)
include (cmake/miniupnpc.cmake)

add_subdirectory(ext)
add_subdirectory(node)
add_subdirectory(osdep)
add_subdirectory(service)
add_subdirectory(nonfree)

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
    Threads::Threads
)

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

if (APPLE)
    find_library(COREFOUNDATION_LIBRARY CoreFoundation)
    find_library(SECURITY_LIBRARY Security)
    find_library(SYSTEM_CONFIGURATION_LIBRARY SystemConfiguration)
    find_library(CARBON Carbon)
    find_library(CORESERVICES_LIBRARY CoreServices)
    list(APPEND LINKED_LIBRARIES ${COREFOUNDATION_LIBRARY} ${SECURITY_LIBRARY} ${CARBON_LIBRARY} ${SYSTEM_CONFIGURATION_LIBRARY} ${CORESERVICES_LIBRARY})
endif()

add_executable(zerotier-one 
    one.cpp 
    ext/http-parser/http_parser.c)

target_link_libraries(zerotier-one 
    ${LINKED_LIBRARIES}
)

add_executable(zerotier-selftest 
    selftest.cpp 
)

target_link_libraries(zerotier-selftest 
    zerotier-core
    zerotier-osdep
    Threads::Threads
    nlohmann_json::nlohmann_json 
    prometheus-cpp-lite
    Threads::Threads)
