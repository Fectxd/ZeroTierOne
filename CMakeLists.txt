# CMake build script for libzerotiercore.a

cmake_minimum_required(VERSION 3.15)
project(zerotier-one LANGUAGES CXX C)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(ZT1_CENTRAL_CONTROLLER "Build with ZeroTier Central Controller support" OFF)
option(ADDRESS_SANITIZER "Build with Address Sanitizer enabled (only for x86_64/arm64)" OFF)
option(EXT_OSDEP "Build with external osdep feature" OFF)
option(ZT_IA32 "Force a 32-bit (i386) build on x86 hosts mismarked as i386" OFF)

# Feature toggles mirrored from make-linux.mk's ZT_* switches.
option(ZT_TRACE "Enable tracing (-DZT_TRACE)" OFF)
option(ZT_RULES_ENGINE_DEBUGGING "Enable rules-engine debugging" OFF)
option(ZT_DEBUG_TRACE "Enable debug tracing" OFF)
option(ZT_USE_TEST_TAP "Build with the test tap device" OFF)
option(ZT_VAULT_SUPPORT "Enable HashiCorp Vault support (links libcurl)" OFF)
option(ZT_STATIC "Link the executables statically" OFF)
option(ZT_QNAP "Build for QNAP (embedded)" OFF)
option(ZT_UBIQUITI "Build for Ubiquiti (embedded)" OFF)
option(ZT_SYNOLOGY "Build for Synology (embedded)" OFF)

set(PROJ_DIR ${PROJECT_SOURCE_DIR})

# make builds release by default and switches to debug flags under ZT_DEBUG=1;
# mirror that with CMAKE_BUILD_TYPE, defaulting to Release when unset.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# ---------------------------------------------------------------------------
# CPU architecture detection and architecture-specific flags
#
# Translated from make-linux.mk. CMAKE_SYSTEM_PROCESSOR is CMake's canonical
# target-processor variable: it reflects the host for native builds and is set
# by the toolchain file when cross-compiling. ZT_ARCHITECTURE mirrors the
# numeric arch codes used by the Makefile and is exported to the binary as
# ZT_BUILD_ARCHITECTURE (read by one.cpp via version.h).
#
# Note: node/Constants.hpp already auto-defines ZT_NO_TYPE_PUNNING for every
# non-x86 architecture, and AES.hpp auto-defines ZT_AES_NEON whenever ARM
# crypto extensions are present (-march=armv8-a+crypto). The explicit -D flags
# below are therefore belt-and-suspenders that keep this a faithful mirror of
# make-linux.mk.
# ---------------------------------------------------------------------------
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" CPU_ARCHITECTURE)
message(STATUS "Target CPU (CMAKE_SYSTEM_PROCESSOR): ${CMAKE_SYSTEM_PROCESSOR}")

# ZeroTier architecture code (see node/Constants.hpp); 999 = unknown.
set(ZT_ARCHITECTURE 999)
# Crypto-assembly gates, also consumed by node/CMakeLists.txt to select sources.
set(ZT_USE_X64_ASM_SALSA FALSE)
set(ZT_USE_X64_ASM_ED25519 FALSE)
set(ZT_USE_ARM32_NEON_ASM_CRYPTO FALSE)
set(ZT_SSO_SUPPORTED FALSE)

if(CPU_ARCHITECTURE STREQUAL "x86_64" OR CPU_ARCHITECTURE STREQUAL "amd64")
    set(ZT_ARCHITECTURE 2)
    set(ZT_USE_X64_ASM_SALSA TRUE)
    set(ZT_USE_X64_ASM_ED25519 TRUE)
    set(ZT_SSO_SUPPORTED TRUE)
    add_compile_options(-msse -msse2)
elseif(CPU_ARCHITECTURE STREQUAL "e2k" OR CPU_ARCHITECTURE STREQUAL "e2k64")
    # Elbrus 2000: x86-compatible arch code, but no x86 crypto assembly.
    set(ZT_ARCHITECTURE 2)
elseif(CPU_ARCHITECTURE STREQUAL "i386" OR CPU_ARCHITECTURE STREQUAL "i486"
        OR CPU_ARCHITECTURE STREQUAL "i586" OR CPU_ARCHITECTURE STREQUAL "i686")
    set(ZT_ARCHITECTURE 1)
    set(ZT_SSO_SUPPORTED TRUE)
elseif(CPU_ARCHITECTURE STREQUAL "aarch64" OR CPU_ARCHITECTURE STREQUAL "arm64")
    set(ZT_ARCHITECTURE 4)
    set(ZT_SSO_SUPPORTED TRUE)
    add_definitions(-DZT_NO_TYPE_PUNNING -DZT_ARCH_ARM_HAS_NEON)
    add_compile_options(-march=armv8-a+crypto -mtune=generic -mstrict-align)
elseif(CPU_ARCHITECTURE MATCHES "^(arm|armel|armhf|armv6|armv6l|armv6k|armv6kz|armv6zk|armv7|armv7l|armv7hl|armv7ve)$")
    set(ZT_ARCHITECTURE 3)
    set(ZT_USE_ARM32_NEON_ASM_CRYPTO TRUE)
    add_definitions(-DZT_NO_TYPE_PUNNING)
    if(CPU_ARCHITECTURE STREQUAL "armhf")
        set(ZT_SSO_SUPPORTED TRUE)
    endif()
elseif(CPU_ARCHITECTURE STREQUAL "powerpc64le")
    set(ZT_ARCHITECTURE 8)
    add_definitions(-DZT_NO_TYPE_PUNNING)
elseif(CPU_ARCHITECTURE STREQUAL "powerpc")
    set(ZT_ARCHITECTURE 8)
    add_definitions(-DZT_NO_TYPE_PUNNING -DZT_NO_CAPABILITIES)
elseif(CPU_ARCHITECTURE STREQUAL "ppc64le" OR CPU_ARCHITECTURE STREQUAL "ppc64el")
    set(ZT_ARCHITECTURE 8)
elseif(CPU_ARCHITECTURE STREQUAL "mips" OR CPU_ARCHITECTURE STREQUAL "mipsel")
    set(ZT_ARCHITECTURE 5)
    add_definitions(-DZT_NO_TYPE_PUNNING)
elseif(CPU_ARCHITECTURE STREQUAL "mips64" OR CPU_ARCHITECTURE STREQUAL "mips64el")
    set(ZT_ARCHITECTURE 6)
    add_definitions(-DZT_NO_TYPE_PUNNING)
elseif(CPU_ARCHITECTURE STREQUAL "s390x")
    set(ZT_ARCHITECTURE 16)
elseif(CPU_ARCHITECTURE STREQUAL "riscv64")
    set(ZT_ARCHITECTURE 0)
elseif(CPU_ARCHITECTURE STREQUAL "loongarch64")
    set(ZT_ARCHITECTURE 17)
    add_definitions(-DZT_NO_TYPE_PUNNING)
endif()

# Embedded platform markers (make-linux.mk); these also disable SSO.
if(ZT_QNAP)
    add_definitions(-D__QNAP__)
    set(ZT_EMBEDDED TRUE)
endif()
if(ZT_UBIQUITI)
    add_definitions(-D__UBIQUITI__)
    set(ZT_EMBEDDED TRUE)
endif()
if(ZT_SYNOLOGY)
    add_definitions(-D__SYNOLOGY__)
    set(ZT_EMBEDDED TRUE)
endif()

# SSO is unavailable for external-osdep and embedded builds (make-linux.mk).
if(EXT_OSDEP OR ZT_EMBEDDED)
    set(ZT_SSO_SUPPORTED FALSE)
endif()

# Fail if the architecture could not be determined.
if(ZT_ARCHITECTURE EQUAL 999)
    message(FATAL_ERROR
        "Unsupported/undetected CPU architecture '${CMAKE_SYSTEM_PROCESSOR}'. "
        "Add a case for it above (mirror make-linux.mk).")
endif()
message(STATUS "ZeroTier architecture code (ZT_BUILD_ARCHITECTURE): ${ZT_ARCHITECTURE}")

# Intel 32-bit override: some images mismark x86_64 as i386. Force 32-bit and
# disable x86-64 crypto assembly.
if(ZT_IA32)
    add_compile_options(-m32)
    add_link_options(-m32)
    set(ZT_USE_X64_ASM_SALSA FALSE)
    set(ZT_USE_X64_ASM_ED25519 FALSE)
endif()

# "ARM32 hell": conservative per-board flags. Mirrors make-linux.mk, which
# branches on the Debian architecture (host-based dpkg probe, as in the
# Makefile) and the EXT_OSDEP feature. NEON crypto assembly ends up disabled
# on arm32 except in the EXT_OSDEP path.
if(ZT_ARCHITECTURE EQUAL 3)
    execute_process(
        COMMAND dpkg --print-architecture
        OUTPUT_VARIABLE ZT_DPKG_ARCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(ZT_DPKG_ARCH STREQUAL "armel")
        add_compile_options(-march=armv5t -mfloat-abi=soft -msoft-float -mno-unaligned-access -marm)
        set(ZT_USE_ARM32_NEON_ASM_CRYPTO FALSE)
    elseif(NOT EXT_OSDEP)
        add_compile_options(-mfloat-abi=hard -march=armv6zk -marm -mfpu=vfp -mno-unaligned-access -mtp=cp15 -mcpu=arm1176jzf-s)
        add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fexceptions>)
        set(ZT_USE_ARM32_NEON_ASM_CRYPTO FALSE)
    else()
        add_definitions(-DZT_NO_PEER_METRICS)
    endif()
endif()

# Single sign-on (OIDC) support.
if(ZT_SSO_SUPPORTED)
    add_definitions(-DZT_SSO_SUPPORTED=1)
endif()

# Faster crypto assembly. These -D gates pair with the source selection in
# node/CMakeLists.txt, which reads the ZT_USE_*_ASM_* variables set above.
if(ZT_USE_X64_ASM_SALSA)
    add_definitions(-DZT_USE_X64_ASM_SALSA2012)
endif()
if(ZT_USE_X64_ASM_ED25519)
    add_definitions(-DZT_USE_FAST_X64_ED25519)
endif()
if(ZT_USE_ARM32_NEON_ASM_CRYPTO)
    add_definitions(-DZT_USE_ARM32_NEON_ASM_SALSA2012)
endif()

# Build platform code reported into the binary (read by one.cpp via version.h).
# Codes match the other build systems: make-linux.mk = 1, Windows .vcxproj = 2,
# make-mac.mk = 3, root Makefile FreeBSD = 7 / OpenBSD = 9. NetBSD has no
# assigned code anywhere, so it falls back to version.h's default of 0.
if(APPLE)
    set(ZT_BUILD_PLATFORM 3)
elseif(WIN32)
    set(ZT_BUILD_PLATFORM 2)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(ZT_BUILD_PLATFORM 7)
elseif(CMAKE_SYSTEM_NAME STREQUAL "OpenBSD")
    set(ZT_BUILD_PLATFORM 9)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(ZT_BUILD_PLATFORM 1)
else()
    set(ZT_BUILD_PLATFORM 0)
endif()
add_definitions(-DZT_BUILD_PLATFORM=${ZT_BUILD_PLATFORM} -DZT_BUILD_ARCHITECTURE=${ZT_ARCHITECTURE})

# Address Sanitizer (only meaningful on x86_64/arm64).
if(ADDRESS_SANITIZER AND (ZT_ARCHITECTURE EQUAL 2 OR ZT_ARCHITECTURE EQUAL 4))
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
endif()

# ---------------------------------------------------------------------------
# C++ language standard
#
# make-linux.mk compiles everything as C++17 (controller and daemon alike), so
# we do the same. Extensions stay ON, which yields -std=gnu++17 -- a superset of
# the Makefile's -std=c++17, so anything that builds under make also builds here.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard to conform to" FORCE)
set(CMAKE_CXX_STANDARD_REQUIRED True CACHE BOOL "C++ standard required" FORCE)
set(CMAKE_CXX_EXTENSIONS ON CACHE BOOL "Enable compiler-specific extensions" FORCE)

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
# Compiler and linker flags (translated from make-linux.mk)
#
# CMAKE_BUILD_TYPE supplies -O3 -DNDEBUG for Release and -g for Debug; the lines
# below add the remaining flags make uses so the two builds match. make applies
# these to ZeroTier's own objects only; here they are global, so bundled
# third-party targets see them too (harmless -- we set no -Werror).
# ---------------------------------------------------------------------------

# Warnings + position independence: every configuration, matching make.
add_compile_options(-Wall -Wno-deprecated -fPIC -fPIE)

# Release adds a stack protector; debug uses -O1 globally (with -O2 for the hot
# crypto sources, set per-file in node/CMakeLists.txt).
add_compile_options($<$<NOT:$<CONFIG:Debug>>:-fstack-protector>)
add_compile_options($<$<CONFIG:Debug>:-O1>)

# make's debug build also defines these (ZT_DEBUG implies ZT_TRACE).
add_compile_definitions($<$<CONFIG:Debug>:ZT_DEBUG> $<$<CONFIG:Debug>:ZT_TRACE>)

# Stop libstdc++ from pulling in its mt/pool/extptr/debug allocators (no-op on
# macOS/libc++).
add_definitions(-D_MT_ALLOCATOR_H -D_POOL_ALLOCATOR_H -D_EXTPTR_ALLOCATOR_H -D_DEBUG_ALLOCATOR_H)

# GNU-ld hardening (not understood by the macOS linker). make applies
# -Wl,-z,noexecstack to every build and -pie -Wl,-z,relro,-z,now to release, and
# disables self-update on Linux/BSD (left to package management).
if(NOT APPLE)
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,-z,noexecstack")
    string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " -pie -Wl,-z,relro,-z,now")
    add_compile_definitions(ZT_SOFTWARE_UPDATE_DEFAULT="disable")
endif()

# Optional feature toggles (mirror make-linux.mk's ZT_* switches).
if(ZT_TRACE)
    add_definitions(-DZT_TRACE)
endif()
if(ZT_RULES_ENGINE_DEBUGGING)
    add_definitions(-DZT_RULES_ENGINE_DEBUGGING)
endif()
if(ZT_DEBUG_TRACE)
    add_definitions(-DZT_DEBUG_TRACE)
endif()
if(ZT_USE_TEST_TAP)
    add_definitions(-DZT_USE_TEST_TAP)
endif()
if(ZT_VAULT_SUPPORT)
    add_definitions(-DZT_VAULT_SUPPORT=1)
endif()
if(ZT_STATIC)
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -static")
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

# HashiCorp Vault support links libcurl (make-linux.mk).
if(ZT_VAULT_SUPPORT)
    find_package(CURL REQUIRED)
    list(APPEND LINKED_LIBRARIES CURL::libcurl)
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
