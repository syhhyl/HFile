cmake_minimum_required(VERSION 3.16)

project(HFile VERSION 0.0.1 LANGUAGES C)

add_executable(hf
  src/hfile.c
  src/server.c
  src/http.c
  src/message_store.c
  src/webui.c
  src/client.c
  src/protocol.c
  src/helper.c
  src/cli.c
  src/net.c
  src/fs.c
)

add_library(lz4 STATIC
  external/lz4/lz4.c
)

target_include_directories(lz4 PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/external/lz4
)

target_include_directories(hf PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(hf PRIVATE lz4)

if (MSVC)
  target_compile_options(hf PRIVATE /W4)
else()
  target_compile_options(hf PRIVATE -Wall -Wextra)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
  target_link_libraries(hf PRIVATE ws2_32 shell32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(hf PRIVATE Threads::Threads)
endif()

target_compile_definitions(hf PRIVATE
  $<$<CONFIG:Debug>:DEBUG>
)

include(GNUInstallDirs)

install(TARGETS hf
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
