cmake_minimum_required(VERSION 3.20)
project(traff_mirror VERSION 0.2.0 LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/bin")
endforeach()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(MIRROR_LINUX_DEFAULT ON)
else()
    set(MIRROR_LINUX_DEFAULT OFF)
endif()
option(MIRROR_BUILD_RECEIVER "Build the Linux AF_PACKET receiver" ${MIRROR_LINUX_DEFAULT})
option(MIRROR_BUILD_TEST_TOOLS "Build traffic completeness test tools" ${MIRROR_LINUX_DEFAULT})

find_package(OpenSSL REQUIRED)
if(NOT WIN32)
    find_package(Threads REQUIRED)
endif()

function(mirror_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /experimental:c11atomics)
        target_compile_definitions(${target} PRIVATE
            _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
            _WIN32_WINNT=0x0602)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
    endif()
endfunction()

set(MIRROR_COMMON_SOURCES
    src/common/config.c
    src/common/protocol.c
    src/common/tls.c
    src/common/util.c
    src/common/vlan.c)

add_library(mirror_common STATIC ${MIRROR_COMMON_SOURCES})
target_include_directories(mirror_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(mirror_common PUBLIC OpenSSL::SSL OpenSSL::Crypto)
if(WIN32)
    target_link_libraries(mirror_common PUBLIC ws2_32 iphlpapi)
else()
    target_link_libraries(mirror_common PUBLIC Threads::Threads)
endif()
mirror_warnings(mirror_common)

set(MIRROR_AGENT_SOURCES
    src/agent/main.c
    src/agent/platform.c
    src/agent/sync.c
    src/agent/segment.c
    src/agent/queue.c
    src/agent/batch.c
    src/agent/self_filter.c)

if(WIN32)
    find_package(Npcap REQUIRED)
    list(APPEND MIRROR_AGENT_SOURCES src/agent/capture_npcap.c)
else()
    list(APPEND MIRROR_AGENT_SOURCES src/agent/capture.c)
endif()

add_executable(mirror-agent ${MIRROR_AGENT_SOURCES})
target_include_directories(mirror-agent PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(mirror-agent PRIVATE mirror_common)
if(WIN32)
    target_link_libraries(mirror-agent PRIVATE Npcap::Npcap psapi bcrypt)
else()
    target_link_libraries(mirror-agent PRIVATE Threads::Threads)
endif()
mirror_warnings(mirror-agent)

if(MIRROR_BUILD_RECEIVER)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "mirror-receiver requires Linux AF_PACKET")
    endif()
    find_path(XXHASH_INCLUDE_DIR xxhash.h REQUIRED)
    find_library(XXHASH_LIBRARY NAMES xxhash REQUIRED)
    add_executable(mirror-receiver
        src/receiver/main.c
        src/receiver/client.c
        src/receiver/output.c
        src/receiver/pipeline.c
        src/receiver/fingerprint.c
        src/receiver/dedup.c
        src/receiver/checksum.c
        src/agent/segment.c)
    target_include_directories(mirror-receiver PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include" "${XXHASH_INCLUDE_DIR}")
    target_link_libraries(mirror-receiver PRIVATE
        mirror_common "${XXHASH_LIBRARY}" Threads::Threads)
    mirror_warnings(mirror-receiver)
endif()

if(MIRROR_BUILD_TEST_TOOLS)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "Traffic test tools currently require Linux")
    endif()
    if(NOT XXHASH_LIBRARY)
        find_path(XXHASH_INCLUDE_DIR xxhash.h REQUIRED)
        find_library(XXHASH_LIBRARY NAMES xxhash REQUIRED)
    endif()
    add_library(mirror_test_traffic STATIC src/test/traffic.c)
    target_include_directories(mirror_test_traffic PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include" "${XXHASH_INCLUDE_DIR}")
    target_link_libraries(mirror_test_traffic PUBLIC "${XXHASH_LIBRARY}")
    mirror_warnings(mirror_test_traffic)
    foreach(tool sender sink verifier)
        add_executable(mirror-test-${tool} src/test/${tool}.c)
        target_link_libraries(mirror-test-${tool} PRIVATE mirror_test_traffic)
        mirror_warnings(mirror-test-${tool})
    endforeach()
endif()

include(CTest)
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    function(mirror_add_test name)
        add_executable(test_${name} tests/test_${name}.c ${ARGN})
        target_include_directories(test_${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
        if(MSVC)
            target_compile_options(test_${name} PRIVATE /UNDEBUG)
        else()
            target_compile_options(test_${name} PRIVATE -UNDEBUG)
        endif()
        mirror_warnings(test_${name})
        add_test(NAME ${name} COMMAND test_${name})
    endfunction()

    mirror_add_test(protocol)
    target_link_libraries(test_protocol PRIVATE mirror_common)

    mirror_add_test(queue src/agent/queue.c src/agent/sync.c)
    target_link_libraries(test_queue PRIVATE mirror_common Threads::Threads)

    mirror_add_test(batch src/agent/batch.c src/agent/queue.c src/agent/sync.c)
    target_link_libraries(test_batch PRIVATE mirror_common Threads::Threads)

    mirror_add_test(vlan)
    target_link_libraries(test_vlan PRIVATE mirror_common)

    mirror_add_test(self_filter src/agent/self_filter.c src/agent/sync.c)
    target_link_libraries(test_self_filter PRIVATE mirror_common Threads::Threads)

    mirror_add_test(config)
    target_link_libraries(test_config PRIVATE mirror_common)

    mirror_add_test(fingerprint src/receiver/fingerprint.c)
    target_include_directories(test_fingerprint PRIVATE "${XXHASH_INCLUDE_DIR}")
    target_link_libraries(test_fingerprint PRIVATE "${XXHASH_LIBRARY}")

    mirror_add_test(dedup src/receiver/dedup.c)
    target_include_directories(test_dedup PRIVATE "${XXHASH_INCLUDE_DIR}")
    target_link_libraries(test_dedup PRIVATE mirror_common "${XXHASH_LIBRARY}" Threads::Threads)

    mirror_add_test(checksum src/receiver/checksum.c)
    mirror_add_test(segment src/agent/segment.c)

    mirror_add_test(pipeline
        src/receiver/pipeline.c src/receiver/output.c)
    target_link_libraries(test_pipeline PRIVATE mirror_common Threads::Threads)

    mirror_add_test(traffic)
    target_link_libraries(test_traffic PRIVATE mirror_test_traffic)

    add_executable(test_output_veth
        tests/test_output_veth.c src/receiver/output.c)
    target_include_directories(test_output_veth PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(test_output_veth PRIVATE mirror_common)
    target_compile_options(test_output_veth PRIVATE -UNDEBUG)
    mirror_warnings(test_output_veth)
    add_custom_target(integration-test
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/mirror-interface-setup.sh"
        COMMAND $<TARGET_FILE:test_output_veth>
        DEPENDS test_output_veth
        USES_TERMINAL)
endif()

include(GNUInstallDirs)
install(TARGETS mirror-agent RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
if(TARGET mirror-receiver)
    install(TARGETS mirror-receiver RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
endif()
if(TARGET mirror-test-sender)
    install(TARGETS mirror-test-sender mirror-test-sink mirror-test-verifier
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(WIN32)
    install(FILES examples/agent-windows.conf
        DESTINATION . RENAME mirror-agent.conf)
else()
    install(FILES examples/agent.conf
        DESTINATION /etc/mirror-agent RENAME agent.conf.example)
    if(TARGET mirror-receiver)
        install(FILES examples/receiver.conf
            DESTINATION /etc/mirror-receiver RENAME receiver.conf.example)
        install(PROGRAMS scripts/mirror-interface-setup.sh DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
        install(FILES systemd/mirror-agent.service systemd/mirror-interface.service
                      systemd/mirror-receiver.service
                DESTINATION /etc/systemd/system)
    endif()
endif()
