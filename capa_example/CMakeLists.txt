cmake_minimum_required(VERSION 3.16)
project(capa_example)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(capa_core REQUIRED)
find_package(capa_flux REQUIRED)
find_package(flux_cpp REQUIRED)
find_package(flux_std_adapters REQUIRED)
find_package(sensor_msgs REQUIRED)

add_executable(preflight src/preflight.cpp)
target_link_libraries(preflight capa_core::capa_core)
target_compile_options(preflight PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS preflight DESTINATION lib/${PROJECT_NAME})

add_executable(chain src/chain.cpp)
target_link_libraries(chain capa_flux::capa_flux flux_cpp::flux_cpp)
target_include_directories(chain PRIVATE ${flux_std_adapters_INCLUDE_DIRS})
ament_target_dependencies(chain rclcpp sensor_msgs)
target_compile_options(chain PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS chain DESTINATION lib/${PROJECT_NAME})

# The C++ fences of docs/*/api.*.md, compiled so a fence cannot name what does not exist. Never
# run and never installed.
add_library(capa_doc_examples STATIC src/doc_api.cpp)
target_link_libraries(capa_doc_examples capa_flux::capa_flux flux_cpp::flux_cpp)
ament_target_dependencies(capa_doc_examples rclcpp)
target_compile_options(capa_doc_examples PRIVATE -Wall -Wextra -Wpedantic)

install(DIRECTORY config DESTINATION share/${PROJECT_NAME})

ament_package()
