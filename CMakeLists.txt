cmake_minimum_required(VERSION 3.16)

project(HFile VERSION 0.0.1 LANGUAGES C)

add_executable(hf
  src/hfile.c
  src/node.c
  src/discovery.c
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

add_executable(hf_unit_test
  test/test_unit.c
)
target_include_directories(hf_unit_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/test
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(hf_unit_test PRIVATE -Wall -Wextra)

add_executable(hf_int_test
  test/test_integration.c
)
target_include_directories(hf_int_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/test
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(hf_int_test PRIVATE -Wall -Wextra)

add_test(NAME unit COMMAND hf_unit_test)
add_test(NAME integration COMMAND hf_int_test "$<TARGET_FILE:hf>" "${PROJECT_SOURCE_DIR}")

include(GNUInstallDirs)

install(TARGETS hf
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
