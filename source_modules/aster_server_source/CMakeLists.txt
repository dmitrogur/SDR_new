cmake_minimum_required(VERSION 3.13)
project(aster_server_source)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(aster_server_source PRIVATE "src/")
