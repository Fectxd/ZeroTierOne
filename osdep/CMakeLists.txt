cmake_minimum_required(VERSION 3.13)
project(zerotier-osdep LANGUAGES CXX ASM)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(SRC_FILES
    Arp.cpp
    Arp.hpp
    Binder.hpp
    BlockingQueue.hpp
    EthernetTap.cpp
    EthernetTap.hpp
    Http.cpp
    Http.hpp
    ManagedRoute.cpp
    ManagedRoute.hpp
    NeighborDiscovery.cpp
    NeighborDiscovery.hpp
    OSUtils.cpp
    OSUtils.hpp
    Phy.hpp
    PortMapper.cpp
    PortMapper.hpp
    Thread.hpp
)

option(EXT_OSDEP "Build with external osdep feature" OFF)

if(EXT_OSDEP)
    add_definitions(-DZT_EXTOSDEP)
    list(APPEND SRC_FILES
        ExtOsdep.cpp
        ExtOsdep.hpp)
endif()

if(LINUX)
    list(APPEND SRC_FILES
        LinuxEthernetTap.cpp
        LinuxEthernetTap.hpp
        LinuxNetLink.cpp
        LinuxNetLink.hpp)
endif()

if(APPLE)
    list(APPEND SRC_FILES
        MacEthernetTap.cpp
        MacEthernetTap.hpp
        MacKextEthernetTap.cpp
        MacKextEthernetTap.hpp
        MacDNSHelper.mm
        MacDNSHelper.hpp)
endif()

if(WIN32)
    list(APPEND SRC_FILES
        WindowsEthernetTap.cpp
        WindowsEthernetTap.hpp
        WinDNSHelper.cpp
        WinDNSHelper.hpp
        WinFWHelper.cpp
        WinFWHelper.hpp)
endif()

add_library(zerotier-osdep STATIC ${SRC_FILES})
target_link_libraries(zerotier-osdep
    PRIVATE 
        nlohmann_json::nlohmann_json
        Threads::Threads
        prometheus-cpp-lite
        Threads::Threads
        miniupnpc::miniupnpc
        natpmp
)
