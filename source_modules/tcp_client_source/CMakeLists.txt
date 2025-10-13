cmake_minimum_required(VERSION 3.13)
project(tcp_client_source)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(tcp_client_source PRIVATE "src/")
