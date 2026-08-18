cmake_minimum_required(VERSION 3.16)
project(cachelens CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_executable(cachelens src/main.cpp)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDW REQUIRED libdw)
target_include_directories(cachelens PRIVATE ${LIBDW_INCLUDE_DIRS})
target_link_libraries(cachelens PRIVATE ${LIBDW_LIBRARIES})
