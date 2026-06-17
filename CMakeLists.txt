cmake_minimum_required(VERSION 3.5)
project(esdk2_access_sample CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread -Wall")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pthread")

add_definitions(-DDJI_LOGGER_LEVEL=DJI_LOGGER_LEVEL_DEBUG)

# Custom CMake modules (e.g. FindMosquitto.cmake).
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# --- mosquitto (C++ binding) ---
find_package(Mosquitto REQUIRED)

# Header search paths shared across modules.
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include       # cross-module public headers (mqtt/, rid/, common/)
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party    # json/json.hpp
)

# --- example executable: rid_report_demo ---
add_executable(rid_report_demo
    examples/rid_report_demo/main.cpp
    src/common/log.cpp
    src/mqtt/mqtt_client.cpp
    src/mqtt/mqtt_client_manager.cpp
    src/mqtt/broker_discovery.cpp
    src/rid/rid_reporter.cpp
)

target_link_libraries(rid_report_demo PRIVATE Mosquitto::Mosquitto)
