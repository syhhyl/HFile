cmake_minimum_required(VERSION 3.16)

project(HFile VERSION 0.0.1 LANGUAGES C)

add_executable(hf
  src/main.c
  src/server.c
  src/client.c
  src/helper.c
)

target_include_directories(hf PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(hf PRIVATE -Wall -Wextra)

if(WIN32)
  target_link_libraries(hf PRIVATE ws2_32)
endif()

target_compile_definitions(hf PRIVATE
  $<$<CONFIG:Debug>:DEBUG>
)

include(GNUInstallDirs)

install(TARGETS hf
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)



