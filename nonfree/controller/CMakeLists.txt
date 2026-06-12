cmake_minimum_required(VERSION 3.13)
project(zerotier-controller)


set(SRC_FILES
    DB.cpp
    DB.hpp
    DBMirrorSet.cpp
    DBMirrorSet.hpp
    EmbeddedNetworkController.cpp
    EmbeddedNetworkController.hpp
    FileDB.cpp
    FileDB.hpp
    CtlUtil.cpp
    CtlUtil.hpp
)

set(INCLUDE_DIRS
    "${httplib_SOURCE_DIR}"
    ${RUSTYBITS_INCLUDE_DIR}
)

set(LINK_LIBS
    zerotier-osdep
    nlohmann_json::nlohmann_json 
    opentelemetry-cpp::api
    rustybits
    Threads::Threads
    prometheus-cpp-lite
    Threads::Threads
)

if (ZT1_CENTRAL_CONTROLLER)
    find_package(PostgreSQL REQUIRED)
    find_package(protobuf CONFIG REQUIRED)

    list(APPEND SRC_FILES
        CentralDB.cpp
        CentralDB.hpp
        ControllerConfig.hpp
        ControllerChangeNotifier.cpp
        ControllerChangeNotifier.hpp
        NotificationListener.hpp
        PostgreSQL.cpp
        PostgreSQL.hpp
        PubSubListener.cpp
        PubSubListener.hpp
        PubSubWriter.cpp
        PubSubWriter.hpp
        Redis.hpp
        RedisListener.cpp
        RedisListener.hpp
        StatusWriter.cpp
        StatusWriter.hpp
        BigTableStatusWriter.cpp
        BigTableStatusWriter.hpp
        PostgresStatusWriter.cpp
        PostgresStatusWriter.hpp
        Redis.hpp
        RedisStatusWriter.cpp
        RedisStatusWriter.hpp
    )

    list(APPEND INCLUDE_DIRS
        ${PostgreSQL_INCLUDE_DIRS}
        "${redis++_BUILD_DIR}/src"
        ${pqxx_INCLUDE_DIRS}
        "${CMAKE_CURRENT_BINARY_DIR}" 
    )

    list(APPEND LINK_LIBS
        redis++::redis++_static
        pqxx
        ${PostgreSQL_LIBRARIES}
        google-cloud-cpp::bigtable
        google-cloud-cpp::pubsub
    )
endif()

add_library(zerotier-controller STATIC ${SRC_FILES})

if (ZT1_CENTRAL_CONTROLLER)
    file(GLOB PROTO_FILES "${CMAKE_CURRENT_SOURCE_DIR}/protobuf/*.proto")
    protobuf_generate(
        TARGET zerotier-controller
        LANGUAGE cpp
        PROTOS ${PROTO_FILES}
        APPEND_PATH
    )
endif()

target_include_directories(zerotier-controller PRIVATE ${INCLUDE_DIRS})

add_dependencies(zerotier-controller redis++::redis++)

target_link_libraries(zerotier-controller 
    ${LINK_LIBS}
)
