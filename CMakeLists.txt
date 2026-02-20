#==============================================================================
# CMakeLists.txt - HFile Build Configuration
#==============================================================================
#
# Build options:
#   - DCROSS_COMPILE_WINDOWS=ON  : Cross-compile for Windows using mingw-w64
#   - DCMAKE_BUILD_TYPE=Release  : Build in Release mode (default: Debug)
#
# Examples:
#   cmake -S . -B build -G Ninja                                    # Debug
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release        # Release
#   cmake -S . -B build -G Ninja -DCROSS_COMPILE_WINDOWS=ON        # Windows
#==============================================================================

cmake_minimum_required(VERSION 3.16)

#------------------------------------------------------------------------------
# Cross-compilation option for Windows
#------------------------------------------------------------------------------
option(CROSS_COMPILE_WINDOWS
  "Cross-compile for Windows using mingw-w64 (requires mingw-w64 installed)"
  OFF)

if(CROSS_COMPILE_WINDOWS)
  # Set target system to Windows for cross-compilation
  set(CMAKE_SYSTEM_NAME Windows)
  set(CMAKE_SYSTEM_PROCESSOR x86_64)

  # Use mingw-w64 toolchain if not specified
  if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
  endif()
  if(NOT CMAKE_RC_COMPILER)
    set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
  endif()
endif()

#------------------------------------------------------------------------------
# Project definition
#------------------------------------------------------------------------------
project(HFile VERSION 0.0.1 LANGUAGES C)

#------------------------------------------------------------------------------
# Executable target
#------------------------------------------------------------------------------
add_executable(hf
  src/main.c
  src/server.c
  src/client.c
  src/helper.c
)

#------------------------------------------------------------------------------
# Include directories
#------------------------------------------------------------------------------
target_include_directories(hf PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

#------------------------------------------------------------------------------
# Compiler options
#------------------------------------------------------------------------------
# Enable strict warnings for better code quality
target_compile_options(hf PRIVATE -Wall -Wextra)

# Enable debug symbols in Debug builds
target_compile_definitions(hf PRIVATE
  $<$<CONFIG:Debug>:DEBUG>
)

#------------------------------------------------------------------------------
# Link libraries
#------------------------------------------------------------------------------
# Link Windows Sockets library (ws2_32) for Windows targets
# - Native Windows build (CMAKE_SYSTEM_NAME is Windows)
# - Cross-compilation for Windows (CROSS_COMPILE_WINDOWS)
if(CROSS_COMPILE_WINDOWS OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
  target_link_libraries(hf PRIVATE ws2_32)
endif()

#------------------------------------------------------------------------------
# Installation
#------------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS hf
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
