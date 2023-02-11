cmake_minimum_required (VERSION 3.16)

set(CMAKE_CXX_STANDARD 17)

project(sandbox LANGUAGES CXX)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(GLIB REQUIRED glib-2.0)
pkg_check_modules(GST REQUIRED gstreamer-1.0)
pkg_check_modules(GST_BASE REQUIRED gstreamer-plugins-base-1.0)

add_executable(sandbox sandbox.cpp)

target_include_directories(sandbox PRIVATE ${GST_INCLUDE_DIRS} ${GST_BASE_INCLUDE_DIRS})
target_link_directories(sandbox PRIVATE ${GST_LIBRARY_DIRS} ${GST_BASE_LIBRARY_DIRS})
target_link_libraries(sandbox PRIVATE Threads::Threads ${GST_LIBRARIES} gstvideo-1.0 gstapp-1.0)
