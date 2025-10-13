cmake_minimum_required(VERSION 3.13)
project(audio_sink)

# Находим RtAudio через pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(RTAUDIO REQUIRED rtaudio)

# Собираем исходники
file(GLOB SRC "src/*.cpp")

# Подключаем модульную систему SDR++
include(${SDRPP_MODULE_CMAKE})

# Подключаем RtAudio к уже созданной цели audio_sink
target_include_directories(audio_sink PRIVATE ${RTAUDIO_INCLUDE_DIRS})
target_link_directories(audio_sink PRIVATE ${RTAUDIO_LIBRARY_DIRS})
target_link_libraries(audio_sink PRIVATE ${RTAUDIO_LIBRARIES})
