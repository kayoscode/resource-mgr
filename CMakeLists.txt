cmake_minimum_required(VERSION 4.0)
enable_testing()

set(PROJ_NAME resource-manager)

cmake_minimum_required(VERSION 3.14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
project(${PROJ_NAME} VERSION 1.0 LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
FetchContent_MakeAvailable(googletest)
enable_testing()

set(HEADERS
   include/ResourceManager.h)

add_library(${PROJ_NAME} STATIC main.cpp ${HEADERS})
target_include_directories(${PROJ_NAME} PUBLIC include)

add_subdirectory(tests)