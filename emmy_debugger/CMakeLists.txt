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
        src/api/lua_abi_descriptor.cpp
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
        src/debugger/hook_dispatcher.cpp
        src/debugger/extension_point.cpp

        #src/vm
        src/vm/vm_registry.cpp
        src/vm/host_vm_registry.cpp
        src/vm/host_value_provider.cpp
        src/vm/source_registry.cpp

        #src/proto
        src/proto/proto.cpp
        src/proto/proto_handler.cpp
        src/proto/protocol_session.cpp
        src/proto/protocol_v2.cpp
        src/proto/restricted_eval.cpp

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
    if(WIN32 AND NOT EMMY_USE_LUA_SOURCE)
        add_executable(emmy_dynamic_lua_loader_test ${emmy_SOURCE_DIR}/tests/dynamic_lua_loader_test.cpp)
        target_link_libraries(emmy_dynamic_lua_loader_test PRIVATE emmy_debugger)
    endif()
    add_executable(emmy_source_registry_test ${emmy_SOURCE_DIR}/tests/source_registry_test.cpp src/vm/source_registry.cpp)
    target_include_directories(emmy_source_registry_test PRIVATE ${emmy_debugger_SOURCE_DIR}/include)
    add_test(NAME emmy_source_registry_test COMMAND emmy_source_registry_test)
    add_executable(emmy_transporter_concurrency_test ${emmy_SOURCE_DIR}/tests/transporter_concurrency_test.cpp)
    target_link_libraries(emmy_transporter_concurrency_test PRIVATE emmy_debugger)
    add_test(NAME emmy_transporter_concurrency_test COMMAND emmy_transporter_concurrency_test)
    set_tests_properties(emmy_transporter_concurrency_test PROPERTIES TIMEOUT 30)
    if(WIN32)
        add_test(NAME emmy_transporter_concurrency_pipe_test
                COMMAND emmy_transporter_concurrency_test --pipe)
        set_tests_properties(emmy_transporter_concurrency_pipe_test PROPERTIES TIMEOUT 30)
    endif()
    add_executable(emmy_host_abi_compatibility_test
            ${emmy_SOURCE_DIR}/tests/host_abi_compatibility_test.cpp
            src/api/lua_abi_descriptor.cpp src/api/lua_version.cpp)
    target_include_directories(emmy_host_abi_compatibility_test PRIVATE ${emmy_debugger_SOURCE_DIR}/include)
    add_test(NAME emmy_host_abi_compatibility_test COMMAND emmy_host_abi_compatibility_test)
    add_executable(emmy_lua_abi_descriptor_test
            ${emmy_SOURCE_DIR}/tests/lua_abi_descriptor_test.cpp
            src/api/lua_abi_descriptor.cpp src/api/lua_version.cpp)
    target_include_directories(emmy_lua_abi_descriptor_test PRIVATE ${emmy_debugger_SOURCE_DIR}/include)
    add_test(NAME emmy_lua_abi_descriptor_test COMMAND emmy_lua_abi_descriptor_test)

    add_executable(emmy_host_value_provider_test
            ${emmy_SOURCE_DIR}/tests/host_value_provider_test.cpp src/vm/host_value_provider.cpp)
    target_include_directories(emmy_host_value_provider_test PRIVATE
            ${emmy_debugger_SOURCE_DIR}/include ${emmy_SOURCE_DIR}/third-party/nlohmann/include)
    add_test(NAME emmy_host_value_provider_test COMMAND emmy_host_value_provider_test)
    if(EMMY_USE_LUA_SOURCE AND NOT EMMY_LUA_VERSION STREQUAL "jit")
        add_executable(emmy_native_ide_fixture ${emmy_SOURCE_DIR}/tests/native_ide_fixture.cpp)
        target_link_libraries(emmy_native_ide_fixture PRIVATE emmy_debugger)

        # Real attach fixture: deliberately links only the shared Lua runtime.
        # emmy_tool injects emmy_hook.dll after this process starts, exercising
        # the same symbol discovery and fallback VM registration path as UE.
        add_executable(emmy_attach_lua_fixture ${emmy_SOURCE_DIR}/tests/native_attach_lua_fixture.cpp)
        target_include_directories(emmy_attach_lua_fixture PRIVATE
                ${emmy_SOURCE_DIR}/third-party/${EMMY_LUA_DIR}/src)
        target_link_libraries(emmy_attach_lua_fixture PRIVATE lua${EMMY_LUA_VERSION})
        add_dependencies(emmy_attach_lua_fixture lua${EMMY_LUA_VERSION})
        if(WIN32 AND LUA_BUILD_AS_DLL)
            add_custom_command(TARGET emmy_attach_lua_fixture POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:lua${EMMY_LUA_VERSION}>"
                    "$<TARGET_FILE_DIR:emmy_attach_lua_fixture>")
        endif()

        add_executable(emmy_native_socket_harness ${emmy_SOURCE_DIR}/tests/native_socket_harness.cpp)
        target_link_libraries(emmy_native_socket_harness PRIVATE emmy_debugger)
        add_test(NAME emmy_native_socket_harness COMMAND emmy_native_socket_harness)
        set_tests_properties(emmy_native_socket_harness PROPERTIES TIMEOUT 30)
        if(WIN32)
            add_test(NAME emmy_native_pipe_harness COMMAND emmy_native_socket_harness --pipe)
            set_tests_properties(emmy_native_pipe_harness PROPERTIES TIMEOUT 30)
        endif()
        add_executable(emmy_hook_dispatcher_test ${emmy_SOURCE_DIR}/tests/hook_dispatcher_test.cpp)
        target_link_libraries(emmy_hook_dispatcher_test PRIVATE emmy_debugger)
        add_test(NAME emmy_hook_dispatcher_test COMMAND emmy_hook_dispatcher_test)
        set_tests_properties(emmy_hook_dispatcher_test PROPERTIES TIMEOUT 20)
        add_executable(emmy_vm_lifecycle_harness ${emmy_SOURCE_DIR}/tests/vm_lifecycle_harness.cpp)
        target_link_libraries(emmy_vm_lifecycle_harness PRIVATE emmy_debugger)
        add_test(NAME emmy_vm_lifecycle_harness COMMAND emmy_vm_lifecycle_harness)
        set_tests_properties(emmy_vm_lifecycle_harness PROPERTIES TIMEOUT 20)
    endif()
    add_executable(emmy_vm_registry_test
            ${emmy_SOURCE_DIR}/tests/vm_registry_test.cpp
            src/api/lua_abi_descriptor.cpp
            src/api/lua_version.cpp
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
            src/api/lua_abi_descriptor.cpp
            src/api/lua_version.cpp
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

    add_executable(emmy_protocol_session_test
            ${emmy_SOURCE_DIR}/tests/protocol_session_test.cpp
            src/proto/protocol_session.cpp
    )
    target_include_directories(emmy_protocol_session_test PRIVATE
            ${emmy_debugger_SOURCE_DIR}/include
    )
    set_target_properties(emmy_protocol_session_test PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
    add_test(NAME emmy_protocol_session_test COMMAND emmy_protocol_session_test)

    add_executable(emmy_restricted_eval_test
            ${emmy_SOURCE_DIR}/tests/restricted_eval_test.cpp
            src/proto/restricted_eval.cpp
    )
    target_include_directories(emmy_restricted_eval_test PRIVATE
            ${emmy_debugger_SOURCE_DIR}/include
            ${emmy_SOURCE_DIR}/third-party/nlohmann/include
    )
    set_target_properties(emmy_restricted_eval_test PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
    add_test(NAME emmy_restricted_eval_test COMMAND emmy_restricted_eval_test)

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

    if(WIN32)
        add_executable(emmy_injection_shared_memory_test
                ${emmy_SOURCE_DIR}/tests/injection_shared_memory_test.cpp
                ${emmy_SOURCE_DIR}/shared/src/shme.cpp)
        target_include_directories(emmy_injection_shared_memory_test PRIVATE ${emmy_SOURCE_DIR}/shared/include)
        add_test(NAME emmy_injection_shared_memory_test COMMAND emmy_injection_shared_memory_test)
        set_tests_properties(emmy_injection_shared_memory_test PROPERTIES TIMEOUT 20)
        add_executable(emmy_hook_manager_test
                ${emmy_SOURCE_DIR}/tests/hook_manager_test.cpp
                ${emmy_SOURCE_DIR}/emmy_hook/src/hook_manager.cpp
        )
        target_include_directories(emmy_hook_manager_test PRIVATE
                ${emmy_SOURCE_DIR}/emmy_hook/src
        )
        set_target_properties(emmy_hook_manager_test PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
        add_test(NAME emmy_hook_manager_test COMMAND emmy_hook_manager_test)
        set_tests_properties(emmy_hook_manager_test PROPERTIES WORKING_DIRECTORY ${emmy_SOURCE_DIR})

        add_executable(emmy_native_contract_test
                ${emmy_SOURCE_DIR}/tests/native_contract_test.cpp
                ${emmy_SOURCE_DIR}/emmy_hook/src/hook_manager.cpp
                src/api/lua_abi_descriptor.cpp
                src/api/lua_version.cpp
                src/vm/host_value_provider.cpp
        )
        target_include_directories(emmy_native_contract_test PRIVATE
                ${emmy_debugger_SOURCE_DIR}/include
                ${emmy_SOURCE_DIR}/emmy_hook/src
        )
        set_target_properties(emmy_native_contract_test PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
        add_test(NAME emmy_native_contract_test COMMAND emmy_native_contract_test)
        set_tests_properties(emmy_native_contract_test PROPERTIES WORKING_DIRECTORY ${emmy_SOURCE_DIR})
    endif()

    if(NOT WIN32)
        add_executable(emmy_native_contract_test
                ${emmy_SOURCE_DIR}/tests/native_contract_test.cpp
                ${emmy_SOURCE_DIR}/emmy_hook/src/hook_manager.cpp
                src/api/lua_abi_descriptor.cpp
                src/api/lua_version.cpp
                src/vm/host_value_provider.cpp
        )
        target_include_directories(emmy_native_contract_test PRIVATE
                ${emmy_debugger_SOURCE_DIR}/include
                ${emmy_SOURCE_DIR}/emmy_hook/src
        )
        set_target_properties(emmy_native_contract_test PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
        add_test(NAME emmy_native_contract_test COMMAND emmy_native_contract_test)
        set_tests_properties(emmy_native_contract_test PROPERTIES WORKING_DIRECTORY ${emmy_SOURCE_DIR})
    endif()
    set_tests_properties(
            emmy_vm_registry_test
            emmy_protocol_v2_test
            emmy_protocol_session_test
            emmy_restricted_eval_test
            emmy_transporter_frame_test
            emmy_debugger_isolation_test
            emmy_native_contract_test
            PROPERTIES WORKING_DIRECTORY ${emmy_SOURCE_DIR}
    )
    target_include_directories(emmy_native_contract_test PRIVATE ${emmy_SOURCE_DIR}/third-party/nlohmann/include)
endif()
