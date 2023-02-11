cmake_minimum_required (VERSION 3.16)

set(CMAKE_CXX_STANDARD 17)

project(sandbox LANGUAGES CXX C)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(GLIB REQUIRED glib-2.0)
pkg_check_modules(GST REQUIRED gstreamer-1.0)
pkg_check_modules(GST_BASE REQUIRED gstreamer-plugins-base-1.0)

if(NOT WIN32)
    option(WITH_APPSINK "Use internal version of appsink element" ON)
else()
    set(WITH_APPSINK OFF)
endif()

set(SOURCE sandbox.cpp)
if(WITH_APPSINK)
    list(APPEND SOURCE app/gstappsink.c)
endif()
set(LIBRARY gstvideo-1.0 gstbase-1.0)
if(NOT WITH_APPSINK)
    list(APPEND LIBRARY gstapp-1.0)
endif()

add_executable(sandbox ${SOURCE})

target_compile_definitions(sandbox PRIVATE $<$<BOOL:${WITH_APPSINK}>:WITH_APPSINK>)
target_include_directories(sandbox PRIVATE ${GST_INCLUDE_DIRS} ${GST_BASE_INCLUDE_DIRS})
target_link_directories(sandbox PRIVATE ${GST_LIBRARY_DIRS} ${GST_BASE_LIBRARY_DIRS})
target_link_libraries(sandbox PRIVATE Threads::Threads ${GST_LIBRARIES} ${GST_BASE_LIBRARIES} ${LIBRARY})
