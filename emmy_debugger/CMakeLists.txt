cmake_minimum_required(VERSION 3.14)

project(emmy_debugger)

add_library(emmy_debugger STATIC)

add_dependencies(
        emmy_debugger
        uv_a
)

target_include_directories(emmy_debugger PUBLIC
        ${emmy_debugger_SOURCE_DIR}/include
        ${emmy_SOURCE_DIR}/third-party/nlohmann/include
        ${emmy_SOURCE_DIR}/third-party/libuv-1.46.0/include
        PRIVATE src
)

target_sources(emmy_debugger
        PRIVATE
        #src/api
        src/api/lua_api.cpp
        src/api/lua_state.cpp
        src/api/lua_version.cpp

        #src/transporter
        src/transporter/pipeline_client_transporter.cpp
        src/transporter/pipeline_server_transporter.cpp
        src/transporter/socket_client_transporter.cpp
        src/transporter/socket_server_transporter.cpp
        src/transporter/transporter.cpp
        src/transporter/transport_auth.cpp

        #src/debugger
        src/debugger/emmy_debugger.cpp
        src/debugger/emmy_debugger_manager.cpp
        src/debugger/emmy_debugger_lib.cpp
        src/debugger/hook_state.cpp
        src/debugger/extension_point.cpp

        #src/vm
        src/vm/vm_registry.cpp
        src/vm/host_vm_registry.cpp

        #src/proto
        src/proto/proto.cpp
        src/proto/proto_handler.cpp
        src/proto/protocol_session.cpp
        src/proto/protocol_v2.cpp

        #src/arena
        src/arena/arena.cpp

        #src
        src/emmy_facade.cpp
        src/util.cpp
)


if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(emmy_debugger PUBLIC /utf-8)
endif ()


if (NOT WIN32)
    target_compile_options(emmy_debugger PUBLIC -fPIC)
endif ()

if (EMMY_USE_LUA_SOURCE)
    target_compile_definitions(emmy_debugger PUBLIC -DEMMY_USE_LUA_SOURCE)
    target_include_directories(emmy_debugger PUBLIC
            ${emmy_SOURCE_DIR}/third-party/${EMMY_LUA_DIR}/src
    )
    if (${EMMY_LUA_VERSION} STREQUAL "jit")
        target_link_directories(emmy_debugger PUBLIC
                ${emmy_SOURCE_DIR}/third-party/${EMMY_LUA_DIR}/src
        )
        target_link_libraries(
                emmy_debugger
                PRIVATE lua51.lib
        )
        target_sources(emmy_debugger
                PRIVATE
                src/api/lua_state/lua_state_jit.cpp
        )
    else ()
        add_dependencies(emmy_debugger lua${EMMY_LUA_VERSION})
        target_link_libraries(
                emmy_debugger
                PUBLIC lua${EMMY_LUA_VERSION}
        )
        target_sources(emmy_debugger
                PRIVATE
                src/api/lua_state/lua_state_${EMMY_LUA_VERSION}.cpp
        )
    endif ()


elseif (EMMY_COMPILE_AS_LIB)
    message("emmy build as lib")
    target_compile_definitions(emmy_debugger PUBLIC -DEMMY_USE_LUA_SOURCE)
    target_sources(emmy_debugger
            PRIVATE
            src/api/lua_state/lua_state_${EMMY_LUA_VERSION}.cpp
    )
else ()
    target_include_directories(emmy_debugger PRIVATE
            ${emmy_SOURCE_DIR}/third-party
    )

    target_sources(emmy_debugger
            PUBLIC
            src/api/lua_api_loader.cpp

            PRIVATE
            src/api/lua_state/lua_state_54.cpp
            src/api/lua_state/lua_state_53.cpp
            src/api/lua_state/lua_state_52.cpp
            src/api/lua_state/lua_state_51.cpp
            src/api/lua_state/lua_state_jit.cpp
    )

endif (EMMY_USE_LUA_SOURCE)


target_link_libraries(
        emmy_debugger
        PUBLIC uv_a
)

if(EMMY_VM_REGISTRY_TEST)
    add_executable(emmy_vm_registry_test
            ${emmy_SOURCE_DIR}/tests/vm_registry_test.cpp
            src/vm/vm_registry.cpp
            src/vm/host_vm_registry.cpp
    )
    target_include_directories(emmy_vm_registry_test PRIVATE
            ${emmy_debugger_SOURCE_DIR}/include
    )
    set_target_properties(emmy_vm_registry_test PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
    add_test(NAME emmy_vm_registry_test COMMAND emmy_vm_registry_test)

    add_executable(emmy_protocol_v2_test
            ${emmy_SOURCE_DIR}/tests/protocol_v2_test.cpp
            src/proto/protocol_v2.cpp
            src/proto/protocol_session.cpp
            src/transporter/transport_auth.cpp
    )
    target_include_directories(emmy_protocol_v2_test PRIVATE
            ${emmy_debugger_SOURCE_DIR}/include
            ${emmy_SOURCE_DIR}/third-party/nlohmann/include
            ${emmy_SOURCE_DIR}/third-party/libuv-1.46.0/include
    )
    set_target_properties(emmy_protocol_v2_test PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
    add_test(NAME emmy_protocol_v2_test COMMAND emmy_protocol_v2_test)

    add_executable(emmy_transporter_frame_test
            ${emmy_SOURCE_DIR}/tests/transporter_frame_test.cpp
    )
    target_include_directories(emmy_transporter_frame_test PRIVATE
            ${emmy_debugger_SOURCE_DIR}/include
            ${emmy_SOURCE_DIR}/third-party/nlohmann/include
            ${emmy_SOURCE_DIR}/third-party/libuv-1.46.0/include
    )
    target_link_libraries(emmy_transporter_frame_test PRIVATE emmy_debugger)
    add_dependencies(emmy_transporter_frame_test emmy_debugger)
    set_target_properties(emmy_transporter_frame_test PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
    add_test(NAME emmy_transporter_frame_test COMMAND emmy_transporter_frame_test)

    add_executable(emmy_debugger_isolation_test
            ${emmy_SOURCE_DIR}/tests/debugger_isolation_test.cpp
    )
    target_include_directories(emmy_debugger_isolation_test PRIVATE
            ${emmy_debugger_SOURCE_DIR}/include
            ${emmy_SOURCE_DIR}/third-party/nlohmann/include
            ${emmy_SOURCE_DIR}/third-party/libuv-1.46.0/include
    )
    target_link_libraries(emmy_debugger_isolation_test PRIVATE emmy_debugger)
    add_dependencies(emmy_debugger_isolation_test emmy_debugger)
    set_target_properties(emmy_debugger_isolation_test PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
    add_test(NAME emmy_debugger_isolation_test COMMAND emmy_debugger_isolation_test)
    set_tests_properties(
            emmy_vm_registry_test
            emmy_protocol_v2_test
            emmy_transporter_frame_test
            emmy_debugger_isolation_test
            PROPERTIES WORKING_DIRECTORY ${emmy_SOURCE_DIR}
    )
endif()
