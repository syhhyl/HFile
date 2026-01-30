cmake_minimum_required(VERSION 3.16)

project(HFile VERSION 0.0.1 LANGUAGES C)

add_executable(hf
  main.c 
  server.c
  client.c
)

target_compile_options(hf PRIVATE -Wall -Wextra)
target_compile_definitions(hf PRIVATE
  $<$<CONFIG:Debug>:DEBUG>
)

include(GNUInstallDirs)

install(TARGETS hf
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)




