cmake_minimum_required(VERSION 3.16)

project(HFile C)

add_executable(hf main.c server.c client.c)



foreach(t hf)
  target_compile_features(${t} PRIVATE c_std_11)
  target_compile_options(${t} PRIVATE -Wall -Wextra)
endforeach()




