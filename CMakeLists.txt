cmake_minimum_required(VERSION 3.13)
project(sdrpp)

### cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-O3 -march=native -flto -std=c++17" ..

find_package(CURL REQUIRED)

# Backends
option(OPT_BACKEND_GLFW "Use the GLFW backend" ON)
option(OPT_BACKEND_ANDROID "Use the Android backend" OFF)

# Compatibility Options
option(OPT_OVERRIDE_STD_FILESYSTEM "Use a local version of std::filesystem on systems that don't have it yet" OFF)

# Sources
option(OPT_BUILD_AIRSPY_SOURCE "Build Airspy Source Module (Dependencies: libairspy)" ON)
option(OPT_BUILD_AUDIO_SOURCE "Build Audio Source Module (Dependencies: rtaudio)" ON)
option(OPT_BUILD_FILE_SOURCE "Wav file source" ON)
option(OPT_BUILD_SDRPP_SERVER_SOURCE "Build Malva Server Source Module (no dependencies required)" ON)
option(OPT_BUILD_RFSPACE_SOURCE "Build RFspace Source Module (no dependencies required)" OFF)
option(OPT_BUILD_REMOTE_CONTROL "REMOTE_CONTROL mode" ON)
option(OPT_BUILD_BLADERF_SOURCE "Build BLADERF Source Module" ON)

# Sinks
option(OPT_BUILD_AUDIO_SINK "Build Audio Sink Module (Dependencies: rtaudio)" ON)
option(OPT_BUILD_NETWORK_SINK "Build Audio Sink Module (no dependencies required)" ON)

# Decoders
option(OPT_BUILD_RADIO "Main audio modulation decoder (AM, FM, SSB, etc...)" ON)

# Misc
option(OPT_BUILD_RECORDER "Audio and baseband recorder" ON)
option(OPT_BUILD_SCANNER2 "Search mode" ON)
option(OPT_BUILD_SCANNER3 "Observation mode" ON)
option(OPT_BUILD_SUPERVISION4 "Multichannel Observation mode" ON)
option(OPT_BUILD_NOISE_REDUCTION_LOGMMSE "Build LOGMMSE noise reduction" OFF)
option(OPT_BUILD_SCANNER "Frequency scanner" OFF)


# Other options
option(USE_INTERNAL_LIBCORRECT "Use an internal version of libcorrect" ON)
option(USE_BUNDLE_DEFAULTS "Set the default resource and module directories to the right ones for a MacOS .app" OFF)

# Module cmake path
set(SDRPP_MODULE_CMAKE "${CMAKE_SOURCE_DIR}/sdrpp_module.cmake")

# Root source folder
set(SDRPP_CORE_ROOT "${CMAKE_SOURCE_DIR}/core/src/")

# Compiler flags
if (${CMAKE_BUILD_TYPE} MATCHES "Debug")
    # Debug Flags
    if (MSVC)
        set(SDRPP_COMPILER_FLAGS /std:c++17 /EHsc)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(SDRPP_COMPILER_FLAGS -g -Og -std=c++17 -Wno-unused-command-line-argument -undefined dynamic_lookup)
    else ()
        set(SDRPP_COMPILER_FLAGS -g -Og -std=c++17)
    endif ()
else()
    # Normal Flags
    if (MSVC)
        set(SDRPP_COMPILER_FLAGS /O2 /Ob2 /std:c++17 /EHsc)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(SDRPP_COMPILER_FLAGS -O3 -std=c++17 -Wno-unused-command-line-argument -undefined dynamic_lookup)
    else ()
        # set(SDRPP_COMPILER_FLAGS -O3 -std=c++17)
        set(SDRPP_COMPILER_FLAGS -O2 -march=native -funroll-loops -std=c++17)
    endif ()
endif()
set(SDRPP_MODULE_COMPILER_FLAGS ${SDRPP_COMPILER_FLAGS})

# Set a default install prefix
if (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
        set(CMAKE_INSTALL_PREFIX "/usr/local" CACHE PATH "..." FORCE)
    else()
        set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "..." FORCE)
    endif()
endif()

# Configure toolchain for android
if (ANDROID)
    set(CMAKE_SHARED_LINKER_FLAGS
        "${CMAKE_SHARED_LINKER_FLAGS} -u ANativeActivity_onCreate"
    )
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_CXX_STANDARD 14)
    set(CMAKE_CXX14_EXTENSION_COMPILE_OPTION "-std=c++17")
endif (ANDROID)

# Core of SDR++
add_subdirectory("core")

# Source modules
if (OPT_BUILD_AIRSPY_SOURCE)
add_subdirectory("source_modules/airspy_source")
endif (OPT_BUILD_AIRSPY_SOURCE)

if (OPT_BUILD_AUDIO_SOURCE)
add_subdirectory("source_modules/audio_source")
endif (OPT_BUILD_AUDIO_SOURCE)

if (OPT_BUILD_FILE_SOURCE)
add_subdirectory("source_modules/file_source")
endif (OPT_BUILD_FILE_SOURCE)

if (OPT_BUILD_SDRPP_SERVER_SOURCE)
add_subdirectory("source_modules/sdrpp_server_source")
add_subdirectory("source_modules/tcp_client_source")
add_subdirectory("source_modules/aster_server_source")
endif (OPT_BUILD_SDRPP_SERVER_SOURCE)

if (OPT_BUILD_REMOTE_CONTROL)
add_subdirectory("source_modules/remote_control")
endif (OPT_BUILD_REMOTE_CONTROL)

if (OPT_BUILD_BLADERF_SOURCE)
add_subdirectory("source_modules/bladerf_source")
endif (OPT_BUILD_BLADERF_SOURCE)

# Sink modules
if (OPT_BUILD_AUDIO_SINK)
add_subdirectory("sink_modules/audio_sink")
endif (OPT_BUILD_AUDIO_SINK)

if (OPT_BUILD_PORTAUDIO_SINK)
add_subdirectory("sink_modules/portaudio_sink")
endif (OPT_BUILD_PORTAUDIO_SINK)

if (OPT_BUILD_NETWORK_SINK)
add_subdirectory("sink_modules/network_sink")
endif (OPT_BUILD_NETWORK_SINK)

# Decoders
if (OPT_BUILD_RADIO)
add_subdirectory("decoder_modules/radio")
endif (OPT_BUILD_RADIO)

# Misc
if (OPT_BUILD_RECORDER)
add_subdirectory("misc_modules/recorder")
endif (OPT_BUILD_RECORDER)

if (OPT_BUILD_SCANNER2)
add_subdirectory("misc_modules/scanner2")
endif (OPT_BUILD_SCANNER2)

if (OPT_BUILD_SCANNER3)
add_subdirectory("misc_modules/scanner3")
endif (OPT_BUILD_SCANNER3)

if (OPT_BUILD_SUPERVISION4)
add_subdirectory("misc_modules/supervision4")
endif (OPT_BUILD_SUPERVISION4)

if (OPT_BUILD_SCANNER)
add_subdirectory("misc_modules/scanner")
endif (OPT_BUILD_SCANNER)

if (OPT_BUILD_NOISE_REDUCTION_LOGMMSE)
add_subdirectory("misc_modules/noise_reduction_logmmse")
endif (OPT_BUILD_NOISE_REDUCTION_LOGMMSE)

# Применяем libcurl ко всем модулям в misc_modules
foreach(module IN ITEMS recorder scanner2 scanner3 supervision4)
    set(target_name ${module})
    if (TARGET ${target_name})
        target_link_libraries(${target_name} PRIVATE CURL::libcurl)
    endif()
endforeach()

add_executable(sdrpp "src/main.cpp" "win32/resources.rc")
target_link_libraries(sdrpp PRIVATE sdrpp_core)

# Compiler arguments
target_compile_options(sdrpp PRIVATE ${SDRPP_COMPILER_FLAGS})

# Copy dynamic libs over
if (MSVC)
    add_custom_target(do_always ALL xcopy /s \"$<TARGET_FILE_DIR:sdrpp_core>\\*.dll\" \"$<TARGET_FILE_DIR:sdrpp>\" /Y)
    add_custom_target(do_always_volk ALL xcopy /s \"C:/Program Files/PothosSDR/bin\\volk.dll\" \"$<TARGET_FILE_DIR:sdrpp>\" /Y)
endif ()


if (${CMAKE_SYSTEM_NAME} MATCHES "OpenBSD")
    add_custom_target(do_always ALL cp \"$<TARGET_FILE_DIR:sdrpp_core>/libsdrpp_core.so\" \"$<TARGET_FILE_DIR:sdrpp>\")
endif ()

if (${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD")
    target_link_libraries(sdrpp PUBLIC pthread)
    add_custom_target(do_always ALL cp \"$<TARGET_FILE_DIR:sdrpp_core>/libsdrpp_core.so\" \"$<TARGET_FILE_DIR:sdrpp>\")
endif ()

if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    add_custom_target(do_always ALL cp \"$<TARGET_FILE_DIR:sdrpp_core>/libsdrpp_core.so\" \"$<TARGET_FILE_DIR:sdrpp>\")
endif ()

if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    add_custom_target(do_always ALL cp \"$<TARGET_FILE_DIR:sdrpp_core>/libsdrpp_core.dylib\" \"$<TARGET_FILE_DIR:sdrpp>\")
endif ()

# Create module cmake file
configure_file(${CMAKE_SOURCE_DIR}/sdrpp_module.cmake ${CMAKE_CURRENT_BINARY_DIR}/sdrpp_module.cmake @ONLY)

# Install directives
install(TARGETS sdrpp DESTINATION bin)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/root/res/bandplans DESTINATION share/sdrpp)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/root/res/colormaps DESTINATION share/sdrpp)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/root/res/fonts DESTINATION share/sdrpp)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/root/res/icons DESTINATION share/sdrpp)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/root/res/themes DESTINATION share/sdrpp)
configure_file(${CMAKE_SOURCE_DIR}/sdrpp.desktop ${CMAKE_CURRENT_BINARY_DIR}/sdrpp.desktop @ONLY)

if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sdrpp.desktop DESTINATION share/applications)
endif ()

# Create uninstall target
configure_file(${CMAKE_SOURCE_DIR}/cmake_uninstall.cmake ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake @ONLY)
add_custom_target(uninstall ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)