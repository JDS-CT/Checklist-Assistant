cmake_minimum_required(VERSION 3.28)

project(checklist-assistant-server VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

enable_testing()

if (WIN32)
  enable_language(RC)
  set(CHAX_CHECKLIST_ASSISTANT_PLATFORM_SOURCES
    src/platform/windows/checklist_assistant.rc
  )
endif()

option(CHAX_ENFORCE_CLANG_TIDY "Require clang-tidy and run it during C++ builds." ON)

set(CHAX_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

if (MSVC)
  set(CHAX_WARNINGS /W4 /permissive-)
else()
  set(CHAX_WARNINGS -Wall -Wextra -Wpedantic)
endif()

set(CHAX_CLANG_TIDY_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/.clang-tidy")
get_filename_component(CHAX_COMPILER_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
find_program(
  CHAX_CLANG_TIDY_EXECUTABLE
  NAMES clang-tidy clang-tidy.exe
  HINTS ${CHAX_COMPILER_BIN_DIR}
)

if (CHAX_ENFORCE_CLANG_TIDY)
  if (NOT EXISTS "${CHAX_CLANG_TIDY_CONFIG_FILE}")
    message(FATAL_ERROR "CHAX_ENFORCE_CLANG_TIDY=ON requires ${CHAX_CLANG_TIDY_CONFIG_FILE}.")
  endif()
  if (NOT CHAX_CLANG_TIDY_EXECUTABLE)
    message(FATAL_ERROR "CHAX_ENFORCE_CLANG_TIDY=ON but clang-tidy was not found on PATH.")
  endif()
endif()

if (CHAX_CLANG_TIDY_EXECUTABLE)
  set(CMAKE_CXX_CLANG_TIDY
    ${CHAX_CLANG_TIDY_EXECUTABLE}
    --config-file=${CHAX_CLANG_TIDY_CONFIG_FILE}
    --extra-arg=-Wno-unknown-warning-option
  )
endif()

function(chax_stage_mingw_runtime target_name)
  if (WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    get_filename_component(_chax_compiler_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(_chax_runtime_dll IN ITEMS libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
      set(_chax_runtime_dll_path "${_chax_compiler_bin_dir}/${_chax_runtime_dll}")
      if (EXISTS "${_chax_runtime_dll_path}")
        add_custom_command(
          TARGET ${target_name}
          POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_chax_runtime_dll_path}"
            "$<TARGET_FILE_DIR:${target_name}>"
          VERBATIM
        )
      else()
        message(WARNING "MinGW runtime DLL not found for ${target_name}: ${_chax_runtime_dll_path}")
      endif()
    endforeach()
  endif()
endfunction()

add_library(apim-sqlite3 STATIC
  third_party/sqlite/sqlite3.c
)
target_include_directories(apim-sqlite3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite)
target_compile_definitions(apim-sqlite3 PUBLIC SQLITE_OMIT_LOAD_EXTENSION SQLITE_THREADSAFE=1)
target_compile_options(apim-sqlite3 PRIVATE ${CHAX_WARNINGS})

add_library(apim-xxhash STATIC
  third_party/xxhash/xxhash.c
)
target_include_directories(apim-xxhash PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xxhash)
target_compile_options(apim-xxhash PRIVATE ${CHAX_WARNINGS})

add_library(chax-mcp STATIC
  src/core/mcp_bridge.cpp
  src/platform/http_client.cpp
)

target_include_directories(chax-mcp
  PUBLIC
    ${CHAX_INCLUDE_DIRS}
)

target_compile_options(chax-mcp PUBLIC ${CHAX_WARNINGS})

add_executable(checklist_assistant_server
  src/core/app.cpp
  src/core/asset_pack.cpp
  src/core/checklist_markdown.cpp
  src/core/checklist_store.cpp
  src/core/logging.cpp
  src/core/oauth.cpp
  src/core/report_generator.cpp
  src/core/main.cpp
  src/platform/http_client.cpp
  src/platform/http_server.cpp
  src/platform/system.cpp
)

target_include_directories(checklist_assistant_server
  PRIVATE
    ${CHAX_INCLUDE_DIRS}
)

target_compile_options(checklist_assistant_server PRIVATE ${CHAX_WARNINGS})
target_link_libraries(checklist_assistant_server PRIVATE apim-sqlite3 apim-xxhash)

if (WIN32)
  target_link_libraries(checklist_assistant_server PRIVATE ws2_32 iphlpapi)
endif()

if (WIN32)
  add_executable(checklist_assistant WIN32
    src/tools/checklist_assistant.cpp
    src/platform/system.cpp
    ${CHAX_CHECKLIST_ASSISTANT_PLATFORM_SOURCES}
  )
else()
  add_executable(checklist_assistant
    src/tools/checklist_assistant.cpp
    src/platform/system.cpp
  )
endif()
target_include_directories(checklist_assistant PRIVATE ${CHAX_INCLUDE_DIRS})
target_compile_options(checklist_assistant PRIVATE ${CHAX_WARNINGS})

add_executable(chax-mcp-bridge
  src/tools/chax_mcp_bridge.cpp
)

target_link_libraries(chax-mcp-bridge PRIVATE chax-mcp)
target_compile_options(chax-mcp-bridge PRIVATE ${CHAX_WARNINGS})
target_include_directories(chax-mcp-bridge PRIVATE ${CHAX_INCLUDE_DIRS})

add_executable(unit-test-controller
  src/tools/unit_test_controller.cpp
  src/platform/http_client.cpp
)
target_include_directories(unit-test-controller PRIVATE ${CHAX_INCLUDE_DIRS})
target_compile_options(unit-test-controller PRIVATE ${CHAX_WARNINGS})
if (WIN32)
  target_link_libraries(unit-test-controller PRIVATE ws2_32 iphlpapi)
endif()

add_executable(mcp-bridge-test
  tests/mcp_bridge_test.cpp
  src/core/app.cpp
  src/core/asset_pack.cpp
  src/core/checklist_markdown.cpp
  src/core/checklist_store.cpp
  src/core/logging.cpp
  src/core/oauth.cpp
  src/core/report_generator.cpp
  src/platform/http_server.cpp
  src/platform/system.cpp
)

target_link_libraries(mcp-bridge-test PRIVATE chax-mcp)
target_include_directories(mcp-bridge-test PRIVATE ${CHAX_INCLUDE_DIRS})
target_compile_options(mcp-bridge-test PRIVATE ${CHAX_WARNINGS})
target_link_libraries(mcp-bridge-test PRIVATE apim-sqlite3 apim-xxhash)

add_executable(mcp-voice-helper-test
  tests/mcp_voice_helper_test.cpp
)
target_compile_options(mcp-voice-helper-test PRIVATE ${CHAX_WARNINGS})

add_executable(report-generation-test
  tests/report_generation_test.cpp
  src/core/asset_pack.cpp
  src/core/report_generator.cpp
  src/core/checklist_store.cpp
  src/core/logging.cpp
)
target_include_directories(report-generation-test PRIVATE ${CHAX_INCLUDE_DIRS})
target_compile_options(report-generation-test PRIVATE ${CHAX_WARNINGS})
target_link_libraries(report-generation-test PRIVATE apim-sqlite3 apim-xxhash)
if (WIN32)
  target_link_libraries(report-generation-test PRIVATE ws2_32 iphlpapi)
endif()

add_executable(markdown-compat-test
  tests/markdown_compat_test.cpp
  src/core/checklist_markdown.cpp
  src/core/asset_pack.cpp
  src/core/checklist_store.cpp
  src/core/logging.cpp
)
target_include_directories(markdown-compat-test PRIVATE ${CHAX_INCLUDE_DIRS})
target_compile_options(markdown-compat-test PRIVATE ${CHAX_WARNINGS})
target_link_libraries(markdown-compat-test PRIVATE apim-sqlite3 apim-xxhash)
if (WIN32)
  target_link_libraries(markdown-compat-test PRIVATE ws2_32 iphlpapi)
endif()

add_executable(markdown-relationships-test
  tests/markdown_relationships_test.cpp
  src/core/checklist_markdown.cpp
  src/core/asset_pack.cpp
  src/core/checklist_store.cpp
  src/core/logging.cpp
)
target_include_directories(markdown-relationships-test PRIVATE ${CHAX_INCLUDE_DIRS})
target_compile_options(markdown-relationships-test PRIVATE ${CHAX_WARNINGS})
target_link_libraries(markdown-relationships-test PRIVATE apim-sqlite3 apim-xxhash)
if (WIN32)
  target_link_libraries(markdown-relationships-test PRIVATE ws2_32 iphlpapi)
endif()

if (WIN32)
  target_link_libraries(chax-mcp-bridge PRIVATE ws2_32 iphlpapi)
  target_link_libraries(mcp-bridge-test PRIVATE ws2_32 iphlpapi)
endif()

add_test(NAME mcp-bridge COMMAND mcp-bridge-test)
set_tests_properties(mcp-bridge PROPERTIES LABELS "smoke")
add_test(NAME mcp-voice-helper COMMAND mcp-voice-helper-test)
set_tests_properties(mcp-voice-helper PROPERTIES LABELS "docs")
add_test(NAME report-generation COMMAND report-generation-test)
set_tests_properties(report-generation PROPERTIES LABELS "smoke")
add_test(NAME markdown-compat COMMAND markdown-compat-test)
set_tests_properties(markdown-compat PROPERTIES LABELS "smoke")
add_test(NAME markdown-relationships COMMAND markdown-relationships-test)
set_tests_properties(markdown-relationships PROPERTIES LABELS "smoke")

add_executable(integration-schema-test
  tests/integration_schema_test.cpp
  src/core/asset_pack.cpp
  src/core/checklist_store.cpp
  src/core/logging.cpp
)
target_include_directories(integration-schema-test PRIVATE ${CHAX_INCLUDE_DIRS})
target_compile_options(integration-schema-test PRIVATE ${CHAX_WARNINGS})
target_link_libraries(integration-schema-test PRIVATE apim-sqlite3 apim-xxhash)
if (WIN32)
  target_link_libraries(integration-schema-test PRIVATE ws2_32 iphlpapi)
endif()

add_test(NAME integration-schema COMMAND integration-schema-test)
set_tests_properties(integration-schema PROPERTIES LABELS "smoke")

add_executable(http-api-test
  tests/http_api_test.cpp
  src/core/app.cpp
  src/core/asset_pack.cpp
  src/core/checklist_markdown.cpp
  src/core/checklist_store.cpp
  src/core/logging.cpp
  src/core/oauth.cpp
  src/core/report_generator.cpp
  src/platform/http_server.cpp
  src/platform/system.cpp
)

target_include_directories(http-api-test PRIVATE ${CHAX_INCLUDE_DIRS})
target_compile_options(http-api-test PRIVATE ${CHAX_WARNINGS})
target_link_libraries(http-api-test PRIVATE chax-mcp apim-sqlite3 apim-xxhash)
if (WIN32)
  target_link_libraries(http-api-test PRIVATE ws2_32 iphlpapi)
endif()

add_test(NAME http-api COMMAND http-api-test)
set_tests_properties(http-api PROPERTIES LABELS "all")

add_executable(predicate-daemon-exhaustive-test
  tests/predicate_daemon_exhaustive_test.cpp
  src/core/checklist_markdown.cpp
  src/core/asset_pack.cpp
  src/core/checklist_store.cpp
  src/core/logging.cpp
)

target_include_directories(predicate-daemon-exhaustive-test PRIVATE ${CHAX_INCLUDE_DIRS})
target_compile_options(predicate-daemon-exhaustive-test PRIVATE ${CHAX_WARNINGS})
target_link_libraries(predicate-daemon-exhaustive-test PRIVATE apim-sqlite3 apim-xxhash)

add_test(NAME predicate-daemon-exhaustive COMMAND predicate-daemon-exhaustive-test)
set_tests_properties(predicate-daemon-exhaustive PROPERTIES LABELS "all")

add_executable(predicate-daemon-exhaustive-csv-test
  tests/predicate_daemon_exhaustive_csv_test.cpp
  src/core/asset_pack.cpp
  src/core/checklist_store.cpp
  src/core/logging.cpp
)

target_include_directories(predicate-daemon-exhaustive-csv-test PRIVATE ${CHAX_INCLUDE_DIRS})
target_compile_options(predicate-daemon-exhaustive-csv-test PRIVATE ${CHAX_WARNINGS})
target_link_libraries(predicate-daemon-exhaustive-csv-test PRIVATE apim-sqlite3 apim-xxhash)

add_test(NAME predicate-daemon-exhaustive-csv COMMAND predicate-daemon-exhaustive-csv-test)
set_tests_properties(predicate-daemon-exhaustive-csv PROPERTIES LABELS "all")

add_executable(report-api-flow-test
  tests/report_api_flow_test.cpp
  src/core/app.cpp
  src/core/asset_pack.cpp
  src/core/checklist_markdown.cpp
  src/core/checklist_store.cpp
  src/core/logging.cpp
  src/core/oauth.cpp
  src/core/report_generator.cpp
  src/platform/http_server.cpp
  src/platform/system.cpp
)
target_include_directories(report-api-flow-test PRIVATE ${CHAX_INCLUDE_DIRS})
target_compile_options(report-api-flow-test PRIVATE ${CHAX_WARNINGS})
target_link_libraries(report-api-flow-test PRIVATE chax-mcp apim-sqlite3 apim-xxhash)
if (WIN32)
  target_link_libraries(report-api-flow-test PRIVATE ws2_32 iphlpapi)
endif()

add_test(NAME report-api-flow COMMAND report-api-flow-test)
set_tests_properties(report-api-flow PROPERTIES LABELS "smoke")

add_executable(oauth-flow-test
  tests/oauth_flow_test.cpp
  src/core/app.cpp
  src/core/asset_pack.cpp
  src/core/checklist_markdown.cpp
  src/core/checklist_store.cpp
  src/core/logging.cpp
  src/core/oauth.cpp
  src/core/report_generator.cpp
  src/platform/http_server.cpp
  src/platform/system.cpp
)
target_include_directories(oauth-flow-test PRIVATE ${CHAX_INCLUDE_DIRS})
target_compile_options(oauth-flow-test PRIVATE ${CHAX_WARNINGS})
target_link_libraries(oauth-flow-test PRIVATE chax-mcp apim-sqlite3 apim-xxhash)
if (WIN32)
  target_link_libraries(oauth-flow-test PRIVATE ws2_32 iphlpapi)
endif()
add_test(NAME oauth-flow COMMAND oauth-flow-test)
set_tests_properties(oauth-flow PROPERTIES LABELS "auth")

add_executable(oauth-authorize-ui-test
  tests/oauth_authorize_ui_test.cpp
  src/core/app.cpp
  src/core/asset_pack.cpp
  src/core/checklist_markdown.cpp
  src/core/checklist_store.cpp
  src/core/logging.cpp
  src/core/oauth.cpp
  src/core/report_generator.cpp
  src/platform/http_client.cpp
  src/platform/http_server.cpp
  src/platform/system.cpp
)
target_include_directories(oauth-authorize-ui-test PRIVATE ${CHAX_INCLUDE_DIRS})
target_compile_options(oauth-authorize-ui-test PRIVATE ${CHAX_WARNINGS})
target_link_libraries(oauth-authorize-ui-test PRIVATE chax-mcp apim-sqlite3 apim-xxhash)
if (WIN32)
  target_link_libraries(oauth-authorize-ui-test PRIVATE ws2_32 iphlpapi)
endif()
add_test(NAME oauth-authorize-ui COMMAND oauth-authorize-ui-test)
set_tests_properties(oauth-authorize-ui PROPERTIES LABELS "auth")

find_program(POWERSHELL_EXECUTABLE NAMES pwsh pwsh.exe powershell powershell.exe)
if (POWERSHELL_EXECUTABLE)
  add_test(NAME e2e-smoke
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/scripts/e2e_smoke_test.ps1)
  set_tests_properties(e2e-smoke PROPERTIES LABELS "e2e")
  add_test(NAME server-start-stop
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/tools/automation/smoke_server_start_stop.ps1)
  set_tests_properties(server-start-stop PROPERTIES LABELS "smoke")
else()
  message(WARNING "PowerShell not found; smoke automation tests disabled")
endif()

add_executable(oauth-store-repro-test
  tests/oauth_store_repro_test.cpp
  src/core/logging.cpp
  src/core/oauth.cpp
)
target_include_directories(oauth-store-repro-test PRIVATE ${CHAX_INCLUDE_DIRS})
target_compile_options(oauth-store-repro-test PRIVATE ${CHAX_WARNINGS})
target_link_libraries(oauth-store-repro-test PRIVATE apim-sqlite3 apim-xxhash)
if (WIN32)
  target_link_libraries(oauth-store-repro-test PRIVATE ws2_32 iphlpapi)
endif()
add_test(NAME oauth-store-repro COMMAND oauth-store-repro-test)
set_tests_properties(oauth-store-repro PROPERTIES LABELS "auth")

foreach(chax_executable_target IN ITEMS
  checklist_assistant_server
  checklist_assistant
  chax-mcp-bridge
  unit-test-controller
  mcp-bridge-test
  mcp-voice-helper-test
  report-generation-test
  markdown-compat-test
  markdown-relationships-test
  integration-schema-test
  http-api-test
  predicate-daemon-exhaustive-test
  predicate-daemon-exhaustive-csv-test
  report-api-flow-test
  oauth-flow-test
  oauth-authorize-ui-test
  oauth-store-repro-test
)
  chax_stage_mingw_runtime(${chax_executable_target})
endforeach()
