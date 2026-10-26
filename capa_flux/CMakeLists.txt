cmake_minimum_required(VERSION 3.16)
project(capa_flux VERSION 0.0.0 LANGUAGES CXX)

# capa_flux applies capa declarations to the threads of a flux PartitionedExecutor, through the
# executor's on_thread_start() hook. It does not link flux; its tests do.

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ament_cmake REQUIRED)
find_package(capa_core REQUIRED)
find_package(rclcpp REQUIRED)

add_library(capa_flux src/flux.cpp)
target_include_directories(capa_flux PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(capa_flux PUBLIC capa_core::capa_core)
ament_target_dependencies(capa_flux PUBLIC rclcpp)
target_compile_options(capa_flux PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(capa_flux PROPERTIES POSITION_INDEPENDENT_CODE ON)

install(DIRECTORY include/ DESTINATION include)
install(TARGETS capa_flux
  EXPORT export_capa_flux
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES DESTINATION include)
ament_export_targets(export_capa_flux HAS_LIBRARY_TARGET)
ament_export_include_directories(include)
ament_export_dependencies(capa_core rclcpp)

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(std_msgs REQUIRED)
  find_package(flux_cpp REQUIRED)
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/run)
  ament_add_gtest(test_capa_flux test/test_schedule.cpp test/test_chain.cpp
    ENV CAPA_RUN_DIR=${CMAKE_CURRENT_BINARY_DIR}/run)
  if(TARGET test_capa_flux)
    target_link_libraries(test_capa_flux capa_flux flux_cpp::flux_cpp)
    ament_target_dependencies(test_capa_flux rclcpp std_msgs)
  endif()
endif()

ament_package()
