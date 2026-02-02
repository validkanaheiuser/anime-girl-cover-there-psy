cmake_minimum_required(VERSION 3.22.1)

project("zygisk")

link_libraries(log dl)

add_library(zygisk SHARED module.cpp)
