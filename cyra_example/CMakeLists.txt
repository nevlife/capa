cmake_minimum_required(VERSION 3.16)
project(cyra_example)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(cyra_core REQUIRED)
find_package(cyra_flux REQUIRED)
find_package(flux_cpp REQUIRED)
find_package(flux_example_msgs REQUIRED)

add_executable(preflight src/preflight.cpp)
target_link_libraries(preflight cyra_core::cyra_core)
target_compile_options(preflight PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS preflight DESTINATION lib/${PROJECT_NAME})

add_executable(chain src/chain.cpp)
target_link_libraries(chain cyra_flux::cyra_flux flux_cpp::flux_cpp)
ament_target_dependencies(chain rclcpp flux_example_msgs)
target_compile_options(chain PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS chain DESTINATION lib/${PROJECT_NAME})

install(DIRECTORY config DESTINATION share/${PROJECT_NAME})

ament_package()
