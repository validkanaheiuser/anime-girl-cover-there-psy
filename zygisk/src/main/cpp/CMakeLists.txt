cmake_minimum_required(VERSION 3.30.5)

project("mockgps")

link_libraries(log dl)

add_library(mockgps SHARED module.cpp)

target_link_options(mockgps PRIVATE -Wl,--gc-sections)
