cmake_minimum_required(VERSION 3.16)

project(HFile VERSION 0.0.1 LANGUAGES C)

add_executable(hf
  src/hfile.c
  src/node.c
  src/net.c
)

target_include_directories(hf PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_compile_options(hf PRIVATE -Wall -Wextra)

target_compile_definitions(hf PRIVATE
  $<$<CONFIG:Debug>:DEBUG>
)

# --- Tests ---

enable_testing()

add_executable(hf_unit_cli_test
  test/test_unit_cli.c
)
target_include_directories(hf_unit_cli_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/test
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(hf_unit_cli_test PRIVATE -Wall -Wextra)

add_executable(hf_unit_node_test
  test/test_unit_node.c
)
target_include_directories(hf_unit_node_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/test
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(hf_unit_node_test PRIVATE -Wall -Wextra)

add_executable(hf_unit_net_test
  test/test_unit_net.c
)
target_include_directories(hf_unit_net_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/test
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(hf_unit_net_test PRIVATE -Wall -Wextra)

add_executable(hf_integration_cli_test
  test/test_integration_cli.c
  test/integration_support.c
)
target_include_directories(hf_integration_cli_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/test
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(hf_integration_cli_test PRIVATE -Wall -Wextra)

add_executable(hf_integration_transfer_test
  test/test_integration_transfer.c
  test/integration_support.c
)
target_include_directories(hf_integration_transfer_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/test
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(hf_integration_transfer_test PRIVATE -Wall -Wextra)

add_executable(hf_integration_protocol_test
  test/test_integration_protocol.c
  test/integration_support.c
)
target_include_directories(hf_integration_protocol_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/test
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(hf_integration_protocol_test PRIVATE -Wall -Wextra)

add_test(NAME unit_cli COMMAND hf_unit_cli_test)
add_test(NAME unit_node COMMAND hf_unit_node_test)
add_test(NAME unit_net COMMAND hf_unit_net_test)
add_test(NAME integration_cli
  COMMAND hf_integration_cli_test "$<TARGET_FILE:hf>" "${PROJECT_SOURCE_DIR}"
)
add_test(NAME integration_transfer
  COMMAND hf_integration_transfer_test "$<TARGET_FILE:hf>" "${PROJECT_SOURCE_DIR}"
)
add_test(NAME integration_protocol
  COMMAND hf_integration_protocol_test "$<TARGET_FILE:hf>" "${PROJECT_SOURCE_DIR}"
)

set_tests_properties(unit_cli PROPERTIES LABELS "unit;cli")
set_tests_properties(unit_node PROPERTIES LABELS "unit;node")
set_tests_properties(unit_net PROPERTIES LABELS "unit;net")
set_tests_properties(integration_cli PROPERTIES
  LABELS "integration;cli"
  TIMEOUT 30
)
set_tests_properties(integration_transfer PROPERTIES
  LABELS "integration;transfer;server"
  TIMEOUT 60
)
set_tests_properties(integration_protocol PROPERTIES
  LABELS "integration;protocol;server"
  TIMEOUT 90
)

include(GNUInstallDirs)

install(TARGETS hf
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
