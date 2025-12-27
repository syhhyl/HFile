cmake_minimum_required(VERSION 3.16)

project(HFile C)

add_executable(client client.c utils.c utils.h)
add_executable(server server.c)



foreach(t client server)
  target_compile_features(${t} PRIVATE c_std_11)
  target_compile_options(${t} PRIVATE -Wall -Wextra)
endforeach()




