cmake_minimum_required(VERSION 3.16)

project(snecoin
  VERSION 0.1.0
  DESCRIPTION "SNECoin ledger client"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

find_package(CURL REQUIRED)

add_executable(sne
  src/main.cpp
  src/config.cpp
  src/etcd_client.cpp
  src/ledger.cpp)

target_compile_definitions(sne PRIVATE SNE_VERSION="${PROJECT_VERSION}")
target_compile_options(sne PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(sne PRIVATE CURL::libcurl)
