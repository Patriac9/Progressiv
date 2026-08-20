cmake_minimum_required(VERSION 4.2)
project(Fluid)

set(CMAKE_CXX_STANDARD 20)

find_package(Vulkan REQUIRED)

add_library(Fluid STATIC
        src/window.cpp)
target_include_directories(Fluid PUBLIC ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_SOURCE_DIR}/third_party/glfw-3.4/include)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/glfw-3.4)
target_link_libraries(Fluid PUBLIC glfw)
target_link_libraries(Fluid PUBLIC Vulkan::Vulkan)


